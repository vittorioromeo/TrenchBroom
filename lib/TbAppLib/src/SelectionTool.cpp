/*
 Copyright (C) 2010 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ui/SelectionTool.h"

#include "base/Color.h"
#include "base/PreferenceManager.h"
#include "gl/Camera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h" // IWYU pragma: keep
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/NodeHandleManager.h"
#include "mdl/NodeHandles.h"
#include "mdl/PatchNode.h"
#include "mdl/Transaction.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "render/RenderContext.h"
#include "render/RenderService.h"
#include "ui/GestureTracker.h"
#include "ui/InputState.h"
#include "ui/MapDocument.h"

#include "kd/contracts.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tb::ui
{
namespace
{

mdl::HitFilter isNodeSelectable(const mdl::EditorContext& editorContext)
{
  return [&](const auto& hit) {
    if (const auto faceHandle = mdl::hitToFaceHandle(hit))
    {
      if (!editorContext.selectable(*faceHandle->node(), faceHandle->face()))
      {
        return false;
      }
    }
    if (const auto* node = mdl::hitToNode(hit))
    {
      return editorContext.selectable(*findOutermostClosedGroupOrNode(node));
    }
    return false;
  };
}

bool isFaceClick(const InputState& inputState)
{
  return inputState.modifierKeysDown(ModifierKeys::Shift);
}

bool isMultiClick(const InputState& inputState)
{
  return inputState.modifierKeysDown(ModifierKeys::CtrlCmd);
}

bool isCoplanarFaceClick(const InputState& inputState)
{
  return inputState.checkModifierKeys(
    ModifierKeyPressed::DontCare, ModifierKeyPressed::Yes, ModifierKeyPressed::Yes);
}

const mdl::Hit& firstHit(const InputState& inputState, const mdl::HitFilter& hitFilter)
{
  return inputState.pickResult().first(hitFilter);
}

std::vector<mdl::Node*> collectSelectableChildren(
  const mdl::EditorContext& editorContext, const mdl::Node* node)
{
  return mdl::collectSelectableNodes(node->children(), editorContext);
}

bool canHandleLeftClick(
  const InputState& inputState, const mdl::EditorContext& editorContext)
{
  return inputState.mouseButtonsPressed(MouseButtons::Left)
         && editorContext.canChangeSelection();
}

bool handleClick(const InputState& inputState, const mdl::EditorContext& editorContext)
{
  return canHandleLeftClick(inputState, editorContext)
         && inputState.checkModifierKeys(
           ModifierKeyPressed::DontCare,
           ModifierKeyPressed::No,
           ModifierKeyPressed::DontCare);
}

void replaceOrExtendFaceSelection(
  mdl::Map& map,
  const InputState& inputState,
  const std::vector<mdl::BrushFaceHandle>& faces,
  std::string_view transactionName)
{
  auto transaction = mdl::Transaction{map, std::string{transactionName}};
  if (!isMultiClick(inputState))
  {
    deselectAll(map);
  }
  else if (map.selection().hasNodes())
  {
    convertToFaceSelection(map);
  }
  selectBrushFaces(map, faces);
  transaction.commit();
}

void adjustGrid(const InputState& inputState, mdl::Grid& grid)
{
  const auto factor = pref(Preferences::CameraMouseWheelInvert) ? -1.0f : 1.0f;
  if (factor * inputState.scrollY() < 0.0f)
  {
    grid.incSize();
  }
  else if (factor * inputState.scrollY() > 0.0f)
  {
    grid.decSize();
  }
}

/**
 * Returns a pair where:
 *  - first is the first node in the given list that's currently selected
 *  - second is the next selectable node in the list
 */
template <typename I>
std::pair<mdl::Node*, mdl::Node*> findSelectionPair(I it, I end)
{
  const auto first =
    std::find_if(it, end, [](const auto* node) { return node->selected(); });
  if (first == end)
  {
    return {nullptr, nullptr};
  }

  const auto next = std::next(first);
  if (next == end)
  {
    return {*first, nullptr};
  }

  return {*first, *next};
}

