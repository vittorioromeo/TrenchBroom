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

#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/MapFormat.h"
#include "mdl/SplineEntities.h"
#include "mdl/SplineEntity.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("SplineEntity")
{
  const auto data = SplineEntityData{
    {
      SplinePoint{vm::vec3d{0, 0, 0}, 0.0, 1.0, SplineLock::None},
      SplinePoint{vm::vec3d{64, 32, 16}, 45.0, 2.5, SplineLock::Twist},
      SplinePoint{
        vm::vec3d{128, 0, 0},
        -90.0,
        0.5,
        SplineLock::None,
        false,
        vm::vec3d{-16, -8, 4},
        vm::vec3d{24, 12, -6}},
    },
    12,
    42,
  };

  SECTION("isSplineEntity")
  {
    CHECK_FALSE(isSplineEntity(Entity{}));
    CHECK(isSplineEntity(writeSplineEntity(Entity{}, data)));
  }

  SECTION("writeSplineEntity and parseSplineEntity round-trip")
  {
    const auto entity = writeSplineEntity(Entity{}, data);
    CHECK(entity.classname() == SplineEntityClassname);

    const auto parsed = parseSplineEntity(entity);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == data);
  }

  SECTION("parseSplineEntity returns nullopt for non-spline entities")
  {
    CHECK(parseSplineEntity(Entity{}) == std::nullopt);
  }

  SECTION("the closed flag round-trips")
  {
    auto closedData = data;
    closedData.closed = true;

    const auto entity = writeSplineEntity(Entity{}, closedData);
    CHECK(entity.property(SplinePropertyKeys::Closed) != nullptr);

    const auto parsed = parseSplineEntity(entity);
    REQUIRE(parsed.has_value());
    CHECK(parsed->closed);

    // Reopening the spline removes the property again.
    const auto reopened = writeSplineEntity(entity, data);
    CHECK(reopened.property(SplinePropertyKeys::Closed) == nullptr);
    CHECK(!parseSplineEntity(reopened)->closed);
  }

  SECTION("writeSplineEntity removes stale point properties")
  {
    const auto entity = writeSplineEntity(Entity{}, data);

    auto shorterData = data;
    shorterData.points.pop_back();
    shorterData.templateGroupId = std::nullopt;

    const auto updatedEntity = writeSplineEntity(entity, shorterData);
    CHECK(updatedEntity.property("_spline_point_2") == nullptr);
    CHECK(updatedEntity.property("_spline_template_group") == nullptr);

    const auto parsed = parseSplineEntity(updatedEntity);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == shorterData);
  }

  SECTION("writeSplineEntity preserves unrelated properties")
  {
    auto entity = Entity{};
    entity.addOrUpdateProperty("angle", "45");

    const auto splineEntity = writeSplineEntity(entity, data);
    REQUIRE(splineEntity.property("angle") != nullptr);
    CHECK(*splineEntity.property("angle") == "45");
  }

  SECTION("template brush snapshot round-trip")
  {
    const auto worldBounds = vm::bbox3d{8192.0};
    const auto builder = BrushBuilder{MapFormat::Standard, worldBounds};
    const auto brush =
      builder.createCuboid(vm::bbox3d{{0, -16, -16}, {64, 16, 16}}, "some_material")
      | kdl::value();

    const auto entity = writeSplineTemplateBrushes(Entity{}, {brush});
    CHECK(entity.property("_spline_template_brush_0") != nullptr);

    const auto parsed =
      parseSplineTemplateBrushes(entity, MapFormat::Standard, worldBounds);
    REQUIRE(parsed.size() == 1);

    CHECK(parsed.front().bounds().min == vm::approx{vm::vec3d{0, -16, -16}});
    CHECK(parsed.front().bounds().max == vm::approx{vm::vec3d{64, 16, 16}});
    for (const auto& face : parsed.front().faces())
    {
      CHECK(face.materialName() == "some_material");
    }

    SECTION("an empty snapshot removes stored brushes")
    {
      const auto clearedEntity = writeSplineTemplateBrushes(entity, {});
      CHECK(clearedEntity.property("_spline_template_brush_0") == nullptr);
      CHECK(parseSplineTemplateBrushes(clearedEntity, MapFormat::Standard, worldBounds)
              .empty());
    }
  }

  SECTION("template entities round trip")
  {
    auto templateEntity = Entity{{
      {EntityPropertyKeys::Classname, "light"},
      {EntityPropertyKeys::Origin, "32 0 24"},
      {"light", "200"},
      // A value with spaces and one with a quote in it, which is what the packing has
      // to survive.
      {"message", "a room with a view"},
      {"_note", "the \"good\" one"},
    }};
    templateEntity.setPointEntity(true);

    const auto templateEntities = std::vector<SplineTemplateEntity>{
      SplineTemplateEntity{
        std::move(templateEntity), vm::bbox3d{{24, -8, 16}, {40, 8, 32}}},
    };

    const auto entity = writeSplineTemplateEntities(Entity{}, templateEntities);
    CHECK(entity.property("_spline_template_entity_0") != nullptr);

    const auto parsed = parseSplineTemplateEntities(entity);
    REQUIRE(parsed.size() == 1);

    CHECK(parsed.front().bounds == vm::bbox3d{{24, -8, 16}, {40, 8, 32}});
    CHECK(parsed.front().entity.classname() == "light");
    CHECK(parsed.front().entity.origin() == vm::approx{vm::vec3d{32, 0, 24}});
    CHECK(*parsed.front().entity.property("light") == "200");
    CHECK(*parsed.front().entity.property("message") == "a room with a view");
    CHECK(*parsed.front().entity.property("_note") == "the \"good\" one");

    SECTION("an empty snapshot removes stored entities")
    {
      const auto clearedEntity = writeSplineTemplateEntities(entity, {});
      CHECK(clearedEntity.property("_spline_template_entity_0") == nullptr);
      CHECK(parseSplineTemplateEntities(clearedEntity).empty());
    }
  }

  SECTION("template brush entity snapshot round-trip")
  {
    const auto worldBounds = vm::bbox3d{8192.0};
    const auto builder = BrushBuilder{MapFormat::Standard, worldBounds};
    const auto makeBrush = [&](const vm::bbox3d& bounds) {
      return builder.createCuboid(bounds, "some_material") | kdl::value();
    };

    auto templateEntity = Entity{};
    templateEntity.addOrUpdateProperty("classname", "func_detail");
    templateEntity.addOrUpdateProperty("_phong", "1");

    const auto brushEntities = std::vector<SplineTemplateBrushEntity>{
      SplineTemplateBrushEntity{
        std::move(templateEntity),
        {makeBrush(vm::bbox3d{{0, -16, -16}, {32, 16, 16}}),
         makeBrush(vm::bbox3d{{32, -16, -16}, {64, 16, 16}})}},
    };

    const auto entity = writeSplineTemplateBrushEntities(Entity{}, brushEntities);
    CHECK(entity.property("_spline_template_solid_0") != nullptr);
    CHECK(entity.property("_spline_template_solid_0_brush_0") != nullptr);
    CHECK(entity.property("_spline_template_solid_0_brush_1") != nullptr);

    const auto parsed =
      parseSplineTemplateBrushEntities(entity, MapFormat::Standard, worldBounds);
    REQUIRE(parsed.size() == 1);

    CHECK(parsed.front().entity.classname() == "func_detail");
    CHECK(*parsed.front().entity.property("_phong") == "1");

    REQUIRE(parsed.front().brushes.size() == 2);
    CHECK(parsed.front().brushes[0].bounds().max.x() == vm::approx{32.0});
    CHECK(parsed.front().brushes[1].bounds().min.x() == vm::approx{32.0});

    SECTION("the brush snapshot is written under its own prefix")
    {
      // "_spline_template_brush_" is a prefix of the key these would have had, so
      // rewriting the plain brush snapshot would have taken them with it.
      const auto both = writeSplineTemplateBrushes(
        entity, {makeBrush(vm::bbox3d{{0, 0, 0}, {16, 16, 16}})});

      CHECK(both.property("_spline_template_brush_0") != nullptr);
      CHECK(
        parseSplineTemplateBrushEntities(both, MapFormat::Standard, worldBounds).size()
        == 1);
    }

    SECTION("an empty snapshot removes the entities and their brushes")
    {
      const auto clearedEntity = writeSplineTemplateBrushEntities(entity, {});
      CHECK(clearedEntity.property("_spline_template_solid_0") == nullptr);
      CHECK(clearedEntity.property("_spline_template_solid_0_brush_0") == nullptr);
      CHECK(
        parseSplineTemplateBrushEntities(clearedEntity, MapFormat::Standard, worldBounds)
          .empty());
    }

    SECTION("an entity whose brushes are all gone contributes nothing")
    {
      auto withoutBrushes = entity;
      withoutBrushes.removeProperty("_spline_template_solid_0_brush_0");
      withoutBrushes.removeProperty("_spline_template_solid_0_brush_1");

      CHECK(
        parseSplineTemplateBrushEntities(withoutBrushes, MapFormat::Standard, worldBounds)
          .empty());
    }
  }

  SECTION("splineEntityId")
  {
    // A spline with no data has nothing to tie generated entities to.
    CHECK(splineEntityId(Entity{}).empty());

    // Writing a spline gives it one, and rewriting it keeps the same one, so the
    // entities it generated are still its own after an edit.
    const auto written = writeSplineEntity(Entity{}, data);
    const auto id = splineEntityId(written);
    CHECK_FALSE(id.empty());
    CHECK(splineEntityId(writeSplineEntity(written, data)) == id);
  }
}

} // namespace tb::mdl
