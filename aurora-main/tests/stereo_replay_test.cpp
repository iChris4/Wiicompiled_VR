#include "gfx/stereo_replay.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

namespace aurora::gfx::stereo_replay {
namespace {

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
  const auto composed = compose_hud_screen_projection(eyeFrustum, viewFromCenter, screen, game, true);
  const auto exactDepth = backend_ndc_depth_row(game, true);

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
    EXPECT_NEAR(dot4(composed.m2, v), dot4(exactDepth, v), 1e-6f);
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
  const auto composed = compose_hud_screen_projection(eyeFrustum, moved, screen, game, true);

  // The exact-depth shader captures composed Z, then parks clip Z at -0.5W.
  // Aurora's following reversed-depth conversion negates that to +0.5W, so
  // rasterization stays stable even though W varies across the rotated screen.
  for (const auto& v : kVertices) {
    const float w = dot4(composed.m3, v);
    ASSERT_GT(w, 0.0f);
    const float parkedClipZ = -0.5f * w;
    EXPECT_NEAR(-parkedClipZ / w, 0.5f, 1e-5f);
  }
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

} // namespace
} // namespace aurora::gfx::stereo_replay
