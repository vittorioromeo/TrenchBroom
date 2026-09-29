/*
 Copyright (C) 2010 Kristian Duske

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

#include "ui/MapView3D.h"

#include <QOpenGLContext>
#include <QTimer>

#include "base/PreferenceManager.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/BezierPatch.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map_Picking.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/PointTrace.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "render/BoundsGuideRenderer.h"
#include "render/Compass3D.h"
#include "render/LightPreview.h"
#include "render/MapRenderer.h"
#include "render/RenderBatch.h"
#include "render/RenderContext.h"
#include "render/SelectionBoundsRenderer.h"
#include "ui/AnimationManager.h"
#include "ui/AssembleBrushToolController3D.h"
#include "ui/CameraAnimation.h"
#include "ui/CameraTool3D.h"
#include "ui/ClipToolController.h"
#include "ui/ControlPointTool.h"
#include "ui/ControlPointToolController.h"
#include "ui/CreateEntityToolController.h"
#include "ui/DrawShapeToolController3D.h"
#include "ui/EdgeTool.h"
#include "ui/EdgeToolController.h"
#include "ui/ExtrudeToolController.h"
#include "ui/FaceTool.h" // IWYU pragma: keep
#include "ui/FaceToolController.h"
#include "ui/FlyModeHelper.h"
#include "ui/GlFunctions.h"
#include "ui/GlQt.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/MoveObjectsToolController.h"
#include "ui/RotateToolController.h"
#include "ui/ScaleToolController.h"
#include "ui/SelectionTool.h"
#include "ui/SetBrushFaceAttributesTool.h"
#include "ui/ShearToolController.h"
#include "ui/SplineToolController.h"
#include "ui/TerrainToolController.h"
#include "ui/SweepToolController.h"
#include "ui/VertexTool.h"
#include "ui/VertexToolController.h"

#include "kd/contracts.h"
#include "kd/set_temp.h"

#include "vm/util.h"

namespace tb::ui
{

MapView3D::MapView3D(
  AppController& appController, MapDocument& document, MapViewToolBox& toolBox)
  : MapViewBase{appController, document, toolBox}
  , m_camera{std::make_unique<gl::PerspectiveCamera>()}
  , m_flyModeHelper{std::make_unique<FlyModeHelper>(*m_camera)}
  , m_lightPreview{std::make_unique<render::LightPreview>()}
{
  bindEvents();
  connectObservers();
  initializeCamera();
  initializeToolChain(toolBox);

  m_camera->setFov(pref(Preferences::CameraFov));

  mapViewBaseVirtualInit();
}

MapView3D::~MapView3D() = default;

const gl::PerspectiveCamera& MapView3D::perspectiveCamera() const
{
  return *m_camera;
}

void MapView3D::initializeCamera()
{
  m_camera->moveTo(vm::vec3f{-80.0f, -128.0f, 96.0f});
  m_camera->lookAt(vm::vec3f{0, 0, 0}, vm::vec3f{0, 0, 1});
}

void MapView3D::initializeToolChain(MapViewToolBox& toolBox)
{
  addToolController(std::make_unique<CameraTool3D>(*m_camera));
  addToolController(
    std::make_unique<MoveObjectsToolController>(toolBox.moveObjectsTool()));
  addToolController(std::make_unique<RotateToolController3D>(toolBox.rotateTool()));
  addToolController(std::make_unique<SweepToolController3D>(toolBox.sweepTool()));
  addToolController(std::make_unique<ScaleToolController3D>(toolBox.scaleTool()));
  addToolController(std::make_unique<ShearToolController3D>(toolBox.shearTool()));
  addToolController(std::make_unique<ExtrudeToolController3D>(toolBox.extrudeTool()));
  addToolController(
    std::make_unique<AssembleBrushToolController3D>(toolBox.assembleBrushTool()));
  addToolController(std::make_unique<ClipToolController3D>(toolBox.clipTool()));
  addToolController(std::make_unique<SplineToolController3D>(toolBox.splineTool()));
  addToolController(std::make_unique<TerrainToolController3D>(toolBox.terrainTool()));
  addToolController(std::make_unique<VertexToolController>(toolBox.vertexTool()));
  addToolController(std::make_unique<EdgeToolController>(toolBox.edgeTool()));
  addToolController(std::make_unique<FaceToolController>(toolBox.faceTool()));
  addToolController(
    std::make_unique<ControlPointToolController>(toolBox.controlPointTool()));
  addToolController(
    std::make_unique<CreateEntityToolController3D>(toolBox.createEntityTool()));
  addToolController(std::make_unique<SetBrushFaceAttributesTool>(m_document));
  addToolController(std::make_unique<SelectionTool>(m_document));
  addToolController(
    std::make_unique<DrawShapeToolController3D>(toolBox.drawShapeTool(), m_document));
}

void MapView3D::connectObservers()
{
  m_notifierConnection +=
    m_camera->cameraDidChangeNotifier.connect(this, &MapView3D::cameraDidChange);

  auto& prefs = PreferenceManager::instance();
  m_notifierConnection +=
    prefs.preferenceDidChangeNotifier.connect(this, &MapView3D::preferenceDidChange);

  // Anything that moves geometry, changes a light, hides something or reloads a texture
  // changes what the preview should show, so it starts over.
  m_notifierConnection += m_document.documentWasLoadedNotifier.connect(
    this, &MapView3D::invalidateLightPreview);
  m_notifierConnection += m_document.documentDidChangeNotifier.connect(
    this, &MapView3D::invalidateLightPreview);
  m_notifierConnection += m_document.editorContextDidChangeNotifier.connect(
    this, &MapView3D::invalidateLightPreview);
  // Reloading a collection destroys the materials the preview cached albedo for, so that
  // cache has to go with them.
  m_notifierConnection += m_document.materialCollectionsDidChangeNotifier.connect(
    this, &MapView3D::invalidateLightPreviewMaterials);
  m_notifierConnection += m_document.nodeVisibilityDidChangeNotifier.connect(
    [this](const std::vector<mdl::Node*>&) { invalidateLightPreview(); });

  // Textures and models are loaded in the background, so a map that has just been opened
  // has neither by the time the preview first looks at it. Rebuilding as they arrive is
  // what lets the preview pick up its albedo and its model geometry without being asked.
  m_notifierConnection += m_document.resourcesWereProcessedNotifier.connect(
    [this](const std::vector<gl::ResourceId>&) { invalidateLightPreview(); });
}

void MapView3D::invalidateLightPreview()
{
  m_lightPreview->invalidateScene();
}

void MapView3D::invalidateLightPreviewMaterials()
{
  m_lightPreview->invalidateMaterials();
}

void MapView3D::updateLightPreviewSettings()
{
  m_lightPreview->setEnabled(pref(Preferences::ShowLightPreview));

  const auto& quality = pref(Preferences::LightPreviewQuality);
  m_lightPreview->setQuality(
    quality == Preferences::LightPreviewQualityLow ? render::LightPreview::Quality::Low
    : quality == Preferences::LightPreviewQualityHigh
      ? render::LightPreview::Quality::High
      : render::LightPreview::Quality::Medium);

  m_lightPreview->setShowModels(pref(Preferences::LightPreviewShowModels));

  const auto& indirect = pref(Preferences::LightPreviewIndirect);
  m_lightPreview->setIndirectLight(
    indirect == Preferences::LightPreviewIndirectOn ? render::PreviewIndirectLight::On
    : indirect == Preferences::LightPreviewIndirectOff
      ? render::PreviewIndirectLight::Off
      : render::PreviewIndirectLight::FromMap);

  m_lightPreview->setMaxBounces(pref(Preferences::LightPreviewBounces));
  m_lightPreview->setExposure(pref(Preferences::LightPreviewExposure));
}

void MapView3D::releaseLightPreviewResources()
{
  if (!m_lightPreview->hasGlResources())
  {
    return;
  }

  // Freeing them needs the context that owns them to be current, and this runs while that
  // context is on its way out, so it has to be made current one last time.
  makeCurrent();
  auto gl = GlQt{getGlFunctions("MapView3D::releaseLightPreviewResources", context())};
  m_lightPreview->releaseGlResources(gl);
  doneCurrent();
}

void MapView3D::cameraDidChange(const gl::Camera& /* camera */)
{
  if (!m_ignoreCameraChangeEvents)
  {
    // Don't refresh if the camera was changed in preRender!
    update();
  }
}

