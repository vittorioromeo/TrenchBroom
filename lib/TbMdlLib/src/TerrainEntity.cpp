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

#include "mdl/TerrainEntity.h"

#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/MapSidecar.h"

#include "kd/string_utils.h"

#include "vm/vec.h"
#include "vm/vec_io.h"

#include <fmt/format.h>

#include <sstream>
#include <string>

namespace tb::mdl
{
namespace
{

std::string chunkKey(const std::string& prefix, const size_t index)
{
  return fmt::format("{}{}", prefix, index);
}

/** The number of chunks needed to store the given number of values. */
size_t chunkCount(const size_t valueCount)
{
  return (valueCount + TerrainValuesPerChunk - 1) / TerrainValuesPerChunk;
}

void removeChunks(Entity& entity, const std::string& prefix)
{
  const auto properties = entity.properties();
  for (const auto& property : properties)
  {
    if (property.hasPrefix(prefix))
    {
      entity.removeProperty(property.key());
    }
  }
}

std::vector<double> parseHeights(const Entity& entity, const size_t expectedCount)
{
  auto heights = std::vector<double>{};
  heights.reserve(expectedCount);

  for (size_t chunk = 0;; ++chunk)
  {
    const auto* value =
      entity.property(chunkKey(TerrainPropertyKeys::HeightsPrefix, chunk));
    if (!value)
    {
      break;
    }

    auto stream = std::istringstream{*value};
    auto height = 0.0;
    while (stream >> height)
    {
      heights.push_back(height);
    }
  }

  return heights;
}

std::vector<std::string> parseMaterials(const Entity& entity, const size_t expectedCount)
{
  auto materials = std::vector<std::string>{};
  materials.reserve(expectedCount);

  for (size_t chunk = 0;; ++chunk)
  {
    const auto* value =
      entity.property(chunkKey(TerrainPropertyKeys::MaterialsPrefix, chunk));
    if (!value)
    {
      break;
    }

    // The chunk holds one name per semicolon separated field; a trailing semicolon
    // terminates the last field, so empty names round trip.
    auto field = std::string{};
    for (const auto c : *value)
    {
      if (c == ';')
      {
        materials.push_back(field);
        field.clear();
      }
      else
      {
        field.push_back(c);
      }
    }
  }

  return materials;
}

void writeHeights(Entity& entity, const std::vector<double>& heights)
{
  for (size_t chunk = 0; chunk < chunkCount(heights.size()); ++chunk)
  {
    const auto begin = chunk * TerrainValuesPerChunk;
    const auto end = vm::min(begin + TerrainValuesPerChunk, heights.size());

    auto value = std::string{};
    for (size_t i = begin; i < end; ++i)
    {
      if (i > begin)
      {
        value += ' ';
      }
      value += fmt::format("{:g}", heights[i]);
    }

    entity.addOrUpdateProperty(
      chunkKey(TerrainPropertyKeys::HeightsPrefix, chunk), std::move(value));
  }
}

void writeMaterials(Entity& entity, const std::vector<std::string>& materials)
{
  for (size_t chunk = 0; chunk < chunkCount(materials.size()); ++chunk)
  {
    const auto begin = chunk * TerrainValuesPerChunk;
    const auto end = vm::min(begin + TerrainValuesPerChunk, materials.size());

    auto value = std::string{};
    for (size_t i = begin; i < end; ++i)
    {
      value += materials[i];
      value += ';';
    }

    entity.addOrUpdateProperty(
      chunkKey(TerrainPropertyKeys::MaterialsPrefix, chunk), std::move(value));
  }
}

} // namespace

bool isTerrainEntity(const Entity& entity)
{
  return entity.property(TerrainPropertyKeys::Columns) != nullptr;
}

std::optional<Terrain> parseTerrainEntity(const Entity& entity)
{
  if (!isTerrainEntity(entity))
  {
    return std::nullopt;
  }

  auto terrain = Terrain{};

  if (const auto* origin = entity.property(TerrainPropertyKeys::Origin))
  {
    if (const auto parsed = vm::parse<double, 3>(*origin))
    {
      terrain.origin = *parsed;
    }
  }

  if (const auto* cellSize = entity.property(TerrainPropertyKeys::CellSize))
  {
    // Two numbers, or one for a terrain written before cells could be stretched, whose
    // cells were square.
    if (const auto parsed = vm::parse<double, 2>(*cellSize);
        parsed && parsed->x() > 0.0 && parsed->y() > 0.0)
    {
      terrain.cellSizeX = parsed->x();
      terrain.cellSizeY = parsed->y();
    }
    else if (const auto square = kdl::str_to_double(*cellSize); square && *square > 0.0)
    {
      terrain.cellSizeX = *square;
      terrain.cellSizeY = *square;
    }
  }

  if (const auto* columns = entity.property(TerrainPropertyKeys::Columns))
  {
    if (const auto parsed = kdl::str_to_size(*columns))
    {
      terrain.columns = *parsed;
    }
  }

  if (const auto* rows = entity.property(TerrainPropertyKeys::Rows))
  {
    if (const auto parsed = kdl::str_to_size(*rows))
    {
      terrain.rows = *parsed;
    }
  }

  if (const auto* texScale = entity.property(TerrainPropertyKeys::TexScale))
  {
    if (const auto parsed = vm::parse<float, 2>(*texScale))
    {
      terrain.texScaleX = (*parsed)[0];
      terrain.texScaleY = (*parsed)[1];
    }
  }

  if (const auto* material = entity.property(TerrainPropertyKeys::DefaultMaterial))
  {
    terrain.defaultMaterial = *material;
  }

  terrain.heights = parseHeights(entity, terrainVertexCount(terrain));
  terrain.materials = parseMaterials(entity, terrainCellCount(terrain));

  // Tolerate a missing or truncated material list, which costs nothing but the cells'
  // materials, but reject inconsistent geometry.
  terrain.materials.resize(terrainCellCount(terrain));

  return isValidTerrain(terrain) ? std::optional{std::move(terrain)} : std::nullopt;
}

Entity writeTerrainEntity(const Entity& entity, const Terrain& terrain)
{
  auto result = entity;

  removeChunks(result, TerrainPropertyKeys::HeightsPrefix);
  removeChunks(result, TerrainPropertyKeys::MaterialsPrefix);

  result.addOrUpdateProperty(EntityPropertyKeys::Classname, TerrainEntityClassname);

  // The bulk of this data is written to the map's sidecar file rather than the map
  // itself, so the entity carries an id tying it to its record there. Existing ids are
  // kept so that the link survives an edit.
  if (const auto* id = result.property(SidecarPropertyKeys::DataId); !id || id->empty())
  {
    result.addOrUpdateProperty(SidecarPropertyKeys::DataId, generateSidecarId());
  }

  const auto origin = fmt::format(
    "{:g} {:g} {:g}", terrain.origin.x(), terrain.origin.y(), terrain.origin.z());
  result.addOrUpdateProperty(TerrainPropertyKeys::Origin, origin);

  // The ordinary origin as well, so that the entity has a sensible position of its own:
  // while it has no brushes yet, and wherever the editor asks an entity where it is.
  result.addOrUpdateProperty(EntityPropertyKeys::Origin, origin);
  result.addOrUpdateProperty(
    TerrainPropertyKeys::CellSize,
    fmt::format("{:g} {:g}", terrain.cellSizeX, terrain.cellSizeY));
  result.addOrUpdateProperty(
    TerrainPropertyKeys::Columns, kdl::str_to_string(terrain.columns));
  result.addOrUpdateProperty(TerrainPropertyKeys::Rows, kdl::str_to_string(terrain.rows));
  result.addOrUpdateProperty(
    TerrainPropertyKeys::TexScale,
    fmt::format("{:g} {:g}", terrain.texScaleX, terrain.texScaleY));
  result.addOrUpdateProperty(
    TerrainPropertyKeys::DefaultMaterial, terrain.defaultMaterial);

  writeHeights(result, terrain.heights);
  writeMaterials(result, terrain.materials);

  return result;
}

} // namespace tb::mdl
