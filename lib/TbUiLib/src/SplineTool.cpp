/*
 Copyright (C) 2026 Kristian Duske

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

#include "ui/SplineTool.h"

#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "prefs/Preferences.h"
#include "gl/Camera.h"
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapSidecar.h"
#include "mdl/Map_Nodes.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/SplineBrushes.h"
#include "mdl/SplineEntities.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"
#include "render/RenderService.h"
#include "ui/MapDocument.h"

#include "kd/overload.h"
#include "kd/ranges/to.h"
#include "kd/result.h"
#include "kd/set_temp.h"
#include "kd/string_utils.h"

#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <ranges>
#include <unordered_map>

namespace tb::ui
{

namespace
{

/** The brushes an entity node holds, copied out of it. */
std::vector<mdl::Brush> collectBrushes(const mdl::EntityNode& entityNode)
{
  auto brushes = std::vector<mdl::Brush>{};
  for (const auto* child : entityNode.children())
  {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(child))
    {
      brushes.push_back(brushNode->brush());
    }
  }
  return brushes;
}

/** Fresh nodes holding copies of the brushes an entity node holds. */
std::vector<mdl::Node*> collectBrushNodes(const mdl::EntityNode& entityNode)
{
  auto nodes = std::vector<mdl::Node*>{};
  for (auto& brush : collectBrushes(entityNode))
  {
    nodes.push_back(new mdl::BrushNode{std::move(brush)});
  }
  return nodes;
}

/** Spline point handles are drawn and picked larger than regular point handles. */
constexpr auto SplinePointHandleScale = 5.0;
} // namespace

const mdl::HitType::Type SplineTool::PointHitType = mdl::HitType::freeType();

SplineTool::SplineTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

SplineTool::~SplineTool() = default;

const mdl::Grid& SplineTool::grid() const
{
  return m_document.map().grid();
}

void SplineTool::pick(
  const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult)
{
  // The hit target identifies the spline (by its entity node, null for a new,
  // uncommitted spline), the point index and the picked handle part.
  using Target = std::tuple<mdl::EntityNode*, size_t, SplineHandlePart>;

  const auto handleRadius =
    SplinePointHandleScale * double(pref(Preferences::HandleRadius));

  for (size_t i = 0; i < m_points.size(); ++i)
  {
    if (
      const auto distance =
        camera.pickPointHandle(pickRay, m_points[i].position, handleRadius))
    {
      const auto hitPoint = vm::point_at_distance(pickRay, *distance);
      pickResult.addHit(mdl::Hit{
        PointHitType,
        *distance,
        hitPoint,
        Target{m_splineNode, i, SplineHandlePart::Point}});
    }
  }

  if (m_tangentEditMode)
  {
    for (size_t i = 0; i < m_points.size(); ++i)
    {
      if (m_points[i].autoTangent)
      {
        continue;
      }
      const auto& point = m_points[i];
      const auto handles = std::array<std::pair<SplineHandlePart, vm::vec3d>, 2>{
        std::pair{
          SplineHandlePart::TangentIn,
          point.position + mdl::tangentInOffset(m_points, i, m_closed)},
        std::pair{
          SplineHandlePart::TangentOut,
          point.position + mdl::tangentOutOffset(m_points, i, m_closed)},
      };
      for (const auto& [part, position] : handles)
      {
        if (const auto distance = camera.pickPointHandle(pickRay, position, handleRadius))
        {
          const auto hitPoint = vm::point_at_distance(pickRay, *distance);
          pickResult.addHit(
            mdl::Hit{PointHitType, *distance, hitPoint, Target{m_splineNode, i, part}});
        }
      }
    }
  }

  for (const auto& [entityNode, data] : m_otherSplines)
  {
    for (size_t i = 0; i < data.points.size(); ++i)
    {
      if (
        const auto distance =
          camera.pickPointHandle(pickRay, data.points[i].position, handleRadius))
      {
        const auto hitPoint = vm::point_at_distance(pickRay, *distance);
        pickResult.addHit(mdl::Hit{
          PointHitType,
          *distance,
          hitPoint,
          Target{entityNode, i, SplineHandlePart::Point}});
      }
    }
  }
}

