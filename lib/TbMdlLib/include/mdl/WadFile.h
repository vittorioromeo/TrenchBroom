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

#include "kd/reflection_decl.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tb::mdl
{

/** The lump type of a mip texture, which is what a texture wad is made of. */
constexpr auto WadMipTextureType = char('D');

/**
 * One entry of a wad file, kept as the bytes it is stored as.
 *
 * Reading and writing lumps rather than textures is what lets an existing wad be added
 * to: everything already in it is carried across untouched, whether or not this editor
 * knows how to read it.
 */
struct WadLump
{
  std::string name;
  char type = WadMipTextureType;
  std::vector<uint8_t> data;

  kdl_reflect_decl(WadLump, name, type, data);
};

/**
 * The lumps of the wad file at the given path, in the order the directory lists them.
 *
 * Returns an error if the file cannot be read or is not a wad; a path that does not
 * exist is an error too, so callers that mean "add to it if it is there" check first.
 */
Result<std::vector<WadLump>> readWadLumps(const std::filesystem::path& path);

/**
 * Writes the given lumps to a wad2 file at the given path, replacing whatever was there.
 *
 * Lump names are truncated to the 15 characters the format stores, and a name that
 * repeats replaces the earlier lump rather than being written twice, since a wad with
 * two lumps of one name is ambiguous to everything that reads it.
 */
Result<void> writeWad(
  const std::filesystem::path& path, const std::vector<WadLump>& lumps);

} // namespace tb::mdl
