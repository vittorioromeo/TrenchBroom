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

#include "ui/TerrainTool.h"

#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "prefs/Preferences.h"
#include "fs/DiskIO.h"
#include "fs/File.h"
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
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/TerrainBrushes.h"
#include "mdl/TerrainHeightmap.h"
#include "mdl/Transaction.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "render/RenderService.h"
#include "ui/MapDocument.h"

#include "kd/overload.h"
#include "kd/ranges/to.h"
#include "kd/result.h"
#include "kd/set_temp.h"

#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <ranges>

namespace tb::ui
{

TerrainTool::TerrainTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

TerrainTool::~TerrainTool() = default;

const mdl::Grid& TerrainTool::grid() const
{
  return m_document.map().grid();
}

void TerrainTool::render(
  render::RenderContext& renderContext, render::RenderBatch& renderBatch) const
{
  auto renderService = render::RenderService{renderContext, renderBatch};

  // Outline every terrain in the map so they can be found and picked up.
  renderService.setForegroundColor(pref(Preferences::TerrainBoundsColor));
  renderService.setLineWidth(1.0f);
  for (const auto& [entityNode, terrain] : m_otherTerrains)
  {
    renderService.renderBounds(vm::bbox3f{mdl::terrainBounds(terrain)});
  }

  if (hasTerrain())
  {
    renderService.setLineWidth(2.0f);
    renderService.renderBounds(vm::bbox3f{mdl::terrainBounds(m_terrain)});
  }

  // The sculpting brush: a circle of the current radius lying on the terrain surface.
  // Without a sculpting mode the tool only selects terrains, so there is no brush.
  if (m_brushPosition && sculpting())
  {
    renderService.setShowOccludedObjects();
    renderService.setForegroundColor(pref(Preferences::TerrainBrushColor));
    renderService.setLineWidth(2.0f);
    renderService.renderCircle(
      vm::vec3f{*m_brushPosition}, vm::axis::z, 32, float(m_radius));
    renderService.setForegroundColor(pref(Preferences::TerrainBrushFillColor));
    renderService.renderFilledCircle(
      vm::vec3f{*m_brushPosition}, vm::axis::z, 32, float(m_radius));
  }

  renderStrokePreview(renderContext, renderBatch);
}

void TerrainTool::renderStrokePreview(
  render::RenderContext& renderContext, render::RenderBatch& renderBatch) const
{
  // A stroke only edits the height field, because rebuilding the brushes of every cell
  // under the sculpting brush costs far too much to do on every mouse movement. So the
  // new shape of the swept cells is drawn on top of the stale geometry until the stroke
  // ends and the brushes are built.
  if (!strokeActive() || m_strokeCells.empty() || !hasTerrain())
  {
    return;
  }

  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setShowOccludedObjects();
  renderService.setShowBackfaces();

  auto edges = std::vector<vm::vec3f>{};
  edges.reserve(m_strokeCells.size() * 8);

  renderService.setForegroundColor(pref(Preferences::TerrainPreviewFillColor));
  for (const auto cell : m_strokeCells)
  {
    const auto column = cell % m_terrain.columns;
    const auto row = cell / m_terrain.columns;

    const auto v00 = vm::vec3f{mdl::terrainVertexPosition(m_terrain, column, row)};
    const auto v10 = vm::vec3f{mdl::terrainVertexPosition(m_terrain, column + 1, row)};
    const auto v11 =
      vm::vec3f{mdl::terrainVertexPosition(m_terrain, column + 1, row + 1)};
    const auto v01 = vm::vec3f{mdl::terrainVertexPosition(m_terrain, column, row + 1)};

    // The two triangles match the prisms the cell is generated from, so the preview
    // shows exactly the surface the stroke will produce.
    renderService.renderFilledPolygon({v00, v10, v11});
    renderService.renderFilledPolygon({v00, v11, v01});

    edges.push_back(v00);
    edges.push_back(v10);
    edges.push_back(v10);
    edges.push_back(v11);
    edges.push_back(v11);
    edges.push_back(v01);
    edges.push_back(v01);
    edges.push_back(v00);
  }

  renderService.setForegroundColor(pref(Preferences::TerrainPreviewColor));
  renderService.setLineWidth(1.0f);
  renderService.renderLines(edges);
}

bool TerrainTool::addMode() const
{
  return m_addMode;
}

void TerrainTool::setAddMode(const bool addMode)
{
  if (addMode != m_addMode)
  {
    m_addMode = addMode;
    // Creating and sculpting are never active at once, so turning creation on leaves
    // whatever sculpting mode was selected.
    if (m_addMode)
    {
      m_mode = std::nullopt;
    }
    m_brushPosition = std::nullopt;
    refreshViews();
    terrainDidChangeNotifier();
  }
}

std::optional<TerrainToolMode> TerrainTool::mode() const
{
  return m_mode;
}

void TerrainTool::setMode(const std::optional<TerrainToolMode> mode)
{
  // Picking a sculpting mode leaves add mode, so the two are never active at once.
  const auto changed = mode != m_mode || (mode && m_addMode);
  if (mode)
  {
    m_addMode = false;
  }
  m_mode = mode;

  if (changed)
  {
    if (!sculpting())
    {
      // Without a sculpting mode there is no brush to draw.
      m_brushPosition = std::nullopt;
    }
    refreshViews();
    terrainDidChangeNotifier();
  }
}

bool TerrainTool::sculpting() const
{
  return !m_addMode && m_mode.has_value();
}

std::optional<TerrainToolMode> TerrainTool::effectiveMode(const bool invert) const
{
  if (!invert || !m_mode)
  {
    return m_mode;
  }

  switch (*m_mode)
  {
  case TerrainToolMode::Raise:
    return TerrainToolMode::Lower;
  case TerrainToolMode::Lower:
    return TerrainToolMode::Raise;
  case TerrainToolMode::Flatten:
    return TerrainToolMode::Smooth;
  case TerrainToolMode::Smooth:
    return TerrainToolMode::Flatten;
  case TerrainToolMode::Texture:
    return m_mode;
  }
  return m_mode;
}

double TerrainTool::radius() const
{
  return m_radius;
}

void TerrainTool::setRadius(const double radius)
{
  if (radius > 0.0 && radius != m_radius)
  {
    m_radius = radius;
    refreshViews();
    terrainDidChangeNotifier();
  }
}

double TerrainTool::strength() const
{
  return m_strength;
}

void TerrainTool::setStrength(const double strength)
{
  if (strength > 0.0 && strength != m_strength)
  {
    m_strength = strength;
    terrainDidChangeNotifier();
  }
}

double TerrainTool::cellSize() const
{
  return m_cellSize;
}

void TerrainTool::setCellSize(const double cellSize)
{
  if (cellSize > 0.0 && cellSize != m_cellSize)
  {
    m_cellSize = cellSize;
    terrainDidChangeNotifier();
  }
}

float TerrainTool::texScaleX() const
{
  return m_terrain.texScaleX;
}

float TerrainTool::texScaleY() const
{
  return m_terrain.texScaleY;
}

void TerrainTool::setTexScale(const float x, const float y)
{
  if (
    !hasTerrain() || x == 0.0f || y == 0.0f
    || (m_terrain.texScaleX == x && m_terrain.texScaleY == y))
  {
    return;
  }

  m_terrain.texScaleX = x;
  m_terrain.texScaleY = y;
  commitTerrain("Change Terrain Texture Scale");
}

bool TerrainTool::hasTerrain() const
{
  return mdl::isValidTerrain(m_terrain);
}

const mdl::Terrain& TerrainTool::terrain() const
{
  return m_terrain;
}

bool TerrainTool::createTerrain(const vm::bbox3d& bounds)
{
  auto& map = m_document.map();

  auto terrain = mdl::createTerrain(bounds, m_cellSize, map.currentMaterialName());
  if (!terrain)
  {
    map.logger().error()
      << "Could not create terrain: the box is too small for a single cell, or needs "
         "more than "
      << mdl::TerrainMaxCells << " cells";
    return false;
  }

  m_terrain = std::move(*terrain);
  m_terrainNode = nullptr;
  commitTerrain("Create Terrain");
  return true;
}

mdl::EntityNode* TerrainTool::otherTerrainNodeAt(const mdl::PickResult& pickResult) const
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
          entityNode && entityNode != m_terrainNode
          && mdl::isTerrainEntity(entityNode->entity()))
      {
        return entityNode;
      }
    }
  }
  return nullptr;
}