void SplineTool::render(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const mdl::PickResult& pickResult)
{
  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setPointHandleScale(float(SplinePointHandleScale));
  renderService.setShowOccludedObjects();

  // Draw all other splines so they can be picked up for editing.
  for (const auto& [entityNode, data] : m_otherSplines)
  {
    if (data.points.size() > 1)
    {
      const auto samples = mdl::sampleSpline(data.points, data.subdivisions, data.closed);
      const auto vertices =
        samples
        | std::views::transform([](const auto& point) { return vm::vec3f{point}; })
        | kdl::ranges::to<std::vector>();

      renderService.setForegroundColor(pref(Preferences::SplineLineColor));
      renderService.setLineWidth(1.0f);
      renderService.renderLineStrip(vertices);
    }

    renderService.setForegroundColor(pref(Preferences::OccludedHandleColor));
    for (const auto& point : data.points)
    {
      renderService.renderHandle(vm::vec3f{point.position});
    }
  }

  if (m_points.empty())
  {
    renderHighlight(renderService, pickResult);
    return;
  }

  if (m_points.size() > 1)
  {
    const auto samples = mdl::sampleSpline(m_points, m_subdivisions, m_closed);
    const auto vertices =
      samples | std::views::transform([](const auto& point) { return vm::vec3f{point}; })
      | kdl::ranges::to<std::vector>();

    renderService.setForegroundColor(pref(Preferences::SplineLineColor));
    renderService.setLineWidth(2.0f);
    renderService.renderLineStrip(vertices);
  }

  // Visualize each point's sweep frame with a reference arrow along its up
  // direction; locked points (frame anchors) are drawn in a different color. The
  // selected point additionally shows its right direction.
  if (m_points.size() > 1)
  {
    const auto frames = mdl::computeNodeFrames(m_points, m_closed);
    const auto axisLength = 24.0;

    renderService.setLineWidth(2.0f);
    for (size_t i = 0; i < frames.size() && i < m_points.size(); ++i)
    {
      const auto& frame = frames[i];
      renderService.setForegroundColor(
        m_points[i].locks != mdl::SplineLock::None
          ? pref(Preferences::SelectedHandleColor)
          : pref(Preferences::ZAxisColor));
      renderService.renderLine(
        vm::vec3f{frame.position}, vm::vec3f{frame.position + frame.up * axisLength});

      if (m_selectedIndex == i)
      {
        renderService.setForegroundColor(pref(Preferences::YAxisColor));
        renderService.renderLine(
          vm::vec3f{frame.position},
          vm::vec3f{frame.position + frame.right * axisLength});
      }
    }
  }

  for (size_t i = 0; i < m_points.size(); ++i)
  {
    const auto& point = m_points[i];

    renderService.setForegroundColor(
      m_selectedIndex == i ? pref(Preferences::SelectedHandleColor)
                           : pref(Preferences::HandleColor));
    renderService.renderHandle(vm::vec3f{point.position});
  }

  // In tangent edit mode, show the tangent handles of every point with manual
  // tangents, connected to their points; the selected point's handles are drawn in
  // the full tangent color, all others darker.
  if (m_tangentEditMode)
  {
    renderService.setLineWidth(1.0f);
    for (size_t i = 0; i < m_points.size(); ++i)
    {
      if (m_points[i].autoTangent)
      {
        continue;
      }
      const auto& point = m_points[i];
      const auto inPosition =
        point.position + mdl::tangentInOffset(m_points, i, m_closed);
      const auto outPosition =
        point.position + mdl::tangentOutOffset(m_points, i, m_closed);

      renderService.setForegroundColor(
        m_selectedIndex == i ? pref(Preferences::SplineTangentHandleColor)
                             : pref(Preferences::SplineInactiveTangentHandleColor));
      renderService.renderLine(vm::vec3f{point.position}, vm::vec3f{inPosition});
      renderService.renderLine(vm::vec3f{point.position}, vm::vec3f{outPosition});
      renderService.renderHandle(vm::vec3f{inPosition});
      renderService.renderHandle(vm::vec3f{outPosition});
    }
  }

  renderHighlight(renderService, pickResult);

  if (m_selectedIndex && *m_selectedIndex < m_points.size())
  {
    const auto& point = m_points[*m_selectedIndex];
    renderService.setForegroundColor(pref(Preferences::SelectedHandleColor));
    renderService.renderHandleHighlight(vm::vec3f{point.position});
    renderService.setBackgroundColor(pref(Preferences::InfoOverlayBackgroundColor));
    renderService.renderString(
      fmt::format(
        "Point {} | Roll {:g} | Scale {:g}{}",
        *m_selectedIndex,
        point.roll,
        point.scale,
        point.locks != mdl::SplineLock::None ? " | Locked" : ""),
      vm::vec3f{point.position});
  }
}

void SplineTool::renderHighlight(
  render::RenderService& renderService, const mdl::PickResult& pickResult) const
{
  // Highlight the hovered point; while add point mode is active, clicks add points
  // instead of selecting, so no highlight is shown.
  if (m_addPointMode)
  {
    return;
  }

  using namespace mdl::HitFilters;
  const auto& hit = pickResult.first(type(PointHitType));
  if (hit.isMatch())
  {
    const auto [entityNode, index, part] =
      hit.target<std::tuple<mdl::EntityNode*, size_t, SplineHandlePart>>();
    const auto* points = &m_points;
    if (entityNode != m_splineNode)
    {
      for (const auto& [otherNode, otherData] : m_otherSplines)
      {
        if (otherNode == entityNode)
        {
          points = &otherData.points;
          break;
        }
      }
    }
    if (index < points->size())
    {
      auto position = (*points)[index].position;
      if (part == SplineHandlePart::TangentIn)
      {
        position = position + mdl::tangentInOffset(*points, index, m_closed);
      }
      else if (part == SplineHandlePart::TangentOut)
      {
        position = position + mdl::tangentOutOffset(*points, index, m_closed);
      }
      renderService.setForegroundColor(pref(Preferences::SelectedHandleColor));
      renderService.renderHandleHighlight(vm::vec3f{position});
    }
  }
}

void SplineTool::renderFeedback(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const vm::vec3d& point) const
{
  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setPointHandleScale(float(SplinePointHandleScale));
  renderService.setShowOccludedObjects();
  renderService.setForegroundColor(pref(Preferences::HandleColor));
  renderService.renderHandle(vm::vec3f{point});

  if (!m_points.empty())
  {
    renderService.setForegroundColor(pref(Preferences::SplineLineColor));
    renderService.renderLine(
      vm::vec3f{m_points[addPointAnchorIndex()].position}, vm::vec3f{point});
  }
}

bool SplineTool::addPointMode() const
{
  return m_addPointMode;
}

