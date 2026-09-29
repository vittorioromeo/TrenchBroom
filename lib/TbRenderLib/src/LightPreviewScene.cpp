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

#include "render/LightPreviewScene.h"

#include "base/PreferenceManager.h"
#include "prefs/Preferences.h"
#include "gl/GlInterface.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/Tag.h"
#include "mdl/TagAttribute.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include "vm/constants.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tb::render
{
namespace
{

bool startsWithIgnoringCase(const std::string_view str, const std::string_view prefix)
{
  return str.size() >= prefix.size()
         && std::equal(
           prefix.begin(), prefix.end(), str.begin(), [](const char a, const char b) {
             return std::tolower(static_cast<unsigned char>(a))
                    == std::tolower(static_cast<unsigned char>(b));
           });
}

std::string toLower(std::string_view str)
{
  auto result = std::string{str};
  std::transform(result.begin(), result.end(), result.begin(), [](const char c) {
    return char(std::tolower(static_cast<unsigned char>(c)));
  });
  return result;
}

/**
 * The name a texture is known by inside the map, with any directory prefix removed. A
 * Quake face names its texture directly, but a Quake 3 shader is named by its path, and
 * light entities refer to the last component either way.
 */
std::string materialBaseName(const std::string& name)
{
  const auto slash = name.find_last_of("/\\");
  return toLower(slash == std::string::npos ? name : name.substr(slash + 1));
}

/**
 * Decides whether a face is one the game never draws.
 *
 * The entity definitions cannot answer this: an FGD says whether a class is solid or a
 * point, and what properties it takes, but nothing about whether it is drawn. What it
 * does give is the classname, and every Quake family game names its trigger classes the
 * same way, which is why the game configs can tag them by pattern. The rest comes from
 * the game config's own surface and content flags, from Quake 3 surface parameters, and
 * from the handful of texture names the whole family uses for faces that exist only for
 * the compiler.
 *
 * These faces are left out of the scene altogether rather than made invisible: they are
 * not drawn, they do not block light, and they must not colour the light that bounces
 * around them either.
 *
 * NOTE: the texture names below are conventions rather than anything a game states, so
 * this is one of the places to look first when a game previews something it should not.
 */
class HiddenFaceClassifier
{
private:
  int m_surfaceFlags = 0;
  int m_contentFlags = 0;

public:
  explicit HiddenFaceClassifier(const mdl::GameConfig& gameConfig)
  {
    const auto& faceAttribs = gameConfig.faceAttribsConfig;
    for (const auto* flag : {"nodraw", "hint", "skip"})
    {
      m_surfaceFlags |= faceAttribs.surfaceFlags.flagValue(flag);
    }
    for (const auto* flag : {"playerclip", "monsterclip", "origin"})
    {
      m_contentFlags |= faceAttribs.contentFlags.flagValue(flag);
    }
  }

  bool isHidden(
    const gl::Material* material,
    const std::string& materialName,
    const int surfaceFlags,
    const int contentFlags) const
  {
    if (
      (m_surfaceFlags != 0 && (surfaceFlags & m_surfaceFlags) != 0)
      || (m_contentFlags != 0 && (contentFlags & m_contentFlags) != 0))
    {
      return true;
    }

    if (material)
    {
      for (const auto* parm :
           {"nodraw", "hint", "skip", "trigger", "areaportal", "clusterportal"})
      {
        if (material->surfaceParms().contains(parm))
        {
          return true;
        }
      }
    }

    // The same patterns the game configs match these by, spelled out so that the rule
    // still holds for a game whose config does not carry the tag.
    const auto name = materialBaseName(materialName);
    static const auto exactNames = std::unordered_set<std::string>{
      "aaatrigger",
      "areaportal",
      "clusterportal",
      "donotenter",
      "nodraw",
      "null",
      "origin",
      "skip",
      "trigger",
    };

    return exactNames.contains(name) || name.ends_with("clip") || name.starts_with("hint")
           || name.find("caulk") != std::string::npos;
  }
};

/**
 * Whether a brush entity is one the game never draws.
 *
 * Triggers are the case that matters. The classname is the only thing the entity
 * definitions offer here, but it is enough: every game in the family spells its trigger
 * classes the same way, which is why the game configs tag them by that same pattern.
 */
bool isHiddenBrushEntity(const mdl::EntityNodeBase* entityNode)
{
  return entityNode
         && startsWithIgnoringCase(entityNode->entity().classname(), "trigger");
}

/**
 * Whether a texture name marks a liquid.
 *
 * Quake names its liquid textures with a leading asterisk, and the compilers never put
 * those in the solid hull, so light crosses them as if they were not there.
 */
bool isLiquidMaterialName(const std::string& name)
{
  return materialBaseName(name).starts_with("*");
}

/**
 * Whether a face is sky.
 *
 * There is no portable answer, so all three conventions are tried: the Quake 2 style
 * surface flag named "sky" in the game config, the Quake 3 shader surface parameter, and
 * the plain Quake and GoldSrc convention of naming the texture "sky" something. A ray
 * that hits sky leaves the map rather than being shaded, so a false positive here would
 * punch a hole in the preview; all three tests are deliberately narrow.
 *
 * NOTE: sky faces are treated as a plain hole into the sky dome. Games lay out sky
 * textures in their own way, and none of that is reproduced here.
 */
struct SkyClassifier
{
  int skySurfaceFlag = 0;

  explicit SkyClassifier(const mdl::GameConfig& gameConfig)
    : skySurfaceFlag{gameConfig.faceAttribsConfig.surfaceFlags.flagValue("sky")}
  {
  }

  bool isSky(const gl::Material* material, const int surfaceFlags) const
  {
    if (skySurfaceFlag != 0 && (surfaceFlags & skySurfaceFlag) != 0)
    {
      return true;
    }

    if (!material)
    {
      return false;
    }

    if (material->surfaceParms().contains("sky"))
    {
      return true;
    }

    return materialBaseName(material->name()).starts_with("sky");
  }
};

/** Below this the texture is a hole rather than a picture. */
constexpr auto TransparentAlpha = uint8_t(128);

/**
 * Reads back one material's texture so that the tracer can look up albedo without the GL
 * context.
 *
 * The image is boxed down to at most maxSize on a side. The preview is far too noisy for
 * texture detail to survive, and a map's worth of full resolution images is a lot of
 * memory to keep resident for no visible gain.
 */
PreviewMaterial readMaterial(
  const gl::Material& material, gl::Gl& gl, const size_t maxSize)
{
  auto result = PreviewMaterial{};
  result.name = material.name();

  const auto* texture = gl::getTexture(&material);
  if (!texture)
  {
    return result;
  }

  result.averageColor = vm::vec3f{texture->averageColor().to<RgbF>().toVec()};

  const auto sourceWidth = texture->width();
  const auto sourceHeight = texture->height();
  if (sourceWidth == 0 || sourceHeight == 0)
  {
    return result;
  }

  auto pixels = std::vector<unsigned char>{};
  if (texture->activate(gl, GL_NEAREST, GL_NEAREST))
  {
    // Reading as RGBA bytes makes every row a multiple of four bytes long, which is what
    // the default pack alignment expects, and lets the driver convert whatever internal
    // format the texture has, compressed ones included.
    pixels.resize(sourceWidth * sourceHeight * 4);
    gl.getTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    texture->deactivate(gl);
  }
  else
  {
    // The texture has not been uploaded yet, which happens for materials on faces the
    // editor has not drawn. The loaded buffers are still around in that case.
    const auto& buffers = texture->buffersIfLoaded();
    if (buffers.empty())
    {
      return result;
    }

    const auto format = texture->format();
    const auto components = format == GL_RGBA  ? size_t(4)
                            : format == GL_RGB ? size_t(3)
                                               : size_t(0);
    if (
      components == 0 || buffers.front().size() < sourceWidth * sourceHeight * components)
    {
      return result;
    }

    const auto* source = buffers.front().data();
    pixels.resize(sourceWidth * sourceHeight * 4);
    for (size_t i = 0; i < sourceWidth * sourceHeight; ++i)
    {
      pixels[i * 4 + 0] = source[i * components + 0];
      pixels[i * 4 + 1] = source[i * components + 1];
      pixels[i * 4 + 2] = source[i * components + 2];
      pixels[i * 4 + 3] = components == 4 ? source[i * components + 3] : 255;
    }
  }

  if (pixels.empty())
  {
    return result;
  }

  const auto targetWidth = std::max(size_t(1), std::min(sourceWidth, maxSize));
  const auto targetHeight = std::max(size_t(1), std::min(sourceHeight, maxSize));

  result.width = targetWidth;
  result.height = targetHeight;
  result.texels.resize(targetWidth * targetHeight);
  result.opaque.assign(targetWidth * targetHeight, uint8_t(1));

  for (size_t y = 0; y < targetHeight; ++y)
  {
    const auto sourceY0 = y * sourceHeight / targetHeight;
    const auto sourceY1 = std::max(sourceY0 + 1, (y + 1) * sourceHeight / targetHeight);

    for (size_t x = 0; x < targetWidth; ++x)
    {
      const auto sourceX0 = x * sourceWidth / targetWidth;
      const auto sourceX1 = std::max(sourceX0 + 1, (x + 1) * sourceWidth / targetWidth);

      // Only the opaque source texels contribute a colour: letting a see-through one in
      // would drag the colour towards whatever is stored behind the hole, which for a
      // masked Quake texture is the palette's transparent index and nothing like the
      // picture.
      auto sum = vm::vec3f{0, 0, 0};
      auto opaqueCount = 0.0f;
      auto totalCount = 0.0f;
      for (auto sy = sourceY0; sy < sourceY1; ++sy)
      {
        for (auto sx = sourceX0; sx < sourceX1; ++sx)
        {
          const auto* texel = &pixels[(sy * sourceWidth + sx) * 4];
          totalCount += 1.0f;
          if (texel[3] >= TransparentAlpha)
          {
            sum = sum
                  + vm::vec3f{
                    float(texel[0]) / 255.0f,
                    float(texel[1]) / 255.0f,
                    float(texel[2]) / 255.0f};
            opaqueCount += 1.0f;
          }
        }
      }

      result.texels[y * targetWidth + x] =
        opaqueCount > 0.0f ? sum / opaqueCount : result.averageColor;

      // The preview works at a lower resolution than the texture, so a texel here can
      // cover both hole and picture. It counts as a hole when most of what it covers is
      // one, which keeps a silhouette close to the shape the texture actually has.
      if (opaqueCount * 2.0f < totalCount)
      {
        result.opaque[y * targetWidth + x] = 0;
        result.masked = true;
      }
    }
  }

  if (!result.masked)
  {
    // Nothing to look up per texel, so the mask costs no memory and no test.
    result.opaque.clear();
  }

  return result;
}

/**
 * Binds the material cache to the GL context and resolution the current build is using,
 * so that collecting geometry only has to ask for an index.
 */
struct MaterialLookup
{
  PreviewMaterialCache& cache;
  gl::Gl& gl;
  size_t maxTextureSize;

  uint32_t indexOf(const gl::Material* material) const
  {
    return cache.indexOf(material, gl, maxTextureSize);
  }

  const PreviewMaterial& at(const uint32_t index) const { return cache.at(index); }

  /** Looks the material up and records on the shading what a ray needs to know. */
  void assignTo(const gl::Material* material, PreviewTriangleShading& shading) const
  {
    shading.materialIndex = indexOf(material);
    shading.maskedTexture = at(shading.materialIndex).masked;
  }
};

/**
 * The lighting keys a brush model can carry, which apply to every face of that model.
 */
struct BrushModelLighting
{
  vm::vec3f minLight = vm::vec3f{0, 0, 0};
  int32_t objectChannelMask = 1;
  bool castsShadows = true;
  bool receivesLight = true;
};

std::vector<float> parsePropertyFloats(const std::string& str)
{
  auto result = std::vector<float>{};
  const auto* begin = str.c_str();
  const auto* const end = begin + str.size();

  while (begin != end)
  {
    char* next = nullptr;
    const auto value = std::strtof(begin, &next);
    if (next == begin)
    {
      ++begin;
      continue;
    }
    result.push_back(value);
    begin = next;
  }

  return result;
}

BrushModelLighting readBrushModelLighting(const mdl::EntityNodeBase* entityNode)
{
  auto result = BrushModelLighting{};
  if (!entityNode)
  {
    return result;
  }

  const auto& entity = entityNode->entity();
  const auto number = [&](const std::string& key) -> std::optional<float> {
    if (const auto* value = entity.property(key))
    {
      const auto numbers = parsePropertyFloats(*value);
      if (!numbers.empty())
      {
        return numbers.front();
      }
    }
    return std::nullopt;
  };

  if (const auto minLight = number("_minlight"))
  {
    auto color = vm::vec3f{1, 1, 1};
    for (const auto* key : {"_minlight_color", "_mincolor"})
    {
      if (const auto* value = entity.property(std::string{key}))
      {
        const auto numbers = parsePropertyFloats(*value);
        if (numbers.size() >= 3)
        {
          const auto raw = vm::vec3f{numbers[0], numbers[1], numbers[2]};
          const auto maximum = std::max({raw.x(), raw.y(), raw.z()});
          color = maximum > 1.0f ? raw / 255.0f : raw;
          break;
        }
      }
    }
    result.minLight = color * *minLight;
  }

  if (const auto mask = number("_object_channel_mask"))
  {
    result.objectChannelMask = int32_t(*mask);
  }

  // A brush model is a separate model in the BSP, and the compilers do not trace against
  // it unless "_shadow" "1" asks them to; the world itself, and the brushes merged into
  // it by func_detail and func_group, always cast unless "_shadow" "-1" says otherwise.
  const auto& classname = entityNode->entity().classname();
  const auto partOfWorld = classname == mdl::EntityPropertyValues::WorldspawnClassname
                           || startsWithIgnoringCase(classname, "func_detail")
                           || startsWithIgnoringCase(classname, "func_group");

  const auto shadow = number("_shadow").value_or(0.0f);
  result.castsShadows = shadow > 0.0f ? true : shadow < 0.0f ? false : partOfWorld;

  if (const auto lightIgnore = number("_lightignore"))
  {
    result.receivesLight = *lightIgnore == 0.0f;
  }

  return result;
}

struct TriangleSink
{
  PreviewScene& scene;
  std::optional<vm::bbox3f> bounds;

  void add(
    const vm::vec3f& p0,
    const vm::vec3f& p1,
    const vm::vec3f& p2,
    const vm::vec2f& uv0,
    const vm::vec2f& uv1,
    const vm::vec2f& uv2,
    const vm::vec3f& normal,
    const PreviewTriangleShading& prototype)
  {
    const auto e1 = p1 - p0;
    const auto e2 = p2 - p0;

    // Degenerate triangles have no surface to shade and would divide by zero in the
    // intersection test, so they never make it into the hierarchy.
    const auto cross = vm::cross(e1, e2);
    if (vm::squared_length(cross) < 1.0e-12f)
    {
      return;
    }

    auto shading = prototype;
    shading.normal = normal;
    shading.uv0 = uv0;
    shading.uv1 = uv1;
    shading.uv2 = uv2;

    scene.trianglePositions.push_back(PreviewTrianglePos{p0, e1, e2});
    scene.triangleShading.push_back(shading);

    for (const auto& point : {p0, p1, p2})
    {
      bounds = bounds ? vm::merge(*bounds, point) : vm::bbox3f{point};
    }
  }
};

void addBrushFace(
  const mdl::BrushNode& brushNode,
  const mdl::BrushFace& face,
  const BrushModelLighting& brushModel,
  const SkyClassifier& skyClassifier,
  const HiddenFaceClassifier& hiddenClassifier,
  const int lightSurfaceFlag,
  const float transparentAlpha,
  MaterialLookup& materials,
  TriangleSink& sink)
{
  const auto* material = face.material();
  const auto surfaceFlags = face.resolvedSurfaceFlags();

  if (hiddenClassifier.isHidden(
        material,
        face.materialName(),
        surfaceFlags,
        face.resolvedSurfaceContents()))
  {
    return;
  }

  auto shading = PreviewTriangleShading{};
  materials.assignTo(material, shading);
  shading.objectChannelMask = brushModel.objectChannelMask;
  shading.receivesLight = brushModel.receivesLight;
  shading.surfaceMinLight = brushModel.minLight;

  if (skyClassifier.isSky(material, surfaceFlags))
  {
    shading.kind = PreviewSurfaceKind::Sky;
    shading.occludes = false;
  }
  else
  {
    // Water, slime, lava, triggers, clip and hint brushes are all outside the solid hull
    // the compiler traces against, so light passes straight through them. TrenchBroom
    // already knows which those are: the game config tags them transparent.
    //
    // A texture whose name starts with an asterisk is a liquid, and that holds whether or
    // not the game config happens to carry the tag for it, so it is checked directly. The
    // name comes from the face rather than from the material, so that it still applies
    // when the material itself could not be resolved.
    const auto nonSolid = brushNode.hasAttribute(mdl::TagAttributes::Transparency)
                          || face.hasAttribute(mdl::TagAttributes::Transparency)
                          || isLiquidMaterialName(face.materialName());
    shading.kind = nonSolid ? PreviewSurfaceKind::NonSolid : PreviewSurfaceKind::Solid;
    shading.occludes = !nonSolid && brushModel.castsShadows;

    // Drawn as see-through as the editor draws it, so that the preview shows the same
    // pool of water the view underneath it does. Light ignores it either way.
    shading.alpha = nonSolid ? transparentAlpha : 1.0f;
  }

  // Quake 2 marks emissive faces with a surface flag and puts the brightness in the
  // face's value field, rather than using a light entity template.
  if (
    lightSurfaceFlag != 0 && (surfaceFlags & lightSurfaceFlag) != 0
    && shading.kind != PreviewSurfaceKind::Sky)
  {
    const auto value = face.resolvedSurfaceValue();
    if (value > 0.0f)
    {
      // The scene's own list is not filled in until the walk is over, so the colour comes
      // from the cache the walk is populating.
      const auto color = materials.at(shading.materialIndex).averageColor;
      shading.emission = color * value * PreviewLightUnitScale;
    }
  }

  const auto normal = vm::vec3f{face.normal()};
  const auto vertices = face.vertexPositions();
  if (vertices.size() < 3)
  {
    return;
  }

  const auto uv = [&](const vm::vec3d& position) { return face.uvCoords(position); };

  // A brush face is convex, so a fan from its first vertex covers it exactly.
  for (size_t i = 1; i + 1 < vertices.size(); ++i)
  {
    sink.add(
      vm::vec3f{vertices[0]},
      vm::vec3f{vertices[i]},
      vm::vec3f{vertices[i + 1]},
      uv(vertices[0]),
      uv(vertices[i]),
      uv(vertices[i + 1]),
      normal,
      shading);
  }
}

void addPatch(
  const mdl::PatchNode& patchNode,
  const BrushModelLighting& brushModel,
  const SkyClassifier& skyClassifier,
  const HiddenFaceClassifier& hiddenClassifier,
  MaterialLookup& materials,
  TriangleSink& sink)
{
  const auto& grid = patchNode.grid();
  if (grid.pointRowCount < 2 || grid.pointColumnCount < 2)
  {
    return;
  }

  const auto* material = patchNode.patch().material();

  // A patch carries no surface or content flags of its own, so only its material can say
  // that it is one of the faces the game never draws.
  if (hiddenClassifier.isHidden(material, patchNode.patch().materialName(), 0, 0))
  {
    return;
  }

  auto shading = PreviewTriangleShading{};
  materials.assignTo(material, shading);
  shading.objectChannelMask = brushModel.objectChannelMask;
  shading.receivesLight = brushModel.receivesLight;
  shading.surfaceMinLight = brushModel.minLight;

  if (skyClassifier.isSky(material, 0))
  {
    shading.kind = PreviewSurfaceKind::Sky;
    shading.occludes = false;
  }
  else
  {
    shading.kind = PreviewSurfaceKind::Solid;
    shading.occludes = brushModel.castsShadows;
  }

  const auto addQuadTriangle = [&](
                                 const mdl::PatchGrid::Point& a,
                                 const mdl::PatchGrid::Point& b,
                                 const mdl::PatchGrid::Point& c) {
    const auto p0 = vm::vec3f{a.position};
    const auto p1 = vm::vec3f{b.position};
    const auto p2 = vm::vec3f{c.position};

    // Take the normal from the grid rather than from the winding, so that a curved patch
    // keeps the smooth normals it is drawn with.
    auto normal = vm::vec3f{a.normal + b.normal + c.normal};
    if (vm::squared_length(normal) < vm::constants<float>::almost_zero())
    {
      normal = vm::cross(p1 - p0, p2 - p0);
      if (vm::squared_length(normal) < vm::constants<float>::almost_zero())
      {
        return;
      }
    }

    sink.add(
      p0,
      p1,
      p2,
      vm::vec2f{a.uvCoords},
      vm::vec2f{b.uvCoords},
      vm::vec2f{c.uvCoords},
      vm::normalize(normal),
      shading);
  };

  for (size_t row = 0; row < grid.quadRowCount(); ++row)
  {
    for (size_t col = 0; col < grid.quadColumnCount(); ++col)
    {
      const auto& p00 = grid.point(row, col);
      const auto& p01 = grid.point(row, col + 1);
      const auto& p11 = grid.point(row + 1, col + 1);
      const auto& p10 = grid.point(row + 1, col);

      addQuadTriangle(p00, p01, p11);
      addQuadTriangle(p11, p10, p00);
    }
  }
}

/**
 * Adds an entity's model to the scene, so that monsters, torches and pickups are lit
 * along with the brushwork around them.
 *
 * The model contributes its shape and one colour, not its skin: the geometry comes from
 * the triangles the frame keeps for hit testing, which carry no UV coordinates, so the
 * albedo is the average colour of the skin the frame would be drawn with.
 *
 * NOTE: models are lit but do not cast shadows, which is what the compilers do. A model
 * entity is not part of the BSP, so nothing about it reaches the lightmap; a preview that
 * let one cast a shadow would be showing something the compiled map will not have.
 */
void addEntityModel(
  const mdl::EntityNode& entityNode, MaterialLookup& materials, TriangleSink& sink)
{
  const auto& entity = entityNode.entity();

  const auto* model = entity.model();
  const auto* modelData = model ? model->data() : nullptr;
  const auto* frame = entity.modelFrame();
  if (!modelData || !frame)
  {
    return;
  }

  // Anything but Oriented is a sprite, turned to face the camera as it is drawn. It has
  // no fixed shape to trace against, so it is left out rather than dropped into the scene
  // at whatever angle it happens to be modelled at.
  if (modelData->orientation() != mdl::Orientation::Oriented)
  {
    return;
  }

  const auto& triangles = frame->triangles();
  if (triangles.size() < 3)
  {
    return;
  }

  const auto transformation = vm::mat4x4f{entity.modelTransformation(
    entityNode.entityPropertyConfig().defaultModelScaleExpression)};

  const auto* skin = modelData->surfaceCount() > 0
                       ? modelData->surface(0).skin(frame->skinOffset())
                       : nullptr;

  auto shading = PreviewTriangleShading{};
  materials.assignTo(skin, shading);
  shading.kind = PreviewSurfaceKind::NonSolid;
  shading.occludes = false;

  const auto uv = vm::vec2f{0, 0};

  for (size_t i = 0; i + 2 < triangles.size(); i += 3)
  {
    const auto p0 = transformation * triangles[i];
    const auto p1 = transformation * triangles[i + 1];
    const auto p2 = transformation * triangles[i + 2];

    auto normal = vm::cross(p1 - p0, p2 - p0);
    if (vm::squared_length(normal) < vm::constants<float>::almost_zero())
    {
      continue;
    }

    sink.add(p0, p1, p2, uv, uv, uv, vm::normalize(normal), shading);
  }
}

/**
 * Turns the faces named by a surface light template into emitters.
 *
 * NOTE: ericw-tools subdivides an emitting face into a grid of point lights, and the
 * spacing is configurable. Here the triangles emit as area lights instead, which needs no
 * spacing at all and converges to the same answer, so "_surflightsubdivision" and the
 * "-surflight_subdivide" switch have no effect on the preview.
 */
void applySurfaceLights(PreviewScene& scene, const PreviewLighting& lighting)
{
  if (lighting.surfaceLights.empty())
  {
    return;
  }

  auto byName = std::unordered_map<std::string, const PreviewSurfaceLightTemplate*>{};
  for (const auto& surfaceLight : lighting.surfaceLights)
  {
    byName.emplace(materialBaseName(surfaceLight.materialName), &surfaceLight);
  }

  for (auto& shading : scene.triangleShading)
  {
    if (shading.kind == PreviewSurfaceKind::Sky)
    {
      continue;
    }

    const auto& materialName = scene.materials[shading.materialIndex]->name;
    const auto it = byName.find(materialBaseName(materialName));
    if (it == byName.end())
    {
      continue;
    }

    const auto& surfaceLight = *it->second;
    const auto scale = lighting.globals.surfaceLightScale * surfaceLight.styleScale;

    shading.emission =
      shading.emission
      + surfaceLight.color * surfaceLight.intensity * scale * PreviewLightUnitScale;
  }
}

/**
 * Collects the emissive triangles into a list that can be sampled by area.
 */
void buildEmitterList(PreviewScene& scene)
{
  scene.emitters.clear();
  scene.totalEmitterArea = 0.0f;

  for (uint32_t i = 0; i < uint32_t(scene.triangleShading.size()); ++i)
  {
    const auto& shading = scene.triangleShading[i];
    if (vm::squared_length(shading.emission) <= 0.0f)
    {
      continue;
    }

    const auto& position = scene.trianglePositions[i];
    const auto area = 0.5f * vm::length(vm::cross(position.e1, position.e2));
    if (area <= 0.0f)
    {
      continue;
    }

    scene.totalEmitterArea += area;
    scene.emitters.push_back(PreviewEmitter{i, area, scene.totalEmitterArea});
  }
}

void resolveProjectedTextures(PreviewScene& scene, const PreviewMaterialCache& materials)
{
  for (auto& light : scene.lights)
  {
    if (light.projectedTextureName.empty())
    {
      continue;
    }

    if (const auto index = materials.findByName(light.projectedTextureName))
    {
      light.projectedTextureIndex = int32_t(*index);
    }
  }
}

} // namespace

PreviewMaterialCache::PreviewMaterialCache()
{
  // Index 0 is the fallback for faces whose material could not be resolved.
  auto fallback = PreviewMaterial{};
  fallback.averageColor = vm::vec3f{0.5f, 0.5f, 0.5f};
  m_materials.push_back(std::make_shared<const PreviewMaterial>(std::move(fallback)));
}

uint32_t PreviewMaterialCache::indexOf(
  const gl::Material* material, gl::Gl& gl, const size_t maxTextureSize)
{
  if (!material)
  {
    return 0;
  }

  if (const auto it = m_indices.find(material); it != m_indices.end())
  {
    // Materials are read as the preview meets them, which on a map that has just been
    // opened can be before their textures have finished uploading. Such a material is
    // read again here rather than staying flat for the life of the cache. Replacing the
    // entry is safe while a trace is running: the scene it is tracing holds its own
    // pointer to the old albedo, and keeps it until the trace is thrown away.
    if (m_materials[it->second]->texels.empty())
    {
      auto reread = readMaterial(*material, gl, maxTextureSize);
      if (!reread.texels.empty())
      {
        m_materials[it->second] =
          std::make_shared<const PreviewMaterial>(std::move(reread));
      }
    }
    return it->second;
  }

  const auto index = uint32_t(m_materials.size());
  m_materials.push_back(
    std::make_shared<const PreviewMaterial>(readMaterial(*material, gl, maxTextureSize)));
  m_indices.emplace(material, index);
  m_indicesByName.emplace(materialBaseName(material->name()), index);
  return index;
}

const PreviewMaterial& PreviewMaterialCache::at(const uint32_t index) const
{
  return *m_materials[index];
}

std::optional<uint32_t> PreviewMaterialCache::findByName(const std::string& name) const
{
  const auto it = m_indicesByName.find(materialBaseName(name));
  return it != m_indicesByName.end() ? std::optional{it->second} : std::nullopt;
}

const std::vector<std::shared_ptr<const PreviewMaterial>>& PreviewMaterialCache::
  materials() const
{
  return m_materials;
}

void PreviewMaterialCache::clear()
{
  m_materials.clear();
  m_indices.clear();
  m_indicesByName.clear();

  auto fallback = PreviewMaterial{};
  fallback.averageColor = vm::vec3f{0.5f, 0.5f, 0.5f};
  m_materials.push_back(std::make_shared<const PreviewMaterial>(std::move(fallback)));
}

namespace
{

/**
 * The texel a coordinate lands on. Textures tile, so the coordinates wrap; std::fmod
 * keeps the sign of its argument, hence the extra shift for faces with negative offsets.
 */
size_t wrapTexel(const float value, const size_t size)
{
  const auto scaled = value * float(size);
  auto index = int64_t(std::floor(scaled)) % int64_t(size);
  if (index < 0)
  {
    index += int64_t(size);
  }
  return size_t(index);
}

} // namespace

vm::vec3f PreviewMaterial::sample(const vm::vec2f& uv) const
{
  if (texels.empty() || width == 0 || height == 0)
  {
    return averageColor;
  }

  return texels[wrapTexel(uv.y(), height) * width + wrapTexel(uv.x(), width)];
}

bool PreviewMaterial::transparentAt(const vm::vec2f& uv) const
{
  if (!masked || opaque.empty() || width == 0 || height == 0)
  {
    return false;
  }

  return opaque[wrapTexel(uv.y(), height) * width + wrapTexel(uv.x(), width)] == 0;
}

PreviewScene buildPreviewScene(
  const mdl::Map& map,
  gl::Gl& gl,
  PreviewMaterialCache& materialCache,
  const PreviewSceneOptions& options)
{
  auto scene = PreviewScene{};

  const auto lighting = extractLighting(map);
  scene.globals = lighting.globals;
  scene.lights = lighting.lights;

  const auto& gameConfig = map.gameInfo().gameConfig;
  const auto skyClassifier = SkyClassifier{gameConfig};
  const auto hiddenClassifier = HiddenFaceClassifier{gameConfig};
  const auto lightSurfaceFlag =
    gameConfig.faceAttribsConfig.surfaceFlags.flagValue("light");
  const auto transparentAlpha =
    std::clamp(pref(Preferences::TransparentFaceAlpha), 0.0f, 1.0f);

  auto materials = MaterialLookup{materialCache, gl, options.maxTextureSize};
  auto sink = TriangleSink{scene, {}};

  const auto& editorContext = map.editorContext();

  map.worldNode().accept(kdl::overload(
    [](auto&& thisLambda, const mdl::WorldNode& node) { node.visitChildren(thisLambda); },
    [&](auto&& thisLambda, const mdl::LayerNode& node) {
      if (editorContext.visible(node))
      {
        node.visitChildren(thisLambda);
      }
    },
    [&](auto&& thisLambda, const mdl::GroupNode& node) {
      if (editorContext.visible(node))
      {
        node.visitChildren(thisLambda);
      }
    },
    [&](auto&& thisLambda, const mdl::EntityNode& node) {
      if (editorContext.visible(node))
      {
        if (options.includeEntityModels)
        {
          addEntityModel(node, materials, sink);
        }
        node.visitChildren(thisLambda);
      }
    },
    [&](const mdl::BrushNode& node) {
      if (!editorContext.visible(node))
      {
        return;
      }

      // A trigger is never drawn, so nothing about it belongs in the scene: not its
      // shape, not the light it would block, and not the colour it would bounce.
      if (isHiddenBrushEntity(node.entity()))
      {
        return;
      }

      const auto brushModel = readBrushModelLighting(node.entity());
      for (const auto& face : node.brush().faces())
      {
        if (editorContext.visible(node, face))
        {
          addBrushFace(
            node,
            face,
            brushModel,
            skyClassifier,
            hiddenClassifier,
            lightSurfaceFlag,
            transparentAlpha,
            materials,
            sink);
        }
      }
    },
    [&](const mdl::PatchNode& node) {
      if (editorContext.visible(node) && !isHiddenBrushEntity(node.entity()))
      {
        addPatch(
          node,
          readBrushModelLighting(node.entity()),
          skyClassifier,
          hiddenClassifier,
          materials,
          sink);
      }
    }));

  scene.bounds = sink.bounds.value_or(vm::bbox3f{0.0f, 0.0f});

  scene.hasSkyFaces = std::any_of(
    scene.triangleShading.begin(), scene.triangleShading.end(), [](const auto& shading) {
      return shading.kind == PreviewSurfaceKind::Sky;
    });

  // Every material the build touched is in the cache by now, so the scene can take the
  // whole list; the entries are shared rather than copied.
  scene.materials = materialCache.materials();

  applySurfaceLights(scene, lighting);
  buildEmitterList(scene);
  resolveProjectedTextures(scene, materialCache);

  return scene;
}

void buildPreviewSceneBvh(PreviewScene& scene)
{
  scene.bvh.build(scene.trianglePositions);
}

} // namespace tb::render