bool TerrainTool::canSelectTerrainAt(const mdl::PickResult& pickResult) const
{
  return otherTerrainNodeAt(pickResult) != nullptr;
}

bool TerrainTool::selectTerrainAt(const mdl::PickResult& pickResult)
{
  if (auto* entityNode = otherTerrainNodeAt(pickResult))
  {
    loadTerrainNode(entityNode);
    return true;
  }
  return false;
}

std::optional<vm::vec3d> TerrainTool::pickSurface(const vm::ray3d& ray) const
{
  return hasTerrain() ? mdl::pickTerrain(m_terrain, ray) : std::nullopt;
}

bool TerrainTool::canRemoveTerrain() const
{
  return m_terrainNode != nullptr;
}

void TerrainTool::removeTerrain()
{
  if (!canRemoveTerrain())
  {
    return;
  }

  auto& map = m_document.map();

  {
    const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
    auto transaction = mdl::Transaction{map, "Remove Terrain"};
    removeNodes(map, {m_terrainNode});
    transaction.commit();
  }

  m_terrainNode = nullptr;
  m_terrain = mdl::Terrain{};
  m_strokeOriginal = std::nullopt;
  m_strokeCells.clear();
  m_brushPosition = std::nullopt;

  refreshOtherTerrains();
  refreshViews();
  terrainDidChangeNotifier();
}

