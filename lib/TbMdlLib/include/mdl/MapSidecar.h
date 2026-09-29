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
#include "mdl/EntityProperties.h"

#include "kd/reflection_decl.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Entity;
class WorldNode;

/**
 * The tool data of the spline and terrain tools is kept out of the map file and written
 * to a sidecar file next to it instead.
 *
 * A height field or a spline's template brushes make for entity property values tens of
 * kilobytes long, which is far past the token length a map compiler will accept, and
 * they are of no use to anything but the editor. Both tools generate ordinary brushes
 * for their geometry, so a map without its sidecar still compiles and still shows the
 * right geometry; it just cannot be edited with the tools any more.
 *
 * The entities keep a short id property that ties them to their record in the sidecar.
 */
namespace SidecarPropertyKeys
{
/** Identifies an entity's record in the sidecar file. Under the _tb_ prefix so that it
 * is stripped from exports along with the editor's other bookkeeping. */
inline const std::string DataId = EntityPropertyKeys::TbPrefix + "tool_data";
} // namespace SidecarPropertyKeys

/**
 * Quotes a key or value the way the map format does, so that one containing spaces
 * survives being packed into a larger string.
 */
std::string quoteSidecarString(const std::string& str);

/**
 * Reads a string quoted by quoteSidecarString, starting at the given position, which
 * must be the opening quote, and moves the position past the closing quote. Returns
 * nullopt if the string is not terminated.
 */
std::optional<std::string> unquoteSidecarString(const std::string& str, size_t& position);

/** The property key prefixes whose values live in the sidecar file. */
const std::vector<std::string>& sidecarPropertyPrefixes();

/** Whether the given property key holds tool data that belongs in the sidecar. */
bool isSidecarPropertyKey(const std::string& key);

/** The path of the sidecar file belonging to the map at the given path. */
std::filesystem::path sidecarPathForMap(const std::filesystem::path& mapPath);

/** The tool data of one entity. */
struct SidecarRecord
{
  std::string id;
  std::vector<EntityProperty> properties;

  kdl_reflect_decl(SidecarRecord, id, properties);
};

/** A fresh id for an entity that does not have one yet. */
std::string generateSidecarId();

/**
 * Makes sure every entity carrying tool data has an id of its own.
 *
 * An entity read from a map written before the sidecar existed carries its data inline
 * and has no id at all, and copying an entity copies its id, so two entities can end up
 * sharing one. Either way the entity would lose its data on the next save, so both are
 * repaired here: the first entity keeps a shared id and the others are given fresh ones.
 *
 * Returns whether anything was changed.
 */
bool assignSidecarIds(WorldNode& worldNode);

/**
 * The sidecar records of every entity in the given world that carries tool data. An
 * entity without an id, or without any tool data, contributes no record, so call
 * assignSidecarIds first.
 */
std::vector<SidecarRecord> collectSidecarRecords(const WorldNode& worldNode);

/** Adds the tool data of the matching record to the entity, if there is one. */
void applySidecarRecords(WorldNode& worldNode, const std::vector<SidecarRecord>& records);

std::string serializeSidecar(const std::vector<SidecarRecord>& records);
Result<std::vector<SidecarRecord>> parseSidecar(const std::string& str);

/**
 * Writes the given records to the sidecar file at the given path, or deletes the file
 * if there are none, so that a map that no longer has any tool data does not leave a
 * stale sidecar behind.
 */
Result<void> writeSidecarFile(
  const std::filesystem::path& path, const std::vector<SidecarRecord>& records);

/**
 * Reads the sidecar file at the given path. A missing file is not an error and yields no
 * records: the map then simply keeps the generated brushes as ordinary geometry.
 */
Result<std::vector<SidecarRecord>> readSidecarFile(const std::filesystem::path& path);

} // namespace tb::mdl