void drillSelection(const InputState& inputState, mdl::Map& map)
{
  using namespace mdl::HitFilters;

  const auto& editorContext = map.editorContext();

  const auto hits = inputState.pickResult().all(
    type(mdl::nodeHitType()) && isNodeSelectable(editorContext));

  // Hits may contain multiple brush/entity hits that are inside closed groups. These need
  // to be converted to group hits using findOutermostClosedGroupOrNode() and multiple
  // hits on the same Group need to be collapsed.
  const auto hitNodes = hitsToNodesWithGroupPicking(hits);

  const auto forward =
    (inputState.scrollY() > 0.0f) != (pref(Preferences::CameraMouseWheelInvert));
  const auto nodePair = forward
                          ? findSelectionPair(std::begin(hitNodes), std::end(hitNodes))
                          : findSelectionPair(std::rbegin(hitNodes), std::rend(hitNodes));

  auto* selectedNode = nodePair.first;
  auto* nextNode = nodePair.second;

  if (nextNode)
  {
    auto transaction = mdl::Transaction{map, "Drill Selection"};
    deselectNodes(map, {selectedNode});
    selectNodes(map, {nextNode});
    transaction.commit();
  }
}

class PaintSelectionDragTracker : public GestureTracker
{
private:
  mdl::Map& m_map;

public:
  explicit PaintSelectionDragTracker(mdl::Map& map)
    : m_map{map}
  {
  }

  bool update(const InputState& inputState) override
  {
    using namespace mdl::HitFilters;

    const auto& editorContext = m_map.editorContext();
    if (m_map.selection().hasBrushFaces())
    {
      const auto hit = firstHit(
        inputState,
        type(mdl::BrushNode::BrushHitType) && isNodeSelectable(editorContext));
      if (const auto faceHandle = mdl::hitToFaceHandle(hit))
      {
        const auto* brushNode = faceHandle->node();
        const auto& face = faceHandle->face();
        if (!face.selected() && editorContext.selectable(*brushNode, face))
        {
          selectBrushFaces(m_map, {*faceHandle});
        }
      }
    }
    else
    {
      contract_assert(m_map.selection().hasNodes());

      const auto hit =
        firstHit(inputState, type(mdl::nodeHitType()) && isNodeSelectable(editorContext));
      if (hit.isMatch())
      {
        auto* node = findOutermostClosedGroupOrNode(mdl::hitToNode(hit));
        if (!node->selected() && editorContext.selectable(*node))
        {
          selectNodes(m_map, {node});
        }
      }
    }
    return true;
  }

  void end(const InputState&) override { m_map.commitTransaction(); }

  void cancel() override { m_map.cancelTransaction(); }
};

struct MarqueeRect
{
  float minX;
  float minY;
  float maxX;
  float maxY;

  static MarqueeRect between(const vm::vec2f& a, const vm::vec2f& b)
  {
    return {
      std::min(a.x(), b.x()),
      std::min(a.y(), b.y()),
      std::max(a.x(), b.x()),
      std::max(a.y(), b.y()),
    };
  }

  bool contains(const vm::vec2f& point) const
  {
    return point.x() >= minX && point.x() <= maxX && point.y() >= minY
           && point.y() <= maxY;
  }

  bool contains(const MarqueeRect& other) const
  {
    return other.minX >= minX && other.maxX <= maxX && other.minY >= minY
           && other.maxY <= maxY;
  }

  bool intersects(const MarqueeRect& other) const
  {
    return other.maxX >= minX && other.minX <= maxX && other.maxY >= minY
           && other.minY <= maxY;
  }
};

std::optional<vm::vec2f> projectToView(const gl::Camera& camera, const vm::vec3d& point)
{
  const auto projected = camera.project(vm::vec3f{point});
  // Allow for rounding at the near and far planes when clipping bounding boxes.
  if (projected.z() < -0.0001f || projected.z() > 1.0001f)
  {
    return std::nullopt;
  }
  return vm::vec2f{
    projected.x(), static_cast<float>(camera.viewport().height) - projected.y()};
}

