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

#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/WorldNode.h"
#include "render/LightPreviewLights.h"

#include "vm/approx.h"
#include "vm/scalar.h"
#include "vm/vec_io.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::render
{
namespace
{

mdl::Entity makeEntity(std::vector<mdl::EntityProperty> properties)
{
  return mdl::Entity{std::move(properties)};
}

void addEntity(mdl::Map& map, std::vector<mdl::EntityProperty> properties)
{
  auto* entityNode = new mdl::EntityNode{makeEntity(std::move(properties))};
  addNodes(map, {{&parentForNodes(map), {entityNode}}});
}

void setWorldspawn(mdl::Map& map, std::vector<mdl::EntityProperty> properties)
{
  properties.emplace_back("classname", "worldspawn");
  map.worldNode().setEntity(makeEntity(std::move(properties)));
}

} // namespace

TEST_CASE("averageLightStyleBrightness")
{
  SECTION("a steady light is at full brightness")
  {
    CHECK(averageLightStyleBrightness(0, nullptr) == Catch::Approx(1.0));
  }

  SECTION("an animated style previews at the average of its animation")
  {
    // "mamamamamama" alternates between full and dark, so it averages to half.
    CHECK(averageLightStyleBrightness(4, nullptr) == Catch::Approx(0.5).margin(0.01));

    // A flicker that spends most of its time near full stays near full.
    CHECK(averageLightStyleBrightness(1, nullptr) > 0.9f);

    // A slow strong pulse sweeps the whole range.
    CHECK(averageLightStyleBrightness(2, nullptr) == Catch::Approx(1.0).margin(0.15));
  }

  SECTION("a custom appearance string overrides the built in style")
  {
    const auto pattern = std::string{"a"};
    CHECK(averageLightStyleBrightness(0, &pattern) == Catch::Approx(0.0));

    const auto full = std::string{"m"};
    CHECK(averageLightStyleBrightness(4, &full) == Catch::Approx(1.0));
  }

  SECTION("an unknown style previews lit rather than dark")
  {
    CHECK(averageLightStyleBrightness(42, nullptr) == Catch::Approx(1.0));
  }
}

TEST_CASE("extractLighting")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create();

  const auto findLight = [](
                           const PreviewLighting& lighting, const PreviewLightKind kind) {
    const auto it = std::ranges::find_if(
      lighting.lights, [&](const auto& light) { return light.kind == kind; });
    return it != lighting.lights.end() ? &*it : nullptr;
  };

  SECTION("point lights")
  {
    SECTION("reads position, brightness and colour")
    {
      addEntity(
        map,
        {{"classname", "light"},
         {"origin", "16 32 64"},
         {"light", "250"},
         {"_color", "255 128 0"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);

      const auto& light = lighting.lights.front();
      CHECK(light.kind == PreviewLightKind::Point);
      CHECK(light.origin == vm::approx{vm::vec3f{16, 32, 64}});
      CHECK(light.intensity == Catch::Approx(250.0));
      CHECK(light.color == vm::approx{vm::vec3f{1.0f, 128.0f / 255.0f, 0.0f}, 0.001f});
    }

    SECTION("accepts a colour written as fractions")
    {
      addEntity(map, {{"classname", "light"}, {"_color", "1 0.5 0"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().color == vm::approx{vm::vec3f{1.0f, 0.5f, 0.0f}});
    }

    SECTION("reads the GoldSrc _light form")
    {
      addEntity(map, {{"classname", "light"}, {"_light", "255 128 0 400"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().intensity == Catch::Approx(400.0));
      CHECK(
        lighting.lights.front().color
        == vm::approx{vm::vec3f{1.0f, 128.0f / 255.0f, 0.0f}, 0.001f});

      // A light written the GoldSrc way falls off the GoldSrc way unless it says
      // otherwise.
      CHECK(lighting.lights.front().attenuation == PreviewAttenuation::InverseSquare);
    }

    SECTION("delay wins over the attenuation implied by the spelling")
    {
      addEntity(
        map, {{"classname", "light"}, {"_light", "255 255 255 400"}, {"delay", "0"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().attenuation == PreviewAttenuation::Linear);
    }

    SECTION("ZHLT _fade scales the fade distance like wait does")
    {
      addEntity(
        map, {{"classname", "light"}, {"_light", "255 255 255 400"}, {"_fade", "2"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().wait == Catch::Approx(2.0));
    }

    SECTION("applies the light keys to any classname beginning with light")
    {
      addEntity(map, {{"classname", "light_flame_small_yellow"}, {"light", "150"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().intensity == Catch::Approx(150.0));
    }

    SECTION("ignores an entity the compiler would skip")
    {
      addEntity(map, {{"classname", "light"}, {"_nostaticlight", "1"}});

      CHECK(extractLighting(map).lights.empty());
    }

    SECTION("reads the attenuation formula from delay")
    {
      addEntity(map, {{"classname", "light"}, {"delay", "2"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().attenuation == PreviewAttenuation::InverseSquare);
    }

    SECTION("delay 4 becomes a local minimum light")
    {
      addEntity(map, {{"classname", "light"}, {"delay", "4"}, {"light", "80"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().kind == PreviewLightKind::LocalMinLight);
    }
  }

  SECTION("spotlights")
  {
    SECTION("a light that targets an entity aims at it")
    {
      addEntity(
        map, {{"classname", "light"}, {"origin", "0 0 100"}, {"target", "spot_target"}});
      addEntity(
        map,
        {{"classname", "info_null"}, {"origin", "0 0 0"}, {"targetname", "spot_target"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);

      const auto& light = lighting.lights.front();
      CHECK(light.kind == PreviewLightKind::Spot);
      CHECK(light.spot);
      CHECK(light.direction == vm::approx{vm::vec3f{0, 0, -1}, 0.001f});
    }

    SECTION("a light with mangle aims where it says")
    {
      addEntity(map, {{"classname", "light"}, {"mangle", "0 -90 0"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().kind == PreviewLightKind::Spot);
      CHECK(lighting.lights.front().direction == vm::approx{vm::vec3f{0, 0, -1}, 0.001f});
    }

    SECTION("angles alone does not make a light a spotlight")
    {
      // Editors write angles on all sorts of point entities, so treating it as aim would
      // turn every such light into a spot.
      addEntity(map, {{"classname", "light"}, {"angles", "0 0 0"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().kind == PreviewLightKind::Point);
    }

    SECTION("light_spot is a spotlight whatever else it says")
    {
      addEntity(map, {{"classname", "light_spot"}, {"angles", "45 90 0"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().kind == PreviewLightKind::Spot);
    }

    SECTION("reads the cone from either spelling")
    {
      SECTION("ericw-tools angle and _softangle")
      {
        addEntity(
          map,
          {{"classname", "light"},
           {"mangle", "0 -90 0"},
           {"angle", "60"},
           {"_softangle", "30"}});

        const auto lighting = extractLighting(map);
        REQUIRE(lighting.lights.size() == 1);

        const auto& light = lighting.lights.front();
        CHECK(light.cosOuterCone == Catch::Approx(std::cos(vm::to_radians(60.0f))));
        CHECK(light.cosInnerCone == Catch::Approx(std::cos(vm::to_radians(30.0f))));
      }

      SECTION("GoldSrc _cone and _cone2")
      {
        addEntity(map, {{"classname", "light"}, {"_cone", "20"}, {"_cone2", "45"}});

        const auto lighting = extractLighting(map);
        REQUIRE(lighting.lights.size() == 1);

        const auto& light = lighting.lights.front();
        CHECK(light.cosOuterCone == Catch::Approx(std::cos(vm::to_radians(45.0f))));
        CHECK(light.cosInnerCone == Catch::Approx(std::cos(vm::to_radians(20.0f))));
      }
    }

    SECTION("GoldSrc pitch is measured upwards")
    {
      addEntity(map, {{"classname", "light_spot"}, {"pitch", "90"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().direction == vm::approx{vm::vec3f{0, 0, 1}, 0.001f});
    }
  }

  SECTION("suns")
  {
    SECTION("worldspawn _sunlight")
    {
      setWorldspawn(
        map,
        {{"_sunlight", "300"},
         {"_sunlight_color", "255 200 150"},
         {"_sunlight_mangle", "0 -90 0"},
         {"_sunlight_penumbra", "5"}});

      const auto lighting = extractLighting(map);
      const auto* sun = findLight(lighting, PreviewLightKind::Sun);
      REQUIRE(sun);
      CHECK(sun->intensity == Catch::Approx(300.0));
      CHECK(sun->direction == vm::approx{vm::vec3f{0, 0, -1}, 0.001f});
      CHECK(sun->penumbra == Catch::Approx(5.0));
      CHECK(sun->attenuation == PreviewAttenuation::None);
    }

    SECTION("_sun2 is a second, independent sun")
    {
      setWorldspawn(
        map, {{"_sunlight", "300"}, {"_sun2", "100"}, {"_sun2_mangle", "90 -45 0"}});

      const auto lighting = extractLighting(map);
      CHECK(
        std::ranges::count_if(
          lighting.lights,
          [](const auto& light) { return light.kind == PreviewLightKind::Sun; })
        == 2);
    }

    SECTION("a light entity carrying _sun is a sun, not a point light")
    {
      addEntity(
        map,
        {{"classname", "light"},
         {"_sun", "1"},
         {"light", "400"},
         {"mangle", "0 -90 0"},
         {"deviance", "3"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);

      const auto& sun = lighting.lights.front();
      CHECK(sun.kind == PreviewLightKind::Sun);
      CHECK(sun.intensity == Catch::Approx(400.0));
      // On a sun entity the penumbra is spelled without the leading underscore.
      CHECK(sun.penumbra == Catch::Approx(3.0));
    }

    SECTION("GoldSrc light_environment is a sun")
    {
      addEntity(
        map,
        {{"classname", "light_environment"},
         {"_light", "255 255 255 200"},
         {"pitch", "-90"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);

      const auto& sun = lighting.lights.front();
      CHECK(sun.kind == PreviewLightKind::Sun);
      CHECK(sun.direction == vm::approx{vm::vec3f{0, 0, -1}, 0.001f});
      CHECK(sun.intensity == Catch::Approx(200.0));
    }
  }

  SECTION("sky and ground domes")
  {
    SECTION("worldspawn _sunlight2 and _sunlight3")
    {
      setWorldspawn(
        map,
        {{"_sunlight2", "60"},
         {"_sunlight2_color", "128 128 255"},
         {"_sunlight3", "20"}});

      const auto lighting = extractLighting(map);
      CHECK(lighting.globals.skyDome.z() == Catch::Approx(60.0));
      CHECK(lighting.globals.skyDome.x() == Catch::Approx(60.0f * 128.0f / 255.0f));
      CHECK(lighting.globals.groundDome.z() == Catch::Approx(20.0));
    }

    SECTION("a light entity carrying _sunlight2 configures the dome and emits nothing")
    {
      addEntity(map, {{"classname", "light"}, {"_sunlight2", "1"}, {"light", "80"}});

      const auto lighting = extractLighting(map);
      CHECK(lighting.lights.empty());
      CHECK(lighting.globals.skyDome.x() == Catch::Approx(80.0));
    }
  }

  SECTION("global keys")
  {
    SECTION("minimum light, with light as the legacy spelling")
    {
      setWorldspawn(map, {{"_minlight", "25"}, {"_minlight_color", "255 0 0"}});

      const auto lighting = extractLighting(map);
      CHECK(lighting.globals.minLight == vm::approx{vm::vec3f{25, 0, 0}, 0.01f});
    }

    SECTION("the compilers do not bounce light unless the map asks them to")
    {
      setWorldspawn(map, {});

      const auto defaults = extractLighting(map).globals;
      CHECK(defaults.bounces == 0);
      // Nor does a bounce pick up the colour of the surface it came off by default.
      CHECK(defaults.bounceColorScale == Catch::Approx(0.0));
      CHECK(defaults.bounceScale == Catch::Approx(1.0));

      setWorldspawn(
        map, {{"_bounce", "1"}, {"_bouncecolorscale", "1"}, {"_bouncescale", "0.5"}});

      const auto enabled = extractLighting(map).globals;
      CHECK(enabled.bounces == 1);
      CHECK(enabled.bounceColorScale == Catch::Approx(1.0));
      CHECK(enabled.bounceScale == Catch::Approx(0.5));

      // "_bounce" is a count, not a switch: the number is how many bounces to make.
      setWorldspawn(map, {{"_bounce", "5"}});
      CHECK(extractLighting(map).globals.bounces == 5);

      // A map asking for more than the preview will trace gets what the preview will
      // trace, and a negative count is no bouncing at all.
      setWorldspawn(map, {{"_bounce", "1000"}});
      CHECK(extractLighting(map).globals.bounces == MaxPreviewBounces);

      setWorldspawn(map, {{"_bounce", "-1"}});
      CHECK(extractLighting(map).globals.bounces == 0);
    }

    SECTION("tone and scale controls")
    {
      setWorldspawn(
        map,
        {{"_dist", "0.5"},
         {"_range", "1.5"},
         {"_gamma", "1.2"},
         {"_maxlight", "200"},
         {"_anglescale", "0.75"}});

      const auto lighting = extractLighting(map);
      CHECK(lighting.globals.distScale == Catch::Approx(0.5));
      CHECK(lighting.globals.rangeScale == Catch::Approx(1.5));
      CHECK(lighting.globals.gamma == Catch::Approx(1.2));
      CHECK(lighting.globals.maxLight == Catch::Approx(200.0));
      CHECK(lighting.globals.defaultAngleScale == Catch::Approx(0.75));
    }

    SECTION("a light without _anglescale takes worldspawn's")
    {
      setWorldspawn(map, {{"_anglescale", "0.25"}});
      addEntity(map, {{"classname", "light"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.lights.size() == 1);
      CHECK(lighting.lights.front().angleScale == Catch::Approx(0.25));
    }
  }

  SECTION("surface lights")
  {
    SECTION("_surface names the texture that emits")
    {
      addEntity(
        map,
        {{"classname", "light"},
         {"_surface", "+0light"},
         {"light", "180"},
         {"_color", "255 255 128"}});

      const auto lighting = extractLighting(map);
      CHECK(lighting.lights.empty());
      REQUIRE(lighting.surfaceLights.size() == 1);

      const auto& surfaceLight = lighting.surfaceLights.front();
      CHECK(surfaceLight.materialName == "+0light");
      CHECK(surfaceLight.intensity == Catch::Approx(180.0));
    }

    SECTION("info_texlights maps texture names to values")
    {
      addEntity(
        map,
        {{"classname", "info_texlights"},
         {"origin", "0 0 0"},
         {"+0~light", "255 200 100 300"}});

      const auto lighting = extractLighting(map);
      REQUIRE(lighting.surfaceLights.size() == 1);
      CHECK(lighting.surfaceLights.front().materialName == "+0~light");
      CHECK(lighting.surfaceLights.front().intensity == Catch::Approx(300.0));
    }
  }

  SECTION("projected textures")
  {
    addEntity(
      map,
      {{"classname", "light"},
       {"_project_texture", "gobo"},
       {"_project_fov", "60"},
       {"_project_mangle", "0 -90 0"}});

    const auto lighting = extractLighting(map);
    REQUIRE(lighting.lights.size() == 1);

    const auto& light = lighting.lights.front();
    CHECK(light.projectedTextureName == "gobo");
    CHECK(light.spot);
    CHECK(light.projectTanHalfFov == Catch::Approx(std::tan(vm::to_radians(30.0f))));
    CHECK(light.direction == vm::approx{vm::vec3f{0, 0, -1}, 0.001f});
    // The projection frame has to be square to the direction it projects along.
    CHECK(
      vm::dot(light.projectRight, light.direction) == Catch::Approx(0.0).margin(1e-5));
    CHECK(vm::dot(light.projectUp, light.direction) == Catch::Approx(0.0).margin(1e-5));
    CHECK(
      vm::dot(light.projectRight, light.projectUp) == Catch::Approx(0.0).margin(1e-5));
  }

  SECTION("light channels")
  {
    addEntity(map, {{"classname", "light"}, {"_light_channel_mask", "6"}});

    const auto lighting = extractLighting(map);
    REQUIRE(lighting.lights.size() == 1);
    CHECK(lighting.lights.front().lightChannelMask == 6);
    // The shadow mask follows the light mask unless it is given separately.
    CHECK(lighting.lights.front().shadowChannelMask == 6);
  }
}

} // namespace tb::render
