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

#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/MapFormat.h"
#include "mdl/Spline.h"
#include "mdl/SplineBrushes.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{
namespace
{

const auto worldBounds = vm::bbox3d{8192.0};

Brush makeCuboid(const vm::bbox3d& bounds, const std::string& materialName)
{
  const auto builder = BrushBuilder{MapFormat::Standard, worldBounds};
  return builder.createCuboid(bounds, materialName) | kdl::value();
}

vm::bbox3d unitedBounds(const std::vector<Brush>& brushes)
{
  auto bounds = brushes.front().bounds();
  for (const auto& brush : brushes)
  {
    bounds = vm::merge(bounds, brush.bounds());
  }
  return bounds;
}

} // namespace

TEST_CASE("createSplineBrushes")
{
  const auto templateBounds = vm::bbox3d{{0, -16, -16}, {64, 16, 16}};
  const auto templateBrush = makeCuboid(templateBounds, "some_material");
  const auto templateBrushes = std::vector<const Brush*>{&templateBrush};

  SECTION("fails for degenerate input")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
    };

    CHECK(createSplineBrushes(
            MapFormat::Standard, worldBounds, {}, templateBrushes, templateBounds)
            .is_error());
    CHECK(
      createSplineBrushes(MapFormat::Standard, worldBounds, points, {}, templateBounds)
        .is_error());
    CHECK(createSplineBrushes(
            MapFormat::Standard,
            worldBounds,
            points,
            templateBrushes,
            vm::bbox3d{{0, 0, 0}, {0, 16, 16}})
            .is_error());
  }

  SECTION("createSplineBrushCopies keeps the copies apart")
  {
    // Four template lengths of spline, so four copies, which is what a template brush
    // entity needs kept apart to become an entity per copy.
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{256, 0, 0}},
    };

    const auto copies =
      createSplineBrushCopies(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    REQUIRE(copies.size() == 4);

    // Every copy holds the same brushes it would hold in the flat list, in the same
    // order, and each one sits a template length further along than the last.
    const auto flat =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    auto flattened = std::vector<Brush>{};
    for (const auto& copy : copies)
    {
      CHECK_FALSE(copy.empty());
      for (const auto& brush : copy)
      {
        flattened.push_back(brush);
      }
    }

    REQUIRE(flattened.size() == flat.size());
    for (size_t i = 0; i < flat.size(); ++i)
    {
      CHECK(flattened[i].vertexPositions() == flat[i].vertexPositions());
    }

    // The copies run along the curve in order and meet end to end, so each one is a
    // whole copy rather than a slice of two. Their lengths differ, since it is the
    // curve's own parameterisation that decides where one ends and the next begins.
    CHECK(unitedBounds(copies.front()).min.x() == vm::approx{0.0});
    CHECK(unitedBounds(copies.back()).max.x() == vm::approx{256.0});

    for (size_t i = 0; i + 1 < copies.size(); ++i)
    {
      CHECK(
        unitedBounds(copies[i]).max.x()
        == vm::approx{unitedBounds(copies[i + 1]).min.x()});
    }
  }

  SECTION("straight spline reproduces the tiled template")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    // Two spans, one template copy each; every copy is decomposed into tetrahedra
    // (two per quad face of the cuboid).
    CHECK(brushes.size() == 2 * 12);

    const auto bounds = unitedBounds(brushes);
    CHECK(bounds.min == vm::approx{vm::vec3d{0, -16, -16}});
    CHECK(bounds.max == vm::approx{vm::vec3d{128, 16, 16}});

    for (const auto& brush : brushes)
    {
      CHECK(brush.fullySpecified());
      for (const auto& face : brush.faces())
      {
        CHECK(face.materialName() == "some_material");
      }
    }
  }

  SECTION("each segment is tiled with stretched copies")
  {
    // The template brush covers only the first half of the template bounds, so the
    // generated geometry reveals where each copy starts and how it is stretched.
    const auto halfBrush =
      makeCuboid(vm::bbox3d{{0, -16, -16}, {32, 16, 16}}, "some_material");
    const auto halfBrushes = std::vector<const Brush*>{&halfBrush};

    // One segment of length 96 with a forward size of 64 holds round(1.5) = 2
    // copies, each stretched to span 48 units; the half brush fills the first half
    // of each span.
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{96, 0, 0}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, halfBrushes, templateBounds)
      | kdl::value();

    REQUIRE(!brushes.empty());

    // Each copy is decomposed into tetrahedra, so instead of comparing individual
    // brushes, partition them by span and compare the united bounds per span.
    auto firstCopy = std::vector<Brush>{};
    auto secondCopy = std::vector<Brush>{};
    for (const auto& brush : brushes)
    {
      if (brush.bounds().max.x() <= 24.0)
      {
        firstCopy.push_back(brush);
      }
      else
      {
        REQUIRE(brush.bounds().min.x() >= 48.0);
        secondCopy.push_back(brush);
      }
    }

    REQUIRE(!firstCopy.empty());
    REQUIRE(!secondCopy.empty());
    CHECK(unitedBounds(firstCopy) == vm::bbox3d{{0, -16, -16}, {24, 16, 16}});
    CHECK(unitedBounds(secondCopy) == vm::bbox3d{{48, -16, -16}, {72, 16, 16}});
  }

  SECTION("cross-section scale tapers the sweep")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}, 0.0, 1.0},
      SplinePoint{vm::vec3d{128, 0, 0}, 0.0, 0.5},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    REQUIRE(!brushes.empty());

    // The frames sit at scales 1.0, 0.75 and 0.5, so the second cell's widest
    // cross-section is 0.75 * 16. The cell consists of the tetrahedra beyond x = 64.
    auto secondCell = std::vector<Brush>{};
    for (const auto& brush : brushes)
    {
      if (brush.bounds().min.x() >= 64.0)
      {
        secondCell.push_back(brush);
      }
    }
    REQUIRE(!secondCell.empty());
    const auto secondCellBounds = unitedBounds(secondCell);
    CHECK(secondCellBounds.max.y() == vm::approx{12.0});
    CHECK(secondCellBounds.min.y() == vm::approx{-12.0});

    const auto bounds = unitedBounds(brushes);
    CHECK(bounds.max.y() == vm::approx{16.0});
    CHECK(bounds.min.y() == vm::approx{-16.0});
  }

  SECTION("curved spline produces valid brushes along the curve")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
      SplinePoint{vm::vec3d{256, 128, 0}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    CHECK(brushes.size() > 1);

    for (const auto& brush : brushes)
    {
      CHECK(brush.fullySpecified());
      CHECK(worldBounds.contains(brush.bounds()));
    }
  }

  SECTION("face UV attributes carry over to the generated brushes")
  {
    // Note: offsets cannot be verified here because the test faces have no loaded
    // material, so the alignment lock wraps them modulo a 1x1 texture size.
    auto uvTemplate = makeCuboid(templateBounds, "some_material");
    for (auto& face : uvTemplate.faces())
    {
      auto attributes = face.uvAttributes();
      attributes.scale = vm::vec2f{2.0f, 2.0f};
      attributes.rotation = 30.0f;
      face.setUvAttributes(attributes);
    }
    const auto uvTemplates = std::vector<const Brush*>{&uvTemplate};

    // A straight sweep at the template's natural size deforms with the identity
    // transform, so the template's UV attributes must arrive unchanged on the
    // generated faces that correspond to template faces (the axis aligned ones; the
    // tetrahedra's interior faces are invisible).
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{128, 0, 0}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, uvTemplates, templateBounds)
      | kdl::value();

    REQUIRE(!brushes.empty());

    auto checkedFaces = 0;
    for (const auto& brush : brushes)
    {
      for (const auto& face : brush.faces())
      {
        const auto& normal = face.normal();
        const auto axisAligned = vm::abs(normal.x()) > 0.999
                                 || vm::abs(normal.y()) > 0.999
                                 || vm::abs(normal.z()) > 0.999;
        if (axisAligned)
        {
          CHECK(face.uvAttributes().scale == vm::vec2f{2.0f, 2.0f});
          CHECK(face.uvAttributes().rotation == 30.0f);
          CHECK(face.materialName() == "some_material");
          ++checkedFaces;
        }
      }
    }
    CHECK(checkedFaces > 0);
  }

  SECTION("a copy's texture sits where the template's does")
  {
    // The offsets are the point of this one, and an offset is kept modulo the texture,
    // so it only means anything on a face that knows how big its texture is.
    const auto mapFormat = GENERATE(MapFormat::Standard, MapFormat::Valve);
    CAPTURE(mapFormat);

    auto texture = gl::Texture{64, 64};
    auto material =
      gl::Material{"some_material", gl::createTextureResource(std::move(texture))};

    const auto uvBuilder = BrushBuilder{mapFormat, worldBounds};
    auto uvTemplate =
      uvBuilder.createCuboid(templateBounds, "some_material") | kdl::value();
    for (auto& face : uvTemplate.faces())
    {
      auto attributes = face.uvAttributes();
      attributes.scale = vm::vec2f{1.0f, 1.0f};
      attributes.offset = vm::vec2f{11.0f, 7.0f};
      face.setUvAttributes(attributes);
      face.setMaterial(&material);
    }
    const auto uvTemplates = std::vector<const Brush*>{&uvTemplate};

    // A long curving run, like a track.
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{1792, 288, 0}},
      SplinePoint{vm::vec3d{4096, 2304, 0}},
    };

    auto brushes = createSplineBrushes(
                     mapFormat,
                     worldBounds,
                     points,
                     uvTemplates,
                     templateBounds,
                     false,
                     SplineUVMode::Follow,
                     true)
                   | kdl::value();
    REQUIRE(!brushes.empty());
    for (auto& brush : brushes)
    {
      for (auto& face : brush.faces())
      {
        face.setMaterial(&material);
      }
    }

    /** A texture repeats, so a whole tile of difference is the same picture. */
    const auto wrapped = [](const float delta) {
      const auto fraction = double(delta) - std::floor(double(delta));
      return std::min(fraction, 1.0 - fraction);
    };

    // Walk the sweep the way the generator does, so every triangle it cut can be found
    // again by the corners it was cut from, and ask what UV it shows there.
    const auto forwardSize = templateBounds.size().x();
    const auto frames = buildSweepFrames(points, forwardSize, false, true);

    auto worst = 0.0;
    auto checked = 0;

    for (size_t i = 0; i + 1 < frames.size(); ++i)
    {
      const auto forward = vm::normalize(frames[i + 1].position - frames[i].position);
      const auto end = SweepFrame{
        frames[i].position + forward * forwardSize,
        frames[i].right,
        frames[i].up,
        frames[i].scale};

      for (const auto& templateFace : uvTemplate.faces())
      {
        const auto corners = templateFace.vertexPositions();
        auto deformed = std::vector<vm::vec3d>{};
        for (const auto& corner : corners)
        {
          deformed.push_back(
            vm::round(deformIntoSpan(corner, templateBounds, frames[i], end)));
        }

        for (size_t j = 1; j + 1 < deformed.size(); ++j)
        {
          const auto cut =
            std::vector<vm::vec3d>{deformed[0], deformed[j], deformed[j + 1]};
          const auto sources =
            std::vector<vm::vec3d>{corners[0], corners[j], corners[j + 1]};

          for (const auto& brush : brushes)
          {
            for (const auto& face : brush.faces())
            {
              const auto have = face.vertexPositions();
              if (have.size() != 3 || !std::ranges::all_of(cut, [&](const auto& wanted) {
                    return std::ranges::any_of(have, [&](const auto& vertex) {
                      return vm::squared_length(vertex - wanted) < 1.0e-6;
                    });
                  }))
              {
                continue;
              }

              for (size_t k = 0; k < 3; ++k)
              {
                const auto templateUv = templateFace.uvCoords(sources[k]);
                const auto uv = face.uvCoords(cut[k]);
                worst = std::max(
                  worst,
                  std::max(
                    wrapped(uv.x() - templateUv.x()), wrapped(uv.y() - templateUv.y())));
                ++checked;
              }
              goto nextTriangle;
            }
          }
        nextTriangle:;
        }
      }
    }

    REQUIRE(checked > 0);
    // A face that came out of the sweep knowing nothing about its texture has its offset
    // kept modulo a one by one texture, which throws the offset away and leaves the
    // picture up to half a tile out of place. Within a twentieth of a tile is the
    // picture sitting where the template has it.
    CHECK(worst < 0.05);
  }

  SECTION("Lock UVs keeps the picture from being squashed")
  {
    const auto mapFormat = GENERATE(MapFormat::Standard, MapFormat::Valve);
    CAPTURE(mapFormat);

    auto texture = gl::Texture{64, 64};
    auto material =
      gl::Material{"some_material", gl::createTextureResource(std::move(texture))};

    // A slanted wedge, like a billboard: its faces are not axis aligned, which is where
    // the sweep's squeezing shows up in the texture.
    const auto uvBuilder = BrushBuilder{mapFormat, worldBounds};
    auto uvTemplate = uvBuilder.createBrush(
                        std::vector<vm::vec3d>{
                          {0, -16, -16},
                          {64, -16, -16},
                          {0, 16, 16},
                          {64, 16, 16},
                          {0, -14, -16},
                          {64, -14, -16},
                          {0, 18, 16},
                          {64, 18, 16}},
                        "some_material")
                      | kdl::value();
    for (auto& face : uvTemplate.faces())
    {
      auto attributes = face.uvAttributes();
      attributes.scale = vm::vec2f{3.25f, 4.5f};
      face.setUvAttributes(attributes);
      face.setMaterial(&material);
    }
    const auto uvTemplates = std::vector<const Brush*>{&uvTemplate};

    /**
     * How much the face squashes the picture: the ratio of the two singular values of
     * the map it makes from world distance to texture distance. One means the picture
     * keeps its shape in every direction. Both singular values are independent of which
     * direction is measured, unlike the stretch along any one edge, which changes when
     * a face is merely turned.
     */
    const auto squash = [](const BrushFace& face) {
      const auto normal = face.normal();
      auto e1 = vm::cross(normal, vm::vec3d{0, 0, 1});
      if (vm::squared_length(e1) < 1.0e-6)
      {
        e1 = vm::cross(normal, vm::vec3d{0, 1, 0});
      }
      e1 = vm::normalize(e1);
      const auto e2 = vm::normalize(vm::cross(normal, e1));

      const auto origin = face.center();
      const auto uv = face.uvCoords(origin);
      const auto du = face.uvCoords(origin + e1) - uv;
      const auto dv = face.uvCoords(origin + e2) - uv;

      const auto a = double(du.x());
      const auto b = double(dv.x());
      const auto c = double(du.y());
      const auto d = double(dv.y());
      const auto mean = (a * a + b * b + c * c + d * d) / 2.0;
      const auto spread = std::sqrt(std::max(
        0.0,
        std::pow((a * a + b * b - c * c - d * d) / 2.0, 2.0)
          + std::pow(a * c + b * d, 2.0)));
      const auto larger = std::sqrt(std::max(0.0, mean + spread));
      const auto smaller = std::sqrt(std::max(0.0, mean - spread));
      return smaller > 0.0 ? larger / smaller : 0.0;
    };

    auto templateSquash = 0.0;
    for (const auto& face : uvTemplate.faces())
    {
      templateSquash = std::max(templateSquash, squash(face));
    }
    REQUIRE(templateSquash > 0.0);

    /** The worst a copy is squashed beyond how the template drew it. */
    const auto worstDistortion =
      [&](const std::vector<SplinePoint>& points, const SplineUVMode uvMode) {
        auto brushes =
          createSplineBrushes(
            mapFormat, worldBounds, points, uvTemplates, templateBounds, false, uvMode)
          | kdl::value();

        auto worst = 1.0;
        for (auto& brush : brushes)
        {
          for (auto& face : brush.faces())
          {
            face.setMaterial(&material);
            if (const auto faceSquash = squash(face); faceSquash > 0.0)
            {
              const auto relative = faceSquash / templateSquash;
              worst = std::max(worst, std::max(relative, 1.0 / relative));
            }
          }
        }
        return worst;
      };

    // A segment that is not a whole number of templates long, so the sweep squeezes
    // every copy to make it fit, and a long curving run like a track, where how much it
    // squeezes changes from copy to copy.
    const auto stretched = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{100, 0, 0}},
    };
    const auto curved = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}},
      SplinePoint{vm::vec3d{1792, 288, 0}},
      SplinePoint{vm::vec3d{4096, 2304, 0}},
    };

    for (const auto& points : {stretched, curved})
    {
      const auto locked = worstDistortion(points, SplineUVMode::Lock);
      const auto followed = worstDistortion(points, SplineUVMode::Follow);

      // Following lets the sweep's squeezing through into the picture, in both formats.
      CHECK(followed > 1.01);

      if (mapFormat == MapFormat::Valve)
      {
        // A format that stores its UV axes can hold the template's alignment exactly, so
        // the picture comes out as the template drew it however the copy was squeezed.
        CHECK(locked < 1.01);
      }
      else
      {
        // The paraxial format picks its UV axes from the face's own normal, so a turned
        // face cannot hold an undistorted picture whatever it is told to do. Locking
        // takes most of the squeezing out of a long curve there, but can leave more of
        // it in than following does on a short one, so there is no promise to make
        // beyond its staying in the same range.
        CHECK(locked < 2.0);
      }
    }
  }

  SECTION("vertices are snapped to integer coordinates")
  {
    const auto points = std::vector<SplinePoint>{
      SplinePoint{vm::vec3d{0, 0, 0}, 30.0},
      SplinePoint{vm::vec3d{100, 30, 10}, 45.0},
      SplinePoint{vm::vec3d{250, 130, 50}},
    };

    const auto brushes =
      createSplineBrushes(
        MapFormat::Standard, worldBounds, points, templateBrushes, templateBounds)
      | kdl::value();

    CHECK(!brushes.empty());

    for (const auto& brush : brushes)
    {
      for (const auto& vertex : brush.vertexPositions())
      {
        CHECK(vertex == vm::round(vertex));
      }
    }
  }
}

} // namespace tb::mdl
