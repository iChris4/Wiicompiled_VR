// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR) && defined(MKW_PLATFORM_VISIONOS)

#include "vr/openxr_backend.h"
#include "vr/openxr_runtime.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mkw::vr {

struct OpenXRMetalGraphicsRequirements {
    // MTLPixelFormat of Aurora's eye output, learnt at BindAurora.
    int64_t aurora_pixel_format = 0;
};

// Apple Vision Pro backend: Dawn Metal on one side, the visionOS OpenXR
// provider (CompositorServices) on the other.
//
// Method surface and threading rules are identical to OpenXRD3D12Backend so
// the pacing thread in openxr_integration.cpp is shared. Like D3D12 and unlike
// the Quest, Aurora renders straight into the acquired swapchain images: they
// are IOSurfaces (vr/visionos/xr_visionos.h) that Aurora imports as Dawn shared
// texture memory (aurora/metal_interop.h), and the compositor side samples them
// into its drawable. Ordering between Dawn's queue and the compositor's uses
// MTLSharedEvents: the provider tells the backend which event value the
// compositor's last read of an image reaches (the acquire fence Aurora waits
// on), and Dawn's EndAccess yields the event value its copy completes at (the
// release fence the backend hands back before releasing the image).
//
// QueryGraphicsRequirements() runs after OpenXRRuntime::Initialize() and before
// aurora_initialize(); BindAurora() runs afterwards, while Aurora's frame worker
// is idle. Everything from BeginFrame() through FinishFrame() belongs to one XR
// pacing thread; Aurora's callback only publishes a token and the release fences.
class OpenXRMetalBackend final {
public:
    explicit OpenXRMetalBackend(OpenXRLogCallback logger = {});
    ~OpenXRMetalBackend();

    OpenXRMetalBackend(const OpenXRMetalBackend&) = delete;
    OpenXRMetalBackend& operator=(const OpenXRMetalBackend&) = delete;
    OpenXRMetalBackend(OpenXRMetalBackend&&) = delete;
    OpenXRMetalBackend& operator=(OpenXRMetalBackend&&) = delete;

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime);
    bool BindAurora(OpenXRRuntime& runtime);

    // As OpenXRD3D12Backend::SetRenderScale: the writable pair is rebuilt at the
    // new size the next time Aurora writes it; Aurora drops its IOSurface imports
    // of a replaced pair before it is destroyed.
    void SetRenderScale(float scale);

    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus WaitForSubmission(const OpenXRBackendFrame& frame, uint32_t timeout_ms = UINT32_MAX);
    bool TryCancelPendingFrame(OpenXRBackendFrame& frame);
    bool RepeatFrame(const OpenXRBackendFrame& frame);
    bool FinishFrame(OpenXRBackendFrame& frame, bool submit_layer);

    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet);
    bool TryCancelPendingPacket(OpenXRBackendFrame& packet);
    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame);
    OpenXRBeginStatus KeepAliveCycle();

    bool Shutdown();

    bool IsBound() const;
    bool PanelLayerAvailable() const;
    const OpenXRMetalGraphicsRequirements& GraphicsRequirements() const;
    int64_t SwapchainFormat() const;
    const std::string& LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(MKW_PLATFORM_VISIONOS)
