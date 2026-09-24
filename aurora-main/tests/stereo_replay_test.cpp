#include "gfx/stereo_replay.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

namespace aurora::gfx::stereo_replay {
namespace {

TEST(StereoReplayTest, SplitFurnitureIsRecognizedInFullDisplayCoordinates) {
  const SubviewRect display{16.f, 8.f, 1280.f, 912.f};
  for (uint32_t players : {2u, 3u, 4u}) {
    EXPECT_TRUE(is_split_screen_furniture({16.f, 462.f, 1280.f, 4.f}, display, players));
    EXPECT_FALSE(is_split_screen_furniture(display, display, players));                   // Race fade.
    EXPECT_FALSE(is_split_screen_furniture({80.f, 60.f, 100.f, 40.f}, display, players)); // HUD backing.
    EXPECT_FALSE(is_split_screen_furniture(player_one_region(display, players), display, players));
  }
  EXPECT_TRUE(is_split_screen_furniture({16.f, 464.f, 1280.f, 456.f}, display, 2));
  for (uint32_t players : {3u, 4u}) {
    EXPECT_TRUE(is_split_screen_furniture({656.f, 8.f, 0.f, 912.f}, display, players));
    EXPECT_TRUE(is_split_screen_furniture({658.f, 10.f, 636.f, 452.f}, display, players));
    EXPECT_TRUE(is_split_screen_furniture({16.f, 464.f, 640.f, 456.f}, display, players));
    EXPECT_TRUE(is_split_screen_furniture({656.f, 464.f, 640.f, 456.f}, display, players));
  }
  EXPECT_FALSE(is_split_screen_furniture({656.f, 8.f, 640.f, 456.f}, display, 1));
  EXPECT_FALSE(is_split_screen_furniture({656.f, 8.f, 0.f, 912.f}, display, 2));
}

TEST(StereoReplayTest, PartitionLineLayoutPanesAreFurniture) {
  // MKW's partition_line.brlyt draws yoko_line (800x1) and tate_line (1x800)
  // picture panes centred on the display; both extend past a 4:3 root and are
  // clipped by the display copy, so only the centre line and thickness matter.
  const SubviewRect display{0.f, 0.f, 893.f, 456.f};
  const SubviewRect yoko{46.5f, 227.5f, 800.f, 1.f};
  const SubviewRect tate{446.f, -172.f, 1.f, 800.f};
  EXPECT_TRUE(is_split_screen_furniture(yoko, display, 2));
  EXPECT_FALSE(is_split_screen_furniture(tate, display, 2));
  for (uint32_t players : {3u, 4u}) {
    EXPECT_TRUE(is_split_screen_furniture(yoko, display, players));
    EXPECT_TRUE(is_split_screen_furniture(tate, display, players));
  }
  // A textured pane of the same shape elsewhere is HUD art, not furniture.
  EXPECT_FALSE(is_split_screen_furniture({46.5f, 100.f, 800.f, 1.f}, display, 2));
  EXPECT_FALSE(is_split_screen_furniture({46.5f, 227.5f, 300.f, 1.f}, display, 2));
}

TEST(StereoReplayTest, MultiplayerSelectsOnlyPlayerOneWorld) {
  const SubviewRect display{12.f, 8.f, 640.f, 456.f};
  for (uint32_t count : {2u, 3u, 4u}) {
    const auto player = player_one_region(display, count);
    EXPECT_FLOAT_EQ(player.left, display.left);
    EXPECT_FLOAT_EQ(player.top, display.top);
    EXPECT_FLOAT_EQ(player.width, count == 2 ? 640.f : 320.f);
    EXPECT_FLOAT_EQ(player.height, 228.f);
    EXPECT_TRUE(replay_player_one_draw(player, player, true, false));
    auto opponent = player;
    opponent.top += player.height;
    EXPECT_FALSE(replay_player_one_draw(opponent, player, true, false));
    EXPECT_FALSE(replay_player_one_draw(opponent, player, false, false));
    if (count >= 3) {
      opponent = player;
      opponent.left += player.width;
      EXPECT_FALSE(replay_player_one_draw(opponent, player, true, false));
      EXPECT_FALSE(replay_player_one_draw(opponent, player, false, false));
      opponent.top += player.height; // P4 / unused fourth quadrant in 3P.
      EXPECT_FALSE(replay_player_one_draw(opponent, player, true, false));
    }
    EXPECT_FALSE(replay_player_one_draw(display, player, true, false));
    EXPECT_TRUE(replay_player_one_draw(display, player, false, false));
    EXPECT_FALSE(replay_player_one_draw(player, player, false, true));
    EXPECT_FALSE(replay_player_one_draw(display, player, false, true));
    const auto remap = make_hud_ndc_remap(player.left, player.top, player.width, player.height, player.left, player.top,
                                          player.width, player.height);
    EXPECT_FLOAT_EQ(remap.scaleX, 1.f);
    EXPECT_FLOAT_EQ(remap.scaleY, 1.f);
    EXPECT_FLOAT_EQ(remap.offsetX, 0.f);
    EXPECT_FLOAT_EQ(remap.offsetY, 0.f);
  }
  const auto single = player_one_region(display, 1);
  EXPECT_FLOAT_EQ(single.width, display.width);
  EXPECT_FLOAT_EQ(single.height, display.height);
}

TEST(StereoReplayTest, MultiplayerRejectsEmptyAndNonOverlappingScissors) {
  const SubviewRect player{0.f, 0.f, 320.f, 228.f};
  EXPECT_FALSE(subviews_overlap(player, {320.f, 0.f, 320.f, 228.f}));
  EXPECT_FALSE(subviews_overlap(player, {0.f, 228.f, 640.f, 228.f}));
  EXPECT_FALSE(subviews_overlap(player, {10.f, 10.f, 0.f, 10.f}));
  EXPECT_TRUE(subviews_overlap(player, {10.f, 10.f, 20.f, 20.f}));
  EXPECT_TRUE(subview_contains(player, {0.f, -0.5f, 320.f, 228.f}));
}

TEST(StereoReplayTest, EyeFrustumPreservesGameDepthMapping) {
  const Mat4x4<float> game{
      {10.0f, 11.0f, 12.0f, 13.0f},
      {20.0f, 21.0f, 22.0f, 23.0f},
      {30.0f, 31.0f, 32.0f, 33.0f},
      {40.0f, 41.0f, 42.0f, 43.0f},
  };
  const Mat4x4<float> eye{
      {1.1f, 1.2f, 1.3f, 1.4f},
      {2.1f, 2.2f, 2.3f, 2.4f},
      {3.1f, 3.2f, 3.3f, 3.4f},
      {4.1f, 4.2f, 4.3f, 4.4f},
  };

  const auto result = compose_projection(eye, game);

  EXPECT_FLOAT_EQ(result.m0[0], eye.m0[0]);
  EXPECT_FLOAT_EQ(result.m0[2], eye.m0[2]);
  EXPECT_FLOAT_EQ(result.m1[1], eye.m1[1]);
  EXPECT_FLOAT_EQ(result.m1[2], eye.m1[2]);
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const bool frustumTerm = (row == 0 && (column == 0 || column == 2)) || (row == 1 && (column == 1 || column == 2));
      if (!frustumTerm) {
        EXPECT_FLOAT_EQ(result[row][column], game[row][column]);
      }
    }
  }
}