void SplineTool::setAddPointMode(const bool addPointMode)
{
  if (addPointMode != m_addPointMode)
  {
    m_addPointMode = addPointMode;
    refreshViews();
    splineDidChangeNotifier();
  }
}

bool SplineTool::hasPoints() const
{
  return !m_points.empty();
}

const std::vector<mdl::SplinePoint>& SplineTool::points() const
{
  return m_points;
}

std::optional<vm::vec3d> SplineTool::lastPointPosition() const
{
  return !m_points.empty() ? std::optional{m_points[addPointAnchorIndex()].position}
                           : std::nullopt;
}

size_t SplineTool::addPointAnchorIndex() const
{
  // If a point between two other points is selected, new points are inserted after
  // it; otherwise they are appended after the last point.
  return m_selectedIndex && *m_selectedIndex + 1 < m_points.size() ? *m_selectedIndex
                                                                   : m_points.size() - 1;
}

void SplineTool::addPoint(const vm::vec3d& point)
{
  const auto index = m_points.empty() ? 0 : addPointAnchorIndex() + 1;
  m_points.insert(m_points.begin() + std::ptrdiff_t(index), mdl::SplinePoint{point});
  m_selectedIndex = index;
  m_selectedPart = SplineHandlePart::Point;
  commitSpline("Add Spline Point");
}

bool SplineTool::canRemovePoint() const
{
  return !m_points.empty();
}

void SplineTool::removePoint()
{
  if (m_points.empty())
  {
    return;
  }

  const auto index = m_selectedIndex ? *m_selectedIndex : m_points.size() - 1;
  m_points.erase(std::next(m_points.begin(), std::ptrdiff_t(index)));
  m_selectedIndex = std::nullopt;
  commitSpline("Remove Spline Point");
}

bool SplineTool::selectPoint(const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  const auto& hit = pickResult.first(type(PointHitType));
  if (!hit.isMatch())
  {
    return false;
  }

  // Hitting another spline's point picks up that spline for editing first.
  const auto [entityNode, index, part] =
    hit.target<std::tuple<mdl::EntityNode*, size_t, SplineHandlePart>>();
  if (entityNode != m_splineNode)
  {
    if (!entityNode)
    {
      return false;
    }
    loadSplineNode(entityNode);
  }

  if (index >= m_points.size())
  {
    return false;
  }

  m_selectedIndex = index;
  m_selectedPart = part;
  refreshViews();
  splineDidChangeNotifier();
  return true;
}

bool SplineTool::selectSpline(const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  const auto& hit = pickResult.first(type(mdl::BrushNode::BrushHitType));
  if (const auto faceHandle = mdl::hitToFaceHandle(hit))
  {
    for (auto* candidate = static_cast<mdl::Node*>(faceHandle->node());
         candidate != nullptr;
         candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && entityNode != m_splineNode
          && mdl::isSplineEntity(entityNode->entity()))
      {
        loadSplineNode(entityNode);
        return true;
      }
    }
  }
  return false;
}

void SplineTool::deselectPoint()
{
  m_selectedIndex = std::nullopt;
  m_selectedPart = SplineHandlePart::Point;
  refreshViews();
  splineDidChangeNotifier();
}

std::optional<size_t> SplineTool::selectedPointIndex() const
{
  return m_selectedIndex;
}

std::optional<std::tuple<vm::vec3d, vm::vec3d>> SplineTool::beginDragPoint(
  const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  const auto& hit = pickResult.first(type(PointHitType));
  if (!hit.isMatch())
  {
    return std::nullopt;
  }

  // Hitting another spline's point picks up that spline for editing first.
  const auto [entityNode, index, part] =
    hit.target<std::tuple<mdl::EntityNode*, size_t, SplineHandlePart>>();
  if (entityNode != m_splineNode)
  {
    if (!entityNode)
    {
      return std::nullopt;
    }
    loadSplineNode(entityNode);
  }

  if (index >= m_points.size())
  {
    return std::nullopt;
  }

  m_selectedIndex = index;
  m_selectedPart = part;
  m_dragState = DragState{index, part, m_points[index]};
  splineDidChangeNotifier();

  auto initialPosition = m_points[index].position;
  if (part == SplineHandlePart::TangentIn)
  {
    initialPosition = initialPosition + mdl::tangentInOffset(m_points, index, m_closed);
  }
  else if (part == SplineHandlePart::TangentOut)
  {
    initialPosition = initialPosition + mdl::tangentOutOffset(m_points, index, m_closed);
  }
  return {{initialPosition, hit.hitPoint()}};
}

bool SplineTool::draggingTangentHandle() const
{
  return m_dragState && m_dragState->part != SplineHandlePart::Point;
}

bool SplineTool::dragPoint(const vm::vec3d& newPosition)
{
  if (!m_dragState)
  {
    return false;
  }

  auto& point = m_points[m_dragState->index];
  switch (m_dragState->part)
  {
  case SplineHandlePart::Point:
    point.position = newPosition;
    break;
  case SplineHandlePart::TangentIn:
    point.tangentIn = newPosition - point.position;
    break;
  case SplineHandlePart::TangentOut:
    point.tangentOut = newPosition - point.position;
    break;
  }
  refreshViews();
  return true;
}

void SplineTool::endDragPoint()
{
  const auto movedTangent = m_dragState && m_dragState->part != SplineHandlePart::Point;
  m_dragState = std::nullopt;
  commitSpline(movedTangent ? "Move Spline Tangent" : "Move Spline Point");
}

void SplineTool::cancelDragPoint()
{
  if (m_dragState)
  {
    m_points[m_dragState->index] = m_dragState->originalPoint;
    m_dragState = std::nullopt;
    refreshViews();
  }
}