bool TerrainTool::importHeightmap(
  const std::filesystem::path& path, const std::optional<mdl::RawSampleFormat> format)
{
  auto& map = m_document.map();
  if (!hasTerrain())
  {
    return false;
  }

  return fs::Disk::openFile(path) | kdl::and_then([&](auto file) {
           const auto reader = file->reader().buffer();
           // An extension that names the sample format settles it when the caller did
           // not; .raw itself says nothing, so the file has to be judged instead.
           return mdl::parseRawHeightmap(
             reader.stringView(),
             format ? format : mdl::rawSampleFormatForExtension(path));
         })
         | kdl::and_then([&](const auto& heightmap) -> Result<void> {
             if (!mdl::applyRawHeightmap(m_terrain, heightmap))
             {
               return Error{"the height map could not be applied to this terrain"};
             }

             map.logger().info()
               << "Imported a " << heightmap.size << "x" << heightmap.size << " "
               << mdl::describeRawSampleFormat(heightmap.format) << " height map from "
               << path;
             commitTerrain("Import Terrain Heightmap");
             return Result<void>{};
           })
         | kdl::transform([]() { return true; })
         | kdl::transform_error([&](const auto& e) {
             map.logger().error()
               << "Could not import height map from " << path << ": " << e.msg;
             return false;
           })
         | kdl::value();
}

bool TerrainTool::canBreakTerrain() const
{
  return m_terrainNode != nullptr && !m_terrainNode->children().empty();
}

void TerrainTool::breakTerrain()
{
  if (!canBreakTerrain())
  {
    return;
  }

  auto& map = m_document.map();

  // Duplicate the generated brushes as standard brushes outside the terrain entity,
  // so they stay behind as editable geometry when the terrain is removed.
  auto duplicates = std::vector<mdl::Node*>{};
  for (auto* child : m_terrainNode->children())
  {
    if (const auto* brushNode = dynamic_cast<mdl::BrushNode*>(child))
    {
      duplicates.push_back(new mdl::BrushNode{brushNode->brush()});
    }
  }

  auto* parent = m_terrainNode->parent();

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  auto transaction = mdl::Transaction{map, "Break Terrain"};
  if (addNodes(map, {{parent, duplicates}}).empty())
  {
    transaction.cancel();
    return;
  }

  removeNodes(map, {m_terrainNode});
  m_terrainNode = nullptr;
  m_terrain = mdl::Terrain{};
  transaction.commit();

  refreshOtherTerrains();
  refreshViews();
  terrainDidChangeNotifier();
}

const std::optional<vm::vec3d>& TerrainTool::brushPosition() const
{
  return m_brushPosition;
}