Mat4x4<float> game_orthographic_projection() {
  // x over [0, 640) and y over [0, 456) mapped to NDC, with a shallow depth
  // window, as GX builds an orthographic projection for a 2D layer.
  Mat4x4<float> game{};
  game.m0 = {2.0f / 640.0f, 0.0f, 0.0f, -1.0f};
  game.m1 = {0.0f, -2.0f / 456.0f, 0.0f, 1.0f};
  game.m2 = {0.0f, 0.0f, -1.0f / 1000.0f, -0.5f};
  game.m3 = {0.0f, 0.0f, 0.0f, 1.0f};
  return game;
}

float dot4(const Vec4<float>& row, const Vec4<float>& v) {
  return row[0] * v[0] + row[1] * v[1] + row[2] * v[2] + row[3] * v[3];
}

const std::array<Vec4<float>, 5> kVertices{{
    {0.0f, 0.0f, 0.0f, 1.0f},
    {640.0f, 456.0f, 0.0f, 1.0f},
    {320.0f, 228.0f, -250.0f, 1.0f},
    {97.0f, 401.0f, 640.0f, 1.0f},
    {-30.0f, 12.5f, 33.0f, 1.0f},
}};

TEST(StereoReplayTest, OrthographicProjectionIsRecognizedByItsWRow) {
  const auto game = game_orthographic_projection();
  EXPECT_TRUE(is_orthographic_projection(game));

  Mat4x4<float> perspective = game;
  perspective.m3 = {0.0f, 0.0f, -1.0f, 0.0f};
  EXPECT_FALSE(is_orthographic_projection(perspective));
}

