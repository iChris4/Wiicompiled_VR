#pragma once

#include <aurora/math.hpp>

namespace aurora::gfx::stereo_replay {

// An OpenXR eye supplies the shape of its asymmetric frustum, but the sealed
// GX draw already contains the depth mapping adjusted for that draw's GX
// viewport and Aurora's reversed-Z convention. Replacing the complete matrix
// would pair an unrelated depth range with the original pipeline compare and
// clear state, which can reject the entire eye. Replace only the four
// perspective-frustum coefficients and preserve every depth-related element.
inline Mat4x4<float> compose_projection(const Mat4x4<float>& eyeFrustum, const Mat4x4<float>& gameProjection) noexcept {
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
inline Mat3x4<float> compose_affine(const Mat3x4<float>& viewFromCenter, const Mat3x4<float>& objectToCenter) noexcept {
  Mat3x4<float> out{};
  for (size_t row = 0; row < 3; ++row) {
    auto& dst = *(&out.m0 + row);
    const auto& view = *(&viewFromCenter.m0 + row);
    for (size_t column = 0; column < 3; ++column) {
      dst[column] = view[0] * objectToCenter.m0[column] + view[1] * objectToCenter.m1[column] +
                    view[2] * objectToCenter.m2[column];
    }
    dst[3] = view[3] + view[0] * objectToCenter.m0[3] + view[1] * objectToCenter.m1[3] + view[2] * objectToCenter.m2[3];
  }
  return out;
}

// Normals receive only the eye transform's linear part. OpenXR view deltas
// are rigid transforms, so no inverse-transpose correction is needed here.
inline Mat3x4<float> compose_normal(const Mat3x4<float>& viewFromCenter, const Mat3x4<float>& objectToCenter) noexcept {
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

// A fixed virtual screen for the game's 2D content, sized and placed in the
// recorded center-eye view space: a rectangle `distance` units straight ahead
// of the game camera, `halfWidth` by `halfHeight` units across. It stays where
// the camera puts it, so turning the head looks around it rather than dragging
// it along.
struct HudScreen {
  float halfWidth = 0.0f;
  float halfHeight = 0.0f;
  float distance = 0.0f;

  [[nodiscard]] bool valid() const noexcept { return halfWidth > 0.0f && halfHeight > 0.0f && distance > 0.0f; }
};

// Converts a draw's viewport-local NDC into the NDC of the complete displayed
// frame. It is identity for a full-frame viewport. Virtual-screen replay uses a
// full-eye host viewport, so this keeps sub-pane HUD elements in their original
// part of the 2D screen instead of applying their viewport twice.
struct HudNdcRemap {
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  float offsetX = 0.0f;
  float offsetY = 0.0f;
};

inline HudNdcRemap make_hud_ndc_remap(float viewportLeft, float viewportTop, float viewportWidth, float viewportHeight,
                                      float frameLeft, float frameTop, float frameWidth, float frameHeight) noexcept {
  if (!(frameWidth > 0.0f) || !(frameHeight > 0.0f)) {
    return {};
  }
  return {
      .scaleX = viewportWidth / frameWidth,
      .scaleY = viewportHeight / frameHeight,
      .offsetX = (2.0f * (viewportLeft - frameLeft) + viewportWidth) / frameWidth - 1.0f,
      .offsetY = 1.0f - (2.0f * (viewportTop - frameTop) + viewportHeight) / frameHeight,
  };
}

inline Mat4x4<float> remap_hud_ndc(const Mat4x4<float>& projection, const HudNdcRemap& remap) noexcept {
  Mat4x4<float> out = projection;
  for (size_t i = 0; i < 4; ++i) {
    out.m0[i] = projection.m0[i] * remap.scaleX + projection.m3[i] * remap.offsetX;
    out.m1[i] = projection.m1[i] * remap.scaleY + projection.m3[i] * remap.offsetY;
  }
  return out;
}

// A GX orthographic projection is affine: apply_xf_projection writes exactly
// (0, 0, 0, 1) into its w row, and the renderer's depth-window flip only ever
// touches the z row. An orthographic draw's clip position is therefore already
// its NDC position, which is what compose_hud_screen_projection relies on.
inline bool is_orthographic_projection(const Mat4x4<float>& projection) noexcept {
  return projection.m3[0] == 0.0f && projection.m3[1] == 0.0f && projection.m3[2] == 0.0f && projection.m3[3] == 1.0f;
}

// The stored GX projection has not yet passed through Aurora's final clip-depth
// conversion. Turn its Z row into the 0..1 backend NDC value that the original
// orthographic draw would have produced. The virtual-screen shader captures
// this row before replacing raster depth with a stable midrange value.
inline Vec4<float> backend_ndc_depth_row(const Mat4x4<float>& projection, bool reversedDepth) noexcept {
  Vec4<float> row{};
  for (size_t i = 0; i < 4; ++i) {
    row[i] = reversedDepth ? -projection.m2[i] : projection.m2[i] + projection.m3[i];
  }
  return row;
}

// Replaces an orthographic draw's projection so its 2D output lands on the
// fixed virtual screen instead of being stretched across the whole eye.
//
// The GX vertex shader computes `vec4(mv_pos, 1) * proj`, reading m0..m3 as the
// x/y/z/w rows of that product, so for an orthographic draw m0 and m1 already
// yield the game's NDC x/y and m2 its NDC depth. This composes three more steps
// into the same matrix:
//
//   1. NDC to a point on the screen rectangle in the recorded center-eye view
//      space: (ndc.x * halfWidth, ndc.y * halfHeight, -distance).
//   2. That space into this eye's view space, through viewFromCenter.
//   3. Eye view space into clip space, through the OpenXR frustum's four terms.
//
// Each step is affine in the vertex position, so the whole chain collapses into
// one projection matrix and the draw's own position matrices stay untouched.
//
// The composed Z row carries the original flat-screen NDC depth. The exact-depth
// vertex variant captures it, then parks clip depth in the middle of the volume
// for stable rasterization; the fragment variant exports the captured value.
// Keeping original depth out of the VR perspective divide is what makes
// equal-depth 2D layers deterministic under head rotation and translation.
inline Mat4x4<float> compose_hud_screen_projection(const Mat4x4<float>& eyeFrustum, const Mat3x4<float>& viewFromCenter,
                                                   const HudScreen& screen, const Mat4x4<float>& gameProjection,
                                                   bool reversedDepth, const HudNdcRemap& ndcRemap = {}) noexcept {
  const Mat4x4<float> frameProjection = remap_hud_ndc(gameProjection, ndcRemap);
  // The screen point's three coordinates, each as a functional of (mv_pos, 1).
  Mat3x4<float> screenPoint{};
  for (size_t i = 0; i < 4; ++i) {
    screenPoint.m0[i] = frameProjection.m0[i] * screen.halfWidth;
    screenPoint.m1[i] = frameProjection.m1[i] * screen.halfHeight;
    screenPoint.m2[i] = 0.0f;
  }
  screenPoint.m2[3] = -screen.distance;

  // The same functionals carried into eye view space. viewFromCenter's own
  // translation column joins the constant term, the one place the implicit 1 of
  // the homogeneous screen point contributes.
  Mat3x4<float> eyePoint{};
  for (size_t row = 0; row < 3; ++row) {
    auto& dst = *(&eyePoint.m0 + row);
    const auto& view = *(&viewFromCenter.m0 + row);
    for (size_t i = 0; i < 4; ++i) {
      dst[i] = view[0] * screenPoint.m0[i] + view[1] * screenPoint.m1[i] + view[2] * screenPoint.m2[i];
    }
    dst[3] += view[3];
  }

  const Vec4<float> exactDepthRow = backend_ndc_depth_row(gameProjection, reversedDepth);
  Mat4x4<float> out{};
  for (size_t i = 0; i < 4; ++i) {
    out.m0[i] = eyeFrustum.m0[0] * eyePoint.m0[i] + eyeFrustum.m0[2] * eyePoint.m2[i];
    out.m1[i] = eyeFrustum.m1[1] * eyePoint.m1[i] + eyeFrustum.m1[2] * eyePoint.m2[i];
    out.m3[i] = -eyePoint.m2[i];
    // The exact-depth shader captures this original flat-screen value before
    // parking the geometry at 0.5 for rasterization.
    out.m2[i] = exactDepthRow[i];
  }
  return out;
}

} // namespace aurora::gfx::stereo_replay