void TerrainTool::setBrushPosition(std::optional<vm::vec3d> position)
{
  if (position != m_brushPosition)
  {
    m_brushPosition = std::move(position);
    refreshViews();
  }
}

std::string TerrainTool::strokeCommandName() const
{
  switch (m_mode.value_or(TerrainToolMode::Raise))
  {
  case TerrainToolMode::Flatten:
    return "Flatten Terrain";
  case TerrainToolMode::Smooth:
    return "Smooth Terrain";
  case TerrainToolMode::Texture:
    return "Paint Terrain";
  case TerrainToolMode::Raise:
  case TerrainToolMode::Lower:
    break;
  }
  return "Sculpt Terrain";
}

bool TerrainTool::canUpdateCellsInPlace() const
{
  return m_terrainNode
         && m_terrainNode->childCount()
              == mdl::terrainCellCount(m_terrain) * mdl::TerrainBrushesPerCell;
}

void TerrainTool::beginStroke()
{
  if (!hasTerrain())
  {
    return;
  }

  m_strokeOriginal = m_terrain;
  m_strokeCells.clear();
}

std::vector<size_t> TerrainTool::cellsInRadius(const vm::vec3d& position) const
{
  auto cells = std::vector<size_t>{};
  if (!hasTerrain())
  {
    return cells;
  }

  // A cell is affected when one of its corners lies within the radius, so the search
  // is limited to the cells the brush's bounding square covers.
  const auto toCell = [&](
                        const double world,
                        const double origin,
                        const double cellSize,
                        const size_t count) {
    const auto index = std::llround(std::floor((world - origin) / cellSize));
    return size_t(vm::clamp(index, 0ll, std::llround(double(count) - 1.0)));
  };

  const auto minColumn = toCell(
    position.x() - m_radius - m_terrain.cellSizeX,
    m_terrain.origin.x(),
    m_terrain.cellSizeX,
    m_terrain.columns);
  const auto maxColumn = toCell(
    position.x() + m_radius + m_terrain.cellSizeX,
    m_terrain.origin.x(),
    m_terrain.cellSizeX,
    m_terrain.columns);
  const auto minRow = toCell(
    position.y() - m_radius - m_terrain.cellSizeY,
    m_terrain.origin.y(),
    m_terrain.cellSizeY,
    m_terrain.rows);
  const auto maxRow = toCell(
    position.y() + m_radius + m_terrain.cellSizeY,
    m_terrain.origin.y(),
    m_terrain.cellSizeY,
    m_terrain.rows);

  for (auto row = minRow; row <= maxRow; ++row)
  {
    for (auto column = minColumn; column <= maxColumn; ++column)
    {
      cells.push_back(row * m_terrain.columns + column);
    }
  }
  return cells;
}

bool TerrainTool::applyStroke(const vm::vec3d& position, const bool invert)
{
  const auto mode = effectiveMode(invert);
  if (!hasTerrain() || !mode)
  {
    return false;
  }

  if (mode == TerrainToolMode::Texture)
  {
    // The painted material is the one selected in the material browser.
    if (!mdl::paintTerrain(
          m_terrain, position, m_radius, m_document.map().currentMaterialName()))
    {
      return false;
    }
  }
  else
  {
    const auto sculptMode =
      mode == TerrainToolMode::Raise     ? mdl::TerrainSculptMode::Raise
      : mode == TerrainToolMode::Lower   ? mdl::TerrainSculptMode::Lower
      : mode == TerrainToolMode::Flatten ? mdl::TerrainSculptMode::Flatten
                                         : mdl::TerrainSculptMode::Smooth;

    if (!mdl::sculptTerrain(m_terrain, position, m_radius, m_strength, sculptMode))
    {
      return false;
    }
  }

  // Building a brush costs roughly 45us, far too much to redo for every cell under the
  // brush on every mouse movement. So a stroke only edits the height field here and the
  // result is drawn as a preview (see render); the brushes of the cells the stroke swept
  // are built once when it ends.
  for (const auto cell : cellsInRadius(position))
  {
    m_strokeCells.insert(cell);
  }

  refreshViews();
  return true;
}

