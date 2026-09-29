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

#include "ui/TerrainToolController.h"

#include <QTimer>

#include "base/PreferenceManager.h"
#include "prefs/Preferences.h"
#include "gl/Camera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Grid.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/PickResult.h"
#include "render/RenderService.h"
#include "ui/GestureTracker.h"
#include "ui/HandleDragTracker.h"
#include "ui/InputState.h"
#include "ui/TerrainTool.h"

#include "vm/intersection.h"
#include "vm/line.h"
#include "vm/plane.h"
#include "vm/polygon.h"
#include "vm/segment.h"
#include "vm/vec.h"

#include <memory>
#include <optional>
#include <utility>

namespace tb::ui
{
namespace
{

/**
 * Encapsulates the difference between the 2D and 3D views: the plane a new terrain's
 * box is dragged out on.
 */
class PartDelegateBase
{
protected:
  TerrainTool& m_tool;

public:
  explicit PartDelegateBase(TerrainTool& tool)
    : m_tool{tool}
  {
  }

  virtual ~PartDelegateBase() = default;

  TerrainTool& tool() const { return m_tool; }

  /** The plane new terrains are dragged out on. */
  virtual vm::plane3d creationPlane(const InputState& inputState) const = 0;

  /** The point on the given plane under the mouse, snapped to the grid. */
  std::optional<vm::vec3d> pointOnPlane(
    const InputState& inputState, const vm::plane3d& plane) const
  {
    const auto& pickRay = inputState.pickRay();
    if (const auto distance = vm::intersect_ray_plane(pickRay, plane))
    {
      return m_tool.grid().snap(vm::point_at_distance(pickRay, *distance));
    }
    return std::nullopt;
  }
};

class PartDelegate2D : public PartDelegateBase
{
public:
  using PartDelegateBase::PartDelegateBase;

  vm::plane3d creationPlane(const InputState& inputState) const override
  {
    // In a 2D view the box is dragged out on the view plane through the origin.
    const auto& camera = inputState.camera();
    const auto normal = vm::get_abs_max_component_axis(vm::vec3d{camera.direction()});
    return vm::plane3d{vm::vec3d{0, 0, 0}, normal};
  }
};

class PartDelegate3D : public PartDelegateBase
{
public:
  using PartDelegateBase::PartDelegateBase;

  vm::plane3d creationPlane(const InputState& inputState) const override
  {
    using namespace mdl::HitFilters;

    // Prefer the surface under the mouse so terrains can be placed on existing
    // geometry, and fall back to the horizontal plane through the origin.
    const auto& hit = inputState.pickResult().first(type(mdl::BrushNode::BrushHitType));
    if (const auto faceHandle = mdl::hitToFaceHandle(hit))
    {
      const auto& boundary = faceHandle->face().boundary();
      if (vm::abs(boundary.normal.z()) > 0.5)
      {
        return vm::plane3d{hit.hitPoint(), vm::vec3d{0, 0, 1}};
      }
    }
    return vm::plane3d{vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 1}};
  }
};

class PartBase
{
protected:
  std::unique_ptr<PartDelegateBase> m_delegate;

  explicit PartBase(std::unique_ptr<PartDelegateBase> delegate)
    : m_delegate{std::move(delegate)}
  {
  }

public:
  virtual ~PartBase() = default;
};

/**
 * Drags out the footprint of a new terrain. The box is as tall as one grid step, so
 * the new terrain starts out as a thin slab that can immediately be sculpted.
 */
class CreateTerrainTracker : public GestureTracker
{
private:
  TerrainTool& m_tool;
  PartDelegateBase& m_delegate;
  /** The plane is captured when the drag starts so that it cannot shift while the
   * mouse moves over other geometry. */
  vm::plane3d m_plane;
  vm::vec3d m_start;
  std::optional<vm::bbox3d> m_bounds;

public:
  CreateTerrainTracker(
    TerrainTool& tool,
    PartDelegateBase& delegate,
    const vm::plane3d& plane,
    const vm::vec3d& start)
    : m_tool{tool}
    , m_delegate{delegate}
    , m_plane{plane}
    , m_start{start}
  {
  }

