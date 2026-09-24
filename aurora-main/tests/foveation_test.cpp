#include "gfx/foveation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace aurora::gfx::foveation {
namespace {

constexpr float kDegrees = 3.14159265358979f / 180.0f;

// Roughly a Quest 3 left eye: the wider side is the outer (left) one.
EyeFov left_eye() {
  return EyeFov{.tanLeft = std::tan(-54.0f * kDegrees),
                .tanRight = std::tan(43.0f * kDegrees),
                .tanDown = std::tan(-50.0f * kDegrees),
                .tanUp = std::tan(47.0f * kDegrees)};
}

EyeFov right_eye() {
  const EyeFov left = left_eye();
  return EyeFov{.tanLeft = -left.tanRight, .tanRight = -left.tanLeft, .tanDown = left.tanDown, .tanUp = left.tanUp};
}

Map build_map(Level level, const EyeFov& fov = left_eye(), uint32_t width = 1344, uint32_t height = 1408,
              uint32_t texel = 32) {
  Map map;
  foveation::build(width, height, texel, fov, level, map);
  return map;
}

uint8_t at(const Map& map, uint32_t x, uint32_t y) { return map.rg8[(static_cast<size_t>(y) * map.width + x) * 2]; }

// The tangents at a texel centre, as build computes them.
std::pair<float, float> tangents(const Map& map, const EyeFov& fov, uint32_t x, uint32_t y, uint32_t width,
                                 uint32_t height, uint32_t texel) {
  const float u = std::min((x + 0.5f) * texel, static_cast<float>(width)) / width;
  const float v = std::min((y + 0.5f) * texel, static_cast<float>(height)) / height;
  return {fov.tanLeft + (fov.tanRight - fov.tanLeft) * u, fov.tanUp + (fov.tanDown - fov.tanUp) * v};
}

TEST(Foveation, MapCoversTheWholeEye) {
  const Map quest = build_map(Level::Medium);
  EXPECT_EQ(quest.width, 42u);
  EXPECT_EQ(quest.height, 44u);
  EXPECT_EQ(quest.rg8.size(), 42u * 44u * 2u);

  // render_scale 0.75: the last column and row overhang the eye.
  const Map scaled = build_map(Level::Medium, left_eye(), 1260, 1320);
  EXPECT_EQ(scaled.width, 40u);
  EXPECT_EQ(scaled.height, 42u);
}

TEST(Foveation, WritesOnlyWholeHalfAndQuarterDensities) {
  for (Level level : {Level::Low, Level::Medium, Level::High}) {
    const Map map = build_map(level);
    for (size_t i = 0; i < map.rg8.size(); i += 2) {
      const uint8_t value = map.rg8[i];
      EXPECT_TRUE(value == kFullDensity || value == kHalfDensity || value == kQuarterDensity) << int(value);
      // The same density in both directions.
      EXPECT_EQ(map.rg8[i], map.rg8[i + 1]);
    }
  }
  // A half must stay below 1/2 so the fragment size cannot round down to a single pixel.
  EXPECT_LE(kHalfDensity / 255.0f, 0.5f);
  EXPECT_LE(kQuarterDensity / 255.0f, 0.25f);
  EXPECT_GT(kHalfDensity / 255.0f, 0.25f);
}

TEST(Foveation, OffShadesEverythingFully) {
  const Map map = build_map(Level::Off);
  EXPECT_TRUE(std::all_of(map.rg8.begin(), map.rg8.end(), [](uint8_t value) { return value == kFullDensity; }));
}

TEST(Foveation, DensityNeverRisesAwayFromTheForwardDirection) {
  const EyeFov fov = left_eye();
  for (Level level : {Level::Low, Level::Medium, Level::High}) {
    const Map map = build_map(level, fov);
    std::vector<std::pair<float, uint8_t>> texels;
    for (uint32_t y = 0; y < map.height; ++y) {
      for (uint32_t x = 0; x < map.width; ++x) {
        const auto [tanX, tanY] = tangents(map, fov, x, y, 1344, 1408, 32);
        texels.emplace_back(eccentricity_degrees(tanX, tanY), at(map, x, y));
      }
    }
    std::sort(texels.begin(), texels.end());
    for (size_t i = 1; i < texels.size(); ++i) {
      EXPECT_LE(texels[i].second, texels[i - 1].second);
    }
    // Every level shades the centre fully and saves something at the edges.
    EXPECT_EQ(texels.front().second, kFullDensity);
    EXPECT_LT(texels.back().second, kFullDensity);
  }
}

TEST(Foveation, EachEyeCentresOnItsOwnForwardDirection) {
  // The asymmetric frustum puts the forward direction off the image centre, towards the nose.
  const auto fullColumns = [](const Map& map) {
    double sum = 0.0;
    uint32_t count = 0;
    for (uint32_t y = 0; y < map.height; ++y) {
      for (uint32_t x = 0; x < map.width; ++x) {
        if (at(map, x, y) == kFullDensity) {
          sum += x + 0.5;
          ++count;
        }
      }
    }
    return count > 0 ? sum / count : 0.0;
  };
  const Map left = build_map(Level::High, left_eye());
  const Map right = build_map(Level::High, right_eye());
  const EyeFov fov = left_eye();
  const double forward = -fov.tanLeft / (fov.tanRight - fov.tanLeft) * left.width;
  EXPECT_NEAR(fullColumns(left), forward, 1.0);
  EXPECT_GT(fullColumns(left), left.width / 2.0);
  EXPECT_NEAR(fullColumns(right), right.width - fullColumns(left), 1.0);
}

TEST(Foveation, HigherLevelsNeverShadeMore) {
  const Map low = build_map(Level::Low);
  const Map medium = build_map(Level::Medium);
  const Map high = build_map(Level::High);
  for (size_t i = 0; i < low.rg8.size(); ++i) {
    EXPECT_LE(medium.rg8[i], low.rg8[i]);
    EXPECT_LE(high.rg8[i], medium.rg8[i]);
  }
  // Low never goes below half.
  EXPECT_TRUE(std::none_of(low.rg8.begin(), low.rg8.end(), [](uint8_t value) { return value == kQuarterDensity; }));
}

TEST(Foveation, LowAndMediumKeepTheHudScreenAtHalfDensity) {
  // The default HUD screen: 2.4 m wide at 2 m, with a 4:3 picture, looking straight ahead.
  constexpr float kHalfWidth = 1.2f / 2.0f;
  constexpr float kHalfHeight = 0.9f / 2.0f;
  const EyeFov fov = left_eye();
  for (Level level : {Level::Low, Level::Medium}) {
    const Map map = build_map(level, fov);
    uint32_t covered = 0;
    for (uint32_t y = 0; y < map.height; ++y) {
      for (uint32_t x = 0; x < map.width; ++x) {
        const auto [tanX, tanY] = tangents(map, fov, x, y, 1344, 1408, 32);
        if (std::abs(tanX) <= kHalfWidth && std::abs(tanY) <= kHalfHeight) {
          EXPECT_GE(at(map, x, y), kHalfDensity) << "level " << int(level) << " at " << x << "," << y;
          ++covered;
        }
      }
    }
    EXPECT_GT(covered, 100u);
  }
}

TEST(Foveation, ReadsTheFieldOfViewBackFromTheEyeProjection) {
  const EyeFov fov = left_eye();
  std::array<float, 16> projection{};
  // openxr_integration.cpp's ProjectionFromFov.
  projection[0] = 2.0f / (fov.tanRight - fov.tanLeft);
  projection[2] = (fov.tanRight + fov.tanLeft) / (fov.tanRight - fov.tanLeft);
  projection[5] = 2.0f / (fov.tanUp - fov.tanDown);
  projection[6] = (fov.tanUp + fov.tanDown) / (fov.tanUp - fov.tanDown);
  const EyeFov read = fov_from_projection(projection.data());
  EXPECT_NEAR(read.tanLeft, fov.tanLeft, 1e-5f);
  EXPECT_NEAR(read.tanRight, fov.tanRight, 1e-5f);
  EXPECT_NEAR(read.tanDown, fov.tanDown, 1e-5f);
  EXPECT_NEAR(read.tanUp, fov.tanUp, 1e-5f);

  // A projection without a frustum scale leaves the symmetric default.
  const std::array<float, 16> empty{};
  const EyeFov fallback = fov_from_projection(empty.data());
  EXPECT_EQ(fallback.tanLeft, -1.0f);
  EXPECT_EQ(fallback.tanUp, 1.0f);
}

} // namespace
} // namespace aurora::gfx::foveation
