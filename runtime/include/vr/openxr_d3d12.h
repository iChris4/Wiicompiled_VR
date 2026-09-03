// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)

#include "vr/openxr_runtime.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace mkw::vr {

enum class OpenXRD3D12FrameMode {
    ImmersiveProjection,
    VirtualScreen,
};

enum class OpenXRD3D12BeginStatus {
    Ready,
    SessionNotRunning,
    ExitRequested,
    Error,
};

enum class OpenXRD3D12SubmissionStatus {
    Success,
    Failed,
    Timeout,
    ShuttingDown,
};

struct OpenXRD3D12GraphicsRequirements {
    uint32_t adapter_luid_low = 0;
    int32_t adapter_luid_high = 0;
    uint32_t minimum_feature_level = 0;
};

struct OpenXRD3D12Presentation {
    OpenXRD3D12FrameMode mode = OpenXRD3D12FrameMode::ImmersiveProjection;

    // Used only by VirtualScreen. The quad is head-locked in XR_VIEW_SPACE
    // and centered straight ahead at -Z.
    float quad_distance_meters = 2.0f;
    float quad_width_meters = 2.4f;
};

struct OpenXRD3D12Frame {
    OpenXRFrame xr_frame;
    OpenXRD3D12Presentation presentation;
    std::array<uint32_t, kOpenXREyeCount> render_width{};
    std::array<uint32_t, kOpenXREyeCount> render_height{};
    bool expects_gpu_submission = false;
};

// Same-device Dawn/OpenXR D3D12 backend.
//
// Startup is deliberately split in two. QueryGraphicsRequirements() runs
// after OpenXRRuntime::Initialize() but before aurora_initialize(), allowing
// its LUID to be placed in AuroraConfig. BindAurora() runs afterwards and
// rejects any device/queue that does not match the queried runtime adapter.
//
// All methods from BeginFrame() through FinishFrame(), plus PollEvents on the
// associated OpenXRRuntime, belong to one XR pacing thread. Aurora's frame
// worker never calls OpenXR: its post-submit callback only publishes a token
// that WaitForSubmission() consumes. This is the synchronization boundary
// required by the asynchronous sealed-frame renderer.
class OpenXRD3D12Backend final {
public:
    explicit OpenXRD3D12Backend(OpenXRLogCallback logger = {});
    ~OpenXRD3D12Backend();

    OpenXRD3D12Backend(const OpenXRD3D12Backend&) = delete;
    OpenXRD3D12Backend& operator=(const OpenXRD3D12Backend&) = delete;
    OpenXRD3D12Backend(OpenXRD3D12Backend&&) = delete;
    OpenXRD3D12Backend& operator=(OpenXRD3D12Backend&&) = delete;

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime);
    bool BindAurora(OpenXRRuntime& runtime);

    OpenXRD3D12BeginStatus BeginFrame(const OpenXRD3D12Presentation& presentation,
                                      OpenXRD3D12Frame& frame);

    // timeout_ms == UINT32_MAX waits until Aurora publishes this token or
    // Shutdown() interrupts the wait. A timeout does not release XR images;
    // the caller must first drain/cancel Aurora, then FinishFrame(false).
    OpenXRD3D12SubmissionStatus WaitForSubmission(const OpenXRD3D12Frame& frame,
                                                  uint32_t timeout_ms = UINT32_MAX);

    // Withdraws this token only if Aurora has not encoded it. On success no GPU
    // command can reference the acquired images, and FinishFrame(frame, false)
    // is required to release them and close the compositor frame.
    bool TryCancelPendingFrame(OpenXRD3D12Frame& frame);

    // Releases acquired images and calls xrEndFrame. submit_layer must only be
    // true after WaitForSubmission returned Success. Immersive frames submit
    // XrCompositionLayerProjection; virtual-screen frames submit a head-locked
    // XrCompositionLayerQuad using the single mono target.
    bool FinishFrame(OpenXRD3D12Frame& frame, bool submit_layer);

    // Call on the XR owner thread after Aurora's worker is idle and before
    // aurora_shutdown(). Safe to repeat. False means a submitted D3D12 command
    // could not be fenced; the caller must retain this backend and its runtime
    // for the process lifetime instead of destroying possibly live resources.
    bool Shutdown();

    bool IsBound() const;
    const OpenXRD3D12GraphicsRequirements& GraphicsRequirements() const;
    int64_t SwapchainFormat() const;
    const std::string& LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