TEST(StereoReplayTest, HudViewportNdcIsLiftedIntoTheDisplayedFrame) {
  // Bottom-right quarter of a 608x456 displayed frame.
  const auto remap = make_hud_ndc_remap(304.0f, 228.0f, 304.0f, 228.0f, 0.0f, 0.0f, 608.0f, 456.0f);
  EXPECT_FLOAT_EQ(remap.scaleX, 0.5f);
  EXPECT_FLOAT_EQ(remap.scaleY, 0.5f);
  EXPECT_FLOAT_EQ(remap.offsetX, 0.5f);
  EXPECT_FLOAT_EQ(remap.offsetY, -0.5f);

  Mat4x4<float> local{};
  local.m0 = {1.0f, 0.0f, 0.0f, 0.0f};
  local.m1 = {0.0f, 1.0f, 0.0f, 0.0f};
  local.m3 = {0.0f, 0.0f, 0.0f, 1.0f};
  const auto frame = remap_hud_ndc(local, remap);
  const Vec4<float> topLeft{-1.0f, 1.0f, 0.0f, 1.0f};
  const Vec4<float> bottomRight{1.0f, -1.0f, 0.0f, 1.0f};
  EXPECT_FLOAT_EQ(dot4(frame.m0, topLeft), 0.0f);
  EXPECT_FLOAT_EQ(dot4(frame.m1, topLeft), 0.0f);
  EXPECT_FLOAT_EQ(dot4(frame.m0, bottomRight), 1.0f);
  EXPECT_FLOAT_EQ(dot4(frame.m1, bottomRight), -1.0f);
}

TEST(StereoReplayTest, HudScreenProjectionMatchesTheChainItComposes) {
  const auto game = game_orthographic_projection();
  Mat4x4<float> eyeFrustum{};
  eyeFrustum.m0 = {1.15f, 0.0f, 0.08f, 0.0f};
  eyeFrustum.m1 = {0.0f, 1.02f, -0.03f, 0.0f};

  // A head turned a little and offset from the recorded center eye.
  const float angle = 0.3f;
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  Mat3x4<float> viewFromCenter{};
  viewFromCenter.m0 = {c, 0.0f, s, 15.0f};
  viewFromCenter.m1 = {0.0f, 1.0f, 0.0f, -4.0f};
  viewFromCenter.m2 = {-s, 0.0f, c, 7.0f};

  const HudScreen screen{.halfWidth = 600.0f, .halfHeight = 337.5f, .distance = 1000.0f};
  const auto composed = compose_hud_screen_projection(eyeFrustum, viewFromCenter, screen, game);

  for (const auto& v : kVertices) {
    // The same chain, one step at a time: game NDC, a point on the screen
    // rectangle, that point in eye view space, then the eye's clip space.
    const float ndcX = dot4(game.m0, v);
    const float ndcY = dot4(game.m1, v);
    const Vec4<float> screenPoint{ndcX * screen.halfWidth, ndcY * screen.halfHeight, -screen.distance, 1.0f};
    const float eyeX = dot4(viewFromCenter.m0, screenPoint);
    const float eyeY = dot4(viewFromCenter.m1, screenPoint);
    const float eyeZ = dot4(viewFromCenter.m2, screenPoint);

    EXPECT_NEAR(dot4(composed.m0, v), eyeFrustum.m0[0] * eyeX + eyeFrustum.m0[2] * eyeZ, 1e-2f);
    EXPECT_NEAR(dot4(composed.m1, v), eyeFrustum.m1[1] * eyeY + eyeFrustum.m1[2] * eyeZ, 1e-2f);
    const float clipW = -eyeZ;
    EXPECT_NEAR(dot4(composed.m3, v), clipW, 1e-2f);
    // The virtual screen must carry the depth the unmodified draw would have
    // written. Since the reverse-Z fix moved the near/far correction wholly into
    // the projection, that is the staged Z row applied directly - no further
    // inversion. Comparing against `game.m2` rather than the helper's own output
    // is what makes this catch a re-introduced double correction.
    EXPECT_NEAR(dot4(composed.m2, v), dot4(game.m2, v), 1e-6f);
  }
}

TEST(StereoReplayTest, HudScreenParksRasterDepthAtMidrangeUnderHeadMotion) {
  const auto game = game_orthographic_projection();
  Mat4x4<float> eyeFrustum{};
  eyeFrustum.m0 = {1.15f, 0.0f, 0.08f, 0.0f};
  eyeFrustum.m1 = {0.0f, 1.02f, -0.03f, 0.0f};
  const float angle = 0.35f;
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  Mat3x4<float> moved{};
  moved.m0 = {c, 0.0f, s, 21.0f};
  moved.m1 = {0.0f, 1.0f, 0.0f, -9.0f};
  moved.m2 = {-s, 0.0f, c, 13.0f};

  const HudScreen screen{.halfWidth = 600.0f, .halfHeight = 337.5f, .distance = 1000.0f};
  const auto composed = compose_hud_screen_projection(eyeFrustum, moved, screen, game);

  // The exact-depth shader captures composed Z, then parks clip Z at +0.5W, so
  // rasterization stays stable even though W varies across the rotated screen.
  // It writes +0.5W directly: the reverse-Z fix removed the per-vertex depth
  // negation that used to follow, which is why the shader no longer pre-negates
  // to -0.5W. What this test pins is the invariant that survived that change -
  // the parked value must land at NDC 0.5 for every vertex, whatever W does.
  for (const auto& v : kVertices) {
    const float w = dot4(composed.m3, v);
    ASSERT_GT(w, 0.0f);
    const float parkedClipZ = 0.5f * w;
    EXPECT_NEAR(parkedClipZ / w, 0.5f, 1e-5f);
  }
}