double SplineTool::selectedPointRoll() const
{
  return m_selectedIndex && *m_selectedIndex < m_points.size()
           ? m_points[*m_selectedIndex].roll
           : 0.0;
}

void SplineTool::setSelectedPointRoll(const double roll)
{
  if (
    m_selectedIndex && *m_selectedIndex < m_points.size()
    && m_points[*m_selectedIndex].roll != roll)
  {
    m_points[*m_selectedIndex].roll = roll;
    commitSpline("Rotate Spline Point");
  }
}

double SplineTool::selectedPointScale() const
{
  return m_selectedIndex && *m_selectedIndex < m_points.size()
           ? m_points[*m_selectedIndex].scale
           : 1.0;
}

void SplineTool::setSelectedPointScale(const double scale)
{
  if (
    m_selectedIndex && *m_selectedIndex < m_points.size() && scale > 0.0
    && m_points[*m_selectedIndex].scale != scale)
  {
    m_points[*m_selectedIndex].scale = scale;
    commitSpline("Scale Spline Point");
  }
}

bool SplineTool::selectedPointLock(const mdl::SplineLock::Type lock) const
{
  return m_selectedIndex && *m_selectedIndex < m_points.size()
         && (m_points[*m_selectedIndex].locks & lock) != 0u;
}

void SplineTool::setSelectedPointLock(const mdl::SplineLock::Type lock, const bool set)
{
  if (
    m_selectedIndex && *m_selectedIndex < m_points.size()
    && selectedPointLock(lock) != set)
  {
    auto& locks = m_points[*m_selectedIndex].locks;
    locks = set ? (locks | lock) : (locks & ~lock);
    commitSpline(set ? "Lock Spline Point" : "Unlock Spline Point");
  }
}

void SplineTool::toggleSelectedPointLock(const mdl::SplineLock::Type lock)
{
  setSelectedPointLock(lock, !selectedPointLock(lock));
}

bool SplineTool::selectedPointAutoTangent() const
{
  return !m_selectedIndex || *m_selectedIndex >= m_points.size()
         || m_points[*m_selectedIndex].autoTangent;
}

void SplineTool::setSelectedPointAutoTangent(const bool autoTangent)
{
  if (
    !m_selectedIndex || *m_selectedIndex >= m_points.size()
    || m_points[*m_selectedIndex].autoTangent == autoTangent)
  {
    return;
  }

  auto& point = m_points[*m_selectedIndex];
  if (!autoTangent)
  {
    // Capture the current automatic tangents so the curve stays put until a handle
    // is moved.
    point.tangentIn = mdl::tangentInOffset(m_points, *m_selectedIndex, m_closed);
    point.tangentOut = mdl::tangentOutOffset(m_points, *m_selectedIndex, m_closed);
  }
  point.autoTangent = autoTangent;

  if (autoTangent)
  {
    m_tangentEditMode = false;
    m_selectedPart = SplineHandlePart::Point;
  }
  commitSpline(
    autoTangent ? "Use Automatic Spline Tangents" : "Use Manual Spline Tangents");
}

bool SplineTool::tangentEditMode() const
{
  return m_tangentEditMode;
}

void SplineTool::setTangentEditMode(const bool tangentEditMode)
{
  if (tangentEditMode != m_tangentEditMode)
  {
    m_tangentEditMode = tangentEditMode;
    if (!m_tangentEditMode)
    {
      m_selectedPart = SplineHandlePart::Point;
    }
    refreshViews();
    splineDidChangeNotifier();
  }
}

bool SplineTool::tangentHandlesVisible() const
{
  return m_tangentEditMode && m_selectedIndex && *m_selectedIndex < m_points.size()
         && !m_points[*m_selectedIndex].autoTangent;
}

void SplineTool::moveSelectedPoint(const vm::vec3d& delta)
{
  if (!m_selectedIndex || *m_selectedIndex >= m_points.size())
  {
    return;
  }

  auto& point = m_points[*m_selectedIndex];
  if (m_selectedPart != SplineHandlePart::Point && tangentHandlesVisible())
  {
    auto& tangent =
      m_selectedPart == SplineHandlePart::TangentIn ? point.tangentIn : point.tangentOut;
    tangent = tangent + delta;
    commitSpline("Move Spline Tangent");
  }
  else
  {
    point.position = point.position + delta;
    commitSpline("Move Spline Point");
  }
}

bool SplineTool::closed() const
{
  return m_closed;
}

void SplineTool::setClosed(const bool closed)
{
  if (closed != m_closed)
  {
    m_closed = closed;
    commitSpline(closed ? "Close Spline" : "Open Spline");
  }
}

bool SplineTool::lockUVs() const
{
  return m_lockUVs;
}

void SplineTool::setLockUVs(const bool lockUVs)
{
  if (m_lockUVs != lockUVs)
  {
    m_lockUVs = lockUVs;
    commitSpline("Lock Spline UVs");
  }
}

bool SplineTool::keepSize() const
{
  return m_keepSize;
}

void SplineTool::setKeepSize(const bool keepSize)
{
  if (m_keepSize != keepSize)
  {
    m_keepSize = keepSize;
    commitSpline("Keep Spline Template Size");
  }
}

size_t SplineTool::subdivisions() const
{
  return m_subdivisions;
}

void SplineTool::setSubdivisions(const size_t subdivisions)
{
  if (subdivisions > 0 && subdivisions != m_subdivisions)
  {
    m_subdivisions = subdivisions;
    commitSpline("Change Spline Subdivisions");
  }
}

