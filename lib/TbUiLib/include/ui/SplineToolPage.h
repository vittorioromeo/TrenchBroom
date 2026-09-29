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

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace tb::ui
{
class MapDocument;
class SplineTool;

class SplineToolPage : public QWidget
{
  Q_OBJECT
private:
  MapDocument& m_document;
  SplineTool& m_tool;

  QPushButton* m_addPoints = nullptr;
  QLabel* m_templateLabel = nullptr;
  QPushButton* m_linkButton = nullptr;
  QPushButton* m_unlinkButton = nullptr;
  QPushButton* m_breakButton = nullptr;
  QDoubleSpinBox* m_roll = nullptr;
  QDoubleSpinBox* m_scale = nullptr;
  QCheckBox* m_autoTangent = nullptr;
  QPushButton* m_editTangents = nullptr;
  QCheckBox* m_lockTwist = nullptr;
  QPushButton* m_removePointButton = nullptr;
  QCheckBox* m_closed = nullptr;
  QCheckBox* m_lockUVs = nullptr;
  QCheckBox* m_keepSize = nullptr;

  bool m_updatingControls = false;

  NotifierConnection m_notifierConnection;

public:
  explicit SplineToolPage(
    MapDocument& document, SplineTool& tool, QWidget* parent = nullptr);

private:
  void createGui();
  void connectObservers();
  void updateControls();
};

} // namespace tb::ui