  bool update(const InputState& inputState) override
  {
    if (const auto current = m_delegate.pointOnPlane(inputState, m_plane))
    {
      const auto thickness = vm::max(8.0, double(m_tool.grid().actualSize()));
      const auto min = vm::vec3d{
        vm::min(m_start.x(), current->x()),
        vm::min(m_start.y(), current->y()),
        m_start.z()};
      const auto max = vm::vec3d{
        vm::max(m_start.x(), current->x()),
        vm::max(m_start.y(), current->y()),
        m_start.z() + thickness};
      m_bounds = vm::bbox3d{min, max};
    }
    return true;
  }

  void end(const InputState&) override
  {
    if (m_bounds)
    {
      m_tool.createTerrain(*m_bounds);
    }
  }

  void cancel() override { m_bounds = std::nullopt; }

  void render(
    const InputState&,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) const override
  {
    if (m_bounds)
    {
      auto renderService = render::RenderService{renderContext, renderBatch};
      renderService.setForegroundColor(pref(Preferences::TerrainBoundsColor));
      renderService.renderBounds(vm::bbox3f{*m_bounds});
    }
  }
};

/**
 * What a sculpting drag reports back to the part that owns the stroke. The stroke is
 * started on mouse down rather than by the drag, so the drag only moves it around.
 */
class SculptHandler
{
public:
  virtual ~SculptHandler() = default;

  virtual void sculptMoved(const InputState& inputState) = 0;
  virtual void sculptEnded() = 0;
  virtual void sculptCancelled() = 0;
};

/** Follows the mouse while it is dragged over the terrain. */
class SculptTracker : public GestureTracker
{
private:
  SculptHandler& m_handler;

public:
  explicit SculptTracker(SculptHandler& handler)
    : m_handler{handler}
  {
  }

  bool update(const InputState& inputState) override
  {
    m_handler.sculptMoved(inputState);
    return true;
  }

  void end(const InputState&) override { m_handler.sculptEnded(); }

  void cancel() override { m_handler.sculptCancelled(); }
};

/** Creates new terrains while add mode is enabled. */
class CreateTerrainPart : public ToolController, protected PartBase
{
public:
  explicit CreateTerrainPart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
  {
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    if (
      !m_delegate->tool().addMode() || inputState.mouseButtons() != MouseButtons::Left
      || !inputState.modifierKeysPressed(ModifierKeys::None))
    {
      return nullptr;
    }

    const auto plane = m_delegate->creationPlane(inputState);
    if (const auto start = m_delegate->pointOnPlane(inputState, plane))
    {
      return std::make_unique<CreateTerrainTracker>(
        m_delegate->tool(), *m_delegate, plane, *start);
    }
    return nullptr;
  }

  bool cancel() override { return false; }
};

/**
 * Sculpts and paints the current terrain while add mode is disabled.
 *
 * The stroke starts on mouse down and lasts until the button is released, so a click
 * applies the brush just like a drag does. It is applied repeatedly for as long as the
 * button is held: moving the mouse applies it at the new position, and a repeat timer
 * keeps applying it at the last position while the mouse stands still, so holding the
 * button down in one spot keeps building the terrain up there.
 */
class SculptPart : public ToolController, protected PartBase, private SculptHandler
{
private:
  /** How often the brush is applied while the mouse is held down without moving. */
  static constexpr auto RepeatIntervalMs = 33;

  std::unique_ptr<QTimer> m_repeatTimer;

