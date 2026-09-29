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

#include "base/Notifier.h"
#include "base/NotifierConnection.h"
#include "mdl/Brush.h"
#include "mdl/HitType.h"
#include "mdl/Spline.h"
#include "mdl/SplineEntities.h"
#include "mdl/SplineEntity.h"
#include "ui/Tool.h"

#include "vm/ray.h"
#include "vm/vec.h"

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace tb
{
namespace gl
{
class Camera;
}

namespace mdl
{
class Brush;
class BrushNode;
class EntityNode;
class Grid;
class GroupNode;
class Node;
class PickResult;
} // namespace mdl

namespace render
{
class RenderBatch;
class RenderContext;
class RenderService;
} // namespace render

namespace ui
{
class MapDocument;

/** The parts of a spline control point that can be picked and dragged: the point
 * itself, and its two tangent handles (visible in tangent edit mode). */
enum class SplineHandlePart
{
  Point,
  TangentIn,
  TangentOut,
};

/**
 * A tool for creating and editing splines. A spline is a curve through a sequence of
 * control points; each point can be moved, rotated (rolled around the curve) and
 * locked. A spline can be linked to a group, in which case the group's brushes are
 * used as a template that is deformed along the curve, and the resulting brushes are
 * kept as children of the spline's entity. The group's point entities are replicated
 * along the curve as well, as real entities so that the map compiles the same way it
 * would had they been placed by hand; since an entity holds only brushes and patches,
 * these are kept beside the spline rather than inside it, and carry a property naming
 * the spline they belong to.
 *
 * The spline is persisted in the map as a func_group entity carrying the control
 * points in its properties, so that it remains editable across sessions and its
 * brushes are merged into the world geometry by map compilers.
 */
class SplineTool : public Tool
{
public:
  static const mdl::HitType::Type PointHitType;

  Notifier<> splineDidChangeNotifier;

private:
  MapDocument& m_document;

  std::vector<mdl::SplinePoint> m_points;
  size_t m_subdivisions = mdl::SplineDefaultSubdivisions;

  /** Whether the spline is closed, i.e. the last point connects back to the first. */
  bool m_closed = false;

  /** Whether the copies keep the alignment the template was authored with. */
  bool m_lockUVs = false;

  /** Whether the copies are placed at the template's own size. */
  bool m_keepSize = false;

  /** The template is either a group (referenced by its persistent ID) or a snapshot
   * of individually linked brushes and point entities; at most one of these is set. */
  std::optional<mdl::IdType> m_templateGroupId;
  std::vector<mdl::Brush> m_templateBrushes;
  std::vector<mdl::SplineTemplateEntity> m_templateEntities;
  std::vector<mdl::SplineTemplateBrushEntity> m_templateBrushEntities;

  /** Whether clicking empty space appends new points. */
  bool m_addPointMode = false;

  /** Whether the selected point's tangent handles are shown and editable. */
  bool m_tangentEditMode = false;

  /** The entity node holding the spline currently being edited, if any. */
  mdl::EntityNode* m_splineNode = nullptr;

  /** All other splines in the map, so they can be shown and picked up while the tool
   * is active. Refreshed whenever the document changes. */
  std::vector<std::pair<mdl::EntityNode*, mdl::SplineEntityData>> m_otherSplines;

  std::optional<size_t> m_selectedIndex;
  /** Which handle of the selected point is selected (for keyboard moves). */
  SplineHandlePart m_selectedPart = SplineHandlePart::Point;

  struct DragState
  {
    size_t index;
    SplineHandlePart part;
    mdl::SplinePoint originalPoint;
  };
  std::optional<DragState> m_dragState;

  bool m_ignoreNotifications = false;

  NotifierConnection m_notifierConnection;

public:
  explicit SplineTool(MapDocument& document);
  ~SplineTool() override;

  const mdl::Grid& grid() const;

  void pick(
    const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult);

  void render(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const mdl::PickResult& pickResult);

  void renderFeedback(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const vm::vec3d& point) const;

private:
  void renderHighlight(
    render::RenderService& renderService, const mdl::PickResult& pickResult) const;

public: // add point mode
  /**
   * While add point mode is enabled, clicking empty space appends a new point to the
   * spline; while it is disabled, clicks only select existing points. The mode is
   * always disabled when the tool is activated and must be enabled explicitly on the
   * tool page.
   */
  bool addPointMode() const;
  void setAddPointMode(bool addPointMode);

public: // point management
  bool hasPoints() const;
  const std::vector<mdl::SplinePoint>& points() const;

  /** Returns the position new points are created relative to: the selected point if
   * one between two other points is selected, otherwise the last point, if any. */
  std::optional<vm::vec3d> lastPointPosition() const;

  /**
   * Adds a new point to the spline. If a point between two other points is selected,
   * the new point is inserted between the selected point and the next one; otherwise
   * it is appended after the last point. The new point becomes the selected point.
   */
  void addPoint(const vm::vec3d& point);

  bool canRemovePoint() const;
  /** Removes the selected point, or the last point if none is selected. */
  void removePoint();

  /** Selects the point hit by the given pick result. Returns whether a point was hit. */
  bool selectPoint(const mdl::PickResult& pickResult);

  /**
   * Picks up the spline whose generated geometry is hit by the given pick result for
   * editing. Since generated brushes cannot be selected, this is how an existing
   * spline is chosen while the tool is active. Returns whether a different spline
   * was hit and loaded.
   */
  bool selectSpline(const mdl::PickResult& pickResult);
  void deselectPoint();
  std::optional<size_t> selectedPointIndex() const;

  /** Moves the selected point by the given delta, e.g. when a keyboard move action is
   * triggered. Does nothing if no point is selected. */
  void moveSelectedPoint(const vm::vec3d& delta);

public: // dragging
  std::optional<std::tuple<vm::vec3d, vm::vec3d>> beginDragPoint(
    const mdl::PickResult& pickResult);
  /** Whether the current drag moves a tangent handle rather than a control point. */
  bool draggingTangentHandle() const;
  bool dragPoint(const vm::vec3d& newPosition);
  void endDragPoint();
  void cancelDragPoint();

public: // rotation, scale and locking
  double selectedPointRoll() const;
  void setSelectedPointRoll(double roll);

  double selectedPointScale() const;
  void setSelectedPointScale(double scale);

  bool selectedPointLock(mdl::SplineLock::Type lock) const;
  void setSelectedPointLock(mdl::SplineLock::Type lock, bool set);
  void toggleSelectedPointLock(mdl::SplineLock::Type lock);

public: // tangent editing
  /** Whether the selected point's tangents follow the curve automatically. */
  bool selectedPointAutoTangent() const;
  /** Switching to manual tangents initializes the handles from the current automatic
   * tangents, so the curve does not change until a handle is moved. */
  void setSelectedPointAutoTangent(bool autoTangent);

  /** While tangent edit mode is active, the selected point's tangent handles are
   * shown and can be picked and dragged like control points. */
  bool tangentEditMode() const;
  void setTangentEditMode(bool tangentEditMode);

private:
  /** Whether the tangent handles of the selected point are currently visible. */
  bool tangentHandlesVisible() const;

public:
public: // closing
  /** Whether the spline is closed, i.e. the last point connects back to the first
   * and brushes are created on that segment as well. */
  bool closed() const;
  void setClosed(bool closed);

public: // UVs
  /**
   * Whether every generated copy keeps the alignment the template was authored with,
   * rather than having the template's UVs realigned onto the geometry the sweep
   * produces. See mdl::SplineUVMode.
   */
  bool lockUVs() const;
  void setLockUVs(bool lockUVs);

  /**
   * Whether every copy is placed at the size the template is drawn at, rather than
   * stretched to fill its span. See mdl::createSplineBrushes.
   */
  bool keepSize() const;
  void setKeepSize(bool keepSize);

public: // template group linkage
  size_t subdivisions() const;
  void setSubdivisions(size_t subdivisions);

  /** Whether the current selection contains a group, brushes or point entities that
   * can be linked. */
  bool canLinkTemplate() const;
  /**
   * Links the current selection as the spline's deformation template. A selected
   * group is linked by reference, so later changes to it are picked up when the
   * spline is regenerated; a plain selection of brushes and point entities is linked
   * by taking a snapshot of them.
   */
  void linkTemplate();
  bool hasTemplate() const;
  void unlinkTemplate();
  /** A user facing description of the linked template. */
  std::string templateName() const;

  /** Whether the spline has generated brushes or entities that can be broken out. */
  bool canBreakSpline() const;
  /**
   * Duplicates the spline's generated brushes and point entities as standard,
   * editable ones and unlinks the template, so the spline stops generating anything
   * and the user can edit the copies.
   */
  void breakSpline();

private:
  size_t addPointAnchorIndex() const;

  mdl::GroupNode* findTemplateGroup() const;
  mdl::GroupNode* selectedGroup() const;

  void loadFromSelection();
  void loadSplineNode(mdl::EntityNode* splineNode);
  void clearSpline();
  void refreshOtherSplines();

  /**
   * Writes the current spline state to the document by replacing the spline entity
   * (and its generated brushes) in a single undoable transaction.
   */
  void commitSpline(const std::string& commandName);

  /** The brushes and entities the template is made of, in template space. */
  struct TemplateContents
  {
    /** The template's worldspawn brushes, which sweep into the spline's own entity. */
    std::vector<const mdl::Brush*> brushes;
    /** The template's brush entities, which sweep into entities of their own class. */
    std::vector<mdl::SplineTemplateBrushEntity> brushEntities;
    std::vector<mdl::SplineTemplateEntity> entities;
    /** The lattice the sweep deforms, which the brushes size when there are any. */
    vm::bbox3d bounds;
  };
  std::optional<TemplateContents> collectTemplate() const;

  std::vector<mdl::Node*> createBrushNodes(const TemplateContents& contents) const;

  /**
   * The entities to place along the curve, each marked as belonging to the spline with
   * the given id: a copy of every template point entity, and one of every template
   * brush entity holding that copy's swept brushes. Unlike the worldspawn brushes these
   * cannot be children of the spline's entity, since an entity holds only brushes and
   * patches, so they are added beside it and the marker is what ties them back to it.
   */
  std::vector<mdl::Node*> createEntityNodes(
    const TemplateContents& contents, const std::string& splineId) const;

  /** The entities the spline currently owns, found by their marker. */
  std::vector<mdl::Node*> findGeneratedEntityNodes() const;

private:
  bool doActivate() override;
  bool doDeactivate() override;

  void connectObservers();
  void nodesWereAdded(const std::vector<mdl::Node*>& nodes);
  void nodesWereRemoved(const std::vector<mdl::Node*>& nodes);
  void nodesDidChange(const std::vector<mdl::Node*>& nodes);
  void selectionDidChange();
};

} // namespace ui
} // namespace tb
