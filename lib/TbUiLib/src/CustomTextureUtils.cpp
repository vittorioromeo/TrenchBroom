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

#include "ui/CustomTextureUtils.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "fs/DiskIO.h"
#include "fs/PathInfo.h"
#include "mdl/CustomTextures.h"
#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Selection.h"
#include "mdl/PushSelection.h"
#include "mdl/Transaction.h"
#include "mdl/WadFile.h"
#include "mdl/WadPropertyUtils.h"
#include "mdl/WorldNode.h"
#include "ui/ChoosePathTypeDialog.h"
#include "ui/QPathUtils.h"

#include "kd/result.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>

namespace tb::ui
{
namespace
{

/**
 * The places a wad path in the map can be relative to, which is what the map's wad list
 * is resolved against when the materials are loaded.
 */
std::vector<std::filesystem::path> wadSearchPaths(
  const mdl::Map& map, const std::filesystem::path& mapPath)
{
  return {
    mapPath.parent_path(),
    pref(map.gameInfo().gamePathPreference),
    map.environmentConfig().appFolderPath,
  };
}

/** The wad the map says it keeps its own textures in, if that wad is still there. */
std::optional<std::filesystem::path> existingCustomWadPath(
  const mdl::Map& map, const std::filesystem::path& mapPath)
{
  const auto* pathStr =
    map.worldNode().entity().property(mdl::CustomTextureWadPropertyKey);
  if (!pathStr || pathStr->empty())
  {
    return std::nullopt;
  }

  const auto path = std::filesystem::path{*pathStr};
  const auto exists = [](const auto& candidate) {
    return fs::Disk::pathInfo(candidate) == fs::PathInfo::File;
  };

  if (path.is_absolute())
  {
    return exists(path) ? std::optional{path} : std::nullopt;
  }

  for (const auto& searchPath : wadSearchPaths(map, mapPath))
  {
    if (const auto candidate = searchPath / path; exists(candidate))
    {
      return candidate;
    }
  }

  return std::nullopt;
}

/**
 * The form the wad path is written in: relative to the map when it is somewhere under
 * the map's own folder, which is where a wad belonging to one map usually goes, and
 * absolute otherwise.
 */
std::string wadPathString(
  const mdl::Map& map,
  const std::filesystem::path& mapPath,
  const std::filesystem::path& wadPath)
{
  const auto gamePath = pref(map.gameInfo().gamePathPreference);
  const auto relative =
    convertToPathType(PathType::DocumentRelative, wadPath, mapPath, gamePath);

  // convertToPathType walks out of the map's folder with "..", which is fine but reads
  // badly and breaks as soon as the map moves; an unrelated wad stays absolute.
  const auto goesUp = !relative.empty() && relative.begin()->string() == "..";
  return (relative.empty() || goesUp) ? wadPath.generic_string()
                                      : relative.generic_string();
}

/** Adds the wad to the map's wad list, and records it as the map's own texture wad. */
bool recordCustomWad(
  mdl::Map& map, const std::string& wadPathStr, const std::string& wadPropertyKey)
{
  auto transaction = mdl::Transaction{map, "Set Texture Wad"};

  // The properties belong to worldspawn, not to whatever happens to be selected when
  // the map is saved.
  const auto pushSelection = mdl::PushSelection{map};
  deselectAll(map);

  auto wadPaths = std::vector<std::string>{};
  if (const auto* wadStr = map.worldNode().entity().property(wadPropertyKey))
  {
    wadPaths = mdl::splitWadProperty(*wadStr);
  }

  if (std::ranges::find(wadPaths, wadPathStr) == wadPaths.end())
  {
    wadPaths.push_back(wadPathStr);
  }

  auto success = setEntityProperty(map, wadPropertyKey, mdl::joinWadProperty(wadPaths));
  success =
    success && setEntityProperty(map, mdl::CustomTextureWadPropertyKey, wadPathStr);

  transaction.finish(success);
  return success;
}

} // namespace

bool isCustomTextureImage(const QString& pathQStr)
{
  const auto fileInfo = QFileInfo{pathQStr};
  return fileInfo.isFile() && fileInfo.fileName().toLower().endsWith(".png");
}

bool importCustomTextures(
  const QStringList& pathQStrs, mdl::Map& map, QWidget* dialogParent)
{
  const auto palette = map.materialPalette();
  if (!palette)
  {
    QMessageBox::critical(
      dialogParent,
      "",
      QObject::tr("This game's textures do not come from a palette, so an image cannot "
                  "be converted into one."),
      QMessageBox::Ok);
    return false;
  }

  auto errors = std::vector<std::string>{};
  auto imported = 0;

  for (const auto& pathQStr : pathQStrs)
  {
    const auto path = pathFromQString(pathQStr);
    mdl::loadCustomTexture(path, *palette) | kdl::transform([&](auto customTexture) {
      map.logger().info() << "Imported texture '" << customTexture.name << "' from "
                          << path;
      map.addCustomTexture(std::move(customTexture));
      ++imported;
    }) | kdl::transform_error([&](auto e) { errors.push_back(e.msg); });
  }

  if (!errors.empty())
  {
    QMessageBox::warning(
      dialogParent,
      "",
      QString::fromStdString(kdl::str_join(errors, "\n")),
      QMessageBox::Ok);
  }

  return imported > 0;
}

bool saveCustomTextures(
  mdl::Map& map, const std::filesystem::path& mapPath, QWidget* dialogParent)
{
  if (map.customTextures().empty())
  {
    return true;
  }

  const auto wadPropertyKey = map.gameInfo().gameConfig.materialConfig.property;
  if (!wadPropertyKey)
  {
    // This game does not load textures from wads at all, so there is nowhere to put
    // them. Say so rather than holding up the save over it.
    map.logger().error() << "This game does not use wad files, so the textures brought "
                            "in from images cannot be saved";
    return true;
  }

  auto wadPath = existingCustomWadPath(map, mapPath);
  if (!wadPath)
  {
    const auto defaultPath =
      mapPath.empty() ? std::filesystem::path{"textures.wad"}
                      : mapPath.parent_path() / (mapPath.stem().string() + ".wad");

    const auto chosen = QFileDialog::getSaveFileName(
      dialogParent,
      QObject::tr("Save the map's own textures to a wad file"),
      pathAsQPath(defaultPath),
      QObject::tr("Wad files (*.wad)"));

    if (chosen.isEmpty())
    {
      return false;
    }

    wadPath = pathFromQString(chosen);
  }

  // Everything already in the wad is carried across, so that the textures written on an
  // earlier save, and anything another tool put there, survive this one.
  auto lumps = std::vector<mdl::WadLump>{};
  if (fs::Disk::pathInfo(*wadPath) == fs::PathInfo::File)
  {
    auto existing = mdl::readWadLumps(*wadPath);
    if (existing.is_error())
    {
      const auto answer = QMessageBox::question(
        dialogParent,
        "",
        QObject::tr("'%1' cannot be read as a wad file. Replace it?")
          .arg(pathAsQString(*wadPath)),
        QMessageBox::Yes | QMessageBox::No);

      if (answer != QMessageBox::Yes)
      {
        return false;
      }
    }
    else
    {
      lumps = std::move(existing) | kdl::value();
    }
  }

  for (const auto& customTexture : map.customTextures())
  {
    lumps.push_back(mdl::makeCustomTextureLump(customTexture));
  }

  auto errorMessage = std::string{};
  mdl::writeWad(*wadPath, lumps)
    | kdl::transform_error([&](auto e) { errorMessage = e.msg; });

  if (!errorMessage.empty())
  {
    QMessageBox::critical(
      dialogParent, "", QString::fromStdString(errorMessage), QMessageBox::Ok);
    return false;
  }

  map.logger().info() << "Wrote " << map.customTextures().size() << " texture(s) to "
                      << *wadPath;

  // The property has to be in place before the textures are let go of, or reloading
  // would not find them in the wad they just went into.
  if (!recordCustomWad(map, wadPathString(map, mapPath, *wadPath), *wadPropertyKey))
  {
    return false;
  }

  map.clearCustomTextures();
  return true;
}

} // namespace tb::ui