void MapView3D::preferenceDidChange(const std::filesystem::path& path)
{
  if (path == Preferences::CameraFov.path)
  {
    m_camera->setFov(pref(Preferences::CameraFov));
    update();
  }
}

void MapView3D::keyPressEvent(QKeyEvent* event)
{
  m_flyModeHelper->keyDown(event);

  MapViewBase::keyPressEvent(event);
}

void MapView3D::keyReleaseEvent(QKeyEvent* event)
{
  m_flyModeHelper->keyUp(event);

  MapViewBase::keyReleaseEvent(event);
}

void MapView3D::focusInEvent(QFocusEvent* event)
{
  m_flyModeHelper->resetKeys();

  MapViewBase::focusInEvent(event);
}

void MapView3D::focusOutEvent(QFocusEvent* event)
{
  m_flyModeHelper->resetKeys();

  MapViewBase::focusOutEvent(event);
}

void MapView3D::initializeGL()
{
  MapViewBase::initializeGL();
  setCompass(std::make_unique<render::Compass3D>());

  // The preview's texture and vertex buffer belong to this context, so they have to be
  // released before it goes. The context is destroyed and made again when the view is
  // reparented, not only when the window closes, so the connection is made here rather
  // than once at construction: it follows whichever context is current.
  connect(
    context(),
    &QOpenGLContext::aboutToBeDestroyed,
    this,
    &MapView3D::releaseLightPreviewResources);
}