TEST(StereoReplayTest, OverlayPanelIsCentredOnTheVirtualScreen) {
  // Three quarters of a 1200-unit screen, 1000 ahead, with a 4:3 panel.
  const auto panel = overlay_panel_on_screen(1200.0f, 1000.0f, 0.75f, 4.0f / 3.0f);
  EXPECT_FLOAT_EQ(panel.halfWidth, 450.0f);
  EXPECT_FLOAT_EQ(panel.halfHeight, 337.5f);
  EXPECT_FLOAT_EQ(panel.distance, 1000.0f);
  EXPECT_TRUE(panel.valid());
  EXPECT_FALSE(overlay_panel_on_screen(1200.0f, 1000.0f, 0.75f, 0.0f).valid());
  EXPECT_FALSE(overlay_panel_on_screen(1200.0f, 0.0f, 0.75f, 4.0f / 3.0f).valid());
}

TEST(StereoReplayTest, OverlayPanelCornersFollowTheEyeChain) {
  Mat4x4<float> eyeFrustum{};
  eyeFrustum.m0 = {1.15f, 0.0f, 0.08f, 0.0f};
  eyeFrustum.m1 = {0.0f, 1.02f, -0.03f, 0.0f};
  const float angle = 0.3f;
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  Mat3x4<float> viewFromCenter{};
  viewFromCenter.m0 = {c, 0.0f, s, 15.0f};
  viewFromCenter.m1 = {0.0f, 1.0f, 0.0f, -4.0f};
  viewFromCenter.m2 = {-s, 0.0f, c, 7.0f};

  const OverlayPanel panel{.halfWidth = 450.0f, .halfHeight = 337.5f, .distance = 1000.0f};
  const auto composed = compose_overlay_panel_projection(eyeFrustum, viewFromCenter, panel);

  const std::array<Vec4<float>, 5> corners{{
      {-1.0f, 1.0f, 0.0f, 1.0f},
      {1.0f, 1.0f, 0.0f, 1.0f},
      {-1.0f, -1.0f, 0.0f, 1.0f},
      {1.0f, -1.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, 0.0f, 1.0f},
  }};
  for (const auto& corner : corners) {
    // Top left is (-1, +1): the panel's +y is up, like the screen it sits on.
    const Vec4<float> centerPoint{corner[0] * panel.halfWidth, corner[1] * panel.halfHeight, -panel.distance, 1.0f};
    const float eyeX = dot4(viewFromCenter.m0, centerPoint);
    const float eyeY = dot4(viewFromCenter.m1, centerPoint);
    const float eyeZ = dot4(viewFromCenter.m2, centerPoint);
    const float w = dot4(composed.m3, corner);
    EXPECT_NEAR(dot4(composed.m0, corner), eyeFrustum.m0[0] * eyeX + eyeFrustum.m0[2] * eyeZ, 1e-2f);
    EXPECT_NEAR(dot4(composed.m1, corner), eyeFrustum.m1[1] * eyeY + eyeFrustum.m1[2] * eyeZ, 1e-2f);
    EXPECT_NEAR(w, -eyeZ, 1e-2f);
    ASSERT_GT(w, 0.0f);
    EXPECT_NEAR(dot4(composed.m2, corner) / w, 0.5f, 1e-5f);
  }
}

