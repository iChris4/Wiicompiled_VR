#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Fixed foveated rendering for the immersive eyes: the fragment density map an eye's render pass
// runs under (webgpu/fdm.hpp). Each texel says how finely the framebuffer area it covers is shaded:
// fully at the centre of the view, in 2x2 then 4x4 pixel blocks towards the edges, where the
// headset's lenses blur the picture anyway.
namespace aurora::gfx::foveation {

enum class Level : uint32_t {
  Off = 0,
  Low = 1,
  Medium = 2,
  High = 3,
};
inline constexpr uint32_t kLevelCount = 4;

// A texel's density is its byte over 255 and a fragment covers 1/density pixels in that direction,
// rounded down to a size the GPU supports. A half is therefore written just below 128, so that it
// cannot round back to a single pixel.
inline constexpr uint8_t kFullDensity = 255;
inline constexpr uint8_t kHalfDensity = 127;
inline constexpr uint8_t kQuarterDensity = 63;

// Tangents of an eye's field of view, left and down negative.
struct EyeFov {
  float tanLeft = -1.0f;
  float tanRight = 1.0f;
  float tanDown = -1.0f;
  float tanUp = 1.0f;
};

// From AuroraStereoEye::projection, row-major: [0] = 2/(r-l), [2] = (r+l)/(r-l), [5] = 2/(u-d),
// [6] = (u+d)/(u-d), with l, r, d, u the tangents (openxr_integration.cpp, ProjectionFromFov).
inline EyeFov fov_from_projection(const float* projection) noexcept {
  const float sx = projection[0];
  const float cx = projection[2];
  const float sy = projection[5];
  const float cy = projection[6];
  if (!(sx > 0.0f) || !(sy > 0.0f)) {
    return {};
  }
  return EyeFov{
      .tanLeft = (cx - 1.0f) / sx,
      .tanRight = (cx + 1.0f) / sx,
      .tanDown = (cy - 1.0f) / sy,
      .tanUp = (cy + 1.0f) / sy,
  };
}

// Angles from the eye's forward direction, in degrees, below which a level shades fully and then at
// half density; beyond the second, a quarter. Low never drops below half. The default HUD screen
// (2.4 m wide at 2 m) reaches about 37 degrees at its corners with a 4:3 picture, so Low and Medium
// keep it at half density or better when looking straight ahead.
struct Rings {
  float full = 90.0f;
  float half = 90.0f;
};

inline Rings rings(Level level) noexcept {
  switch (level) {
  case Level::Low:
    return {.full = 30.0f, .half = 90.0f};
  case Level::Medium:
    return {.full = 25.0f, .half = 40.0f};
  case Level::High:
    return {.full = 18.0f, .half = 34.0f};
  default:
    return {};
  }
}

// The angle between the forward direction and the ray through a point at tangents (x, y).
inline float eccentricity_degrees(float tanX, float tanY) noexcept {
  return std::atan(std::sqrt(tanX * tanX + tanY * tanY)) * (180.0f / 3.14159265358979f);
}

inline uint8_t density(Level level, float eccentricity) noexcept {
  const Rings ring = rings(level);
  if (eccentricity < ring.full) {
    return kFullDensity;
  }
  return eccentricity < ring.half ? kHalfDensity : kQuarterDensity;
}

struct Map {
  uint32_t width = 0;
  uint32_t height = 0;
  // Two bytes per texel, horizontal then vertical density, rows packed top to bottom.
  std::vector<uint8_t> rg8;
};

// The map for an eye of `eyeWidth` by `eyeHeight` pixels whose field of view is `fov`, `texel` pixels
// per map texel. The map covers the whole eye, its last row and column possibly overhanging it.
inline void build(uint32_t eyeWidth, uint32_t eyeHeight, uint32_t texel, const EyeFov& fov, Level level,
                  Map& map) {
  map.width = texel > 0 ? (eyeWidth + texel - 1) / texel : 0;
  map.height = texel > 0 ? (eyeHeight + texel - 1) / texel : 0;
  map.rg8.assign(static_cast<size_t>(map.width) * map.height * 2, kFullDensity);
  if (level == Level::Off || eyeWidth == 0 || eyeHeight == 0) {
    return;
  }
  for (uint32_t y = 0; y < map.height; ++y) {
    // Texel centres, clamped to the eye for an overhanging last row or column.
    const float v = std::min((static_cast<float>(y) + 0.5f) * static_cast<float>(texel), static_cast<float>(eyeHeight)) /
                    static_cast<float>(eyeHeight);
    const float tanY = fov.tanUp + (fov.tanDown - fov.tanUp) * v;
    for (uint32_t x = 0; x < map.width; ++x) {
      const float u = std::min((static_cast<float>(x) + 0.5f) * static_cast<float>(texel), static_cast<float>(eyeWidth)) /
                      static_cast<float>(eyeWidth);
      const float tanX = fov.tanLeft + (fov.tanRight - fov.tanLeft) * u;
      const uint8_t value = density(level, eccentricity_degrees(tanX, tanY));
      uint8_t* texelBytes = &map.rg8[(static_cast<size_t>(y) * map.width + x) * 2];
      texelBytes[0] = value;
      texelBytes[1] = value;
    }
  }
}

} // namespace aurora::gfx::foveation