bool SplineTool::canLinkTemplate() const
{
  const auto& selection = m_document.map().selection();
  return selectedGroup() != nullptr || !selection.brushes.empty()
         || !selection.entities.empty();
}

void SplineTool::linkTemplate()
{
  if (const auto* groupNode = selectedGroup())
  {
    if (groupNode->persistentId() && groupNode->persistentId() != m_templateGroupId)
    {
      m_templateGroupId = groupNode->persistentId();
      m_templateBrushes.clear();
      m_templateEntities.clear();
      m_templateBrushEntities.clear();
      commitSpline("Link Spline Template");
    }
    return;
  }

  const auto& selection = m_document.map().selection();
  if (selection.brushes.empty() && selection.entities.empty())
  {
    return;
  }

  // Individually selected nodes cannot be referenced persistently, so take a snapshot
  // of their current state instead.
  m_templateBrushes.clear();
  m_templateBrushEntities.clear();

  // A selected brush keeps the company it was in: one belonging to worldspawn is swept
  // into the spline's own entity, and one belonging to some other class is swept into a
  // copy of that class, so a linked func_detail stays detail.
  auto brushEntityIndices = std::unordered_map<const mdl::EntityNodeBase*, size_t>{};
  for (const auto* brushNode : selection.brushes)
  {
    const auto* entityNode = brushNode->entity();
    if (!entityNode || mdl::isWorldspawn(entityNode->entity().classname()))
    {
      m_templateBrushes.push_back(brushNode->brush());
      continue;
    }

    const auto [it, inserted] =
      brushEntityIndices.try_emplace(entityNode, m_templateBrushEntities.size());
    if (inserted)
    {
      m_templateBrushEntities.push_back(
        mdl::SplineTemplateBrushEntity{entityNode->entity(), {}});
    }
    m_templateBrushEntities[it->second].brushes.push_back(brushNode->brush());
  }

  m_templateEntities.clear();
  for (const auto* entityNode : selection.entities)
  {
    // An entity with brushes of its own is covered by those brushes above; only a point
    // entity is replicated whole.
    if (!entityNode->hasChildren())
    {
      m_templateEntities.push_back(
        mdl::SplineTemplateEntity{entityNode->entity(), entityNode->logicalBounds()});
    }
  }

  m_templateGroupId = std::nullopt;
  commitSpline("Link Spline Template");
}

bool SplineTool::hasTemplate() const
{
  return m_templateGroupId.has_value() || !m_templateBrushes.empty()
         || !m_templateEntities.empty() || !m_templateBrushEntities.empty();
}

void SplineTool::unlinkTemplate()
{
  if (hasTemplate())
  {
    m_templateGroupId = std::nullopt;
    m_templateBrushes.clear();
    m_templateEntities.clear();
    m_templateBrushEntities.clear();
    commitSpline("Unlink Spline Template");
  }
}

bool SplineTool::canBreakSpline() const
{
  if (!m_splineNode)
  {
    return false;
  }
  if (!m_splineNode->children().empty())
  {
    return true;
  }

  // Asking the template whether it makes any entities is the same answer as looking
  // for the generated ones, without walking the whole map to find them.
  const auto contents = collectTemplate();
  return contents && !contents->entities.empty();
}

void SplineTool::breakSpline()
{
  if (!canBreakSpline())
  {
    return;
  }

  auto& map = m_document.map();

  // Duplicate the generated brushes as standard brushes outside the spline entity, so
  // they stay behind as editable geometry when the spline lets go of them.
  auto duplicates = std::vector<mdl::Node*>{};
  for (auto* child : m_splineNode->children())
  {
    if (const auto* brushNode = dynamic_cast<mdl::BrushNode*>(child))
    {
      duplicates.push_back(new mdl::BrushNode{brushNode->brush()});
    }
  }

  // The generated entities stay behind the same way. Dropping the marker is what
  // detaches them: without it the spline no longer owns them, so unlinking the
  // template below takes away the originals and leaves these as ordinary entities.
  for (auto* node : findGeneratedEntityNodes())
  {
    if (const auto* entityNode = dynamic_cast<mdl::EntityNode*>(node))
    {
      auto entity = entityNode->entity();
      entity.removeProperty(mdl::SplinePropertyKeys::GeneratedBy);

      auto* duplicate = new mdl::EntityNode{std::move(entity)};
      // A generated brush entity has the geometry of its copy in it, which is the whole
      // point of it: it has to come along or the copy is lost.
      duplicate->addChildren(collectBrushNodes(*entityNode));
      duplicates.push_back(duplicate);
    }
  }

  if (duplicates.empty())
  {
    return;
  }

  auto* parent = m_splineNode->parent();

  auto transaction = mdl::Transaction{map, "Break Spline"};
  if (addNodes(map, {{parent, duplicates}}).empty())
  {
    transaction.cancel();
    return;
  }

  // Unlink the template so the spline stops generating anything.
  m_templateGroupId = std::nullopt;
  m_templateBrushes.clear();
  m_templateEntities.clear();
  m_templateBrushEntities.clear();
  commitSpline("Break Spline");
  transaction.commit();
}