void MapView3D::bindEvents()
{
  // Fly mode animation
  connect(this, &QOpenGLWidget::frameSwapped, this, &MapView3D::updateFlyMode);

  // The preview refines itself on its own threads, so the view has to come back and ask
  // for the newer image. Fifteen times a second is enough to watch the noise settle
  // without spending the cores that are doing the tracing on redrawing the scene.
  m_lightPreviewTimer = new QTimer{this};
  m_lightPreviewTimer->setInterval(66);
  connect(m_lightPreviewTimer, &QTimer::timeout, this, [this]() { update(); });
}

void MapView3D::updateFlyMode()
{
  if (m_flyModeHelper->anyKeyDown())
  {
    update();
  }
}

void MapView3D::resetFlyModeKeys()
{
  m_flyModeHelper->resetKeys();
}

PickRequest MapView3D::pickRequest(const float x, const float y) const
{
  return {vm::ray3d{m_camera->pickRay(x, y)}, *m_camera};
}

mdl::PickResult MapView3D::pick(const vm::ray3d& pickRay) const
{
  auto& map = m_document.map();
  auto pickResult = mdl::PickResult::byDistance();

  mdl::pick(map, pickRay, pickResult);
  return pickResult;
}

void MapView3D::updateViewport(
  const int x, const int y, const int width, const int height)
{
  m_camera->setViewport({x, y, width, height});
}

vm::vec3d MapView3D::pasteObjectsDelta(
  const vm::bbox3d& bounds, const vm::bbox3d& /* referenceBounds */) const
{
  using namespace mdl::HitFilters;

  auto& map = m_document.map();
  const auto& grid = map.grid();

  const auto pos = QCursor::pos();
  const auto clientCoords = mapFromGlobal(pos);

  if (QRect{0, 0, width(), height()}.contains(clientCoords))
  {
    const auto pickRay =
      vm::ray3d{m_camera->pickRay(float(clientCoords.x()), float(clientCoords.y()))};
    auto pickResult = mdl::PickResult::byDistance();

    mdl::pick(map, pickRay, pickResult);

    const auto& hit = pickResult.first(type(mdl::BrushNode::BrushHitType));
    if (const auto faceHandle = mdl::hitToFaceHandle(hit))
    {
      const auto& face = faceHandle->face();
      return grid.moveDeltaForBounds(face.boundary(), bounds, map.worldBounds(), pickRay);
    }
    else
    {
      const auto point = vm::vec3d{grid.snap(m_camera->defaultPoint(pickRay))};
      const auto targetPlane = vm::plane3d{point, -vm::vec3d{m_camera->direction()}};
      return grid.moveDeltaForBounds(targetPlane, bounds, map.worldBounds(), pickRay);
    }
  }
  else
  {
    const auto oldMin = bounds.min;
    const auto oldCenter = bounds.center();
    const auto newCenter = vm::vec3d{m_camera->defaultPoint()};
    const auto newMin = oldMin + (newCenter - oldCenter);
    return grid.snap(newMin);
  }
}

bool MapView3D::canSelectTall()
{
  return false;
}

void MapView3D::selectTall() {}

void MapView3D::reset2dCameras(const gl::Camera&, const bool)
{
  // nothing to do
}

void MapView3D::focusCameraOnSelection(const bool animate)
{
  const auto& map = m_document.map();
  if (const auto& nodes = map.selection().nodes; !nodes.empty())
  {
    const auto newPosition = focusCameraOnObjectsPosition(nodes);
    moveCameraToPosition(newPosition, animate);
  }
}

