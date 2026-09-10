#include "stereo_interpolation.hpp"
#include <gtest/gtest.h>
#include <cmath>

TEST(StereoInterpolation, ContinuousMotionAcross60HzScenesAtHeadsetRates) {
  constexpr uint64_t interval = 16'666'667;
  // A camera/object moving one unit per guest frame must advance uniformly,
  // even at 72/90 Hz where many samples are neither midpoints nor endpoints.
  for (uint64_t hz : {72u, 90u, 120u}) {
    double previousPosition = -1;
    for (uint64_t sample = 1; sample <= hz; ++sample) {
      const uint64_t displayTime = 1'000'000'000 + sample * 1'000'000'000 / hz;
      const uint64_t scene = (displayTime - 1'000'000'000) / interval;
      const uint64_t boundary = 1'000'000'000 + scene * interval;
      const float weight = aurora::stereo::interpolation_weight(displayTime, boundary, interval);
      const double position = static_cast<double>(scene) + weight;
      if (sample > 1)
        EXPECT_NEAR(position - previousPosition, 1'000'000'000.0 / hz / interval, 1e-5);
      previousPosition = position;
    }
  }
}

TEST(StereoInterpolation, MissingTimingAndStallsDoNotExtrapolate) {
  using aurora::stereo::interpolation_weight;
  EXPECT_FLOAT_EQ(interpolation_weight(0, 100, 10), 1);
  EXPECT_FLOAT_EQ(interpolation_weight(105, 0, 10), 1);
  EXPECT_FLOAT_EQ(interpolation_weight(105, 100, 0), 1);
  EXPECT_FLOAT_EQ(interpolation_weight(95, 100, 10), 0);
  EXPECT_FLOAT_EQ(interpolation_weight(105, 100, 10), 0.5);
  EXPECT_FLOAT_EQ(interpolation_weight(500, 100, 10), 1);
}