std::optional<MarqueeRect> projectBounds(
  const gl::Camera& camera, const vm::bbox3d& bounds, bool& fullyInDepth)
{
  auto result = std::optional<MarqueeRect>{};
  fullyInDepth = true;
  const auto addPoint = [&](const vm::vec3d& vertex) {
    if (const auto point = projectToView(camera, vertex))
    {
      if (result)
      {
        result->minX = std::min(result->minX, point->x());
        result->minY = std::min(result->minY, point->y());
        result->maxX = std::max(result->maxX, point->x());
        result->maxY = std::max(result->maxY, point->y());
      }
      else
      {
        result = MarqueeRect{point->x(), point->y(), point->x(), point->y()};
      }
    }
  };
  const auto depth = [&](const vm::vec3d& vertex) {
    return vm::dot(vm::vec3f{vertex} - camera.position(), camera.direction());
  };

  for (const auto& vertex : bounds.vertices())
  {
    const auto d = depth(vertex);
    if (d >= camera.nearPlane() && d <= camera.farPlane())
    {
      addPoint(vertex);
    }
    else
    {
      fullyInDepth = false;
    }
  }

  // The visible part of a box can cross a clipping plane without containing any
  // visible corner. Project the clipped edges as well as the original corners.
  bounds.for_each_edge([&](const vm::vec3d& a, const vm::vec3d& b) {
    const auto aDepth = depth(a);
    const auto bDepth = depth(b);
    for (const auto clipDepth : std::array{camera.nearPlane(), camera.farPlane()})
    {
      if (
        (aDepth < clipDepth && bDepth > clipDepth)
        || (bDepth < clipDepth && aDepth > clipDepth))
      {
        const auto t = static_cast<double>(clipDepth - aDepth) / (bDepth - aDepth);
        addPoint(a + t * (b - a));
      }
    }
  });
  return result;
}

bool matchesMarqueeMode(
  const mdl::Node& node,
  const MarqueeSelectionMode mode,
  const mdl::EditorContext& editorContext)
{
  const auto* brush = dynamic_cast<const mdl::BrushNode*>(&node);
  const auto* entity = dynamic_cast<const mdl::EntityNode*>(&node);
  switch (mode)
  {
  case MarqueeSelectionMode::AllObjects:
    // A brush entity is selected as one entity, rather than along with its brushes.
    if (brush)
    {
      if (const auto* parent = dynamic_cast<const mdl::EntityNode*>(node.parent()))
      {
        return !editorContext.selectable(*parent);
      }
    }
    return true;
  case MarqueeSelectionMode::Brushes:
    return brush != nullptr;
  case MarqueeSelectionMode::Entities:
    return entity != nullptr;
  case MarqueeSelectionMode::Patches:
    return dynamic_cast<const mdl::PatchNode*>(&node) != nullptr;
  case MarqueeSelectionMode::Vertices:
    return false;
  }
  return false;
}

class MarqueeSelectionDragTracker : public GestureTracker
{
private:
  SelectionTool& m_tool;
  mdl::Map& m_map;
  const gl::Camera& m_camera;
  MarqueeSelectionMode m_mode;
  vm::vec2f m_start;
  vm::vec2f m_current;
  bool m_extend;

public:
  MarqueeSelectionDragTracker(
    SelectionTool& tool,
    mdl::Map& map,
    const gl::Camera& camera,
    const MarqueeSelectionMode mode,
    const InputState& inputState)
    : m_tool{tool}
    , m_map{map}
    , m_camera{camera}
    , m_mode{mode}
    , m_start{inputState.mouseX(), inputState.mouseY()}
    , m_current{m_start}
    , m_extend{inputState.modifierKeysDown(ModifierKeys::Shift)}
  {
  }

  bool update(const InputState& inputState) override
  {
    m_current = vm::vec2f{inputState.mouseX(), inputState.mouseY()};
    return true;
  }