namespace
{

vm::vec3f computeCameraTargetPosition(const std::vector<mdl::Node*>& nodes)
{
  auto center = vm::vec3f{};
  size_t count = 0u;

  const auto handlePoint = [&](const auto& point) {
    center = center + vm::vec3f{point};
    ++count;
  };

  mdl::Node::visitAll(
    nodes,
    kdl::overload(
      [](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::GroupNode& groupNode) {
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        if (!entityNode.hasChildren())
        {
          entityNode.logicalBounds().for_each_vertex(handlePoint);
        }
        else
        {
          entityNode.visitChildren(thisLambda);
        }
      },
      [&](mdl::BrushNode& brushNode) {
        for (const auto* vertex : brushNode.brush().vertices())
        {
          handlePoint(vertex->position());
        }
      },
      [&](mdl::PatchNode& patchNode) {
        for (const auto& controlPoint : patchNode.patch().controlPoints())
        {
          handlePoint(controlPoint.xyz());
        }
      }));

  return center / float(count);
}

float computeCameraOffset(const gl::Camera& camera, const std::vector<mdl::Node*>& nodes)
{
  vm::plane3f frustumPlanes[4];
  camera.frustumPlanes(
    frustumPlanes[0], frustumPlanes[1], frustumPlanes[2], frustumPlanes[3]);

  auto offset = std::numeric_limits<float>::min();
  const auto handlePoint = [&](const vm::vec3d& point, const vm::plane3f& plane) {
    const auto ray = vm::ray3f{camera.position(), -camera.direction()};
    const auto newPlane =
      vm::plane3f{vm::vec3f{point} + 64.0f * plane.normal, plane.normal};
    if (const auto dist = vm::intersect_ray_plane(ray, newPlane); dist && *dist > 0.0f)
    {
      offset = std::max(offset, *dist);
    }
  };

  mdl::Node::visitAll(
    nodes,
    kdl::overload(
      [](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::GroupNode& groupNode) {
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        if (!entityNode.hasChildren())
        {
          for (size_t i = 0u; i < 4u; ++i)
          {
            entityNode.logicalBounds().for_each_vertex(
              [&](const auto& point) { handlePoint(point, frustumPlanes[i]); });
          }
        }
        else
        {
          entityNode.visitChildren(thisLambda);
        }
      },
      [&](mdl::BrushNode& brushNode) {
        for (const auto* vertex : brushNode.brush().vertices())
        {
          for (size_t i = 0u; i < 4u; ++i)
          {
            handlePoint(vertex->position(), frustumPlanes[i]);
          }
        }
      },
      [&](mdl::PatchNode& patchNode) {
        for (const auto& controlPoint : patchNode.patch().controlPoints())
        {
          for (size_t i = 0u; i < 4u; ++i)
          {
            handlePoint(controlPoint.xyz(), frustumPlanes[i]);
          }
        }
      }));

  return offset;
}

} // namespace

vm::vec3f MapView3D::focusCameraOnObjectsPosition(const std::vector<mdl::Node*>& nodes)
{
  const auto newPosition = computeCameraTargetPosition(nodes);

  // act as if the camera were there already:
  const auto oldPosition = m_camera->position();
  m_camera->moveTo(vm::vec3f{newPosition});

  const auto offset = computeCameraOffset(*m_camera, nodes);

  // jump back
  m_camera->moveTo(oldPosition);
  return newPosition - m_camera->direction() * offset;
}

void MapView3D::moveCameraToPosition(const vm::vec3f& position, const bool animate)
{
  if (animate)
  {
    animateCamera(position, m_camera->direction(), m_camera->up(), m_camera->zoom());
  }
  else
  {
    m_camera->moveTo(position);
  }
}

void MapView3D::animateCamera(
  const vm::vec3f& position,
  const vm::vec3f& direction,
  const vm::vec3f& up,
  const float zoom,
  const int duration)
{
  auto animation =
    std::make_unique<CameraAnimation>(*m_camera, position, direction, up, zoom, duration);
  m_animationManager->runAnimation(std::move(animation), true);
}

void MapView3D::moveCameraToCurrentTracePoint()
{
  contract_pre(m_document.isPointFileLoaded());

  if (const auto* pointTrace = m_document.pointTrace())
  {
    const auto position = pointTrace->currentPoint() + vm::vec3f{0.0f, 0.0f, 16.0f};
    const auto direction = pointTrace->currentDirection();
    animateCamera(position, direction, vm::vec3f{0, 0, 1}, m_camera->zoom());
  }
}

