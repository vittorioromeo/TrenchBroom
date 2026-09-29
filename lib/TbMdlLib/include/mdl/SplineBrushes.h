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
#include "mdl/Spline.h"

#include "vm/bbox.h"

#include <vector>

namespace tb::mdl
{
class Brush;
enum class MapFormat;

/**
 * What the sweep does with the UVs of the template faces it copies.
 */
enum class SplineUVMode
{
  /**
   * The UVs are carried across by one map fitted to the whole span.
   *
   * The deformation that map stands in for is not affine, so it is only an
   * approximation of what happened to any particular part of the template, and the
   * copies come out with the texture a little off where the two disagree.
   */
  Follow,
  /**
   * The UVs are carried across by the copy's placement alone: where it ended up and
   * which way it is turned, with the squeezing that made it fit its segment left out.
   *
   * Moving and turning brushes with texture lock on does not distort their textures,
   * because a rigid transform cannot distort anything. This is the same thing for a
   * sweep, and it is what a piece of art wants: a tree stays the shape the template drew
   * it however much its copy was squeezed to fit.
   *
   * A format that stores its UV axes holds this exactly. The paraxial format picks its
   * axes from each face's own normal, so a turned face cannot hold an undistorted
   * picture there whatever it is told to do.
   */
  Lock,
};

/**
 * Creates brushes by sweeping the given template (profile) brushes along the spline
 * through the given control points.
 *
 * The template brushes are interpreted inside their combined bounds (the lattice),
 * with the X axis as the sweep direction: cross-section frames are placed along the
 * curve so that each control point segment holds round(segmentLength / latticeSizeX)
 * copies of the template (at least one), stretched or squished to fit the segment
 * while keeping the cross-section's proportions.
 *
 * Each span between two consecutive frames holds one copy of every template brush:
 * a vertex's X position inside the lattice interpolates linearly between the span's
 * two cross-sections, and its Y / Z offsets from the lattice center are applied along
 * each frame's right / up direction, scaled by the frame's cross-section scale.
 * Consecutive spans share their end cross-sections, so the swept solid is continuous.
 * Face materials and attributes are copied from the best matching template face.
 *
 * If closed is true, the spline wraps around: the segment from the last control point
 * back to the first is swept as well.
 *
 * uvMode decides what happens to the template's UVs. See SplineUVMode.
 *
 * With keepTemplateSize, a copy is placed at the size the template is drawn at instead
 * of being stretched to fill its span, and the span is left a little long rather than a
 * little short so that one copy never runs into the next. Each copy is then a rigid
 * placement of the template: same shape, same size, and so the same texture on it,
 * fitted to its faces exactly as the template's is. Use it for something the sweep is
 * scattering rather than building, a tree or a lamp post, where a copy squeezed to fit
 * is a copy of the wrong shape.
 *
 * Returns an error if the lattice or the spline is degenerate, or if no brushes
 * could be created.
 */
/**
 * The brushes of each copy on its own, in the order the copies run along the spline,
 * rather than all of them together. See createSplineBrushes, which is this flattened.
 *
 * A template brush entity is replicated as an entity per copy, the way a template point
 * entity is, so which copy a brush belongs to is what decides which entity it goes in.
 * A copy whose cells all collapsed contributes an empty entry rather than being left
 * out, so that the entries stay in step with the copies.
 */
Result<std::vector<std::vector<Brush>>> createSplineBrushCopies(
  MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const std::vector<SplinePoint>& points,
  const std::vector<const Brush*>& templateBrushes,
  const vm::bbox3d& templateBounds,
  bool closed = false,
  SplineUVMode uvMode = SplineUVMode::Follow,
  bool keepTemplateSize = false);

Result<std::vector<Brush>> createSplineBrushes(
  MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const std::vector<SplinePoint>& points,
  const std::vector<const Brush*>& templateBrushes,
  const vm::bbox3d& templateBounds,
  bool closed = false,
  SplineUVMode uvMode = SplineUVMode::Follow,
  bool keepTemplateSize = false);

} // namespace tb::mdl
