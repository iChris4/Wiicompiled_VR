// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"
#include "vr/openxr_settings_panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace mkw::vr {

// Backend-neutral frame vocabulary shared by every graphics binding.
//
// The OpenXR pacing thread in openxr_integration.cpp is written against these
// types and the method surface documented on OpenXRD3D12Backend; each concrete
// backend (D3D12 on Windows, Vulkan on Android) implements that same surface so
// the pacing, retained-layer and policy logic is compiled once for both.

enum class OpenXRFrameMode {
    ImmersiveProjection,
    VirtualScreen,
};

enum class OpenXRBeginStatus {
    Ready,
    SessionNotRunning,
    ExitRequested,
    Error,
};

enum class OpenXRSubmissionStatus {
    Success,
    // GPU work may have touched the compositor image or the shared buffers with no completion
    // marker to wait on; the session cannot continue.
    Failed,
    // The eye copy was not submitted and nothing touched the compositor image or the shared
    // buffers, so the frame may end without a layer and the next one is tried normally.
    Skipped,
    Timeout,
    ShuttingDown,
};

// The headset settings panel as a compositor quad layer of its own, over the
// eyes or the menu screen, so the eye resolution never limits its text. Its
// image is rendered with the frame's eyes (Aurora's panel stereo target) into a
// swapchain of the panel canvas's own size. Nothing is allocated or copied
// until the panel first opens, and nothing is submitted while it is closed.
struct OpenXRPanelLayer {
    // The frame renders the panel's image: set by the pacing thread when the
    // panel is open, cleared by a backend that could not provide the layer.
    bool requested = false;
    // Where it hangs in the application space, once the head pose is known.
    bool placed = false;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float width_meters = 0.0f;
    float height_meters = 0.0f;
};

// The panel image's size, which is the settings panel canvas's.
inline constexpr uint32_t kOpenXRPanelLayerWidth = static_cast<uint32_t>(kSettingsPanelWidthPixels);
inline constexpr uint32_t kOpenXRPanelLayerHeight = static_cast<uint32_t>(kSettingsPanelHeightPixels);

// The panel's layer, submitted after (so over) the scene's.
inline XrCompositionLayerQuad OpenXRPanelQuadLayer(const OpenXRPanelLayer& panel, XrSpace space,
                                                   XrSwapchain swapchain) noexcept {
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // ImGui leaves premultiplied colour in the cleared panel image.
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = swapchain;
    quad.subImage.imageRect = {{0, 0},
                               {static_cast<int32_t>(kOpenXRPanelLayerWidth),
                                static_cast<int32_t>(kOpenXRPanelLayerHeight)}};
    quad.subImage.imageArrayIndex = 0;
    quad.pose = panel.pose;
    quad.size = {panel.width_meters, panel.height_meters};
    return quad;
}

// The part of the virtual screen's image that holds anything. Aurora
// letterboxes the desktop snapshot into that eye-sized image exactly like this
// (webgpu::calculate_present_viewport_for_aspect) and, when the settings panel
// is drawn into the eyes, centres it at kSettingsPanelWidthFraction of the
// width; the rest is black. An unknown content_aspect (0) keeps the whole image.
inline XrRect2Di OpenXRVirtualScreenContentRect(uint32_t width, uint32_t height, float content_aspect) noexcept {
    XrRect2Di rect{{0, 0}, {static_cast<int32_t>(width), static_cast<int32_t>(height)}};
    if (width == 0 || height == 0 || !(content_aspect > 0.0f)) {
        return rect;
    }
    uint32_t content_width = width;
    uint32_t content_height = std::min<uint32_t>(
        height, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(
                                           static_cast<double>(width) * static_cast<double>(1.0f / content_aspect)))));
    if (content_height == height) {
        content_width = std::min<uint32_t>(
            width, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(height) *
                                                                            static_cast<double>(content_aspect)))));
    }
    const uint32_t panel_width = std::min<uint32_t>(
        width, static_cast<uint32_t>(std::ceil(static_cast<double>(width) * kSettingsPanelWidthFraction)));
    const uint32_t panel_height = std::min<uint32_t>(
        height, static_cast<uint32_t>(std::ceil(static_cast<double>(panel_width) * kSettingsPanelHeightPixels /
                                                kSettingsPanelWidthPixels)));
    const uint32_t shown_width = std::max(content_width, panel_width);
    const uint32_t shown_height = std::max(content_height, panel_height);
    rect.offset = {static_cast<int32_t>((width - shown_width) / 2), static_cast<int32_t>((height - shown_height) / 2)};
    rect.extent = {static_cast<int32_t>(shown_width), static_cast<int32_t>(shown_height)};
    return rect;
}

struct OpenXRPresentation {
    OpenXRFrameMode mode = OpenXRFrameMode::ImmersiveProjection;

    // Used only by VirtualScreen.
    float quad_distance_meters = 2.0f;
    float quad_width_meters = 2.4f;
    // The desktop snapshot's width over height, which Aurora letterboxes into
    // the screen's image (see OpenXRVirtualScreenContentRect); 0 while unknown.
    float quad_content_aspect = 0.0f;

    // When quad_anchored is set, the quad is placed at quad_pose in the
    // application reference space and stays put as the player looks around.
    // Otherwise it falls back to being head-locked in XR_VIEW_SPACE, centered
    // straight ahead at -Z, which is what happens until tracking has produced a
    // head pose good enough to anchor against.
    bool quad_anchored = false;
    XrPosef quad_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

    // Used only by ImmersiveProjection: Aurora left each eye transparent outside
    // the race's 2D-layer screen (AuroraStereoFrame::window), so the projection
    // layer is blended by its alpha over whatever is under it.
    bool immersive_window = false;

    // Show the room through the headset's cameras around the virtual screen or
    // the immersive window (OpenXRPassthrough). Taken when the presentation is
    // handed to the backend, which starts or pauses the view then; a backend
    // without one ignores it.
    bool passthrough = false;

    OpenXRPanelLayer panel;
};

struct OpenXRBackendFrame {
    OpenXRFrame xr_frame;
    OpenXRPresentation presentation;
    std::array<uint32_t, kOpenXREyeCount> render_width{};
    std::array<uint32_t, kOpenXREyeCount> render_height{};
    bool expects_gpu_submission = false;
};

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
