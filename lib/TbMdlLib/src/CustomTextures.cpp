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

#include "mdl/CustomTextures.h"

#include "base/Color.h"
#include "fs/DiskIO.h"
#include "fs/File.h"
#include "gl/Texture.h"
#include "gl/TextureBuffer.h"
#include "mdl/LoadImageTexture.h"
#include "mdl/MaterialUtils.h"
#include "mdl/Palette.h"

#include "kd/reflection_impl.h"
#include "kd/result.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace tb::mdl
{
namespace
{

/** A mip texture's four levels halve twice, and Quake wants both sides a multiple of
 * sixteen so that the smallest level still lands on whole pixels. */
constexpr auto DimensionMultiple = size_t(16);

/** The header of a mip texture lump: the name, the two dimensions and the four level
 * offsets, the last six being four bytes each. */
constexpr auto MipLumpNameSize = size_t(16);
constexpr auto MipLumpHeaderSize = MipLumpNameSize + 4 * (2 + CustomTextureMipLevels);

bool isNameCharacter(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'
         || c == '+' || c == '*';
}

void writeUint32(std::vector<uint8_t>& out, const size_t offset, const uint32_t value)
{
  out[offset + 0] = uint8_t(value & 0xFF);
  out[offset + 1] = uint8_t((value >> 8) & 0xFF);
  out[offset + 2] = uint8_t((value >> 16) & 0xFF);
  out[offset + 3] = uint8_t((value >> 24) & 0xFF);
}

/** The nearest palette colour to the given one, never the transparent index. */
uint8_t nearestPaletteIndex(
  const std::vector<unsigned char>& table, const int r, const int g, const int b)
{
  auto bestIndex = uint8_t(0);
  auto bestDistance = std::numeric_limits<int>::max();

  // The transparent index is skipped so that an opaque pixel can never be quantized
  // onto it and turn into a hole.
  for (size_t i = 0; i < PaletteTransparentIndex; ++i)
  {
    const auto dr = r - int(table[i * 4 + 0]);
    const auto dg = g - int(table[i * 4 + 1]);
    const auto db = b - int(table[i * 4 + 2]);
    const auto distance = dr * dr + dg * dg + db * db;
    if (distance < bestDistance)
    {
      bestDistance = distance;
      bestIndex = uint8_t(i);
    }
  }

  return bestIndex;
}

/** One RGBA pixel of the given image, in the order the source buffer uses. */
struct Texel
{
  int r = 0;
  int g = 0;
  int b = 0;
  int a = 0;
};

Texel readTexel(const uint8_t* rgba, const size_t pixel, const bool bgraOrder)
{
  const auto* p = rgba + pixel * 4;
  return bgraOrder ? Texel{p[2], p[1], p[0], p[3]} : Texel{p[0], p[1], p[2], p[3]};
}

} // namespace

kdl_reflect_impl(CustomTexture);

std::string makeCustomTextureName(const std::string_view name, const bool masked)
{
  auto result = std::string{};
  for (const auto c : name)
  {
    const auto lower = char(std::tolower(static_cast<unsigned char>(c)));
    // A leading "{" is the editor's to add or leave off, so one in the source name is
    // dropped rather than being kept as part of the name.
    if (lower == '{')
    {
      continue;
    }
    result += isNameCharacter(lower) ? lower : '_';
  }

  // Trim the underscores a run of unusual characters leaves at either end, so that a
  // file named "My Texture (final).png" does not become "my_texture__final_".
  while (!result.empty() && result.front() == '_')
  {
    result.erase(result.begin());
  }
  while (!result.empty() && result.back() == '_')
  {
    result.pop_back();
  }

  if (result.empty())
  {
    result = "texture";
  }

  const auto prefix = masked ? std::string{"{"} : std::string{};
  const auto room = MipLumpNameSize - 1 - prefix.size();
  return prefix + result.substr(0, std::min(result.size(), room));
}

Result<CustomTexture> quantizeCustomTexture(
  const std::string_view name,
  const size_t width,
  const size_t height,
  const uint8_t* rgba,
  const bool bgraOrder,
  const Palette& palette)
{
  if (
    width == 0 || height == 0 || width % DimensionMultiple != 0
    || height % DimensionMultiple != 0)
  {
    return Error{fmt::format(
      "A texture must be a multiple of {} pixels on both sides, and this one is {}x{}",
      DimensionMultiple,
      width,
      height)};
  }

  if (!checkTextureDimensions(width, height))
  {
    return Error{fmt::format("Invalid texture dimensions: {}*{}", width, height)};
  }

  const auto& table = palette.data().opaqueData;
  if (table.size() < 256 * 4)
  {
    return Error{"The game's palette does not have 256 colours"};
  }

  // Quantize the full size level, and keep the colours alongside it so that the mip
  // levels average the image rather than the palette's idea of it.
  auto level = std::vector<Texel>(width * height);
  auto masked = false;

  auto indices = std::vector<uint8_t>(width * height);
  for (size_t i = 0; i < width * height; ++i)
  {
    const auto texel = readTexel(rgba, i, bgraOrder);
    level[i] = texel;

    if (texel.a == 0)
    {
      masked = true;
      indices[i] = PaletteTransparentIndex;
    }
    else
    {
      indices[i] = nearestPaletteIndex(table, texel.r, texel.g, texel.b);
    }
  }

  auto mipLevels = std::vector<std::vector<uint8_t>>{};
  mipLevels.push_back(std::move(indices));

  auto levelWidth = width;
  auto levelHeight = height;

  for (size_t mip = 1; mip < CustomTextureMipLevels; ++mip)
  {
    const auto nextWidth = levelWidth / 2;
    const auto nextHeight = levelHeight / 2;

    auto nextLevel = std::vector<Texel>(nextWidth * nextHeight);
    auto nextIndices = std::vector<uint8_t>(nextWidth * nextHeight);

    for (size_t y = 0; y < nextHeight; ++y)
    {
      for (size_t x = 0; x < nextWidth; ++x)
      {
        // Average the four texels above, counting only the opaque ones: letting a
        // transparent texel contribute would drag the edge towards whatever colour sits
        // behind the hole.
        auto r = 0, g = 0, b = 0, opaque = 0;
        for (size_t dy = 0; dy < 2; ++dy)
        {
          for (size_t dx = 0; dx < 2; ++dx)
          {
            const auto& texel = level[(y * 2 + dy) * levelWidth + (x * 2 + dx)];
            if (texel.a != 0)
            {
              r += texel.r;
              g += texel.g;
              b += texel.b;
              ++opaque;
            }
          }
        }

        const auto target = y * nextWidth + x;
        if (opaque == 0)
        {
          nextLevel[target] = Texel{0, 0, 0, 0};
          nextIndices[target] = PaletteTransparentIndex;
        }
        else
        {
          const auto texel = Texel{r / opaque, g / opaque, b / opaque, 255};
          nextLevel[target] = texel;
          nextIndices[target] = nearestPaletteIndex(table, texel.r, texel.g, texel.b);
        }
      }
    }

    level = std::move(nextLevel);
    mipLevels.push_back(std::move(nextIndices));
    levelWidth = nextWidth;
    levelHeight = nextHeight;
  }

  return CustomTexture{
    makeCustomTextureName(name, masked), width, height, masked, std::move(mipLevels)};
}

Result<CustomTexture> loadCustomTexture(
  const std::filesystem::path& path, const Palette& palette)
{
  return fs::Disk::openFile(path)
         | kdl::and_then([&](const std::shared_ptr<fs::CFile>& file) {
             auto reader = file->reader();
             return loadImageTexture(reader);
           })
         | kdl::and_then([&](const gl::Texture& texture) -> Result<CustomTexture> {
             const auto& buffers = texture.buffersIfLoaded();
             if (buffers.empty())
             {
               return Error{"the image has no pixels"};
             }

             return quantizeCustomTexture(
               path.stem().string(),
               texture.width(),
               texture.height(),
               buffers.front().data(),
               texture.format() == GL_BGRA,
               palette);
           })
         | kdl::or_else([&](auto e) -> Result<CustomTexture> {
             return Error{fmt::format("Could not import '{}': {}", path, e.msg)};
           });
}

Result<gl::Texture> createCustomTextureImage(
  const CustomTexture& customTexture, const Palette& palette)
{
  if (customTexture.mipLevels.size() != CustomTextureMipLevels)
  {
    return Error{"A custom texture must have four mip levels"};
  }

  const auto& table = customTexture.masked ? palette.data().index255TransparentData
                                           : palette.data().opaqueData;

  auto buffers = gl::TextureBufferList{CustomTextureMipLevels};
  setMipBufferSize(
    buffers, CustomTextureMipLevels, customTexture.width, customTexture.height, GL_RGBA);

  for (size_t mip = 0; mip < CustomTextureMipLevels; ++mip)
  {
    const auto& indices = customTexture.mipLevels[mip];
    auto* out = buffers[mip].data();
    for (size_t i = 0; i < indices.size(); ++i)
    {
      std::memcpy(out + i * 4, table.data() + size_t(indices[i]) * 4, 4);
    }
  }

  const auto averageColor = getAverageColor(buffers.front(), GL_RGBA);

  auto texture = gl::Texture{
    customTexture.width,
    customTexture.height,
    averageColor,
    GL_RGBA,
    gl::NoEmbeddedDefaults{},
    std::move(buffers)};
  texture.setAlphaDomain(
    customTexture.masked ? img::ImageAlphaDomain::Binary
                         : img::ImageAlphaDomain::Opaque);
  return texture;
}

WadLump makeCustomTextureLump(const CustomTexture& customTexture)
{
  auto size = MipLumpHeaderSize;
  for (const auto& level : customTexture.mipLevels)
  {
    size += level.size();
  }

  auto data = std::vector<uint8_t>(size, uint8_t(0));

  const auto nameLength = std::min(customTexture.name.size(), MipLumpNameSize - 1);
  std::memcpy(data.data(), customTexture.name.data(), nameLength);

  writeUint32(data, MipLumpNameSize, uint32_t(customTexture.width));
  writeUint32(data, MipLumpNameSize + 4, uint32_t(customTexture.height));

  auto offset = MipLumpHeaderSize;
  for (size_t mip = 0; mip < customTexture.mipLevels.size(); ++mip)
  {
    // The level offsets are relative to the start of the lump, not of the file.
    writeUint32(data, MipLumpNameSize + 8 + mip * 4, uint32_t(offset));

    const auto& level = customTexture.mipLevels[mip];
    std::memcpy(data.data() + offset, level.data(), level.size());
    offset += level.size();
  }

  return WadLump{customTexture.name, WadMipTextureType, std::move(data)};
}

} // namespace tb::mdl