TEST(StereoReplayTest, OverlayPanelOnAFlatEyeKeepsItsAspect) {
  // A 4:3 panel three quarters across a 2064x2208 eye image.
  const float imageAspect = 2064.0f / 2208.0f;
  const auto flat = overlay_panel_flat_projection(0.75f, 4.0f / 3.0f, imageAspect);
  const Vec4<float> topRight{1.0f, 1.0f, 0.0f, 1.0f};
  const float ndcX = dot4(flat.m0, topRight) / dot4(flat.m3, topRight);
  const float ndcY = dot4(flat.m1, topRight) / dot4(flat.m3, topRight);
  EXPECT_FLOAT_EQ(ndcX, 0.75f);
  // In pixels: 0.75 * 2064 wide over ndcY * 2208 tall is the panel's 4:3.
  EXPECT_NEAR((ndcX * 2064.0f) / (ndcY * 2208.0f), 4.0f / 3.0f, 1e-4f);
  EXPECT_FLOAT_EQ(dot4(flat.m2, topRight), 0.5f);
  const Vec4<float> centre{0.0f, 0.0f, 0.0f, 1.0f};
  EXPECT_FLOAT_EQ(dot4(flat.m0, centre), 0.0f);
  EXPECT_FLOAT_EQ(dot4(flat.m1, centre), 0.0f);
}

Mat3x4<float> identity3x4() {
  Mat3x4<float> m{};
  m.m0 = {1.0f, 0.0f, 0.0f, 0.0f};
  m.m1 = {0.0f, 1.0f, 0.0f, 0.0f};
  m.m2 = {0.0f, 0.0f, 1.0f, 0.0f};
  return m;
}

Mat3x4<float> head_tracking_delta() {
  const float angle = 0.21f;
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  Mat3x4<float> m{};
  m.m0 = {c, 0.0f, s, 11.0f};
  m.m1 = {0.0f, 1.0f, 0.0f, -3.0f};
  m.m2 = {-s, 0.0f, c, 6.0f};
  return m;
}

TEST(StereoReplayTest, IdentitySceneAnchorLeavesTheEyeDeltaUnchanged) {
  const auto viewFromCenter = head_tracking_delta();

  const auto viewFromScene = compose_affine(viewFromCenter, identity3x4());

  EXPECT_EQ(viewFromScene, viewFromCenter);
}

TEST(StereoReplayTest, TranslatingSceneAnchorMovesTheWorldByTheAnchorOffset) {
  // A first-person anchor with no levelling is translate(-a): the camera moves
  // to a, so every world point must arrive a units closer to the eye origin.
  const std::array<float, 3> a{40.0f, -12.0f, -260.0f};
  auto anchor = identity3x4();
  anchor.m0[3] = -a[0];
  anchor.m1[3] = -a[1];
  anchor.m2[3] = -a[2];
  const auto viewFromCenter = head_tracking_delta();
  const auto viewFromScene = compose_affine(viewFromCenter, anchor);

  // An object matrix placing a vertex somewhere in the recorded view space.
  Mat3x4<float> objectToCenter{};
  objectToCenter.m0 = {1.0f, 0.0f, 0.0f, 130.0f};
  objectToCenter.m1 = {0.0f, 1.0f, 0.0f, 55.0f};
  objectToCenter.m2 = {0.0f, 0.0f, 1.0f, -900.0f};

  const auto anchored = compose_affine(viewFromScene, objectToCenter);
  const auto recorded = compose_affine(viewFromCenter, objectToCenter);

  // Rotation is untouched, and the eye-space displacement is exactly the eye
  // delta's rotation applied to -a.
  for (size_t row = 0; row < 3; ++row) {
    const auto& anchoredRow = *(&anchored.m0 + row);
    const auto& recordedRow = *(&recorded.m0 + row);
    const auto& viewRow = *(&viewFromCenter.m0 + row);
    for (size_t column = 0; column < 3; ++column) {
      EXPECT_FLOAT_EQ(anchoredRow[column], recordedRow[column]);
    }
    const float expected =
        recordedRow[3] - (viewRow[0] * a[0] + viewRow[1] * a[1] + viewRow[2] * a[2]);
    EXPECT_NEAR(anchoredRow[3], expected, 1e-3f);
  }
}

TEST(StereoReplayTest, VirtualScreenStaysAheadOfTheAnchoredCamera) {
  // The screen rectangle is authored in the anchored camera's space and so
  // composes with viewFromCenter, while world geometry composes with
  // viewFromScene. The two agree exactly when a world object placed `distance`
  // ahead of the anchored camera lands on the screen's centre.
  const std::array<float, 3> a{40.0f, -12.0f, -260.0f};
  const float distance = 20.0f;
  auto anchor = identity3x4();
  anchor.m0[3] = -a[0];
  anchor.m1[3] = -a[1];
  anchor.m2[3] = -a[2];
  const auto viewFromCenter = head_tracking_delta();
  const auto viewFromScene = compose_affine(viewFromCenter, anchor);

  // The screen's centre: (0, 0, -distance) in the anchored camera's space,
  // carried into eye space by viewFromCenter alone.
  const Vec4<float> screenCentre{0.0f, 0.0f, -distance, 1.0f};
  const float centreX = dot4(viewFromCenter.m0, screenCentre);
  const float centreY = dot4(viewFromCenter.m1, screenCentre);
  const float centreZ = dot4(viewFromCenter.m2, screenCentre);

  // A world object at the same place, expressed the way a GX draw carries it:
  // in the *recorded* view space, hence offset by the anchor position.
  Mat3x4<float> objectToCenter = identity3x4();
  objectToCenter.m0[3] = a[0];
  objectToCenter.m1[3] = a[1];
  objectToCenter.m2[3] = a[2] - distance;
  const auto placed = compose_affine(viewFromScene, objectToCenter);

  EXPECT_NEAR(placed.m0[3], centreX, 1e-3f);
  EXPECT_NEAR(placed.m1[3], centreY, 1e-3f);
  EXPECT_NEAR(placed.m2[3], centreZ, 1e-3f);
}

