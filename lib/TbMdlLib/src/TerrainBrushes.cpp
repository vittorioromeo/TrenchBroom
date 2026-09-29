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

#include "mdl/TerrainBrushes.h"

#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/SurfaceAttributes.h"
#include "mdl/UvAttributes.h"

#include "kd/result.h"
#include "kd/result_fold.h"

#include "vm/vec.h"

#include <vector>

namespace tb::mdl
{
namespace
{

/**
 * Builds one triangular prism from its bottom triangle and the matching top triangle.
 *
 * The bottom triangle must wind counter clockwise seen from above, and every top
 * vertex must lie directly above its bottom vertex. The faces are given as point
 * triples whose normal is cross(p2 - p0, p1 - p0), which is TrenchBroom's convention,
 * so the normals below point out of the prism.
 */
Result<Brush> createPrism(
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const vm::vec3d& a0,
  const vm::vec3d& b0,
  const vm::vec3d& c0,
  const vm::vec3d& a1,
  const vm::vec3d& b1,
  const vm::vec3d& c1,
  const std::string& materialName,
  const UvAttributes& uvAttributes)
{
  return std::vector{
           BrushFace::create(
             a0,
             b0,
             c0,
             materialName,
             uvAttributes,
             SurfaceAttributes{},
             mapFormat), // bottom
           BrushFace::create(
             a1,
             c1,
             b1,
             materialName,
             uvAttributes,
             SurfaceAttributes{},
             mapFormat), // top
           BrushFace::create(
             a0,
             a1,
             b0,
             materialName,
             uvAttributes,
             SurfaceAttributes{},
             mapFormat), // side a -> b
           BrushFace::create(
             b0,
             b1,
             c0,
             materialName,
             uvAttributes,
             SurfaceAttributes{},
             mapFormat), // side b -> c
           BrushFace::create(
             c0,
             c1,
             a0,
             materialName,
             uvAttributes,
             SurfaceAttributes{},
             mapFormat), // side c -> a
         }
         | kdl::fold | kdl::and_then([&](auto faces) {
             return Brush::create(worldBounds, std::move(faces));
           });
}

} // namespace

Result<std::vector<Brush>> createTerrainCellBrushes(
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const Terrain& terrain,
  const size_t column,
  const size_t row)
{
  if (!isValidTerrain(terrain) || column >= terrain.columns || row >= terrain.rows)
  {
    return Error{"Terrain cell is out of bounds"};
  }

  auto uvAttributes = UvAttributes{};
  uvAttributes.scale = vm::vec2f{terrain.texScaleX, terrain.texScaleY};
  const auto materialName = terrainCellMaterial(terrain, column, row);

  const auto t00 = terrainVertexPosition(terrain, column, row);
  const auto t10 = terrainVertexPosition(terrain, column + 1, row);
  const auto t11 = terrainVertexPosition(terrain, column + 1, row + 1);
  const auto t01 = terrainVertexPosition(terrain, column, row + 1);

  const auto baseZ = terrain.origin.z();
  const auto atBase = [&](const vm::vec3d& top) {
    return vm::vec3d{top.x(), top.y(), baseZ};
  };

  // Split the cell along the 00-11 diagonal. Both bottom triangles below wind counter
  // clockwise seen from above, and the same diagonal is used in every cell so that
  // neighbouring cells stay conforming.
  return std::vector{
           createPrism(
             mapFormat,
             worldBounds,
             atBase(t00),
             atBase(t10),
             atBase(t11),
             t00,
             t10,
             t11,
             materialName,
             uvAttributes),
           createPrism(
             mapFormat,
             worldBounds,
             atBase(t00),
             atBase(t11),
             atBase(t01),
             t00,
             t11,
             t01,
             materialName,
             uvAttributes),
         }
         | kdl::fold;
}

Result<std::vector<Brush>> createTerrainBrushes(
  const MapFormat mapFormat, const vm::bbox3d& worldBounds, const Terrain& terrain)
{
  if (!isValidTerrain(terrain))
  {
    return Error{"Terrain is degenerate"};
  }

  auto brushes = std::vector<Brush>{};
  brushes.reserve(terrainCellCount(terrain) * TerrainBrushesPerCell);

  for (size_t row = 0; row < terrain.rows; ++row)
  {
    for (size_t column = 0; column < terrain.columns; ++column)
    {
      auto cellBrushes =
        createTerrainCellBrushes(mapFormat, worldBounds, terrain, column, row);
      if (cellBrushes.is_error())
      {
        return Error{"Could not create terrain brushes"};
      }

      for (auto& brush : std::move(cellBrushes) | kdl::value())
      {
        brushes.push_back(std::move(brush));
      }
    }
  }

  if (brushes.empty())
  {
    return Error{"Could not create any terrain brushes"};
  }

  return brushes;
}

} // namespace tb::mdl
