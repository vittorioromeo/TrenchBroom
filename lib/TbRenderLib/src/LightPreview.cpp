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

#include "render/LightPreview.h"

#include "gl/ActiveShader.h"
#include "gl/Camera.h"
#include "gl/GlInterface.h"
#include "gl/PrimType.h"
#include "gl/Shaders.h"
#include "gl/VertexArray.h"
#include "gl/VertexType.h"
#include "render/LightPreviewScene.h"
#include "render/RenderContext.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tb::render
{
namespace
{

/**
 * Tiles are the unit of work. Small enough that the threads stay balanced on a view where
 * one corner is much more expensive than the other, large enough that claiming one is not
 * itself the cost.
 */
constexpr auto TileSize = 32;

/** Where the preview stops refining, well past the point where more is visible. */
constexpr auto MaxPasses = uint32_t(1024);

/** The largest side an albedo texture is kept at. */
constexpr auto MaxAlbedoTextureSize = size_t(64);

/** The widest the preview is traced at, whatever the view's own size. */
constexpr auto MaxPreviewWidth = 1280;
constexpr auto MaxPreviewHeight = 800;

/**
 * How long the view has to hold still before the preview starts working.
 *
 * Without this, dragging the camera would rebuild and restart the trace on every frame,
 * which costs more than it shows.
 */
constexpr auto SettleDelay = std::chrono::milliseconds{150};

/** The image fades in over the first few passes rather than snapping on at full noise. */
constexpr auto FadeInPasses = 6.0f;

} // namespace

/**
 * One progressive render: an immutable scene and camera, the running sums, and the image
 * the render thread uploads.
 */
struct PreviewTraceJob
{
  std::shared_ptr<const PreviewScene> scene;
  PreviewCamera camera;
  PreviewTraceSettings settings;
  float gamma = 1.0f;

  int width = 0;
  int height = 0;
  int tilesX = 0;
  int tilesY = 0;

  /** Running sum of radiance per pixel; divided by the tile's pass count to display. */
  std::vector<vm::vec3f> accumulation;

  /**
   * The next pass each tile is ready for. Work items are handed out in pass order, so a
   * worker that finds a tile not yet ready is waiting on a pass another worker has
   * already claimed and will finish.
   */
  std::vector<std::atomic<uint32_t>> tilePasses;

  std::mutex displayMutex;
  std::vector<uint8_t> display;
  std::atomic<uint64_t> displayVersion = 0;

  std::atomic<uint64_t> nextWorkItem = 0;
  std::atomic<bool> cancelled = false;

  PreviewTraceJob(
    std::shared_ptr<const PreviewScene> i_scene,
    const PreviewCamera& i_camera,
    const PreviewTraceSettings& i_settings,
    const int i_width,
    const int i_height)
    : scene{std::move(i_scene)}
    , camera{i_camera}
    , settings{i_settings}
    , gamma{scene->globals.gamma}
    , width{i_width}
    , height{i_height}
    , tilesX{(i_width + TileSize - 1) / TileSize}
    , tilesY{(i_height + TileSize - 1) / TileSize}
    , accumulation(size_t(i_width) * size_t(i_height))
    , tilePasses(size_t(tilesX) * size_t(tilesY))
    , display(size_t(i_width) * size_t(i_height) * 4, uint8_t(0))
  {
  }

  size_t tileCount() const { return size_t(tilesX) * size_t(tilesY); }

  bool exhausted() const
  {
    return cancelled.load(std::memory_order_relaxed)
           || nextWorkItem.load(std::memory_order_relaxed)
                >= uint64_t(MaxPasses) * uint64_t(tileCount());
  }

  /** The fewest passes any tile has finished, which is what the fade in follows. */
  uint32_t minimumPasses() const
  {
    auto result = MaxPasses;
    for (const auto& tilePass : tilePasses)
    {
      result = std::min(result, tilePass.load(std::memory_order_relaxed));
    }
    return result;
  }
};

namespace
{

void traceTile(PreviewTraceJob& job, const uint32_t pass, const uint32_t tile)
{
  // A tile's passes must be applied in order, since each adds to the same running sum.
  // Work is claimed in pass order, so the pass this one waits for is already under way.
  while (job.tilePasses[tile].load(std::memory_order_acquire) != pass)
  {
    if (job.cancelled.load(std::memory_order_relaxed))
    {
      return;
    }
    std::this_thread::yield();
  }

  const auto x0 = int(tile % uint32_t(job.tilesX)) * TileSize;
  const auto y0 = int(tile / uint32_t(job.tilesX)) * TileSize;
  const auto x1 = std::min(x0 + TileSize, job.width);
  const auto y1 = std::min(y0 + TileSize, job.height);

  for (auto y = y0; y < y1; ++y)
  {
    for (auto x = x0; x < x1; ++x)
    {
      const auto radiance =
        tracePreviewPixel(*job.scene, job.camera, job.settings, x, y, pass);

      auto& accumulated = job.accumulation[size_t(y) * size_t(job.width) + size_t(x)];
      accumulated = pass == 0 ? radiance : accumulated + radiance;
    }
  }

  const auto scale = 1.0f / float(pass + 1);
  const auto inverseGamma = 1.0f / job.gamma;

  {
    const auto lock = std::lock_guard{job.displayMutex};
    for (auto y = y0; y < y1; ++y)
    {
      for (auto x = x0; x < x1; ++x)
      {
        const auto index = size_t(y) * size_t(job.width) + size_t(x);
        const auto color = job.accumulation[index] * scale;

        auto* texel = &job.display[index * 4];
        for (size_t i = 0; i < 3; ++i)
        {
          auto value = std::clamp(color[i], 0.0f, 1.0f);
          if (job.gamma != 1.0f)
          {
            value = std::pow(value, inverseGamma);
          }
          texel[i] = uint8_t(std::lround(value * 255.0f));
        }
        texel[3] = 255;
      }
    }
  }

  job.tilePasses[tile].store(pass + 1, std::memory_order_release);
  job.displayVersion.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

/**
 * The thread pool the preview traces on.
 *
 * The threads outlive individual renders: submitting a new job cancels the old one and
 * wakes them, rather than starting and joining threads every time the camera moves.
 */
class PreviewTraceWorkers
{
private:
  std::vector<std::thread> m_threads;
  std::mutex m_mutex;
  std::condition_variable m_condition;
  std::shared_ptr<PreviewTraceJob> m_job;
  bool m_shutdown = false;

public:
  PreviewTraceWorkers()
  {
    // Leave a core to the editor: a preview that makes the rest of the program stutter is
    // worse than one that converges a little more slowly.
    const auto hardwareThreads = int(std::thread::hardware_concurrency());
    const auto threadCount = std::max(1, hardwareThreads > 0 ? hardwareThreads - 1 : 1);

    m_threads.reserve(size_t(threadCount));
    for (auto i = 0; i < threadCount; ++i)
    {
      m_threads.emplace_back([this]() { run(); });
    }
  }

  ~PreviewTraceWorkers()
  {
    {
      const auto lock = std::lock_guard{m_mutex};
      if (m_job)
      {
        m_job->cancelled.store(true, std::memory_order_relaxed);
      }
      m_shutdown = true;
    }
    m_condition.notify_all();

    for (auto& thread : m_threads)
    {
      thread.join();
    }
  }

  deleteCopyAndMove(PreviewTraceWorkers);

  void submit(std::shared_ptr<PreviewTraceJob> job)
  {
    {
      const auto lock = std::lock_guard{m_mutex};
      if (m_job)
      {
        m_job->cancelled.store(true, std::memory_order_relaxed);
      }
      m_job = std::move(job);
    }
    m_condition.notify_all();
  }

  void cancel()
  {
    {
      const auto lock = std::lock_guard{m_mutex};
      if (m_job)
      {
        m_job->cancelled.store(true, std::memory_order_relaxed);
      }
      m_job = nullptr;
    }
    m_condition.notify_all();
  }

private:
  void run()
  {
    while (true)
    {
      auto job = std::shared_ptr<PreviewTraceJob>{};

      {
        auto lock = std::unique_lock{m_mutex};
        m_condition.wait(
          lock, [&]() { return m_shutdown || (m_job && !m_job->exhausted()); });
        if (m_shutdown)
        {
          return;
        }
        job = m_job;
      }

      const auto tileCount = uint64_t(job->tileCount());
      while (!job->cancelled.load(std::memory_order_relaxed))
      {
        const auto item = job->nextWorkItem.fetch_add(1, std::memory_order_relaxed);
        const auto pass = uint32_t(item / tileCount);
        if (pass >= MaxPasses)
        {
          break;
        }

        traceTile(*job, pass, uint32_t(item % tileCount));
      }
    }
  }
};

LightPreview::LightPreview()
  : m_materialCache{std::make_unique<PreviewMaterialCache>()}
{
}

LightPreview::~LightPreview()
{
  // Cancels the running job and joins the threads. The GL texture cannot be released
  // here, since there is no telling whether a context is current; releaseGlResources does
  // that while the view still has one.
  m_workers.reset();
}

bool LightPreview::enabled() const
{
  return m_enabled;
}

void LightPreview::setEnabled(const bool enabled)
{
  if (m_enabled == enabled)
  {
    return;
  }

  m_enabled = enabled;
  if (!m_enabled)
  {
    cancelJob();
    m_scene.reset();
    m_sceneDirty = true;

    // Nothing is going to be traced until the preview is switched back on, so give the
    // threads back rather than leaving a core's worth of them parked on a condition
    // variable for the rest of the session.
    m_workers.reset();
  }
  else
  {
    invalidateScene();
  }
}

LightPreview::Quality LightPreview::quality() const
{
  return m_quality;
}

void LightPreview::setQuality(const Quality quality)
{
  if (m_quality != quality)
  {
    m_quality = quality;
    cancelJob();
  }
}

float LightPreview::exposure() const
{
  return m_exposure;
}

void LightPreview::setExposure(const float exposure)
{
  if (m_exposure != exposure)
  {
    m_exposure = exposure;
    cancelJob();
  }
}

bool LightPreview::showModels() const
{
  return m_showModels;
}

void LightPreview::setShowModels(const bool showModels)
{
  if (m_showModels != showModels)
  {
    m_showModels = showModels;

    // This changes what is in the scene rather than how it is traced, so the geometry has
    // to be collected again.
    invalidateScene();
  }
}

PreviewIndirectLight LightPreview::indirectLight() const
{
  return m_indirectLight;
}

void LightPreview::setIndirectLight(const PreviewIndirectLight indirectLight)
{
  if (m_indirectLight != indirectLight)
  {
    m_indirectLight = indirectLight;

    // This changes how the scene is traced rather than what is in it, so the trace starts
    // over but the geometry is kept.
    cancelJob();
  }
}

int32_t LightPreview::maxBounces() const
{
  return m_maxBounces;
}

void LightPreview::setMaxBounces(const int32_t maxBounces)
{
  const auto clamped = std::clamp(maxBounces, 0, MaxPreviewBounces);
  if (m_maxBounces != clamped)
  {
    m_maxBounces = clamped;
    cancelJob();
  }
}

void LightPreview::invalidateMaterials()
{
  m_materialCache->clear();
  invalidateScene();
}

void LightPreview::invalidateScene()
{
  m_sceneDirty = true;
  cancelJob();
  m_settleTime = std::chrono::steady_clock::now() + SettleDelay;
}

bool LightPreview::needsUpdate() const
{
  if (!m_enabled)
  {
    return false;
  }

  if (m_sceneDirty || m_sceneBuilding || !m_job)
  {
    return true;
  }

  return !m_job->exhausted();
}

std::string LightPreview::statusText() const
{
  if (!m_enabled)
  {
    return {};
  }

  if (m_sceneBuilding || m_sceneDirty || !m_scene)
  {
    return "Light preview: preparing";
  }

  if (!m_job)
  {
    return "Light preview: waiting";
  }

  const auto passes = m_job->minimumPasses();
  auto result = passes >= MaxPasses
                  ? std::string{"Light preview: converged"}
                  : "Light preview: " + std::to_string(passes) + " samples";

  // Say what the bounce settings came out as. Indirect light is a small effect next to
  // the light that arrives directly, so without this there is no way to tell a setting
  // that is doing very little from one that never took.
  const auto bounces = effectiveBounces(*m_scene, m_job->settings);
  result += bounces == 0   ? ", direct light only"
            : bounces == 1 ? ", 1 bounce"
                           : ", " + std::to_string(bounces) + " bounces";
  return result;
}

int LightPreview::resolutionDivisor() const
{
  switch (m_quality)
  {
  case Quality::Low:
    return 4;
  case Quality::High:
    return 1;
  case Quality::Medium:
    break;
  }
  return 2;
}

PreviewTraceSettings LightPreview::traceSettings() const
{
  auto settings = PreviewTraceSettings{};
  settings.exposure = m_exposure;
  settings.indirectLight = m_indirectLight;

  // How far light bounces is its own setting: it is about what the lighting does, not
  // about how much work the preview puts into resolving it, which is what quality means.
  // The tracer only reads it when indirect light is forced on.
  settings.maxBounces = m_maxBounces;

  switch (m_quality)
  {
  case Quality::Low:
    settings.maxShadowRays = 4;
    break;
  case Quality::Medium:
    settings.maxShadowRays = 8;
    break;
  case Quality::High:
    settings.maxShadowRays = 16;
    break;
  }

  return settings;
}

void LightPreview::cancelJob()
{
  if (m_workers)
  {
    m_workers->cancel();
  }
  m_job.reset();
}

void LightPreview::render(
  RenderContext& renderContext, gl::VboManager& vboManager, const mdl::Map& map)
{
  auto& gl = renderContext.gl();

  if (!m_enabled)
  {
    releaseGlResources(gl);
    return;
  }

  const auto& camera = renderContext.camera();
  const auto& viewport = camera.viewport();
  const auto divisor = resolutionDivisor();
  auto width = std::max(viewport.width / divisor, 1);
  auto height = std::max(viewport.height / divisor, 1);

  // Cap the work a very wide or very tall view asks for, scaling both sides by the same
  // amount so that the preview's pixels stay square.
  if (const auto excess = std::max(
        float(width) / float(MaxPreviewWidth), float(height) / float(MaxPreviewHeight));
      excess > 1.0f)
  {
    width = std::max(int(float(width) / excess), 1);
    height = std::max(int(float(height) / excess), 1);
  }

  const auto snapshot = makePreviewCamera(camera, width, height);
  if (snapshot != m_camera)
  {
    m_camera = snapshot;
    cancelJob();
    m_settleTime = std::chrono::steady_clock::now() + SettleDelay;
  }

  if (std::chrono::steady_clock::now() < m_settleTime)
  {
    return;
  }

  if (m_sceneBuilding)
  {
    if (m_sceneBuild.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
    {
      return;
    }

    auto scene = m_sceneBuild.get();
    m_sceneBuilding = false;

    // The map may have changed again while this was being built, in which case what came
    // back describes a map that no longer exists and is thrown away rather than shown.
    if (!m_sceneDirty)
    {
      m_scene = std::move(scene);
      m_job.reset();
    }
  }

  if (m_sceneDirty && !m_sceneBuilding)
  {
    cancelJob();
    m_scene.reset();

    // Collecting the map's geometry has to happen here, on the thread that owns the GL
    // context and while the editor is not touching the map. Building the hierarchy over
    // it does not, and it is the expensive half, so it goes to a worker.
    const auto options = PreviewSceneOptions{
      .maxTextureSize = MaxAlbedoTextureSize,
      .includeEntityModels = m_showModels,
    };
    auto scene = std::make_unique<PreviewScene>(
      buildPreviewScene(map, gl, *m_materialCache, options));
    m_sceneBuild = std::async(
      std::launch::async,
      [scene = std::move(scene)]() mutable -> std::shared_ptr<const PreviewScene> {
        buildPreviewSceneBvh(*scene);
        return std::shared_ptr<const PreviewScene>{std::move(scene)};
      });
    m_sceneBuilding = true;
    m_sceneDirty = false;
    return;
  }

  if (!m_scene || m_scene->empty())
  {
    return;
  }

  if (!m_job)
  {
    if (!m_workers)
    {
      m_workers = std::make_unique<PreviewTraceWorkers>();
    }

    m_job = std::make_shared<PreviewTraceJob>(
      m_scene, m_camera, traceSettings(), width, height);
    m_workers->submit(m_job);
    return;
  }

  updateTexture(gl);
  renderOverlay(renderContext, vboManager);
}

void LightPreview::updateTexture(gl::Gl& gl)
{
  const auto version = m_job->displayVersion.load(std::memory_order_relaxed);
  const auto sizeChanged =
    m_textureWidth != m_job->width || m_textureHeight != m_job->height;

  if (m_texture != 0 && !sizeChanged && version == m_uploadedVersion)
  {
    return;
  }

  if (m_texture == 0)
  {
    gl.genTextures(1, &m_texture);
  }

  gl.bindTexture(GL_TEXTURE_2D, m_texture);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  {
    const auto lock = std::lock_guard{m_job->displayMutex};
    gl.texImage2D(
      GL_TEXTURE_2D,
      0,
      GL_RGBA,
      m_job->width,
      m_job->height,
      0,
      GL_RGBA,
      GL_UNSIGNED_BYTE,
      m_job->display.data());
  }

  gl.bindTexture(GL_TEXTURE_2D, 0);

  m_textureWidth = m_job->width;
  m_textureHeight = m_job->height;
  m_uploadedVersion = version;
}

void LightPreview::renderOverlay(RenderContext& renderContext, gl::VboManager& vboManager)
{
  const auto passes = m_job->minimumPasses();
  if (passes == 0 || m_texture == 0)
  {
    return;
  }

  const auto alpha = std::min(1.0f, float(passes) / FadeInPasses);

  auto& gl = renderContext.gl();

  if (!m_quad)
  {
    using Vertex = gl::VertexTypes::P2Uv2::Vertex;

    // The quad is written straight in clip space, so the preview covers the view whatever
    // transformation the scene was drawn with.
    //
    // The vertical texture coordinates are flipped because the two sides disagree about
    // where an image starts: the tracer writes the top row of the view first, while a
    // texture's first row is the one at v = 0, which lands at the bottom of the screen.
    m_quad = std::make_unique<gl::VertexArray>(gl::VertexArray::move(std::vector<Vertex>{
      Vertex{{-1.0f, -1.0f}, {0.0f, 1.0f}},
      Vertex{{1.0f, -1.0f}, {1.0f, 1.0f}},
      Vertex{{1.0f, 1.0f}, {1.0f, 0.0f}},
      Vertex{{-1.0f, 1.0f}, {0.0f, 0.0f}},
    }));
  }

  // The map renderer leaves back face culling on with a clockwise front face, and a
  // screen filling quad has no facing worth speaking of, so culling comes off for the
  // draw. It has to go back on afterwards, since the focus indicator is drawn after the
  // view's contents and is wound to match.
  gl.pushAttrib(GL_POLYGON_BIT);
  gl.disable(GL_CULL_FACE);
  gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);

  gl.disable(GL_DEPTH_TEST);
  gl.depthMask(GL_FALSE);
  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  {
    auto shader = gl::ActiveShader{
      gl, renderContext.shaderManager(), gl::Shaders::LightPreviewShader};
    shader.set("Image", 0);
    shader.set("Alpha", alpha);

    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D, m_texture);

    m_quad->prepare(gl, vboManager);
    if (m_quad->setup(gl, shader.program()))
    {
      m_quad->render(gl, gl::PrimType::Quads);
      m_quad->cleanup(gl, shader.program());
    }

    gl.bindTexture(GL_TEXTURE_2D, 0);
  }

  gl.depthMask(GL_TRUE);
  gl.enable(GL_DEPTH_TEST);
  gl.popAttrib();
}

bool LightPreview::hasGlResources() const
{
  return m_texture != 0 || m_quad != nullptr;
}

void LightPreview::releaseGlResources(gl::Gl& gl)
{
  m_quad.reset();

  if (m_texture != 0)
  {
    gl.deleteTextures(1, &m_texture);
    m_texture = 0;
    m_textureWidth = 0;
    m_textureHeight = 0;
    m_uploadedVersion = 0;
  }
}

} // namespace tb::render
