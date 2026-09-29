/*
 Copyright (C) 2022 Kristian Duske

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

#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Picking.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeHandleManager.h"
#include "mdl/NodeHandles.h"
#include "mdl/PickResult.h"
#include "mdl/TestUtils.h"
#include "mdl/WorldNode.h"
#include "ui/GestureTracker.h"
#include "ui/InputState.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/PickRequest.h"
#include "ui/SelectionTool.h"

#include "kd/result.h"

#include <algorithm>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

namespace tb::ui
{
using namespace Catch::Matchers;

TEST_CASE("SelectionTool")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  SECTION("tool returns itself")
  {
    auto tool = SelectionTool{document};
    CHECK(&tool.tool() == static_cast<Tool*>(&tool));
  }

  SECTION("cancel always returns false")
  {
    auto tool = SelectionTool{document};
    CHECK(!tool.cancel());
  }

  SECTION("clicking")
  {
    const auto& worldNode = map.worldNode();
    auto builder = mdl::BrushBuilder{
      worldNode.mapFormat(),
      map.worldBounds(),
      map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes,
      map.gameInfo().gameConfig.faceAttribsConfig.defaultSurfaceAttributes};

    auto tool = SelectionTool{document};

    GIVEN("A group node")
    {
      auto* brushNode =
        new mdl::BrushNode{builder.createCube(32.0, "some_face") | kdl::value()};
      auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"origin", "64 0 0"}}}};
      auto* groupNode = new mdl::GroupNode(mdl::Group{"some_group"});

      addNodes(map, {{&parentForNodes(map), {groupNode}}});
      addNodes(map, {{groupNode, {brushNode, entityNode}}});

      auto camera = gl::OrthographicCamera{};

      AND_GIVEN("A pick ray that points at the top face of the brush")
      {
        camera.moveTo({0, 0, 32});
        camera.setDirection({0, 0, -1}, {0, 1, 0});

        const auto pickRay = vm::ray3d{camera.pickRay({0, 0, 0})};

        auto pickResult = mdl::PickResult{};
        pick(map, pickRay, pickResult);
        REQUIRE(pickResult.all().size() == 1);

        REQUIRE(map.selection().brushFaces.empty());

        auto inputState = InputState{0.0f, 0.0f};
        inputState.setPickRequest({pickRay, camera});
        inputState.setPickResult(std::move(pickResult));

        WHEN("I click once")
        {
          inputState.mouseDown(MouseButtons::Left);
          tool.mouseClick(inputState);
          inputState.mouseUp(MouseButtons::Left);

          THEN("The group gets selected")
          {
            CHECK(map.selection().brushFaces.empty());
            CHECK(map.selection() == mdl::makeSelection(map, {groupNode}));
          }
        }

        WHEN("I double click")
        {
          inputState.mouseDown(MouseButtons::Left);
          tool.mouseDoubleClick(inputState);
          inputState.mouseUp(MouseButtons::Left);

          THEN("The group is opened")
          {
            CHECK(map.selection().brushFaces.empty());
            CHECK(!map.selection().hasNodes());
            CHECK(map.editorContext().currentGroup() == groupNode);
          }
        }
      }
    }

    GIVEN("A brush node and an entity node")
    {
      auto brush = builder.createCube(
                     32.0,
                     "left_face",
                     "right_face",
                     "front_face",
                     "back_face",
                     "top_face",
                     "bottom_face")
                   | kdl::value();
      auto* brushNode = new mdl::BrushNode{std::move(brush)};

      const auto topFaceIndex = *brushNode->brush().findFace("top_face");
      const auto frontFaceIndex = *brushNode->brush().findFace("front_face");

      auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"origin", "64 0 0"}}}};

      addNodes(map, {{&parentForNodes(map), {brushNode, entityNode}}});

      auto camera = gl::OrthographicCamera{};

      AND_GIVEN("A pick ray that points at the top face of the brush")
      {
        camera.moveTo({0, 0, 32});
        camera.setDirection({0, 0, -1}, {0, 1, 0});

        const auto pickRay = vm::ray3d{camera.pickRay({0, 0, 0})};

        auto pickResult = mdl::PickResult{};
        pick(map, pickRay, pickResult);
        REQUIRE(pickResult.all().size() == 1);

        REQUIRE(map.selection().brushFaces.empty());

        auto inputState = InputState{0.0f, 0.0f};
        inputState.setPickRequest({pickRay, camera});
        inputState.setPickResult(std::move(pickResult));

        WHEN("I shift click once")
        {
          inputState.setModifierKeys(ModifierKeys::Shift);
          inputState.mouseDown(MouseButtons::Left);
          tool.mouseClick(inputState);
          inputState.mouseUp(MouseButtons::Left);

          THEN("The top face get selected")
          {
            CHECK(
              map.selection().brushFaces
              == std::vector<mdl::BrushFaceHandle>{{brushNode, topFaceIndex}});
            CHECK(!map.selection().hasNodes());
          }

          AND_WHEN("I shift click on the selected face again")
          {
            inputState.setModifierKeys(ModifierKeys::Shift);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The top face remains selected")
            {
              CHECK(
                map.selection().brushFaces
                == std::vector<mdl::BrushFaceHandle>{{brushNode, topFaceIndex}});
              CHECK(!map.selection().hasNodes());
            }
          }

          AND_WHEN("I shift+ctrl click on the selected face again")
          {
            inputState.setModifierKeys(ModifierKeys::Shift | ModifierKeys::CtrlCmd);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The top face gets deselected")
            {
              CHECK(map.selection().brushFaces.empty());
              CHECK(!map.selection().hasNodes());
            }
          }
        }

        WHEN("I click once")
        {
          inputState.mouseDown(MouseButtons::Left);
          tool.mouseClick(inputState);
          inputState.mouseUp(MouseButtons::Left);

          THEN("The brush gets selected")
          {
            CHECK(map.selection().brushFaces.empty());
            CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));
          }

          AND_WHEN("I click on the selected brushagain")
          {
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The brush remains selected")
            {
              CHECK(map.selection().brushFaces.empty());
              CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));
            }
          }

          AND_WHEN("I ctrl click on the selected brush again")
          {
            inputState.setModifierKeys(ModifierKeys::CtrlCmd);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The brush gets deselected")
            {
              CHECK(map.selection().brushFaces.empty());
              CHECK(!map.selection().hasNodes());
            }
          }
        }

        WHEN("I shift double click")
        {
          inputState.setModifierKeys(ModifierKeys::Shift);
          inputState.mouseDown(MouseButtons::Left);
          tool.mouseDoubleClick(inputState);
          inputState.mouseUp(MouseButtons::Left);

          THEN("All brush faces are selected")
          {
            CHECK(map.selection().brushFaces.size() == 6);
            CHECK(!map.selection().hasNodes());
          }
        }

        WHEN("I double click")
        {
          inputState.mouseDown(MouseButtons::Left);
          tool.mouseDoubleClick(inputState);
          inputState.mouseUp(MouseButtons::Left);

          THEN("All nodes are selected")
          {
            CHECK(map.selection().brushFaces.empty());
            CHECK(map.selection() == mdl::makeSelection(map, {brushNode, entityNode}));
          }
        }

        AND_GIVEN("The front face of the brush is selected")
        {
          selectBrushFaces(map, {{brushNode, frontFaceIndex}});

          WHEN("I shift click once")
          {
            inputState.setModifierKeys(ModifierKeys::Shift);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The top face get selected")
            {
              CHECK(
                map.selection().brushFaces
                == std::vector<mdl::BrushFaceHandle>{{brushNode, topFaceIndex}});
              CHECK(!map.selection().hasNodes());
            }
          }

          WHEN("I shift+ctrl click once")
          {
            inputState.setModifierKeys(ModifierKeys::Shift | ModifierKeys::CtrlCmd);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("Both the front and the top faces are selected")
            {
              CHECK_THAT(
                map.selection().brushFaces,
                UnorderedEquals(std::vector<mdl::BrushFaceHandle>{
                  {brushNode, topFaceIndex}, {brushNode, frontFaceIndex}}));
              CHECK(!map.selection().hasNodes());
            }
          }

          WHEN("A brush node with a coplanar face exists and its front face is selected")
          {
            auto coplanarBrush =
              builder.createCube(
                32.0,
                "left_face",
                "right_face",
                "front_face",
                "back_face",
                "top_face",
                "bottom_face")
              | kdl::and_then([&](auto b) {
                  return b.transform(
                           map.worldBounds(),
                           vm::translation_matrix(vm::vec3d{32, 0, 0}),
                           false)
                         | kdl::transform([&]() { return std::move(b); });
                })
              | kdl::value();

            auto* coplanarBrushNode = new mdl::BrushNode{std::move(coplanarBrush)};
            addNodes(map, {{&parentForNodes(map), {coplanarBrushNode}}});

            const auto coplanarTopFaceIndex =
              *coplanarBrushNode->brush().findFace("top_face");
            mdl::selectBrushFaces(map, {{brushNode, frontFaceIndex}});

            AND_WHEN("I select all adjacent coplanar faces")
            {
              inputState.setModifierKeys(ModifierKeys::Shift | ModifierKeys::Alt);
              inputState.mouseDown(MouseButtons::Left);
              tool.mouseDoubleClick(inputState);
              inputState.mouseUp(MouseButtons::Left);

              THEN("The clicked face's coplanar region is selected")
              {
                CHECK_THAT(
                  map.selection().brushFaces,
                  UnorderedEquals(std::vector<mdl::BrushFaceHandle>{
                    {brushNode, topFaceIndex},
                    {coplanarBrushNode, coplanarTopFaceIndex}}));
                CHECK(!map.selection().hasNodes());
              }
            }

            AND_WHEN("I add all adjacent coplanar faces")
            {
              inputState.setModifierKeys(
                ModifierKeys::CtrlCmd | ModifierKeys::Shift | ModifierKeys::Alt);
              inputState.mouseDown(MouseButtons::Left);
              tool.mouseDoubleClick(inputState);
              inputState.mouseUp(MouseButtons::Left);

              THEN("The clicked face's coplanar region is added to the selection")
              {
                CHECK_THAT(
                  map.selection().brushFaces,
                  UnorderedEquals(std::vector<mdl::BrushFaceHandle>{
                    {brushNode, frontFaceIndex},
                    {brushNode, topFaceIndex},
                    {coplanarBrushNode, coplanarTopFaceIndex}}));
                CHECK(!map.selection().hasNodes());
              }
            }
          }

          WHEN("I click once")
          {
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The brush gets selected")
            {
              CHECK(map.selection().brushFaces.empty());
              CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));
            }
          }

          WHEN("I ctrl click once")
          {
            inputState.setModifierKeys(ModifierKeys::CtrlCmd);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The brush gets selected")
            {
              CHECK(map.selection().brushFaces.empty());
              CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));
            }
          }
        }

        AND_GIVEN("The entity is selected")
        {
          selectNodes(map, {entityNode});

          WHEN("I shift click once")
          {
            inputState.setModifierKeys(ModifierKeys::Shift);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The top face get selected")
            {
              CHECK(
                map.selection().brushFaces
                == std::vector<mdl::BrushFaceHandle>{{brushNode, topFaceIndex}});
              CHECK(!map.selection().hasNodes());
            }
          }

          WHEN("I shift+ctrl click once")
          {
            inputState.setModifierKeys(ModifierKeys::Shift | ModifierKeys::CtrlCmd);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The top face get selected")
            {
              CHECK(
                map.selection().brushFaces
                == std::vector<mdl::BrushFaceHandle>{{brushNode, topFaceIndex}});
              CHECK(!map.selection().hasNodes());
            }
          }

          WHEN("I click once")
          {
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The brush gets selected")
            {
              CHECK(map.selection().brushFaces.empty());
              CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));
            }
          }

          WHEN("I ctrl click once")
          {
            inputState.setModifierKeys(ModifierKeys::CtrlCmd);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("The brush and entity both get selected")
            {
              CHECK(map.selection().brushFaces.empty());
              CHECK(map.selection() == mdl::makeSelection(map, {entityNode, brushNode}));
            }
          }
        }

        AND_GIVEN("The top face is hidden")
        {

          auto hiddenTag = mdl::Tag{"hidden", {}};

          auto newBrush = brushNode->brush();
          newBrush.face(topFaceIndex).addTag(hiddenTag);
          updateNodeContents(
            map, "Set Tag", {{brushNode, mdl::NodeContents{std::move(newBrush)}}});

          REQUIRE(brushNode->brush().face(topFaceIndex).hasTag(hiddenTag));

          map.editorContext().setHiddenTags(hiddenTag.type());
          REQUIRE_FALSE(map.editorContext().visible(
            *brushNode, brushNode->brush().face(topFaceIndex)));

          WHEN("I shift click once")
          {
            inputState.setModifierKeys(ModifierKeys::Shift);
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("Nothing happens")
            {
              CHECK(map.selection().brushFaces.empty());
              CHECK(!map.selection().hasNodes());
            }
          }

          WHEN("I click once")
          {
            inputState.mouseDown(MouseButtons::Left);
            tool.mouseClick(inputState);
            inputState.mouseUp(MouseButtons::Left);

            THEN("Nothing happens")
            {
              CHECK(map.selection().brushFaces.empty());
              CHECK(!map.selection().hasNodes());
            }
          }
        }
      }
    }
  }

  SECTION("clickingThroughHidden")
  {
    const auto& worldNode = map.worldNode();
    auto builder = mdl::BrushBuilder{
      worldNode.mapFormat(),
      map.worldBounds(),
      map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes,
      map.gameInfo().gameConfig.faceAttribsConfig.defaultSurfaceAttributes};

    auto tool = SelectionTool{document};

    GIVEN("A brush visible behind the hidden face of another brush")
    {
      auto visibleBrush = builder.createCube(
                            32.0,
                            "left_face",
                            "right_face",
                            "front_face",
                            "back_face",
                            "top_face",
                            "bottom_face")
                          | kdl::value();
      auto* visibleBrushNode = new mdl::BrushNode{std::move(visibleBrush)};
      const auto visibleTopFaceIndex = *visibleBrushNode->brush().findFace("top_face");

      auto hiddenBrush = builder.createCube(
                           64.0,
                           "left_face",
                           "right_face",
                           "front_face",
                           "back_face",
                           "top_face",
                           "bottom_face")
                         | kdl::value();
      auto* hiddenBrushNode = new mdl::BrushNode{std::move(hiddenBrush)};
      const auto hiddenTopFaceIndex = *hiddenBrushNode->brush().findFace("top_face");

      addNodes(map, {{&parentForNodes(map), {visibleBrushNode, hiddenBrushNode}}});

      const auto hiddenTag = mdl::Tag{"hidden", {}};
      auto taggedBrush = hiddenBrushNode->brush();
      taggedBrush.face(hiddenTopFaceIndex).addTag(hiddenTag);
      updateNodeContents(
        map, "Set Tag", {{hiddenBrushNode, mdl::NodeContents{std::move(taggedBrush)}}});

      map.editorContext().setHiddenTags(hiddenTag.type());

      REQUIRE(hiddenBrushNode->brush().face(hiddenTopFaceIndex).hasTag(hiddenTag));
      CHECK(!map.editorContext().visible(
        *hiddenBrushNode, hiddenBrushNode->brush().face(hiddenTopFaceIndex)));

      auto camera = gl::OrthographicCamera{};
      AND_GIVEN("A pick ray that points at the top face of the brushes")
      {
        camera.moveTo({0, 0, 128});
        camera.setDirection({0, 0, -1}, {0, 1, 0});

        const auto pickRay = vm::ray3d{camera.pickRay({0, 0, 0})};

        auto pickResult = mdl::PickResult{};
        pick(map, pickRay, pickResult);
        CHECK(pickResult.all().size() == 2);
        REQUIRE(map.selection().brushFaces.empty());

        auto inputState = InputState{0.0f, 0.0f};
        inputState.setPickRequest({pickRay, camera});
        inputState.setPickResult(std::move(pickResult));

        WHEN("I shift click once")
        {
          inputState.setModifierKeys(ModifierKeys::Shift);
          inputState.mouseDown(MouseButtons::Left);
          tool.mouseClick(inputState);
          inputState.mouseUp(MouseButtons::Left);

          THEN("The top face of the visible brush get selected")
          {
            CHECK(!map.selection().hasNodes());
            CHECK(
              map.selection().brushFaces
              == std::vector<mdl::BrushFaceHandle>{
                {visibleBrushNode, visibleTopFaceIndex}});
          }
        }

        WHEN("I click once")
        {
          inputState.mouseDown(MouseButtons::Left);
          tool.mouseClick(inputState);
          inputState.mouseUp(MouseButtons::Left);

          THEN("The visible brush gets selected")
          {
            CHECK(map.selection().brushFaces.empty());
            CHECK(map.selection() == mdl::makeSelection(map, {visibleBrushNode}));
          }
        }
      }
    }
  }

  SECTION("scrolling")
  {
    const auto& worldNode = map.worldNode();
    auto builder = mdl::BrushBuilder{
      worldNode.mapFormat(),
      map.worldBounds(),
      map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes,
      map.gameInfo().gameConfig.faceAttribsConfig.defaultSurfaceAttributes};

    auto tool = SelectionTool{document};

    GIVEN("Two nested brushes along the pick ray")
    {
      // the larger brush's top face is higher up, so it is hit first (at the smaller
      // distance) by a ray looking straight down; the smaller brush's top face is hit
      // second
      auto* largeBrushNode =
        new mdl::BrushNode{builder.createCube(64.0, "material") | kdl::value()};
      auto* smallBrushNode =
        new mdl::BrushNode{builder.createCube(32.0, "material") | kdl::value()};
      addNodes(map, {{&parentForNodes(map), {largeBrushNode, smallBrushNode}}});
      selectNodes(map, {largeBrushNode});

      auto camera = gl::OrthographicCamera{};
      camera.moveTo({0, 0, 128});
      camera.setDirection({0, 0, -1}, {0, 1, 0});

      const auto pickRay = vm::ray3d{camera.pickRay({0, 0, 0})};

      auto pickResult = mdl::PickResult{};
      pick(map, pickRay, pickResult);
      REQUIRE(pickResult.all().size() == 2);

      auto inputState = InputState{0.0f, 0.0f};
      inputState.setPickRequest({pickRay, camera});
      inputState.setPickResult(std::move(pickResult));

      WHEN("I scroll forward with just CtrlCmd held")
      {
        inputState.setModifierKeys(ModifierKeys::CtrlCmd);
        inputState.scroll(ScrollSource::Mouse, 0.0f, 1.0f);
        tool.mouseScroll(inputState);

        THEN("The selection drills through to the next node under the ray")
        {
          CHECK(map.selection() == mdl::makeSelection(map, {smallBrushNode}));
        }
      }

      WHEN("I scroll backward with just CtrlCmd held")
      {
        deselectAll(map);
        selectNodes(map, {smallBrushNode});

        inputState.setModifierKeys(ModifierKeys::CtrlCmd);
        inputState.scroll(ScrollSource::Mouse, 0.0f, -1.0f);
        tool.mouseScroll(inputState);

        THEN("The selection drills through to the previous node under the ray")
        {
          CHECK(map.selection() == mdl::makeSelection(map, {largeBrushNode}));
        }
      }

      WHEN("I scroll with CtrlCmd+Alt held")
      {
        const auto sizeBefore = map.grid().size();

        inputState.setModifierKeys(ModifierKeys::CtrlCmd | ModifierKeys::Alt);
        inputState.scroll(ScrollSource::Mouse, 0.0f, 1.0f);
        tool.mouseScroll(inputState);

        THEN("The grid size changes instead of drilling the selection")
        {
          CHECK(map.grid().size() != sizeBefore);
          CHECK(map.selection() == mdl::makeSelection(map, {largeBrushNode}));
        }
      }

      WHEN("I scroll with no modifiers")
      {
        const auto sizeBefore = map.grid().size();

        inputState.scroll(ScrollSource::Mouse, 0.0f, 1.0f);
        tool.mouseScroll(inputState);

        THEN("Neither the grid size nor the selection changes")
        {
          CHECK(map.grid().size() == sizeBefore);
          CHECK(map.selection() == mdl::makeSelection(map, {largeBrushNode}));
        }
      }
    }
  }

  SECTION("dragging")
  {
    const auto& worldNode = map.worldNode();
    auto builder = mdl::BrushBuilder{
      worldNode.mapFormat(),
      map.worldBounds(),
      map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes,
      map.gameInfo().gameConfig.faceAttribsConfig.defaultSurfaceAttributes};

    auto tool = SelectionTool{document};

    GIVEN("A brush node and an entity node")
    {
      auto brush = builder.createCube(
                     32.0,
                     "left_face",
                     "right_face",
                     "front_face",
                     "back_face",
                     "top_face",
                     "bottom_face")
                   | kdl::value();
      auto* brushNode = new mdl::BrushNode{std::move(brush)};

      const auto topFaceIndex = *brushNode->brush().findFace("top_face");
      const auto frontFaceIndex = *brushNode->brush().findFace("front_face");

      auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"origin", "64 0 0"}}}};

      addNodes(map, {{&parentForNodes(map), {brushNode, entityNode}}});

      auto camera = gl::OrthographicCamera{};
      camera.moveTo({0, 0, 32});
      camera.setDirection({0, 0, -1}, {0, 1, 0});

      const auto pickRay = vm::ray3d{camera.pickRay({0, 0, 0})};

      auto pickResult = mdl::PickResult{};
      pick(map, pickRay, pickResult);
      REQUIRE(pickResult.all().size() == 1);

      auto inputState = InputState{0.0f, 0.0f};
      inputState.setPickRequest({pickRay, camera});
      inputState.setPickResult(std::move(pickResult));

      WHEN("I drag without CtrlCmd held")
      {
        inputState.mouseDown(MouseButtons::Left);

        THEN("No drag is accepted")
        {
          CHECK(tool.acceptMouseDrag(inputState) == nullptr);
        }
      }

      WHEN("I shift+ctrl drag starting on the top face")
      {
        inputState.setModifierKeys(ModifierKeys::Shift | ModifierKeys::CtrlCmd);
        inputState.mouseDown(MouseButtons::Left);

        auto tracker = tool.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);

        THEN("The top face is selected")
        {
          CHECK(
            map.selection().brushFaces
            == std::vector<mdl::BrushFaceHandle>{{brushNode, topFaceIndex}});
        }

        AND_WHEN("The drag moves onto the front face")
        {
          auto frontPickResult = mdl::PickResult{};
          frontPickResult.addHit(mdl::Hit{
            mdl::BrushNode::BrushHitType,
            0.0,
            vm::vec3d{0, 0, 0},
            mdl::BrushFaceHandle{brushNode, frontFaceIndex}});

          auto dragInputState = InputState{0.0f, 0.0f};
          dragInputState.setPickRequest({pickRay, camera});
          dragInputState.setPickResult(std::move(frontPickResult));
          dragInputState.setModifierKeys(ModifierKeys::Shift | ModifierKeys::CtrlCmd);
          dragInputState.mouseDown(MouseButtons::Left);

          CHECK(tracker->update(dragInputState));

          THEN("Both faces end up selected")
          {
            CHECK_THAT(
              map.selection().brushFaces,
              UnorderedEquals(std::vector<mdl::BrushFaceHandle>{
                {brushNode, topFaceIndex}, {brushNode, frontFaceIndex}}));
          }

          tracker->end(dragInputState);
        }
      }

      WHEN("I ctrl drag starting on the brush")
      {
        inputState.setModifierKeys(ModifierKeys::CtrlCmd);
        inputState.mouseDown(MouseButtons::Left);

        auto tracker = tool.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);

        THEN("The brush is selected")
        {
          CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));
        }

        AND_WHEN("The drag moves onto the entity")
        {
          auto entityPickResult = mdl::PickResult{};
          entityPickResult.addHit(mdl::Hit{
            mdl::EntityNode::EntityHitType, 0.0, vm::vec3d{64, 0, 0}, entityNode});

          auto dragInputState = InputState{0.0f, 0.0f};
          dragInputState.setPickRequest({pickRay, camera});
          dragInputState.setPickResult(std::move(entityPickResult));
          dragInputState.setModifierKeys(ModifierKeys::CtrlCmd);
          dragInputState.mouseDown(MouseButtons::Left);

          CHECK(tracker->update(dragInputState));

          THEN("Both the brush and the entity end up selected")
          {
            CHECK(map.selection() == mdl::makeSelection(map, {brushNode, entityNode}));
          }

          tracker->cancel();
        }
      }
    }
  }
}

TEST_CASE("SelectionTool marquee selection")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  const auto& worldNode = map.worldNode();
  auto builder = mdl::BrushBuilder{
    worldNode.mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes,
    map.gameInfo().gameConfig.faceAttribsConfig.defaultSurfaceAttributes};
  auto* brushNode =
    new mdl::BrushNode{builder.createCube(32.0, "marquee") | kdl::value()};
  auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"origin", "128 0 0"}}}};
  addNodes(map, {{&parentForNodes(map), {brushNode, entityNode}}});

  const auto exercise = [&](auto& camera) {
    camera.moveTo({0, 0, 256});
    camera.setDirection({0, 0, -1}, {0, 1, 0});

    struct ScreenBounds
    {
      float minX, minY, maxX, maxY;
    };
    const auto screenBounds = [&](const mdl::Node& node) {
      auto result = ScreenBounds{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
      for (const auto& vertex : node.physicalBounds().vertices())
      {
        const auto projected = camera.project(vm::vec3f{vertex});
        const auto y = static_cast<float>(camera.viewport().height) - projected.y();
        result.minX = std::min(result.minX, projected.x());
        result.minY = std::min(result.minY, y);
        result.maxX = std::max(result.maxX, projected.x());
        result.maxY = std::max(result.maxY, y);
      }
      return result;
    };

    auto mode = MarqueeSelectionMode::AllObjects;
    auto tool = SelectionTool{document, &mode};
    auto plainMiddle = InputState{100, 100};
    plainMiddle.mouseDown(MouseButtons::Middle);
    plainMiddle.setPickRequest({vm::ray3d{camera.pickRay(100, 100)}, camera});
    CHECK(tool.acceptMouseDrag(plainMiddle) == nullptr);
    const auto drag =
      [&](const vm::vec2f& start, const vm::vec2f& end, const bool extend = false) {
        auto inputState = InputState{start.x(), start.y()};
        inputState.setModifierKeys(
          ModifierKeys::CtrlCmd | (extend ? ModifierKeys::Shift : ModifierKeys::None));
        inputState.mouseDown(MouseButtons::Middle);
        inputState.setPickRequest(
          {vm::ray3d{camera.pickRay(start.x(), start.y())}, camera});
        auto tracker = tool.acceptMouseDrag(inputState);
        REQUIRE(tracker != nullptr);
        inputState.mouseMove(end.x(), end.y(), end.x() - start.x(), end.y() - start.y());
        REQUIRE(tracker->update(inputState));
        tracker->end(inputState);
      };

    const auto brush = screenBounds(*brushNode);
    const auto entity = screenBounds(*entityNode);
    drag(
      {std::min(brush.minX, entity.minX) - 2, std::min(brush.minY, entity.minY) - 2},
      {std::max(brush.maxX, entity.maxX) + 2, std::max(brush.maxY, entity.maxY) + 2});
    CHECK_THAT(
      map.selection().nodes,
      UnorderedEquals(std::vector<mdl::Node*>{brushNode, entityNode}));

    mode = MarqueeSelectionMode::Brushes;
    drag({brush.minX - 2, brush.minY - 2}, {brush.maxX + 2, brush.maxY + 2});
    CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));

    mode = MarqueeSelectionMode::Entities;
    drag({entity.minX - 2, entity.minY - 2}, {entity.maxX + 2, entity.maxY + 2});
    CHECK(map.selection() == mdl::makeSelection(map, {entityNode}));

    mode = MarqueeSelectionMode::Brushes;
    drag({brush.minX - 2, brush.minY - 2}, {brush.maxX + 2, brush.maxY + 2}, true);
    CHECK_THAT(
      map.selection().nodes,
      UnorderedEquals(std::vector<mdl::Node*>{brushNode, entityNode}));

    // A narrow forward drag does not fully contain the brush. The same rectangle
    // in the reverse direction intersects it and selects it.
    drag({brush.minX - 4, brush.minY - 2}, {brush.minX + 4, brush.maxY + 2});
    CHECK(!map.selection().hasAny());
    drag({brush.minX + 4, brush.minY - 2}, {brush.minX - 4, brush.maxY + 2});
    CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));

    mode = MarqueeSelectionMode::Entities;
    auto canceledInput = InputState{entity.minX - 2, entity.minY - 2};
    canceledInput.setModifierKeys(ModifierKeys::CtrlCmd);
    canceledInput.mouseDown(MouseButtons::Middle);
    canceledInput.setPickRequest(
      {vm::ray3d{camera.pickRay(canceledInput.mouseX(), canceledInput.mouseY())},
       camera});
    auto canceledTracker = tool.acceptMouseDrag(canceledInput);
    REQUIRE(canceledTracker != nullptr);
    canceledInput.mouseMove(entity.maxX + 2, entity.maxY + 2, 0, 0);
    REQUIRE(canceledTracker->update(canceledInput));
    canceledTracker->cancel();
    CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));

    map.nodeHandles().addHandles<mdl::VertexHandle>(*brushNode);
    mode = MarqueeSelectionMode::Vertices;
    const auto vertex = *map.nodeHandles().allHandles<mdl::VertexHandle>().begin();
    const auto point = camera.project(vm::vec3f{vertex.position});
    const auto y = static_cast<float>(camera.viewport().height) - point.y();
    drag({point.x() - 1, y - 1}, {point.x() + 1, y + 1});
    CHECK(map.nodeHandles().selectedHandleCount<mdl::VertexHandle>() > 0);
    CHECK(
      map.nodeHandles().selectedHandleCount<mdl::VertexHandle>()
      < map.nodeHandles().handleCount<mdl::VertexHandle>());
    map.nodeHandles().clear<mdl::VertexHandle>();
  };

  SECTION("2D")
  {
    auto camera = gl::OrthographicCamera{};
    exercise(camera);
  }
  SECTION("3D")
  {
    auto camera = gl::PerspectiveCamera{};
    exercise(camera);

    // The near plane cuts through this brush. Its clipped front edge still
    // fills the right side of the viewport even though its far corners do not.
    deselectAll(map);
    camera.moveTo({0, 0, 16.5f});
    auto mode = MarqueeSelectionMode::Brushes;
    auto tool = SelectionTool{document, &mode};
    const auto start = vm::vec2f{900, 340};
    const auto end = vm::vec2f{800, 260};
    auto inputState = InputState{start.x(), start.y()};
    inputState.setModifierKeys(ModifierKeys::CtrlCmd);
    inputState.mouseDown(MouseButtons::Middle);
    inputState.setPickRequest({vm::ray3d{camera.pickRay(start.x(), start.y())}, camera});
    auto tracker = tool.acceptMouseDrag(inputState);
    REQUIRE(tracker != nullptr);
    inputState.mouseMove(end.x(), end.y(), end.x() - start.x(), end.y() - start.y());
    tracker->end(inputState);
    CHECK(map.selection() == mdl::makeSelection(map, {brushNode}));
  }
}

} // namespace tb::ui