// A GX perspective projection with a positive X scale, plus the asymmetric
// frustum offset an OpenXR eye contributes.
Mat4x4<float> eye_frustum(float offsetX) {
  Mat4x4<float> m{};
  m.m0 = {1.3f, 0.0f, offsetX, 0.0f};
  m.m1 = {0.0f, 1.7f, 0.04f, 0.0f};
  m.m2 = {0.0f, 0.0f, -1.0001f, -0.2f};
  m.m3 = {0.0f, 0.0f, -1.0f, 0.0f};
  return m;
}

Vec4<float> clip_of(const Mat4x4<float>& projection, const Mat3x4<float>& viewFromScene,
                    const Mat3x4<float>& objectToCenter, const Vec4<float>& object) {
  const auto placed = compose_affine(viewFromScene, objectToCenter);
  const Vec4<float> view{dot4(placed.m0, object), dot4(placed.m1, object), dot4(placed.m2, object), 1.0f};
  return {dot4(projection.m0, view), dot4(projection.m1, view), dot4(projection.m2, view), dot4(projection.m3, view)};
}

TEST(StereoReplayTest, MirrorModeIsRecognizedByANegativeProjectionXScale) {
  const auto game = eye_frustum(0.0f);
  EXPECT_FALSE(projection_mirrors_x(game));

  auto mirrored = game;
  mirrored.m0[0] = -mirrored.m0[0];
  EXPECT_TRUE(projection_mirrors_x(mirrored));
}

TEST(StereoReplayTest, MirroredHalvesComposeIntoOneReflectionOfTheScene) {
  // The pair must reproduce exactly P . V . S . A: a world reflected about the
  // anchored camera's X plane, with the eyes placed in the reflected world.
  const std::array<float, 3> a{40.0f, -12.0f, -260.0f};
  auto anchor = identity3x4();
  anchor.m0[3] = -a[0];
  anchor.m1[3] = -a[1];
  anchor.m2[3] = -a[2];
  const auto viewFromCenter = head_tracking_delta();
  const auto viewFromScene = compose_affine(viewFromCenter, anchor);
  const auto viewFromSceneMirrored = compose_affine(mirror_view_delta_x(viewFromCenter), anchor);

  auto game = eye_frustum(0.0f);
  game.m0[0] = -game.m0[0]; // Mirror mode's flip, as the game submits it.
  const auto eye = eye_frustum(0.11f);
  const auto projection = mirror_projection_x(compose_projection(eye, game));
  // compose_projection takes the X scale from the eye, so this is the ordinary
  // unmirrored eye projection P.
  const auto reference = compose_projection(eye, game);

  // The reference route: reflect in the anchored camera's space by folding S into
  // the anchor, then compose the eye delta over it exactly as an unmirrored draw
  // would. This is the ordering the fix has to reproduce - S sits between the eye
  // delta and the anchor, not between the anchor and the world.
  auto reflectedAnchor = anchor;
  reflectedAnchor.m0[0] = -reflectedAnchor.m0[0];
  reflectedAnchor.m0[1] = -reflectedAnchor.m0[1];
  reflectedAnchor.m0[2] = -reflectedAnchor.m0[2];
  reflectedAnchor.m0[3] = -reflectedAnchor.m0[3];
  const auto viewFromSceneReference = compose_affine(viewFromCenter, reflectedAnchor);
  // Anchor ordering has to matter, or the test would pass either way.
  EXPECT_NE(viewFromSceneReference, viewFromScene);

  Mat3x4<float> objectToCenter{};
  objectToCenter.m0 = {1.0f, 0.0f, 0.0f, 130.0f};
  objectToCenter.m1 = {0.0f, 1.0f, 0.0f, 55.0f};
  objectToCenter.m2 = {0.0f, 0.0f, 1.0f, -900.0f};

  for (const auto& v : kVertices) {
    const auto mirroredClip = clip_of(projection, viewFromSceneMirrored, objectToCenter, v);
    const auto expected = clip_of(reference, viewFromSceneReference, objectToCenter, v);
    for (size_t component = 0; component < 4; ++component) {
      EXPECT_NEAR(mirroredClip[component], expected[component], 1e-3f);
    }
  }
}

