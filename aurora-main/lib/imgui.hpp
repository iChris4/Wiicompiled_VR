#pragma once

#include <aurora/event.h>
#include <memory>

union SDL_Event;
struct ImDrawData;

namespace wgpu {
class RenderPassEncoder;
} // namespace wgpu

namespace aurora::imgui {
void create_context() noexcept;
void initialize() noexcept;
void shutdown() noexcept;

void process_event(const SDL_Event& event) noexcept;
bool wants_capture_event(const SDL_Event& event) noexcept;
void new_frame(const AuroraWindowSize& size) noexcept;
// Build this frame's ImGui draw data. Idempotent for the rest of the frame: the lists are built
// once and every presentation slot replays them. Reset by new_frame.
void render_frame_data() noexcept;
void render(const wgpu::RenderPassEncoder& pass) noexcept;

// The headset panel as aurora_imgui_set_stereo_overlay last set it.
struct StereoOverlay {
  ImDrawData* drawData = nullptr;
  float widthFraction = 0.f;
  // aurora_imgui_set_stereo_badge.
  ImDrawData* badgeDrawData = nullptr;
  float badgeWidthFraction = 0.f;
  float badgeCenterX = 0.f;
  float badgeCenterY = 0.f;
};
StereoOverlay latch_stereo_overlay() noexcept;
// Renders another context's draw data with this context's backend. The backend keeps one projection
// uniform for every pass, so a pass whose display size differs from the desktop's must be submitted
// before the next pass is recorded.
bool render_draw_data(const wgpu::RenderPassEncoder& pass, ImDrawData* data) noexcept;

// Host-owned ImGui frames. The host starts each frame on its own thread with host_frame_begin() and
// closes it with host_frame_end(), which renders the frame and copies its draw data out of the shared
// context. The copy is what the sealed frame replays, so the host may start the next frame while the
// worker still encodes this one, and aurora stops starting frames itself once the host has begun one.
struct HostFrame;
using HostFramePtr = std::shared_ptr<HostFrame>;
void host_frame_begin(const AuroraWindowSize& size) noexcept;
HostFramePtr host_frame_end() noexcept;
bool host_frames_active() noexcept;
const ImDrawData* host_frame_draw_data(const HostFrame& frame) noexcept;
// Renders a host frame's copied draw data in place of the shared context's.
void render(const wgpu::RenderPassEncoder& pass, const ImDrawData* data) noexcept;
} // namespace aurora::imgui