  void end(const InputState& inputState) override
  {
    update(inputState);
    const auto area = MarqueeRect::between(m_start, m_current);
    if (m_mode == MarqueeSelectionMode::Vertices)
    {
      auto& handles = m_map.nodeHandles();
      auto selected = std::vector<mdl::VertexHandle>{};
      for (const auto& handle : handles.allHandles<mdl::VertexHandle>())
      {
        if (const auto point = projectToView(m_camera, handle.position);
            point && area.contains(*point))
        {
          selected.push_back(handle);
        }
      }
      if (!m_extend)
      {
        handles.deselectAllHandles<mdl::VertexHandle>();
      }
      handles.selectHandles<mdl::VertexHandle>(selected);
      static_cast<Tool&>(m_tool).refreshViews();
      m_tool.notifyToolHandleSelectionChanged();
      return;
    }

    const auto& editorContext = m_map.editorContext();
    auto selected = std::vector<mdl::Node*>{};
    for (auto* node : mdl::collectSelectableNodes({&m_map.worldNode()}, editorContext))
    {
      if (!matchesMarqueeMode(*node, m_mode, editorContext))
      {
        continue;
      }
      auto fullyInDepth = false;
      if (
        const auto bounds = projectBounds(m_camera, node->physicalBounds(), fullyInDepth);
        bounds
        && (m_current.x() >= m_start.x() ? fullyInDepth && area.contains(*bounds) : area.intersects(*bounds)))
      {
        selected.push_back(node);
      }
    }

    auto transaction = mdl::Transaction{m_map, "Marquee Select"};
    if (!m_extend || m_map.selection().hasBrushFaces())
    {
      deselectAll(m_map);
    }
    selectNodes(m_map, selected);
    transaction.commit();
  }

  void cancel() override {}

  void render(
    const InputState&,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) const override
  {
    const auto area = MarqueeRect::between(m_start, m_current);
    constexpr auto overlayDepth = 0.01f;
    const auto polygon = std::vector{
      m_camera.unproject(area.minX, area.minY, overlayDepth),
      m_camera.unproject(area.minX, area.maxY, overlayDepth),
      m_camera.unproject(area.maxX, area.maxY, overlayDepth),
      m_camera.unproject(area.maxX, area.minY, overlayDepth),
    };

    auto service = render::RenderService{renderContext, renderBatch};
    service.setShowOccludedObjects();
    service.setForegroundColor(RgbaF{0.25f, 0.65f, 1.0f, 0.2f});
    service.renderFilledPolygon(polygon);
    service.setForegroundColor(RgbaF{0.4f, 0.8f, 1.0f, 1.0f});
    service.setLineWidth(2.0f);
    service.renderPolygonOutline(polygon);
  }
};

} // namespace

SelectionTool::SelectionTool(
  MapDocument& document, const MarqueeSelectionMode* marqueeMode)
  : ToolController{}
  , Tool{true}
  , m_document{document}
  , m_marqueeMode{marqueeMode}
{
}

Tool& SelectionTool::tool()
{
  return *this;
}

const Tool& SelectionTool::tool() const
{
  return *this;
}