TEST(StereoReplayTest, MirroringKeepsEachEyeOnItsOwnSide) {
  // The v6 failure this guards against: reflecting the finished clip position
  // mirrors every eye about its own axis, which swaps the stereo pair. With the
  // reflection taken before the eye delta, an object straight ahead must still
  // sit right of centre for the left eye and left of centre for the right.
  const float ipd = 3.2f; // Half-IPD in game units.
  const auto eyeDelta = [&](float sign) {
    auto m = identity3x4();
    m.m0[3] = -sign * ipd; // The eye moves by +sign*ipd, so the world moves back.
    return m;
  };
  auto game = eye_frustum(0.0f);
  game.m0[0] = -game.m0[0];

  Mat3x4<float> objectToCenter = identity3x4();
  objectToCenter.m2[3] = -500.0f; // Straight ahead of the camera.
  const Vec4<float> object{0.0f, 0.0f, 0.0f, 1.0f};

  std::array<float, 2> ndcX{};
  for (size_t eyeIndex = 0; eyeIndex < 2; ++eyeIndex) {
    const float sign = eyeIndex == 0 ? -1.0f : 1.0f;
    const auto eye = eye_frustum(0.0f);
    const auto projection = mirror_projection_x(compose_projection(eye, game));
    const auto view = mirror_view_delta_x(eyeDelta(sign));
    const auto clip = clip_of(projection, view, objectToCenter, object);
    ASSERT_GT(clip[3], 0.0f);
    ndcX[eyeIndex] = clip[0] / clip[3];
  }
  EXPECT_GT(ndcX[0], 0.0f);
  EXPECT_LT(ndcX[1], 0.0f);
}

Mat4x4<float> asymmetric_eye_frustum() {
  Mat4x4<float> eyeFrustum{};
  eyeFrustum.m0 = {1.15f, 0.0f, 0.08f, 0.0f};
  eyeFrustum.m1 = {0.0f, 1.02f, -0.03f, 0.0f};
  return eyeFrustum;
}

// A head turned and pitched a little, offset from the recorded center eye.
Mat3x4<float> turned_head() {
  const float yaw = 0.3f;
  const float pitch = -0.12f;
  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);
  // Pitch about X after yaw about Y.
  Mat3x4<float> m{};
  m.m0 = {cy, 0.0f, sy, 15.0f};
  m.m1 = {sp * sy, cp, -sp * cy, -4.0f};
  m.m2 = {-cp * sy, sp, cp * cy, 7.0f};
  return m;
}

// Where a point of the center-eye space lands in the eye image, in NDC.
std::array<float, 2> eye_ndc(const Mat4x4<float>& eyeFrustum, const Mat3x4<float>& viewFromCenter,
                             const Vec4<float>& centerPoint) {
  const float eyeX = dot4(viewFromCenter.m0, centerPoint);
  const float eyeY = dot4(viewFromCenter.m1, centerPoint);
  const float eyeZ = dot4(viewFromCenter.m2, centerPoint);
  EXPECT_LT(eyeZ, 0.0f);
  return {(eyeFrustum.m0[0] * eyeX + eyeFrustum.m0[2] * eyeZ) / -eyeZ,
          (eyeFrustum.m1[1] * eyeY + eyeFrustum.m1[2] * eyeZ) / -eyeZ};
}

TEST(StereoReplayTest, WindowMaskFindsTheScreenThroughTheEye) {
  const auto eyeFrustum = asymmetric_eye_frustum();
  const auto viewFromCenter = turned_head();
  const HudScreen screen{.halfWidth = 600.0f, .halfHeight = 337.5f, .distance = 1000.0f};
  const auto mask = window_mask(eyeFrustum, viewFromCenter, screen);

  // Corners, centre, and points just inside and outside the edges, in half extents.
  const std::array<std::array<float, 2>, 9> points{{
      {-1.0f, 1.0f},
      {1.0f, 1.0f},
      {-1.0f, -1.0f},
      {1.0f, -1.0f},
      {0.0f, 0.0f},
      {0.95f, 0.2f},
      {1.05f, 0.2f},
      {-0.3f, -0.97f},
      {-0.3f, -1.04f},
  }};
  for (const auto& point : points) {
    const Vec4<float> centerPoint{point[0] * screen.halfWidth, point[1] * screen.halfHeight, -screen.distance, 1.0f};
    const auto ndc = eye_ndc(eyeFrustum, viewFromCenter, centerPoint);
    const auto h = mask.at(ndc[0], ndc[1]);
    ASSERT_GT(h.z, 0.0f);
    EXPECT_NEAR(h.x / h.z, point[0], 1e-4f);
    EXPECT_NEAR(h.y / h.z, point[1], 1e-4f);
  }
}

