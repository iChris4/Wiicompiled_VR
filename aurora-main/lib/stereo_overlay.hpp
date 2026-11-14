#pragma once

#include <aurora/math.hpp>
#include <webgpu/webgpu_cpp.h>

#include "stereo.hpp"

#include <cstdint>

struct ImDrawData;

// The headset-only panel of aurora_imgui_set_stereo_overlay. Everything here
// belongs to the frame worker, like the eye targets it draws into.
namespace aurora::stereo_overlay {

// Renders the panel's draw data into the panel texture and returns the command
// buffer holding that pass, which the caller submits before recording any other
// ImGui pass: the ImGui backend's projection uniform is shared by every pass, and
// the panel's canvas is not the desktop's size. Null draw data hides the panel,
// and so does any failure; both return a null command buffer.
wgpu::CommandBuffer prepare(ImDrawData* drawData, float widthFraction) noexcept;
// The badge (aurora_imgui_set_stereo_badge): the same, into its own texture. Submit it after the
// panel's and before any other ImGui pass, for the same reason.
wgpu::CommandBuffer prepare_badge(ImDrawData* drawData, float widthFraction, float centerX, float centerY) noexcept;

// Draws the most recently prepared panel over a finished immersive eye, on the
// 2D layer's virtual screen seen through that eye's frustum and view, then the
// badge (which layer mode does not take away).
void composite_immersive(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& eye,
                         const Mat4x4<float>& eyeFrustum, const Mat3x4<float>& viewFromCenter,
                         uint32_t eyeIndex) noexcept;

// Draws it over a virtual-screen eye image, which is shown flat as a quad.
void composite_flat(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& eye, const wgpu::Extent3D& size,
                    uint32_t eyeIndex) noexcept;

// Layer mode: the OpenXR backend shows the panel as its own compositor quad
// layer, sharp at any eye resolution, so it is no longer drawn into the eyes
// (both composite functions do nothing). Any thread; read by the frame worker.
void set_layer_mode(bool enabled) noexcept;
bool layer_mode() noexcept;

// Frame worker, inside a stereo sink: the image to copy into a panel layer of
// width x height in the eyes' format. That is the panel while it is showing at
// exactly that size, and otherwise a transparent image of that size, so a layer
// asked for before the panel's first frame (or after it closed) shows nothing.
// A transparent image is cleared once, by a pass recorded into `encoder`.
// False only when no image can be made.
bool layer_source(const wgpu::CommandEncoder& encoder, uint32_t width, uint32_t height, stereo::EyeImage& out) noexcept;

void shutdown() noexcept;

} // namespace aurora::stereo_overlay
