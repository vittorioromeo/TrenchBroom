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

#pragma once

#include "base/Result.h"
#include "mdl/Terrain.h"

#include "vm/bbox.h"

#include <vector>

namespace tb::mdl
{
class Brush;
enum class MapFormat;

/** The number of brushes generated for a single terrain cell. */
constexpr size_t TerrainBrushesPerCell = 2;

/**
 * Creates the two brushes of the cell in the given column and row.
 *
 * The cell is extruded from the terrain's base plane up to its four corner heights and
 * split along a diagonal into two triangular prisms. Such a prism is always convex, so
 * it is a valid brush no matter how the corner heights differ: its bottom lies in the
 * base plane, its top is spanned by three points and therefore always planar, and its
 * three sides are vertical planes. Neighbouring cells share their corner vertices
 * exactly, so the terrain is watertight.
 *
 * The faces are built directly rather than via a convex hull, and each receives the
 * cell's material (or the terrain's default material) and the terrain's texture scale.
 */
Result<std::vector<Brush>> createTerrainCellBrushes(
  MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const Terrain& terrain,
  size_t column,
  size_t row);

/**
 * Creates the solid geometry of the whole terrain: TerrainBrushesPerCell brushes per
 * cell, cell by cell in row major order, so that a cell's brushes can be found again
 * by index and swapped in place when only that cell changes.
 *
 * Returns an error if the terrain is degenerate or no brushes could be created.
 */
Result<std::vector<Brush>> createTerrainBrushes(
  MapFormat mapFormat, const vm::bbox3d& worldBounds, const Terrain& terrain);

} // namespace tb::mdl