std::string SplineTool::templateName() const
{
  if (m_templateGroupId)
  {
    const auto* groupNode = findTemplateGroup();
    return groupNode ? groupNode->group().name() : "";
  }
  auto brushCount = m_templateBrushes.size();
  for (const auto& brushEntity : m_templateBrushEntities)
  {
    brushCount += brushEntity.brushes.size();
  }

  auto parts = std::vector<std::string>{};
  if (brushCount > 0)
  {
    parts.push_back(
      brushCount == 1 ? std::string{"1 brush"} : fmt::format("{} brushes", brushCount));
  }
  if (!m_templateEntities.empty())
  {
    parts.push_back(
      m_templateEntities.size() == 1
        ? std::string{"1 entity"}
        : fmt::format("{} entities", m_templateEntities.size()));
  }
  return kdl::str_join(parts, ", ");
}

mdl::GroupNode* SplineTool::findTemplateGroup() const
{
  if (!m_templateGroupId)
  {
    return nullptr;
  }

  mdl::GroupNode* result = nullptr;
  m_document.map().worldNode().accept(kdl::overload(
    [](auto&& thisLambda, mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, mdl::LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, mdl::GroupNode& groupNode) {
      if (groupNode.persistentId() == m_templateGroupId)
      {
        result = &groupNode;
      }
      else
      {
        groupNode.visitChildren(thisLambda);
      }
    },
    [](mdl::EntityNode&) {},
    [](mdl::BrushNode&) {},
    [](mdl::PatchNode&) {}));

  return result;
}

mdl::GroupNode* SplineTool::selectedGroup() const
{
  const auto& groups = m_document.map().selection().groups;
  return groups.size() == 1 ? groups.front() : nullptr;
}

void SplineTool::refreshOtherSplines()
{
  m_otherSplines.clear();

  m_document.map().worldNode().accept(kdl::overload(
    [](auto&& thisLambda, mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, mdl::LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, mdl::GroupNode& groupNode) {
      groupNode.visitChildren(thisLambda);
    },
    [&](mdl::EntityNode& entityNode) {
      if (&entityNode != m_splineNode && mdl::isSplineEntity(entityNode.entity()))
      {
        if (const auto data = mdl::parseSplineEntity(entityNode.entity()))
        {
          m_otherSplines.emplace_back(&entityNode, *data);
        }
      }
    },
    [](mdl::BrushNode&) {},
    [](mdl::PatchNode&) {}));
}

void SplineTool::loadFromSelection()
{
  for (auto* node : m_document.map().selection().nodes)
  {
    // Look for a selected spline entity, or a selected brush belonging to one.
    for (auto* candidate = node; candidate != nullptr; candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && mdl::isSplineEntity(entityNode->entity()))
      {
        loadSplineNode(entityNode);
        return;
      }
    }
  }
}

void SplineTool::loadSplineNode(mdl::EntityNode* splineNode)
{
  if (const auto data = mdl::parseSplineEntity(splineNode->entity()))
  {
    auto& map = m_document.map();

    m_splineNode = splineNode;
    m_points = data->points;
    m_subdivisions = data->subdivisions;
    m_templateGroupId = data->templateGroupId;
    m_closed = data->closed;
    m_lockUVs = data->lockUVs;
    m_keepSize = data->keepSize;
    m_templateBrushes = mdl::parseSplineTemplateBrushes(
      splineNode->entity(), map.worldNode().mapFormat(), map.worldBounds());
    m_templateEntities = mdl::parseSplineTemplateEntities(splineNode->entity());
    m_templateBrushEntities = mdl::parseSplineTemplateBrushEntities(
      splineNode->entity(), map.worldNode().mapFormat(), map.worldBounds());
    m_selectedIndex = std::nullopt;
    m_dragState = std::nullopt;

    // Picking up an existing spline is usually done to edit its points, so disable
    // add point mode to prevent accidentally appending new points.
    m_addPointMode = false;

    refreshOtherSplines();
    refreshViews();
    splineDidChangeNotifier();
  }
}

void SplineTool::clearSpline()
{
  m_splineNode = nullptr;
  m_points.clear();
  m_subdivisions = mdl::SplineDefaultSubdivisions;
  m_closed = false;
  m_lockUVs = false;
  m_templateGroupId = std::nullopt;
  m_templateBrushes.clear();
  m_templateEntities.clear();
  m_templateBrushEntities.clear();
  m_selectedIndex = std::nullopt;
  m_dragState = std::nullopt;
  refreshOtherSplines();
  refreshViews();
  splineDidChangeNotifier();
}

