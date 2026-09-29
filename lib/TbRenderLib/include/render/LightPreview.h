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

#include "base/Macros.h"
#include "gl/GlUtils.h"
#include "render/LightPreviewTracer.h"

#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace tb::gl
{
class Gl;
class VboManager;
class VertexArray;
} // namespace tb::gl

namespace tb::mdl
{
class Map;
}

namespace tb::render
{

class RenderContext;
class PreviewMaterialCache;
struct PreviewScene;
struct PreviewTraceJob;
class PreviewTraceWorkers;

/**
 * A path traced preview of the map's lighting, drawn over the 3D view.
 *
 * The preview refines itself: a pool of worker threads traces one more sample per pixel
 * at a time and the image gets less noisy the longer the view is left alone, so the
 * editor never blocks waiting for it. Moving the camera or changing the map throws the
 * accumulated image away and starts over, which is why the preview disappears the moment
 * anything moves.
 *
 * Everything the workers read is immutable and shared by value: the scene is rebuilt on
 * the render thread and handed over as a shared, const snapshot, and the camera is copied
 * into a plain struct. That is what keeps the tracer from having to lock against the
 * editor.
 */
class LightPreview
{
public:
  enum class Quality
  {
    Low,
    Medium,
    High,
  };

private:
  bool m_enabled = false;
  Quality m_quality = Quality::Medium;
  float m_exposure = 1.0f;
  bool m_showModels = false;
  PreviewIndirectLight m_indirectLight = PreviewIndirectLight::FromMap;
  int32_t m_maxBounces = 2;

  /** Set whenever the map changes, including while a scene is already being built. */
  bool m_sceneDirty = true;
  /**
   * Survives a scene rebuild, since reading every texture back from the GPU on each edit
   * would be a visible stall.
   */
  std::unique_ptr<PreviewMaterialCache> m_materialCache;
  std::shared_ptr<const PreviewScene> m_scene;
  std::future<std::shared_ptr<const PreviewScene>> m_sceneBuild;
  bool m_sceneBuilding = false;

  std::shared_ptr<PreviewTraceJob> m_job;
  std::unique_ptr<PreviewTraceWorkers> m_workers;

  GLuint m_texture = 0;
  int m_textureWidth = 0;
  int m_textureHeight = 0;
  uint64_t m_uploadedVersion = 0;
  /** The screen filling quad the preview is drawn on, kept so that its buffer is not
   * allocated and thrown away on every frame. */
  std::unique_ptr<gl::VertexArray> m_quad;

  PreviewCamera m_camera;
  /**
   * The moment the preview may start working again. Bumped whenever the view moves or
   * the map changes, so that dragging the camera around does not start a trace per frame.
   */
  std::chrono::steady_clock::time_point m_settleTime;

public:
  LightPreview();
  ~LightPreview();

  deleteCopyAndMove(LightPreview);

  bool enabled() const;
  void setEnabled(bool enabled);

  Quality quality() const;
  void setQuality(Quality quality);

  float exposure() const;
  void setExposure(float exposure);

  /** Whether entity models are traced along with the brushwork. */
  bool showModels() const;
  void setShowModels(bool showModels);

  /** Whether indirect light is computed, overriding the map's "_bounce" key. */
  PreviewIndirectLight indirectLight() const;
  void setIndirectLight(PreviewIndirectLight indirectLight);

  /**
   * How many times light may bounce, used when indirect light is forced on. Following the
   * map uses the map's own "_bounce" count instead.
   */
  int32_t maxBounces() const;
  void setMaxBounces(int32_t maxBounces);

  /**
   * Throws away the scene, so that the next frame collects the map's geometry and lights
   * again. Call this when anything in the map changes.
   */
  void invalidateScene();

  /**
   * Throws away the cached albedo as well as the scene. Call this when the material
   * collections change, since the cache is keyed by material address.
   */
  void invalidateMaterials();

  /**
   * Whether the preview still has work to do, and so whether the view should keep asking
   * for frames.
   */
  bool needsUpdate() const;

  /** A short description of what the preview is doing, for the heads up display. */
  std::string statusText() const;

  /**
   * Advances the preview and draws it over the view.
   *
   * Must be called on the thread holding the GL context, and last, after everything else
   * in the view has been drawn.
   */
  void render(
    RenderContext& renderContext, gl::VboManager& vboManager, const mdl::Map& map);

  /** Whether the preview currently holds anything that has to be freed with a context. */
  bool hasGlResources() const;

  /**
   * Releases the preview's GL objects. Must be called while the context that owns them is
   * still current.
   */
  void releaseGlResources(gl::Gl& gl);

private:
  void cancelJob();
  void updateTexture(gl::Gl& gl);
  void renderOverlay(RenderContext& renderContext, gl::VboManager& vboManager);
  PreviewTraceSettings traceSettings() const;
  int resolutionDivisor() const;
};

} // namespace tb::render
