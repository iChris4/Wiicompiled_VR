#pragma once

#include "webgpu/gpu.hpp"

#include <array>
#include <cstdint>

namespace aurora::stereo {

struct EyeImage {
  // Borrowed for the duration of the sink callback. A sink may encode work
  // that reads the texture, but must not retain these pointers.
  const wgpu::Texture* texture = nullptr;
  const wgpu::TextureView* view = nullptr;
  wgpu::Extent3D size{};
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
};

struct SinkFrame {
  uint64_t frameToken = 0;
  uint32_t logicalFrame = 0;
  AuroraStereoFrameMode mode = AURORA_STEREO_FRAME_IMMERSIVE_REPLAY;
  std::array<EyeImage, AURORA_STEREO_EYE_COUNT> eyes{};
};

// Runs synchronously on the frame worker after both eye replays have been
// encoded and before its command buffer is submitted. Backend interop code
// can append copies/import transitions to the same encoder here.
using SinkCallback = bool (*)(wgpu::CommandEncoder& encoder, const SinkFrame& frame, void* userdata) noexcept;
// Called immediately after the command buffer containing the sink's encoded
// work is submitted to Dawn's queue, while Aurora's queue-submit mutex remains
// held. It may enqueue native work on that same queue, but must not wait or
// call back into Aurora. This is the handoff point for backend-native follow-up
// work; its completion notification may wake the owner thread to release the
// acquired OpenXR images and end the frame.
using SubmitCallback = void (*)(const SinkFrame& frame, void* userdata) noexcept;

// Internal Aurora hook: D3D12/Vulkan OpenXR interop owns this registration.
// Registration changes must happen while the frame worker is idle.
void set_sink(SinkCallback callback, SubmitCallback submitted, void* userdata) noexcept;
inline void set_sink(SinkCallback callback, void* userdata) noexcept { set_sink(callback, nullptr, userdata); }

} // namespace aurora::stereo
