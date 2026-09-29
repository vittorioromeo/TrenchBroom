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

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
struct Terrain;

/** The width of one sample of a raw height map. */
enum class RawSampleFormat
{
  /** 8 bit, 0 to 255. */
  Int8,
  /** Little endian 16 bit, 0 to 65535. */
  Int16,
  /** Little endian 32 bit IEEE 754 floats, with no defined range. */
  Float32,
};

std::string describeRawSampleFormat(RawSampleFormat format);

/**
 * A square height map read from a headerless .raw file.
 *
 * A .raw file is nothing but its samples, so its dimensions have to be deduced from its
 * length. Only square height maps can be deduced, which is what the tools that write
 * them produce.
 */
struct RawHeightmap
{
  /** The number of samples along each side. */
  size_t size = 0;
  RawSampleFormat format = RawSampleFormat::Int8;
  /** One sample per point, normalized to [0, 1], row by row. */
  std::vector<double> samples;
};

/** The sample format the given file extension names (.r8, .r16, .r32, .f32, .flt), or
 * nothing for an extension that does not say, such as .raw. */
std::optional<RawSampleFormat> rawSampleFormatForExtension(
  const std::filesystem::path& path);

/**
 * Reads a headerless .raw height map, as the given format or, when none is given, as the
 * format deduced from the file's length and contents.
 *
 * The length alone settles 16 bit samples, whose length is twice a perfect square and so
 * can be nothing else. It cannot tell 8 bit samples from 32 bit ones, though: a square
 * of 32 bit samples is four times a perfect square, which is always a perfect square
 * too, so an 8 bit height map of twice the side has exactly the same length. Those two
 * are told apart by whether the bytes read as plausible floats, which real 8 bit height
 * map data does not.
 *
 * Returns an error if the length fits no format, or fits the requested one badly, which
 * is what a non-square height map or a file that is not a raw height map looks like.
 */
Result<RawHeightmap> parseRawHeightmap(
  std::string_view data, std::optional<RawSampleFormat> format = std::nullopt);

/**
 * Replaces the terrain's heights with the given height map.
 *
 * The height map is resampled bilinearly onto the terrain's grid, so its resolution does
 * not have to match, and its samples are spread over the terrain's current vertical
 * extent, leaving the terrain the size it already is. Use the Scale mode to make it
 * taller or shorter afterwards.
 *
 * Returns whether the terrain and the height map were usable.
 */
bool applyRawHeightmap(Terrain& terrain, const RawHeightmap& heightmap);

} // namespace tb::mdl