bool TerrainTool::commitStrokeCells()
{
  if (!canUpdateCellsInPlace() || m_strokeCells.empty())
  {
    return false;
  }

  auto& map = m_document.map();
  const auto& children = m_terrainNode->children();

  // Only the cells the stroke swept are rebuilt, and their brushes are swapped into
  // the existing nodes, so the cost depends on the stroke rather than the terrain.
  auto nodesToSwap = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};
  nodesToSwap.reserve(m_strokeCells.size() * mdl::TerrainBrushesPerCell + 1);

  for (const auto cell : m_strokeCells)
  {
    auto cellBrushes = mdl::createTerrainCellBrushes(
      map.worldNode().mapFormat(),
      map.worldBounds(),
      m_terrain,
      cell % m_terrain.columns,
      cell / m_terrain.columns);
    if (cellBrushes.is_error())
    {
      return false;
    }

    auto brushes = std::move(cellBrushes) | kdl::value();
    if (brushes.size() != mdl::TerrainBrushesPerCell)
    {
      return false;
    }

    for (size_t i = 0; i < mdl::TerrainBrushesPerCell; ++i)
    {
      auto* node = children[cell * mdl::TerrainBrushesPerCell + i];
      if (!dynamic_cast<mdl::BrushNode*>(node))
      {
        return false;
      }
      nodesToSwap.emplace_back(node, mdl::NodeContents{std::move(brushes[i])});
    }
  }

  // The height field itself lives in the entity's properties, so it is updated in the
  // same transaction.
  nodesToSwap.emplace_back(
    m_terrainNode,
    mdl::NodeContents{mdl::writeTerrainEntity(m_terrainNode->entity(), m_terrain)});

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  return updateNodeContents(map, strokeCommandName(), std::move(nodesToSwap));
}

void TerrainTool::endStroke()
{
  if (!m_strokeOriginal)
  {
    return;
  }

  const auto original = std::move(*m_strokeOriginal);
  m_strokeOriginal = std::nullopt;

  const auto changed =
    original.heights != m_terrain.heights || original.materials != m_terrain.materials;

  if (changed && !commitStrokeCells())
  {
    commitTerrain(strokeCommandName());
  }

  m_strokeCells.clear();
  refreshViews();
  terrainDidChangeNotifier();
}

void TerrainTool::cancelStroke()
{
  m_strokeCells.clear();

  if (m_strokeOriginal)
  {
    m_terrain = std::move(*m_strokeOriginal);
    m_strokeOriginal = std::nullopt;
    refreshViews();
    terrainDidChangeNotifier();
  }
}

bool TerrainTool::strokeActive() const
{
  return m_strokeOriginal.has_value();
}

void TerrainTool::loadFromSelection()
{
  for (auto* node : m_document.map().selection().nodes)
  {
    for (auto* candidate = node; candidate != nullptr; candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && mdl::isTerrainEntity(entityNode->entity()))
      {
        loadTerrainNode(entityNode);
        return;
      }
    }
  }
}

void TerrainTool::loadTerrainNode(mdl::EntityNode* terrainNode)
{
  if (auto terrain = mdl::parseTerrainEntity(terrainNode->entity()))
  {
    m_terrainNode = terrainNode;
    m_terrain = std::move(*terrain);
    m_strokeOriginal = std::nullopt;
    m_brushPosition = std::nullopt;

    refreshOtherTerrains();
    refreshViews();
    terrainDidChangeNotifier();
  }
}

void TerrainTool::clearTerrain()
{
  m_terrainNode = nullptr;
  m_terrain = mdl::Terrain{};
  m_strokeOriginal = std::nullopt;
  m_brushPosition = std::nullopt;
  refreshOtherTerrains();
  refreshViews();
  terrainDidChangeNotifier();
}

void TerrainTool::refreshOtherTerrains()
{
  m_otherTerrains.clear();

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
      if (&entityNode != m_terrainNode && mdl::isTerrainEntity(entityNode.entity()))
      {
        if (auto terrain = mdl::parseTerrainEntity(entityNode.entity()))
        {
          m_otherTerrains.emplace_back(&entityNode, std::move(*terrain));
        }
      }
    },
    [](mdl::BrushNode&) {},
    [](mdl::PatchNode&) {}));
}