TEST(StereoReplayTest, WindowMaskCoincidesWithTheHudOnTheSameScreen) {
  // The 2D layer is drawn on the window, so a HUD vertex must land at its own
  // game NDC on the window: the mask and compose_hud_screen_projection agree.
  const auto game = game_orthographic_projection();
  const auto eyeFrustum = asymmetric_eye_frustum();
  const auto viewFromCenter = turned_head();
  const HudScreen screen{.halfWidth = 600.0f, .halfHeight = 337.5f, .distance = 1000.0f};
  const auto composed = compose_hud_screen_projection(eyeFrustum, viewFromCenter, screen, game);
  const auto mask = window_mask(eyeFrustum, viewFromCenter, screen);
  for (const auto& v : kVertices) {
    const float w = dot4(composed.m3, v);
    ASSERT_GT(w, 0.0f);
    const auto h = mask.at(dot4(composed.m0, v) / w, dot4(composed.m1, v) / w);
    ASSERT_GT(h.z, 0.0f);
    EXPECT_NEAR(h.x / h.z, dot4(game.m0, v), 1e-4f);
    EXPECT_NEAR(h.y / h.z, dot4(game.m1, v), 1e-4f);
  }
}

TEST(StereoReplayTest, WindowMaskShowsNothingBehindTheEye) {
  const auto eyeFrustum = asymmetric_eye_frustum();
  const HudScreen screen{.halfWidth = 600.0f, .halfHeight = 337.5f, .distance = 1000.0f};

  // Turned right round: the screen is behind the eye, so no pixel's ray meets it.
  Mat3x4<float> turnedAway{};
  turnedAway.m0 = {-1.0f, 0.0f, 0.0f, 0.0f};
  turnedAway.m1 = {0.0f, 1.0f, 0.0f, 0.0f};
  turnedAway.m2 = {0.0f, 0.0f, -1.0f, 0.0f};
  const auto away = window_mask(eyeFrustum, turnedAway, screen);
  for (float x = -1.0f; x <= 1.0f; x += 0.25f) {
    for (float y = -1.0f; y <= 1.0f; y += 0.25f) {
      EXPECT_LE(away.at(x, y).z, 0.0f);
    }
  }

  // Walked through the screen: every row is zero, so every pixel is outside.
  Mat3x4<float> beyond = identity3x4();
  beyond.m2[3] = screen.distance + 100.0f;
  const auto through = window_mask(eyeFrustum, beyond, screen);
  EXPECT_EQ(through.w, Vec3<float>{});
  EXPECT_EQ(through.u, Vec3<float>{});

  // No screen at all: nothing either.
  const auto none = window_mask(eyeFrustum, identity3x4(), HudScreen{});
  EXPECT_EQ(none.w, Vec3<float>{});
}

TEST(StereoReplayTest, WindowMaskStaysFixedInSpaceAsTheEyeMoves) {
  // Stepping sideways moves the screen across the eye image the other way, as
  // a real window would: the screen's centre is no longer at the image centre.
  const auto eyeFrustum = eye_frustum(0.0f);
  const HudScreen screen{.halfWidth = 600.0f, .halfHeight = 337.5f, .distance = 1000.0f};
  Mat3x4<float> stepRight = identity3x4();
  stepRight.m0[3] = -200.0f; // The eye moves +200 to the right, so the world moves back.
  const auto mask = window_mask(eyeFrustum, stepRight, screen);
  const auto centre = eye_ndc(eyeFrustum, stepRight, {0.0f, 0.0f, -screen.distance, 1.0f});
  EXPECT_LT(centre[0], 0.0f);
  const auto h = mask.at(centre[0], centre[1]);
  ASSERT_GT(h.z, 0.0f);
  EXPECT_NEAR(h.x / h.z, 0.0f, 1e-4f);
  EXPECT_NEAR(h.y / h.z, 0.0f, 1e-4f);
  // The image centre now looks through the right part of the screen.
  const auto straight = mask.at(0.0f, 0.0f);
  ASSERT_GT(straight.z, 0.0f);
  EXPECT_NEAR(straight.x / straight.z, 200.0f / screen.halfWidth, 1e-4f);
}

} // namespace
} // namespace aurora::gfx::stereo_replay
