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
#include "mdl/WadFile.h"

#include "kd/reflection_decl.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tb
{
namespace gl
{
class Texture;
}

namespace mdl
{
class Palette;

/**
 * What the material browser groups the map's own textures under, to set them apart from
 * the wads it loaded.
 */
inline const auto CustomTextureCollectionName = std::filesystem::path{"map textures"};

/**
 * The worldspawn property naming the wad the map's own textures are written to.
 *
 * It is what says whether the map already has a wad of its own: without it, saving a map
 * that carries textures asks where to put one. Under the _tb_ prefix, so a compiler
 * never sees it, but kept in the map file, which is what lets a reopened map write its
 * next texture to the same wad.
 */
constexpr auto CustomTextureWadPropertyKey = "_tb_texture_wad";

/** A mip texture carries four levels, each half the size of the one before. */
constexpr auto CustomTextureMipLevels = size_t(4);

/**
 * The index the standard palette reserves for transparency. A texture whose name begins
 * with "{" is drawn with this index left out; in every other texture it is an ordinary
 * colour, which is why an opaque pixel is never quantized onto it.
 */
constexpr auto PaletteTransparentIndex = uint8_t(255);

/**
 * A texture the map carries itself, brought in from an image file and not yet part of
 * any wad.
 *
 * It is held as palette indices rather than as pixels because that is what a wad
 * stores: quantizing once, when the image arrives, means what the editor draws is what
 * the compiler will see, instead of the two being quantized separately and disagreeing.
 */
struct CustomTexture
{
  /** The name as it goes into the wad, and as faces refer to it. */
  std::string name;
  size_t width = 0;
  size_t height = 0;
  /** Whether the image had fully transparent pixels, which the name records with a
   * leading "{". */
  bool masked = false;
  /** CustomTextureMipLevels levels of palette indices, the first at full size. */
  std::vector<std::vector<uint8_t>> mipLevels;

  kdl_reflect_decl(CustomTexture, name, width, height, masked, mipLevels);
};

/**
 * A texture name a wad can store, made from the given one.
 *
 * Quake keeps fifteen characters of a name and compares them without case, and the
 * characters that carry meaning to a compiler are "*" for a liquid, "+" for an
 * animation frame and "{" for a masked texture. The name is lowercased, anything else
 * unusual in it becomes an underscore, and it is cut to fit. A masked texture gets the
 * leading "{" that is what makes an engine treat its transparent index as transparent,
 * and the cut leaves room for it.
 */
std::string makeCustomTextureName(std::string_view name, bool masked);

/**
 * Quantizes the given RGBA image onto the palette.
 *
 * A fully transparent pixel becomes the palette's transparent index and makes the
 * texture masked; every other pixel becomes the nearest of the palette's other colours,
 * so an opaque pixel can never land on the transparent index by accident. The mip
 * levels are averaged from the level above, over that level's opaque pixels only, so a
 * masked edge stays the colour it was instead of bleeding towards whatever the
 * transparent index happens to be.
 *
 * Returns an error unless the image's width and height are both non-zero multiples of
 * sixteen, which is what a mip texture's four levels need in order to halve evenly.
 */
Result<CustomTexture> quantizeCustomTexture(
  std::string_view name,
  size_t width,
  size_t height,
  const uint8_t* rgba,
  bool bgraOrder,
  const Palette& palette);

/**
 * Reads an image file and quantizes it onto the palette, naming the texture after the
 * file. See quantizeCustomTexture.
 */
Result<CustomTexture> loadCustomTexture(
  const std::filesystem::path& path, const Palette& palette);

/** The texture the editor draws for the given custom texture. */
Result<gl::Texture> createCustomTextureImage(
  const CustomTexture& customTexture, const Palette& palette);

/** The wad lump holding the given custom texture, in the mip texture layout. */
WadLump makeCustomTextureLump(const CustomTexture& customTexture);

} // namespace mdl
} // namespace tb
