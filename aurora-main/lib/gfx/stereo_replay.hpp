#pragma once

#include <aurora/math.hpp>

namespace aurora::gfx::stereo_replay {

// An OpenXR eye supplies the shape of its asymmetric frustum, but the sealed
// GX draw already contains the depth mapping adjusted for that draw's GX
// viewport and Aurora's reversed-Z convention. Replacing the complete matrix
// would pair an unrelated depth range with the original pipeline compare and
// clear state, which can reject the entire eye. Replace only the four
// perspective-frustum coefficients and preserve every depth-related element.
inline Mat4x4<float> compose_projection(const Mat4x4<float>& eyeFrustum,
                                        const Mat4x4<float>& gameProjection) noexcept {
  Mat4x4<float> out = gameProjection;
  out.m0[0] = eyeFrustum.m0[0];
  out.m0[2] = eyeFrustum.m0[2];
  out.m1[1] = eyeFrustum.m1[1];
  out.m1[2] = eyeFrustum.m1[2];
  return out;
}

// Aurora stores the GX 3x4 matrices row-major. The vertex shader consumes
// them as vec4 * mat3x4, which is equivalent to the original column-vector
// affine transform. Applying an eye-space delta therefore composes delta *
// objectToCenter in the ordinary row-major notation used below.
inline Mat3x4<float> compose_affine(const Mat3x4<float>& viewFromCenter,
                                    const Mat3x4<float>& objectToCenter) noexcept {
  Mat3x4<float> out{};
  for (size_t row = 0; row < 3; ++row) {
    auto& dst = *(&out.m0 + row);
    const auto& view = *(&viewFromCenter.m0 + row);
    for (size_t column = 0; column < 3; ++column) {
      dst[column] = view[0] * objectToCenter.m0[column] + view[1] * objectToCenter.m1[column] +
                    view[2] * objectToCenter.m2[column];
    }
    dst[3] = view[3] + view[0] * objectToCenter.m0[3] + view[1] * objectToCenter.m1[3] +
             view[2] * objectToCenter.m2[3];
  }
  return out;
}

// Normals receive only the eye transform's linear part. OpenXR view deltas
// are rigid transforms, so no inverse-transpose correction is needed here.
inline Mat3x4<float> compose_normal(const Mat3x4<float>& viewFromCenter,
                                    const Mat3x4<float>& objectToCenter) noexcept {
  Mat3x4<float> out{};
  for (size_t row = 0; row < 3; ++row) {
    auto& dst = *(&out.m0 + row);
    const auto& view = *(&viewFromCenter.m0 + row);
    for (size_t column = 0; column < 3; ++column) {
      dst[column] = view[0] * objectToCenter.m0[column] + view[1] * objectToCenter.m1[column] +
                    view[2] * objectToCenter.m2[column];
    }
    dst[3] = 0.0f;
  }
  return out;
}

} // namespace aurora::gfx::stereo_replay
