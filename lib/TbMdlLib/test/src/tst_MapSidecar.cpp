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

#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/LayerNode.h"
#include "mdl/MapFormat.h"
#include "mdl/MapSidecar.h"
#include "mdl/SplineEntity.h"
#include "mdl/Terrain.h"
#include "mdl/TerrainEntity.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

Terrain makeTerrain(const double firstHeight = 32.0)
{
  auto terrain = *createTerrain(
    vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{128, 128, 32}}, 32.0, "some_material");
  terrain.heights[0] = firstHeight;
  return terrain;
}

/** Strips an entity's tool data, which is what a map whose sidecar is missing holds. */
void stripToolData(EntityNode& entityNode)
{
  auto entity = entityNode.entity();
  for (const auto& property : entityNode.entity().properties())
  {
    if (isSidecarPropertyKey(property.key()))
    {
      entity.removeProperty(property.key());
    }
  }
  entityNode.setEntity(std::move(entity));
}

} // namespace

TEST_CASE("MapSidecar")
{
  SECTION("isSidecarPropertyKey")
  {
    CHECK(isSidecarPropertyKey(TerrainPropertyKeys::Columns));
    CHECK(isSidecarPropertyKey(std::string{TerrainPropertyKeys::HeightsPrefix} + "3"));
    CHECK(isSidecarPropertyKey(std::string{SplinePropertyKeys::PointPrefix} + "0"));
    CHECK(isSidecarPropertyKey(SplinePropertyKeys::Closed));

    // The classname, the origin and the id must stay in the map: without them the
    // entity could not be found again, and the geometry would move.
    CHECK(!isSidecarPropertyKey(EntityPropertyKeys::Classname));
    CHECK(!isSidecarPropertyKey("origin"));
    CHECK(!isSidecarPropertyKey(SidecarPropertyKeys::DataId));
  }

  SECTION("sidecarPathForMap keeps the map's extension")
  {
    // Appended rather than replacing ".map", so a map and a compiled bsp of the same
    // name cannot collide.
    CHECK(sidecarPathForMap("/maps/level.map") == "/maps/level.map.tbtools");
  }

  SECTION("writeTerrainEntity gives the entity an id and keeps it")
  {
    const auto terrain = makeTerrain();

    const auto entity = writeTerrainEntity(Entity{}, terrain);
    const auto* id = entity.property(SidecarPropertyKeys::DataId);
    REQUIRE(id != nullptr);
    CHECK(!id->empty());

    // Editing the terrain must not break the link to its record.
    const auto edited = writeTerrainEntity(entity, terrain);
    CHECK(*edited.property(SidecarPropertyKeys::DataId) == *id);
  }

  SECTION("a terrain entity stripped of its tool data is not a terrain any more")
  {
    auto entity = writeTerrainEntity(Entity{}, makeTerrain());
    REQUIRE(isTerrainEntity(entity));

    // This is what a map whose sidecar has gone missing looks like: the classname and
    // the id remain, so the generated brushes stay as ordinary geometry.
    const auto properties = entity.properties();
    for (const auto& property : properties)
    {
      if (isSidecarPropertyKey(property.key()))
      {
        entity.removeProperty(property.key());
      }
    }

    CHECK(!isTerrainEntity(entity));
    CHECK(parseTerrainEntity(entity) == std::nullopt);
    CHECK(entity.property(SidecarPropertyKeys::DataId) != nullptr);
  }

  SECTION("serializing and parsing round trips the records")
  {
    const auto records = std::vector<SidecarRecord>{
      SidecarRecord{
        "00000000deadbeef",
        {
          EntityProperty{TerrainPropertyKeys::Columns, "4"},
          EntityProperty{TerrainPropertyKeys::DefaultMaterial, "some material"},
        }},
      SidecarRecord{
        "0123456789abcdef",
        {
          EntityProperty{std::string{SplinePropertyKeys::PointPrefix} + "0", "0 0 0"},
        }},
    };

    const auto parsed = parseSidecar(serializeSidecar(records));
    REQUIRE(parsed.is_success());
    CHECK(parsed.value() == records);
  }

  SECTION("values with quotes, backslashes and braces survive the round trip")
  {
    const auto records = std::vector<SidecarRecord>{
      SidecarRecord{
        "1",
        {
          EntityProperty{TerrainPropertyKeys::DefaultMaterial, R"(a "quoted" \ name)"},
          EntityProperty{TerrainPropertyKeys::Rows, "{ } // not a comment"},
        }},
    };

    const auto parsed = parseSidecar(serializeSidecar(records));
    REQUIRE(parsed.is_success());
    CHECK(parsed.value() == records);
  }

  SECTION("an empty sidecar parses to no records")
  {
    const auto parsed = parseSidecar(serializeSidecar({}));
    REQUIRE(parsed.is_success());
    CHECK(parsed.value().empty());
  }

  SECTION("a map written before the sidecar existed keeps its tool data")
  {
    auto worldNode = WorldNode{{}, {}, MapFormat::Standard};

    // Such a map carries the data inline and has no id, so without the repair below the
    // record would not be collected and the data would be dropped on the next save.
    auto entity = writeTerrainEntity(Entity{}, makeTerrain());
    entity.removeProperty(SidecarPropertyKeys::DataId);
    auto* entityNode = new EntityNode{std::move(entity)};
    worldNode.defaultLayer()->addChild(entityNode);

    REQUIRE(collectSidecarRecords(worldNode).empty());

    CHECK(assignSidecarIds(worldNode));
    CHECK(collectSidecarRecords(worldNode).size() == 1u);
    // Nothing left to repair on a second pass.
    CHECK(!assignSidecarIds(worldNode));
  }

  SECTION("copied terrains are given ids of their own")
  {
    auto worldNode = WorldNode{{}, {}, MapFormat::Standard};

    // Copying an entity copies its id, so two entities would share one record and the
    // second one's data would be replaced by the first one's on the next load.
    const auto original = writeTerrainEntity(Entity{}, makeTerrain(40.0));
    const auto copy = writeTerrainEntity(original, makeTerrain(80.0));
    REQUIRE(
      *original.property(SidecarPropertyKeys::DataId)
      == *copy.property(SidecarPropertyKeys::DataId));

    auto* originalNode = new EntityNode{original};
    auto* copyNode = new EntityNode{copy};
    worldNode.defaultLayer()->addChild(originalNode);
    worldNode.defaultLayer()->addChild(copyNode);

    REQUIRE(assignSidecarIds(worldNode));
    CHECK(
      *originalNode->entity().property(SidecarPropertyKeys::DataId)
      != *copyNode->entity().property(SidecarPropertyKeys::DataId));

    const auto records = collectSidecarRecords(worldNode);
    REQUIRE(records.size() == 2u);

    stripToolData(*originalNode);
    stripToolData(*copyNode);
    applySidecarRecords(worldNode, records);

    const auto restoredOriginal = parseTerrainEntity(originalNode->entity());
    const auto restoredCopy = parseTerrainEntity(copyNode->entity());
    REQUIRE(restoredOriginal.has_value());
    REQUIRE(restoredCopy.has_value());
    CHECK(restoredOriginal->heights[0] == 40.0);
    CHECK(restoredCopy->heights[0] == 80.0);
  }

  SECTION("an entity whose record is missing keeps its brushes and its id")
  {
    auto worldNode = WorldNode{{}, {}, MapFormat::Standard};
    auto* entityNode = new EntityNode{writeTerrainEntity(Entity{}, makeTerrain())};
    worldNode.defaultLayer()->addChild(entityNode);

    const auto id = *entityNode->entity().property(SidecarPropertyKeys::DataId);
    stripToolData(*entityNode);

    applySidecarRecords(worldNode, {});

    CHECK(!isTerrainEntity(entityNode->entity()));
    CHECK(*entityNode->entity().property(SidecarPropertyKeys::DataId) == id);
  }

  SECTION("malformed sidecars are rejected rather than silently ignored")
  {
    CHECK(parseSidecar("{\n").is_error());
    CHECK(parseSidecar("}\n").is_error());
    CHECK(parseSidecar("\"_terrain_rows\" \"4\"\n").is_error());
    // A record without an id cannot be matched to an entity.
    CHECK(parseSidecar("{\n\"_terrain_rows\" \"4\"\n}\n").is_error());
    CHECK(parseSidecar("{\n\"unterminated\n}\n").is_error());
  }
}

} // namespace tb::mdl