void SplineTool::commitSpline(const std::string& commandName)
{
  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  auto& map = m_document.map();

  // The point entities the spline generated cannot live inside it, so they are beside
  // it and have to be taken away by hand whenever it is rebuilt.
  const auto generatedEntityNodes = findGeneratedEntityNodes();

  if (m_points.empty())
  {
    if (m_splineNode)
    {
      auto transaction = mdl::Transaction{map, commandName};
      auto nodesToRemove = generatedEntityNodes;
      nodesToRemove.push_back(m_splineNode);
      removeNodes(map, nodesToRemove);
      m_splineNode = nullptr;
      transaction.commit();
    }
    refreshViews();
    splineDidChangeNotifier();
    return;
  }

  const auto data = mdl::SplineEntityData{
    m_points, m_subdivisions, m_templateGroupId, m_closed, m_lockUVs, m_keepSize};
  auto entity = mdl::writeSplineTemplateBrushEntities(
    mdl::writeSplineTemplateEntities(
      mdl::writeSplineTemplateBrushes(
        mdl::writeSplineEntity(
          m_splineNode ? m_splineNode->entity() : mdl::Entity{}, data),
        m_templateBrushes),
      m_templateEntities),
    m_templateBrushEntities);

  // Give the entity an origin so that it has a sensible position while it has no
  // brushes yet.
  entity.addOrUpdateProperty(
    "origin",
    fmt::format(
      "{:g} {:g} {:g}",
      m_points.front().position.x(),
      m_points.front().position.y(),
      m_points.front().position.z()));

  // writeSplineEntity gives every spline a tool data id, which is what the entities it
  // generates carry to say who they belong to.
  const auto splineId = mdl::splineEntityId(entity);
  const auto contents = collectTemplate();

  auto* newNode = new mdl::EntityNode{std::move(entity)};
  if (contents)
  {
    newNode->addChildren(createBrushNodes(*contents));
  }

  auto newNodes = std::vector<mdl::Node*>{newNode};
  if (contents)
  {
    for (auto* node : createEntityNodes(*contents, splineId))
    {
      newNodes.push_back(node);
    }
  }

  auto* parent = m_splineNode ? m_splineNode->parent() : &parentForNodes(map, {});

  auto transaction = mdl::Transaction{map, commandName};
  auto nodesToRemove = generatedEntityNodes;
  if (m_splineNode)
  {
    nodesToRemove.push_back(m_splineNode);
  }
  if (!nodesToRemove.empty())
  {
    removeNodes(map, nodesToRemove);
  }
  const auto addedNodes = addNodes(map, {{parent, newNodes}});
  if (addedNodes.empty())
  {
    transaction.cancel();
    m_splineNode = nullptr;
  }
  else
  {
    m_splineNode = newNode;
    transaction.commit();
  }

  refreshOtherSplines();
  refreshViews();
  splineDidChangeNotifier();
}

std::optional<SplineTool::TemplateContents> SplineTool::collectTemplate() const
{
  auto contents = TemplateContents{};

  if (const auto* groupNode = findTemplateGroup())
  {
    groupNode->visitChildren(kdl::overload(
      [](auto&& thisLambda, mdl::GroupNode& nestedGroup) {
        nestedGroup.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        // A point entity has nothing to sweep and is replicated whole. An entity with
        // brushes is swept, but its class comes along: its copies are entities of their
        // own rather than worldspawn, unless worldspawn is what it was to begin with.
        if (!entityNode.hasChildren())
        {
          contents.entities.push_back(
            mdl::SplineTemplateEntity{entityNode.entity(), entityNode.logicalBounds()});
        }
        else if (mdl::isWorldspawn(entityNode.entity().classname()))
        {
          entityNode.visitChildren(thisLambda);
        }
        else if (auto brushes = collectBrushes(entityNode); !brushes.empty())
        {
          contents.brushEntities.push_back(
            mdl::SplineTemplateBrushEntity{entityNode.entity(), std::move(brushes)});
        }
      },
      [&](mdl::BrushNode& brushNode) { contents.brushes.push_back(&brushNode.brush()); },
      [](mdl::WorldNode&) {},
      [](mdl::LayerNode&) {},
      [](mdl::PatchNode&) {}));
  }
  else
  {
    for (const auto& brush : m_templateBrushes)
    {
      contents.brushes.push_back(&brush);
    }
    contents.entities = m_templateEntities;
    contents.brushEntities = m_templateBrushEntities;
  }

  // Every brush sizes the lattice, whichever entity holds it, since all of them are
  // swept together and have to line up.
  auto brushBounds = std::optional<vm::bbox3d>{};
  const auto growBy = [&](const vm::bbox3d& bounds) {
    brushBounds = brushBounds ? vm::merge(*brushBounds, bounds) : bounds;
  };

  for (const auto* brush : contents.brushes)
  {
    growBy(brush->bounds());
  }
  for (const auto& brushEntity : contents.brushEntities)
  {
    for (const auto& brush : brushEntity.brushes)
    {
      growBy(brush.bounds());
    }
  }

  if (!brushBounds)
  {
    // With no brushes there is no swept solid to size the lattice from, so the point
    // entities size it themselves.
    brushBounds = mdl::splineTemplateEntityBounds(contents.entities);
    if (!brushBounds)
    {
      return std::nullopt;
    }
  }

  // The brushes alone size the lattice even when point entities sit outside it, so that
  // adding one to a template never changes the geometry it already sweeps.
  contents.bounds = *brushBounds;

  return contents;
}

std::vector<mdl::Node*> SplineTool::createBrushNodes(
  const TemplateContents& contents) const
{
  if (m_points.size() < 2 || contents.brushes.empty())
  {
    return {};
  }

  auto& map = m_document.map();
  return mdl::createSplineBrushes(
           map.worldNode().mapFormat(),
           map.worldBounds(),
           m_points,
           contents.brushes,
           contents.bounds,
           m_closed,
           m_lockUVs ? mdl::SplineUVMode::Lock : mdl::SplineUVMode::Follow,
           m_keepSize)
         | kdl::transform([](auto brushes) {
             return brushes | std::views::transform([](auto& brush) {
                      return static_cast<mdl::Node*>(
                        new mdl::BrushNode{std::move(brush)});
                    })
                    | kdl::ranges::to<std::vector>();
           })
         | kdl::transform_error([&](const auto& e) {
             map.logger().error() << "Could not create spline brushes: " << e.msg;
             return std::vector<mdl::Node*>{};
           })
         | kdl::value();
}