  /** The position the brush is applied at while the button is held. It stays put while
   * the mouse does, which also keeps Flatten levelling towards the height that was
   * clicked. */
  std::optional<vm::vec3d> m_position;
  bool m_invert = false;
  /** Whether the button is down and a stroke is being applied. */
  bool m_sculpting = false;

public:
  explicit SculptPart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
    , m_repeatTimer{std::make_unique<QTimer>()}
  {
    m_repeatTimer->setInterval(RepeatIntervalMs);
    // The timer is owned by this part and is also the connection's context, so the
    // lambda cannot outlive it.
    QObject::connect(
      m_repeatTimer.get(), &QTimer::timeout, m_repeatTimer.get(), [this]() {
        applyBrush();
      });
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  void applyBrush()
  {
    if (m_sculpting && m_position)
    {
      m_delegate->tool().applyStroke(*m_position, m_invert);
      // Applying restarts the wait, so a moving mouse never doubles the rate.
      m_repeatTimer->start();
    }
  }

  /** Whether the given input starts or continues a sculpting stroke. */
  bool acceptsSculpting(const InputState& inputState) const
  {
    // With no sculpting mode selected the tool only selects terrains.
    return m_delegate->tool().sculpting()
           && (inputState.modifierKeysPressed(ModifierKeys::None) || inputState.modifierKeysPressed(ModifierKeys::Shift));
  }

  void endSculpting()
  {
    if (m_sculpting)
    {
      m_repeatTimer->stop();
      m_sculpting = false;
      m_delegate->tool().endStroke();
    }
  }

  void mouseDown(const InputState& inputState) override
  {
    auto& tool = m_delegate->tool();
    if (
      m_sculpting || !inputState.mouseButtonsPressed(MouseButtons::Left)
      || !acceptsSculpting(inputState)
      // Clicking another terrain picks that one up instead of sculpting this one.
      || tool.canSelectTerrainAt(inputState.pickResult()))
    {
      return;
    }

    m_position = tool.pickSurface(inputState.pickRay());
    if (!m_position)
    {
      return;
    }

    m_invert = inputState.modifierKeysDown(ModifierKeys::Shift);
    m_sculpting = true;
    tool.setBrushPosition(*m_position);
    tool.beginStroke();
    applyBrush();
  }

  void mouseUp(const InputState&) override { endSculpting(); }

  void mouseMove(const InputState& inputState) override
  {
    auto& tool = m_delegate->tool();
    // The brush is only shown while a sculpting mode is selected.
    tool.setBrushPosition(
      tool.sculpting() ? tool.pickSurface(inputState.pickRay()) : std::nullopt);
  }

  bool mouseClick(const InputState& inputState) override
  {
    // The click already started a stroke on mouse down, which mouse up will end.
    if (m_sculpting)
    {
      return true;
    }

    auto& tool = m_delegate->tool();
    if (
      tool.addMode() || !inputState.mouseButtonsPressed(MouseButtons::Left)
      || !inputState.modifierKeysPressed(ModifierKeys::None))
    {
      return false;
    }

    // Clicking a terrain's geometry picks it up for editing.
    return tool.selectTerrainAt(inputState.pickResult());
  }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    // The stroke is already running; the drag only moves it and ends it.
    // Constructed here rather than with make_unique so that the conversion to the
    // privately inherited handler interface is allowed.
    return m_sculpting && inputState.mouseButtons() == MouseButtons::Left
             ? std::unique_ptr<GestureTracker>{new SculptTracker{*this}}
             : nullptr;
  }

  void sculptMoved(const InputState& inputState) override
  {
    auto& tool = m_delegate->tool();
    if (const auto position = tool.pickSurface(inputState.pickRay()))
    {
      m_position = *position;
      m_invert = inputState.modifierKeysDown(ModifierKeys::Shift);
      tool.setBrushPosition(*position);
      applyBrush();
    }
  }

  void sculptEnded() override { endSculpting(); }

  void sculptCancelled() override
  {
    if (m_sculpting)
    {
      m_repeatTimer->stop();
      m_sculpting = false;
      m_delegate->tool().cancelStroke();
    }
  }

  bool cancel() override { return false; }
};

} // namespace

TerrainToolControllerBase::TerrainToolControllerBase(TerrainTool& tool)
  : m_tool{tool}
{
}

TerrainToolControllerBase::~TerrainToolControllerBase() = default;

Tool& TerrainToolControllerBase::tool()
{
  return m_tool;
}

const Tool& TerrainToolControllerBase::tool() const
{
  return m_tool;
}

void TerrainToolControllerBase::render(
  const InputState& inputState,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  m_tool.render(renderContext, renderBatch);
  ToolControllerGroup::render(inputState, renderContext, renderBatch);
}

bool TerrainToolControllerBase::cancel()
{
  if (m_tool.addMode())
  {
    m_tool.setAddMode(false);
    return true;
  }
  return false;
}

TerrainToolController2D::TerrainToolController2D(TerrainTool& tool)
  : TerrainToolControllerBase{tool}
{
  addController(
    std::make_unique<CreateTerrainPart>(std::make_unique<PartDelegate2D>(tool)));
  addController(std::make_unique<SculptPart>(std::make_unique<PartDelegate2D>(tool)));
}

TerrainToolController3D::TerrainToolController3D(TerrainTool& tool)
  : TerrainToolControllerBase{tool}
{
  addController(
    std::make_unique<CreateTerrainPart>(std::make_unique<PartDelegate3D>(tool)));
  addController(std::make_unique<SculptPart>(std::make_unique<PartDelegate3D>(tool)));
}

} // namespace tb::ui
