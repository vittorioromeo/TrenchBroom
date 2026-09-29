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

#include <QWidget>

#include "base/NotifierConnection.h"

class QComboBox;
class QDoubleSpinBox;
class QPushButton;

namespace tb::ui
{
class MapDocument;
class TerrainTool;

class TerrainToolPage : public QWidget
{
  Q_OBJECT
private:
  MapDocument& m_document;
  TerrainTool& m_tool;

  QPushButton* m_addTerrain = nullptr;
  QDoubleSpinBox* m_cellSize = nullptr;
  QDoubleSpinBox* m_radius = nullptr;
  QDoubleSpinBox* m_strength = nullptr;

  QComboBox* m_mode = nullptr;

  QDoubleSpinBox* m_texScaleX = nullptr;
  QDoubleSpinBox* m_texScaleY = nullptr;

  QPushButton* m_importButton = nullptr;
  QPushButton* m_removeButton = nullptr;
  QPushButton* m_breakButton = nullptr;

  bool m_updatingControls = false;

  NotifierConnection m_notifierConnection;

public:
  explicit TerrainToolPage(
    MapDocument& document, TerrainTool& tool, QWidget* parent = nullptr);

private:
  void createGui();
  void importHeightmap();
  void connectObservers();
  void updateControls();
};

} // namespace tb::ui