gl::Camera& MapView3D::camera()
{
  return *m_camera;
}

vm::vec3d MapView3D::moveDirection(const vm::direction direction) const
{
  switch (direction)
  {
  case vm::direction::forward: {
    const auto plane = vm::plane3d{vm::vec3d{m_camera->position()}, vm::vec3d{0, 0, 1}};
    const auto projectedDirection =
      plane.project_vector(vm::vec3d{m_camera->direction()});
    if (vm::is_zero(projectedDirection, vm::Cd::almost_zero()))
    {
      // camera is looking straight down or up
      if (m_camera->direction().z() < 0.0f)
      {
        return vm::vec3d{vm::get_abs_max_component_axis(m_camera->up())};
      }
      else
      {
        return vm::vec3d{-vm::get_abs_max_component_axis(m_camera->up())};
      }
    }
    return vm::get_abs_max_component_axis(projectedDirection);
  }
  case vm::direction::backward:
    return -moveDirection(vm::direction::forward);
  case vm::direction::left:
    return -moveDirection(vm::direction::right);
  case vm::direction::right: {
    auto dir = vm::vec3d(vm::get_abs_max_component_axis(m_camera->right()));
    if (dir == moveDirection(vm::direction::forward))
    {
      dir = vm::cross(dir, vm::vec3d{0, 0, 1});
    }
    return dir;
  }
  case vm::direction::up:
    return vm::vec3d{0, 0, 1};
  case vm::direction::down:
    return vm::vec3d{0, 0, -1};
    switchDefault();
  }
}

size_t MapView3D::flipAxis(const vm::direction direction) const
{
  return vm::find_abs_max_component(moveDirection(direction));
}

vm::vec3d MapView3D::computePointEntityPosition(const vm::bbox3d& bounds) const
{
  using namespace mdl::HitFilters;

  const auto& map = m_document.map();

  const auto& grid = map.grid();
  const auto& worldBounds = map.worldBounds();

  const auto& hit = pickResult().first(type(mdl::BrushNode::BrushHitType));
  if (const auto faceHandle = mdl::hitToFaceHandle(hit))
  {
    const auto& face = faceHandle->face();
    return grid.moveDeltaForBounds(face.boundary(), bounds, worldBounds, pickRay());
  }
  else
  {
    const auto newPosition = gl::Camera::defaultPoint(pickRay());
    const auto defCenter = bounds.center();
    return grid.moveDeltaForPoint(defCenter, newPosition - defCenter);
  }
}

ActionContext::Type MapView3D::viewActionContext() const
{
  return ActionContext::View3D;
}
void MapView3D::preRender()
{
  const auto ignoreCameraUpdates = kdl::set_temp{m_ignoreCameraChangeEvents};
  m_flyModeHelper->pollAndUpdate();
}

render::RenderMode MapView3D::renderMode()
{
  return render::RenderMode::Render3D;
}

void MapView3D::renderMap(
  render::MapRenderer& renderer,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  renderer.render(renderContext, renderBatch);

  const auto& map = m_document.map();
  if (const auto& bounds = map.selectionBounds();
      bounds && renderContext.showSelectionGuide())
  {
    auto boundsRenderer = render::SelectionBoundsRenderer{*bounds};
    boundsRenderer.render(renderContext, renderBatch);

    auto* guideRenderer = new render::BoundsGuideRenderer{};
    guideRenderer->setColor(pref(Preferences::SelectionBoundsColor));
    guideRenderer->setBounds(*bounds);
    renderBatch.addOneShot(guideRenderer);
  }
}

void MapView3D::renderTools(
  MapViewToolBox& /* toolBox */,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  ToolBoxConnector::renderTools(renderContext, renderBatch);
}

void MapView3D::renderOverlay(render::RenderContext& renderContext)
{
  updateLightPreviewSettings();
  m_lightPreview->render(renderContext, vboManager(), m_document.map());

  if (m_lightPreview->needsUpdate())
  {
    if (!m_lightPreviewTimer->isActive())
    {
      m_lightPreviewTimer->start();
    }
  }
  else if (m_lightPreviewTimer->isActive())
  {
    m_lightPreviewTimer->stop();
  }
}

std::string MapView3D::overlayStatusText() const
{
  return m_lightPreview->statusText();
}

void MapView3D::beforePopupMenu()
{
  m_flyModeHelper->resetKeys();
}

void MapView3D::linkCamera(CameraLinkHelper& /* helper */) {}

} // namespace tb::ui
