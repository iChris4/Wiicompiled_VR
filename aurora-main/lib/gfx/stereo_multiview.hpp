#pragma once

// Vulkan multiview for the stereo replay: both eyes of an immersive frame drawn in one render pass.
//
// Per-eye replay re-encodes every recorded draw once per eye, and on the Quest's tiled Adreno each
// draw costs state setup in the binning pass and in every bin whatever its size. Under multiview the
// eye targets are the two layers of one texture, the eye pass renders into both with view mask 0b11
// and each draw is recorded and bound once: the GPU runs it for both views and the vertex shader
// picks the eye through @builtin(view_index). A draw's uniform stays what the per-eye replay stages,
// the two eye copies back to back (gfx/common.cpp prepare_stereo_replay_uniforms), bound as one
// range of twice the uniform size that the multiview shader indexes by the view
// (gx/shader.cpp). The eyes sample the textures the mono render baked, identical for both eyes, so
// nothing else in the shader depends on the view.
//
// The API comes from Aurora's Dawn patches (patches/dawn/apply.py: the DawnMultiview feature,
// DawnRenderPassMultiview / DawnRenderPipelineMultiview, `enable chromium_experimental_multiview;`),
// which the Quest's Dawn carries. With a stock Dawn it compiles out and the replay keeps its per-eye
// passes. After the Metroid Prime port's stereo_multiview.hpp.

#include <webgpu/webgpu_cpp.h>

#include <cstdint>

namespace aurora::gfx::stereo_multiview {

#if defined(WGPU_DAWN_RENDER_PASS_MULTIVIEW_INIT)
inline constexpr bool kApiAvailable = true;
#else
inline constexpr bool kApiAvailable = false;
#endif

// Both eyes: layer 0 is the left eye, layer 1 the right one.
inline constexpr uint32_t kViewMask = 0b11;
inline constexpr uint32_t kViewCount = 2;

// Whether the device has the feature (webgpu/gpu.cpp, at device creation). Any thread.
void set_supported(bool supported) noexcept;
bool supported() noexcept;

// The host's switch (aurora_set_stereo_multiview); on by default. Live: the next frame's eyes use it.
void set_requested(bool requested) noexcept;
bool requested() noexcept;

// Whether a frame whose eyes are `leftWidth` x `leftHeight` and `rightWidth` x `rightHeight` at
// `sampleCount` replays through multiview: the device has the feature, the host wants it, the eyes
// share a size and are single-sampled (a multisampled target would need layered resolves).
bool usable(uint32_t leftWidth, uint32_t leftHeight, uint32_t rightWidth, uint32_t rightHeight,
            uint32_t sampleCount) noexcept;

} // namespace aurora::gfx::stereo_multiview