void TerrainTool::commitTerrain(const std::string& commandName)
{
  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  auto& map = m_document.map();

  if (!hasTerrain())
  {
    if (m_terrainNode)
    {
      auto transaction = mdl::Transaction{map, commandName};
      removeNodes(map, {m_terrainNode});
      m_terrainNode = nullptr;
      transaction.commit();
    }
    refreshViews();
    terrainDidChangeNotifier();
    return;
  }

  auto entity = mdl::writeTerrainEntity(
    m_terrainNode ? m_terrainNode->entity() : mdl::Entity{}, m_terrain);

  auto* newNode = new mdl::EntityNode{std::move(entity)};
  newNode->addChildren(createBrushNodes());

  auto* parent = m_terrainNode ? m_terrainNode->parent() : &parentForNodes(map, {});

  auto transaction = mdl::Transaction{map, commandName};
  if (m_terrainNode)
  {
    removeNodes(map, {m_terrainNode});
  }
  const auto addedNodes = addNodes(map, {{parent, {newNode}}});
  if (addedNodes.empty())
  {
    transaction.cancel();
    m_terrainNode = nullptr;
  }
  else
  {
    m_terrainNode = newNode;
    transaction.commit();
  }

  refreshOtherTerrains();
  refreshViews();
  terrainDidChangeNotifier();
}

std::vector<mdl::Node*> TerrainTool::createBrushNodes() const
{
  auto& map = m_document.map();
  return mdl::createTerrainBrushes(
           map.worldNode().mapFormat(), map.worldBounds(), m_terrain)
         | kdl::transform([](auto brushes) {
             return brushes | std::views::transform([](auto& brush) {
                      return static_cast<mdl::Node*>(
                        new mdl::BrushNode{std::move(brush)});
                    })
                    | kdl::ranges::to<std::vector>();
           })
         | kdl::transform_error([&](const auto& e) {
             map.logger().error() << "Could not create terrain brushes: " << e.msg;
             return std::vector<mdl::Node*>{};
           })
         | kdl::value();
}

bool TerrainTool::doActivate()
{
  connectObservers();

  // Always start with add mode disabled so that switching to the tool never creates
  // terrains accidentally; the user enables it explicitly on the tool page.
  m_addMode = false;
  m_brushPosition = std::nullopt;
  loadFromSelection();
  refreshOtherTerrains();

  terrainDidChangeNotifier();
  return true;
}

bool TerrainTool::doDeactivate()
{
  m_notifierConnection.disconnect();
  clearTerrain();
  m_otherTerrains.clear();
  return true;
}

void TerrainTool::connectObservers()
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

void TerrainTool::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  refreshOtherTerrains();

  if (m_terrainNode != nullptr)
  {
    return;
  }

  // Adopt a terrain entity that reappears, e.g. when a terrain edit is undone.
  for (auto* node : nodes)
  {
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node);
        entityNode && mdl::isTerrainEntity(entityNode->entity()))
    {
      loadTerrainNode(entityNode);
      return;
    }
  }
}

void TerrainTool::nodesWereRemoved(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  if (m_terrainNode && std::ranges::find(nodes, m_terrainNode) != nodes.end())
  {
    clearTerrain();
  }
  else
  {
    refreshOtherTerrains();
  }
}

void TerrainTool::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications)
  {
    return;
  }

  refreshOtherTerrains();

  if (
    m_terrainNode
    && std::ranges::find(nodes, static_cast<mdl::Node*>(m_terrainNode)) != nodes.end())
  {
    loadTerrainNode(m_terrainNode);
  }
}

void TerrainTool::selectionDidChange()
{
  if (m_ignoreNotifications)
  {
    return;
  }

  // Switch to a newly selected terrain entity, but keep editing the current terrain if
  // the selection does not contain one.
  for (auto* node : m_document.map().selection().nodes)
  {
    for (auto* candidate = node; candidate != nullptr; candidate = candidate->parent())
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(candidate);
          entityNode && entityNode != m_terrainNode
          && mdl::isTerrainEntity(entityNode->entity()))
      {
        loadTerrainNode(entityNode);
        return;
      }
    }
  }
}

} // namespace tb::ui