bool SelectionTool::mouseClick(const InputState& inputState)
{
  using namespace mdl::HitFilters;

  auto& map = m_document.map();
  const auto& editorContext = map.editorContext();

  if (!handleClick(inputState, editorContext))
  {
    return false;
  }

  if (isFaceClick(inputState))
  {
    const auto hit = firstHit(
      inputState, type(mdl::BrushNode::BrushHitType) && isNodeSelectable(editorContext));
    if (const auto faceHandle = mdl::hitToFaceHandle(hit))
    {
      const auto* brushNode = faceHandle->node();
      const auto& face = faceHandle->face();
      if (editorContext.selectable(*brushNode, face))
      {
        if (isMultiClick(inputState))
        {
          const auto objects = map.selection().hasNodes();
          if (objects)
          {
            if (brushNode->selected())
            {
              deselectBrushFaces(map, {*faceHandle});
            }
            else
            {
              auto transaction = mdl::Transaction{map, "Select Brush Face"};
              convertToFaceSelection(map);
              selectBrushFaces(map, {*faceHandle});
              transaction.commit();
            }
          }
          else
          {
            if (face.selected())
            {
              deselectBrushFaces(map, {*faceHandle});
            }
            else
            {
              selectBrushFaces(map, {*faceHandle});
            }
          }
        }
        else
        {
          auto transaction = mdl::Transaction{map, "Select Brush Face"};
          deselectAll(map);
          selectBrushFaces(map, {*faceHandle});
          transaction.commit();
        }
      }
    }
    else
    {
      deselectAll(map);
    }
  }
  else
  {
    const auto hit =
      firstHit(inputState, type(mdl::nodeHitType()) && isNodeSelectable(editorContext));
    if (hit.isMatch())
    {
      auto* node = findOutermostClosedGroupOrNode(mdl::hitToNode(hit));
      if (editorContext.selectable(*node))
      {
        if (isMultiClick(inputState))
        {
          if (node->selected())
          {
            deselectNodes(map, {node});
          }
          else
          {
            auto transaction = mdl::Transaction{map, "Select Object"};
            if (map.selection().hasBrushFaces())
            {
              deselectAll(map);
            }
            selectNodes(map, {node});
            transaction.commit();
          }
        }
        else
        {
          auto transaction = mdl::Transaction{map, "Select Object"};
          deselectAll(map);
          selectNodes(map, {node});
          transaction.commit();
        }
      }
    }
    else
    {
      deselectAll(map);
    }
  }

  return true;
}

bool SelectionTool::mouseDoubleClick(const InputState& inputState)
{
  using namespace mdl::HitFilters;

  auto& map = m_document.map();
  const auto& editorContext = map.editorContext();

  // Shift+Alt double click flood fills the clicked face's coplanar surface. Alt is
  // held here, which handleClick() rejects, so we have to catch it before that.
  if (isCoplanarFaceClick(inputState))
  {
    if (!canHandleLeftClick(inputState, editorContext))
    {
      return false;
    }

    const auto hit = firstHit(inputState, type(mdl::BrushNode::BrushHitType));
    if (const auto faceHandle = mdl::hitToFaceHandle(hit))
    {
      if (editorContext.selectable(*faceHandle->node(), faceHandle->face()))
      {
        const auto region = mdl::collectConnectedCoplanarFaces(
          *faceHandle, editorContext, map.worldNode().nodeTree());
        replaceOrExtendFaceSelection(
          map, inputState, region, "Select Connected Coplanar Faces");
      }
    }
    return true;
  }

  if (!handleClick(inputState, editorContext))
  {
    return false;
  }

  if (isFaceClick(inputState))
  {
    const auto hit = firstHit(inputState, type(mdl::BrushNode::BrushHitType));
    if (const auto faceHandle = mdl::hitToFaceHandle(hit))
    {
      auto* brushNode = faceHandle->node();
      const auto& face = faceHandle->face();
      if (editorContext.selectable(*brushNode, face))
      {
        replaceOrExtendFaceSelection(
          map, inputState, mdl::toHandles(brushNode), "Select Brush Faces");
      }
    }
  }
  else
  {
    const auto* currentGroup = map.editorContext().currentGroup();
    const auto inGroup = currentGroup != nullptr;
    const auto hit =
      firstHit(inputState, type(mdl::nodeHitType()) && isNodeSelectable(editorContext));
    if (hit.isMatch())
    {
      const auto hitInGroup =
        inGroup && mdl::hitToNode(hit)->isDescendantOf(*currentGroup);
      if (!inGroup || hitInGroup)
      {
        // If the hit node is inside a closed group, treat it as a hit on the group insted
        auto* groupNode = findOutermostClosedGroup(mdl::hitToNode(hit));
        if (groupNode != nullptr)
        {
          if (editorContext.selectable(*groupNode))
          {
            openGroup(map, *groupNode);
          }
        }
        else
        {
          const auto* node = mdl::hitToNode(hit);
          if (editorContext.selectable(*node))
          {
            const auto* container = node->parent();
            const auto siblings = collectSelectableChildren(editorContext, container);
            if (isMultiClick(inputState))
            {
              if (map.selection().hasBrushFaces())
              {
                deselectAll(map);
              }
              selectNodes(map, siblings);
            }
            else
            {
              auto transaction = mdl::Transaction{map, "Select Brushes"};
              deselectAll(map);
              selectNodes(map, siblings);
              transaction.commit();
            }
          }
        }
      }
      else if (inGroup)
      {
        closeGroup(map);
      }
    }
    else if (inGroup)
    {
      closeGroup(map);
    }
  }

  return true;
}

