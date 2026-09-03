#include "gfx/stereo_replay.hpp"

#include <gtest/gtest.h>

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
      const bool frustumTerm =
          (row == 0 && (column == 0 || column == 2)) ||
          (row == 1 && (column == 1 || column == 2));
      if (!frustumTerm) {
        EXPECT_FLOAT_EQ(result[row][column], game[row][column]);
      }
    }
  }
}

} // namespace
} // namespace aurora::gfx::stereo_replay
