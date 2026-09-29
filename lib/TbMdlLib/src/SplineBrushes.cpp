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

#include "mdl/SplineBrushes.h"

#include "gl/Material.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/UvCoordSystem.h"

#include "kd/result.h"

#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/scalar.h"
#include "vm/vec.h"
#include "vm/vec_ext.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <ranges>
#include <vector>

namespace tb::mdl
{
namespace
{

/**
 * The affine approximation of the free-form deformation over one span, mapping
 * template (lattice) space into the world. The X axis follows the span between the two
 * frame positions, and the cross-section axes are the averaged, scaled right / up
 * directions of the two frames. Used to realign the template faces' UVs onto the
 * deformed geometry via the regular alignment lock.
 */
vm::mat4x4d spanUVTransform(
  const vm::bbox3d& lattice, const SweepFrame& a, const SweepFrame& b)
{
  const auto sx = vm::max(1e-4, lattice.size().x());
  auto xAxis = (b.position - a.position) / sx;
  if (vm::squared_length(xAxis) < 1e-10)
  {
    // Degenerate span: fall back to the frame's forward direction.
    xAxis = vm::cross(a.right, a.up);
  }
  const auto yAxis = (a.right * a.scale + b.right * b.scale) * 0.5;
  const auto zAxis = (a.up * a.scale + b.up * b.scale) * 0.5;

  const auto origin =
    vm::vec3d{lattice.min.x(), lattice.center().y(), lattice.center().z()};
  const auto translation =
    a.position - xAxis * origin.x() - yAxis * origin.y() - zAxis * origin.z();

  return vm::mat4x4d{
    xAxis.x(),
    yAxis.x(),
    zAxis.x(),
    translation.x(),
    xAxis.y(),
    yAxis.y(),
    zAxis.y(),
    translation.y(),
    xAxis.z(),
    yAxis.z(),
    zAxis.z(),
    translation.z(),
    0.0,
    0.0,
    0.0,
    1.0};
}

/**
 * The end of a span cut back to the template's own length, keeping the start frame's
 * orientation and scale.
 *
 * Sweeping between a frame and this one places the template at the size it is drawn at,
 * turned to face along the span but not bent by it or stretched to fill it, which is
 * what makes every copy identical to the template and to each other.
 */
SweepFrame naturalEndFrame(
  const SweepFrame& a, const SweepFrame& b, const double templateLength)
{
  auto forward = b.position - a.position;
  if (vm::squared_length(forward) < 1.0e-10)
  {
    forward = vm::cross(a.right, a.up);
  }

  return SweepFrame{
    a.position + vm::normalize(forward) * templateLength, a.right, a.up, a.scale};
}

/**
 * The rigid placement of the template into the span: where the copy ends up and which
 * way it is turned, with the stretching that made it fit left out.
 *
 * This is what a locked copy's UVs are carried across by. Moving and turning a selection
 * with texture lock on does not distort its textures, because a rigid transform cannot
 * distort anything; stretching one does. The sweep stretches every copy to fit its
 * segment, so carrying the UVs across by what actually happened to the geometry stretches
 * the picture with it. Carrying them across by the placement alone leaves the picture the
 * size the template drew it, which is what a tree or any other piece of art wants.
 */
vm::mat4x4d rigidSpanTransform(
  const vm::bbox3d& lattice, const SweepFrame& a, const SweepFrame& b)
{
  const auto center = lattice.center();
  const auto rotation = spanOrientation(center, lattice, a, b);
  return vm::translation_matrix(deformIntoSpan(center, lattice, a, b)) * rotation
         * vm::translation_matrix(-center);
}

/**
 * A template face placed into the span two ways: by what actually happened to the
 * geometry, which is the shape a generated face is matched against, and by the rigid
 * placement alone, which is the undistorted alignment a locked copy takes.
 */
struct TemplateFace
{
  BrushFace deformed;
  BrushFace rigid;
};

/**
 * The template brush's faces moved into the span's world space, which is what the
 * generated faces take their attributes and alignment from.
 *
 * Whether the UVs come with them is the whole of the difference between the two modes:
 * following carries them through the deformation, which realigns the texture onto the
 * geometry it produces, and locking leaves them where the template had them. A face
 * whose transformation fails is left where it was.
 */
std::vector<TemplateFace> placeTemplateFaces(
  const Brush& templateBrush,
  const vm::mat4x4d& spanTransform,
  const vm::mat4x4d& rigidTransform)
{
  const auto place = [](const BrushFace& face, const vm::mat4x4d& transform) {
    auto copy = face;
    if (copy.transform(transform, true).is_error())
    {
      copy = face;
    }
    return copy;
  };

  auto faces = std::vector<TemplateFace>{};
  faces.reserve(templateBrush.faceCount());
  for (const auto& face : templateBrush.faces())
  {
    faces.push_back(
      TemplateFace{place(face, spanTransform), place(face, rigidTransform)});
  }
  return faces;
}

/**
 * Copies the attributes and the UV alignment of the best matching template face onto
 * each face of the given brush. Faces are matched by normal in world space against the
 * deformed template faces, since those are the shapes the generated ones were cut from.
 */
void copyFaceAttributes(
  Brush& brush, const std::vector<TemplateFace>& templateFaces, const SplineUVMode uvMode)
{
  for (auto& face : brush.faces())
  {
    const TemplateFace* bestMatch = nullptr;
    auto bestDot = -2.0;
    for (const auto& templateFace : templateFaces)
    {
      const auto d = vm::dot(face.normal(), templateFace.deformed.normal());
      if (d > bestDot)
      {
        bestDot = d;
        bestMatch = &templateFace;
      }
    }

    if (!bestMatch)
    {
      continue;
    }

    const auto& alignment =
      uvMode == SplineUVMode::Lock ? bestMatch->rigid : bestMatch->deformed;

    // The material has to be on the face before its UVs are set, not after. A face with
    // none reports its texture as one pixel by one, and an offset is kept modulo the
    // texture, so every offset the alignment carries would be wrapped down into the
    // first pixel and lost. The brush builder only knows the material's name, so the
    // face is pointed at the template's material here. Only the material's own usage
    // count changes, which is what it counts.
    face.setMaterial(const_cast<gl::Material*>(alignment.material()));

    face.setMaterialName(alignment.materialName());
    face.setUvAttributes(alignment.uvAttributes()) | kdl::ignore();
    face.setSurfaceAttributes(alignment.surfaceAttributes());
    if (const auto snapshot = alignment.takeUvCoordSystemSnapshot())
    {
      // Wrap the source face's UV coordinate system onto this face's plane; for UV
      // coordinate systems without a snapshot (paraxial), the attributes copied above
      // already carry the alignment.
      //
      // Locked, the axes are turned onto the new plane rather than reprojected, since
      // turning them is what a rigid placement does.
      face.copyUvCoordSystemFromFace(
        *snapshot,
        alignment.uvAttributes(),
        alignment.boundary(),
        uvMode == SplineUVMode::Lock ? WrapStyle::Rotation : WrapStyle::Projection);
    }
  }
}

} // namespace

Result<std::vector<std::vector<Brush>>> createSplineBrushCopies(
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const std::vector<SplinePoint>& points,
  const std::vector<const Brush*>& templateBrushes,
  const vm::bbox3d& templateBounds,
  const bool closed,
  const SplineUVMode uvMode,
  const bool keepTemplateSize)
{
  if (templateBounds.size().x() <= 0.0)
  {
    return Error{"Spline template must have a non-zero extent along the X axis"};
  }

  if (points.size() < 2)
  {
    return Error{"Spline must have at least two control points"};
  }

  if (templateBrushes.empty())
  {
    return Error{"Spline template contains no brushes"};
  }

  const auto forwardSize = vm::max(1.0, templateBounds.size().x());
  const auto frames = buildSweepFrames(points, forwardSize, closed, keepTemplateSize);
  if (frames.size() < 2)
  {
    return Error{"Spline has zero length"};
  }

  const auto builder = BrushBuilder{mapFormat, worldBounds};

  auto copies = std::vector<std::vector<Brush>>{};

  // One copy of every template brush per span between two consecutive frames. Each
  // template brush is decomposed into tetrahedra before deforming: a tetrahedron is a
  // simplex, so its four deformed corners always form a valid convex brush, whereas
  // deforming a larger convex brush as a whole can make its vertex set non-convex on
  // curved spans, and the convex hull would then shave off vertices and distort the
  // shape. The decomposition fans out from the brush's vertex centroid: every face is
  // triangulated, and each triangle forms a tetrahedron with the centroid.
  //
  // All deformed vertices are snapped to integer coordinates. Adjacent spans compute
  // identical positions for their shared cross section, and tetrahedra sharing a face
  // or edge share its deformed vertices, so everything rounds consistently and
  // snapping cannot open gaps.
  for (size_t i = 0; i < frames.size() - 1; ++i)
  {
    auto& copy = copies.emplace_back();
    const auto& a = frames[i];
    // Kept at its own size, the copy runs a template's length straight along the span
    // rather than bending around it, so the curve neither stretches nor bends it. It
    // therefore stops short of where the span ends, by however much the curve bulges
    // away from the straight line across it.
    const auto naturalEnd = naturalEndFrame(a, frames[i + 1], forwardSize);
    const auto& b = keepTemplateSize ? naturalEnd : frames[i + 1];

    const auto uvTransform = spanUVTransform(templateBounds, a, b);
    const auto rigidTransform = rigidSpanTransform(templateBounds, a, b);

    for (const auto* templateBrush : templateBrushes)
    {
      const auto vertices = templateBrush->vertexPositions();
      auto apex = vm::vec3d{};
      for (const auto& vertex : vertices)
      {
        apex = apex + vertex;
      }
      apex = apex / double(vertices.size());
      const auto deformedApex = vm::round(deformIntoSpan(apex, templateBounds, a, b));

      const auto templateFaces =
        placeTemplateFaces(*templateBrush, uvTransform, rigidTransform);

      const auto materialName = !templateBrush->faces().empty()
                                  ? templateBrush->faces().front().materialName()
                                  : "";

      for (const auto& face : templateBrush->faces())
      {
        const auto faceVertices = face.vertexPositions();
        auto deformedFaceVertices = std::vector<vm::vec3d>{};
        deformedFaceVertices.reserve(faceVertices.size());
        for (const auto& vertex : faceVertices)
        {
          deformedFaceVertices.push_back(
            vm::round(deformIntoSpan(vertex, templateBounds, a, b)));
        }

        for (size_t j = 1; j + 1 < deformedFaceVertices.size(); ++j)
        {
          builder.createBrush(
            std::vector<vm::vec3d>{
              deformedApex,
              deformedFaceVertices[0],
              deformedFaceVertices[j],
              deformedFaceVertices[j + 1]},
            materialName)
            | kdl::transform([&](Brush brush) {
                copyFaceAttributes(brush, templateFaces, uvMode);
                copy.push_back(std::move(brush));
              })
            | kdl::transform_error([](const auto&) {
                // Skip degenerate tetrahedra, e.g. where the deformation or the
                // rounding collapsed the cell.
              });
        }
      }
    }
  }

  if (std::ranges::all_of(copies, [](const auto& copy) { return copy.empty(); }))
  {
    return Error{"Could not create any spline brushes"};
  }

  return copies;
}

Result<std::vector<Brush>> createSplineBrushes(
  const MapFormat mapFormat,
  const vm::bbox3d& worldBounds,
  const std::vector<SplinePoint>& points,
  const std::vector<const Brush*>& templateBrushes,
  const vm::bbox3d& templateBounds,
  const bool closed,
  const SplineUVMode uvMode,
  const bool keepTemplateSize)
{
  return createSplineBrushCopies(
           mapFormat,
           worldBounds,
           points,
           templateBrushes,
           templateBounds,
           closed,
           uvMode,
           keepTemplateSize)
         | kdl::transform([](auto copies) {
             auto brushes = std::vector<Brush>{};
             for (auto& copy : copies)
             {
               brushes.insert(
                 brushes.end(),
                 std::make_move_iterator(copy.begin()),
                 std::make_move_iterator(copy.end()));
             }
             return brushes;
           });
}

} // namespace tb::mdl