void SelectionTool::mouseScroll(const InputState& inputState)
{
  auto& map = m_document.map();

  if (inputState.checkModifierKeys(
        ModifierKeyPressed::Yes, ModifierKeyPressed::Yes, ModifierKeyPressed::No))
  {
    adjustGrid(inputState, map.grid());
  }
  else if (inputState.checkModifierKeys(
             ModifierKeyPressed::Yes, ModifierKeyPressed::No, ModifierKeyPressed::No))
  {
    drillSelection(inputState, map);
  }
}

std::unique_ptr<GestureTracker> SelectionTool::acceptMouseDrag(
  const InputState& inputState)
{
  using namespace mdl::HitFilters;

  auto& map = m_document.map();
  const auto& editorContext = map.editorContext();

  if (
    inputState.mouseButtonsPressed(MouseButtons::Middle)
    && inputState.checkModifierKeys(
      ModifierKeyPressed::Yes, ModifierKeyPressed::No, ModifierKeyPressed::DontCare)
    && editorContext.canChangeSelection())
  {
    return std::make_unique<MarqueeSelectionDragTracker>(
      *this,
      map,
      inputState.camera(),
      m_marqueeMode ? *m_marqueeMode : MarqueeSelectionMode::AllObjects,
      inputState);
  }

  if (!handleClick(inputState, editorContext) || !isMultiClick(inputState))
  {
    return nullptr;
  }

  if (isFaceClick(inputState))
  {
    const auto hit = firstHit(inputState, type(mdl::BrushNode::BrushHitType));
    if (const auto faceHandle = mdl::hitToFaceHandle(hit))
    {
      const auto* brushNode = faceHandle->node();
      const auto& face = faceHandle->face();
      if (editorContext.selectable(*brushNode, face))
      {
        map.startTransaction(
          "Drag Select Brush Faces", mdl::TransactionScope::LongRunning);
        if (map.selection().hasAny() && !map.selection().hasBrushFaces())
        {
          deselectAll(map);
        }
        if (!face.selected())
        {
          selectBrushFaces(map, {*faceHandle});
        }

        return std::make_unique<PaintSelectionDragTracker>(map);
      }
    }
  }
  else
  {
    const auto hit =
      firstHit(inputState, type(mdl::nodeHitType()) && isNodeSelectable(editorContext));
    if (!hit.isMatch())
    {
      return nullptr;
    }

    auto* node = findOutermostClosedGroupOrNode(mdl::hitToNode(hit));
    if (editorContext.selectable(*node))
    {
      map.startTransaction("Drag Select Objects", mdl::TransactionScope::LongRunning);
      if (map.selection().hasAny() && !map.selection().hasNodes())
      {
        deselectAll(map);
      }
      if (!node->selected())
      {
        selectNodes(map, {node});
      }

      return std::make_unique<PaintSelectionDragTracker>(map);
    }
  }

  return nullptr;
}

void SelectionTool::setRenderOptions(
  const InputState& inputState, render::RenderContext& renderContext) const
{
  using namespace mdl::HitFilters;

  if (const auto hit = firstHit(inputState, type(mdl::nodeHitType())); hit.isMatch())
  {
    auto* node = findOutermostClosedGroupOrNode(mdl::hitToNode(hit));
    if (node->selected())
    {
      renderContext.setShowSelectionGuide();
    }
  }
}

bool SelectionTool::cancel()
{
  // closing the current group is handled in MapViewBase
  return false;
}

} // namespace tb::ui