std::vector<mdl::Node*> SplineTool::createEntityNodes(
  const TemplateContents& contents, const std::string& splineId) const
{
  if (
    m_points.size() < 2 || splineId.empty()
    || (contents.entities.empty() && contents.brushEntities.empty()))
  {
    return {};
  }

  auto& map = m_document.map();
  auto nodes = std::vector<mdl::Node*>{};

  for (auto& entity :
       mdl::createSplineEntities(m_points, contents.entities, contents.bounds, m_closed))
  {
    entity.addOrUpdateProperty(mdl::SplinePropertyKeys::GeneratedBy, splineId);
    nodes.push_back(new mdl::EntityNode{std::move(entity)});
  }

  // A template brush entity is stamped along the curve the way a point entity is: one
  // entity per copy, carrying that copy's swept brushes. Keeping them apart is what
  // makes a template holding a door sweep into doors rather than one door the length of
  // the spline.
  for (const auto& brushEntity : contents.brushEntities)
  {
    auto brushPointers = std::vector<const mdl::Brush*>{};
    brushPointers.reserve(brushEntity.brushes.size());
    for (const auto& brush : brushEntity.brushes)
    {
      brushPointers.push_back(&brush);
    }

    mdl::createSplineBrushCopies(
      map.worldNode().mapFormat(),
      map.worldBounds(),
      m_points,
      brushPointers,
      contents.bounds,
      m_closed,
      m_lockUVs ? mdl::SplineUVMode::Lock : mdl::SplineUVMode::Follow,
      m_keepSize)
      | kdl::transform([&](auto copies) {
          for (auto& copy : copies)
          {
            if (copy.empty())
            {
              continue;
            }

            auto entity = brushEntity.entity;
            entity.addOrUpdateProperty(mdl::SplinePropertyKeys::GeneratedBy, splineId);

            auto* entityNode = new mdl::EntityNode{std::move(entity)};
            for (auto& brush : copy)
            {
              entityNode->addChild(new mdl::BrushNode{std::move(brush)});
            }
            nodes.push_back(entityNode);
          }
        })
      | kdl::transform_error([&](const auto& e) {
          map.logger().error() << "Could not create spline brushes for '"
                               << brushEntity.entity.classname() << "': " << e.msg;
        });
  }

  return nodes;
}

std::vector<mdl::Node*> SplineTool::findGeneratedEntityNodes() const
{
  const auto splineId =
    m_splineNode ? mdl::splineEntityId(m_splineNode->entity()) : std::string{};
  if (splineId.empty())
  {
    return {};
  }

  auto nodes = std::vector<mdl::Node*>{};
  m_document.map().worldNode().accept(kdl::overload(
    [](auto&& thisLambda, mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, mdl::LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [](auto&& thisLambda, mdl::GroupNode& groupNode) {
      groupNode.visitChildren(thisLambda);
    },
    [&](mdl::EntityNode& entityNode) {
      const auto* owner =
        entityNode.entity().property(mdl::SplinePropertyKeys::GeneratedBy);
      if (owner && *owner == splineId)
      {
        nodes.push_back(&entityNode);
      }
    },
    [](mdl::BrushNode&) {},
    [](mdl::PatchNode&) {}));

  return nodes;
}

bool SplineTool::doActivate()
{
  connectObservers();

  // Always start with add point mode disabled so that switching to the tool never
  // creates points accidentally; the user enables it explicitly on the tool page.
  m_addPointMode = false;
  loadFromSelection();
  refreshOtherSplines();

  splineDidChangeNotifier();
  return true;
}

bool SplineTool::doDeactivate()
{
  m_notifierConnection.disconnect();
  clearSpline();
  m_otherSplines.clear();
  return true;
}

void SplineTool::connectObservers()
{
  auto& map = m_document.map();
  m_notifierConnection += map.nodesWereAddedNotifier.connect(
    [this](const auto& nodes) { nodesWereAdded(nodes); });
  m_notifierConnection += map.nodesWereRemovedNotifier.connect(
    [this](const auto& nodes) { nodesWereRemoved(nodes); });
  m_notifierConnection += map.nodesDidChangeNotifier.connect(
    [this](const auto& nodes) { nodesDidChange(nodes); });
  m_notifierConnection +=
    map.selectionDidChangeNotifier.connect([this](const auto&) { selectionDidChange(); });
}

void SplineTool::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  refreshOtherSplines();

  if (m_splineNode != nullptr)
  {
    return;
  }

  // Adopt a spline entity that reappears, e.g. when a spline edit is undone.
  for (auto* node : nodes)
  {
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node);
        entityNode && mdl::isSplineEntity(entityNode->entity()))
    {
      loadSplineNode(entityNode);
      return;
    }
  }
}

void SplineTool::nodesWereRemoved(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  if (m_splineNode && std::ranges::find(nodes, m_splineNode) != nodes.end())
  {
    clearSpline();
  }
  else
  {
    refreshOtherSplines();
  }
}

void SplineTool::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  refreshOtherSplines();

  if (
    m_splineNode
    && std::ranges::find(nodes, static_cast<mdl::Node*>(m_splineNode)) != nodes.end())
  {
    loadSplineNode(m_splineNode);
  }
}

void SplineTool::selectionDidChange()
{
  if (m_ignoreNotifications)
  {
    return;
  }

  // Switch to a newly selected spline entity, but keep editing the current spline if
  // the selection does not contain one.
  for (auto* node : m_document.map().selection().nodes)
  {
    for (auto* candidate = node; candidate != nullptr; candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && entityNode != m_splineNode
          && mdl::isSplineEntity(entityNode->entity()))
      {
        loadSplineNode(entityNode);
        return;
      }
    }
  }

  splineDidChangeNotifier();
}

} // namespace tb::ui
