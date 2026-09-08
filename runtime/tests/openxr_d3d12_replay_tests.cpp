// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real backend against a deterministic compositor and Aurora sink.
// No headset, graphics driver, OpenXR loader, or translated game is required.
#define CINTERFACE
#define XR_USE_GRAPHICS_API_D3D12
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <openxr/openxr_platform.h>
#include <aurora/d3d12_interop.h>
#include "vr/openxr_d3d12.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace mkw::vr;

namespace {
void RequireAt(bool condition, int line, const char* expression) {
    if (!condition) {
        std::cerr << "OpenXR replay test failed at line " << line << ": " << expression << '\n';
        std::abort();
    }
}
#define Require(condition) RequireAt((condition), __LINE__, #condition)
struct Image { uint64_t content = 0; };
struct Swapchain {
    Image image;
    bool acquired = false;
    bool waited = false;
    bool released = false;
};
std::vector<AuroraD3D12StereoTarget> targets;
AuroraD3D12StereoSubmittedCallback callback = nullptr;
void* callback_data = nullptr;
uint64_t pending_token = 0;
bool encoded = false;
bool should_render = true;
bool tracking_valid = true;
bool change_space = false;
bool restart_session = false;
bool stop_session = false;
bool fail_end = false;
uint64_t displayed_content = 0;
uint32_t layer_count = 0;
uint32_t releases = 0;
uint32_t live_swapchains = 0;
XrTime display_time = 0;
XrStructureType layer_type = XR_TYPE_UNKNOWN;
XrPosef quad_pose{};

void Complete(bool success = true) {
    Require(pending_token != 0);
    if (success) {
        for (const auto& target : targets)
            reinterpret_cast<Image*>(target.resource)->content = pending_token;
    }
    callback(pending_token, success, callback_data);
    pending_token = 0;
    encoded = false;
}

HRESULT STDMETHODCALLTYPE FeatureSupport(ID3D12Device*, D3D12_FEATURE,
                                         void* data, UINT) {
    static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(data)->MaxSupportedFeatureLevel =
        D3D_FEATURE_LEVEL_11_0;
    return S_OK;
}
XrResult XRAPI_CALL Requirements(XrInstance, XrSystemId,
                               XrGraphicsRequirementsD3D12KHR* requirements) {
    requirements->adapterLuid = {};
    requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}
}

// A COM vtable in its C representation supplies the single device operation
// used by BindAurora. The production backend is compiled normally as C++.
bool aurora_d3d12_get_native_handles(AuroraD3D12NativeHandles* handles) {
    static ID3D12DeviceVtbl vtable{};
    vtable.CheckFeatureSupport = FeatureSupport;
    static ID3D12Device device{&vtable};
    *handles = {&device, &device, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0};
    return true;
}
bool aurora_d3d12_enable_stereo_bridge(AuroraD3D12StereoSubmittedCallback cb, void* data) {
    callback = cb;
    callback_data = data;
    return true;
}
bool aurora_d3d12_set_stereo_targets(uint64_t token, const AuroraD3D12StereoTarget* data,
                                   uint32_t count) {
    Require(pending_token == 0);
    pending_token = token;
    targets.assign(data, data + count);
    return true;
}
bool aurora_d3d12_cancel_stereo_targets(uint64_t token) {
    if (encoded || token != pending_token) return false;
    pending_token = 0;
    return true;
}
bool aurora_d3d12_disable_stereo_bridge() {
    pending_token = 0;
    encoded = false;
    return true; // Simulate a successful queue drain.
}

XrResult XRAPI_CALL xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain* out) {
    *out = reinterpret_cast<XrSwapchain>(new Swapchain);
    ++live_swapchains;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain handle, uint32_t capacity,
                                             uint32_t* count, XrSwapchainImageBaseHeader* images) {
    *count = 1; // Single-image swapchains also require a separate retained pair.
    if (capacity) reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)->texture =
        reinterpret_cast<ID3D12Resource*>(&reinterpret_cast<Swapchain*>(handle)->image);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain handle,
                                          const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    auto& chain = *reinterpret_cast<Swapchain*>(handle);
    Require(!chain.acquired);
    chain.acquired = true;
    chain.waited = false;
    *index = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain handle, const XrSwapchainImageWaitInfo*) {
    auto& chain = *reinterpret_cast<Swapchain*>(handle);
    Require(chain.acquired);
    chain.waited = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain handle,
                                          const XrSwapchainImageReleaseInfo*) {
    auto& chain = *reinterpret_cast<Swapchain*>(handle);
    Require(chain.acquired && chain.waited);
    chain.acquired = false;
    chain.released = true;
    ++releases;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain handle) {
    auto* chain = reinterpret_cast<Swapchain*>(handle);
    Require(!chain->acquired);
    delete chain;
    --live_swapchains;
    return XR_SUCCESS;
}

namespace mkw::vr {
OpenXRRuntime::OpenXRRuntime(OpenXRLogCallback) {
    m_instance = reinterpret_cast<XrInstance>(this);
    m_swapchain_formats = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    for (auto& view : m_view_configuration) {
        view.render_width = 100;
        view.render_height = 80;
    }
}
OpenXRRuntime::~OpenXRRuntime() = default;
bool OpenXRRuntime::GetInstanceProcAddress(const char*, PFN_xrVoidFunction* out) {
    *out = reinterpret_cast<PFN_xrVoidFunction>(Requirements);
    return true;
}
bool OpenXRRuntime::CreateSession(const void*) {
    m_session = reinterpret_cast<XrSession>(this);
    m_session_running = true;
    ++m_session_run_serial;
    return true;
}
void OpenXRRuntime::DestroySession() { m_session_running = false; }
void OpenXRRuntime::ObserveResult(XrResult) noexcept {}
OpenXREventStatus OpenXRRuntime::PollEvents() {
    if (change_space) { ++m_reference_space_change.serial; change_space = false; }
    if (restart_session) { ++m_session_run_serial; restart_session = false; }
    if (stop_session) { m_session_running = false; stop_session = false; }
    return OpenXREventStatus::Continue;
}
OpenXRFrameStatus OpenXRRuntime::WaitFrame(OpenXRFrame& frame) {
    Require(m_frame_phase == FramePhase::Idle);
    frame = {};
    frame.serial = m_next_frame_serial++;
    frame.predicted_display_time = frame.serial * 11'111'111;
    frame.predicted_display_period = 11'111'111;
    frame.should_render = should_render;
    m_active_frame_serial = frame.serial;
    m_frame_phase = FramePhase::Waited;
    return OpenXRFrameStatus::Ready;
}
bool OpenXRRuntime::BeginFrame(const OpenXRFrame& frame) {
    Require(m_frame_phase == FramePhase::Waited && frame.serial == m_active_frame_serial);
    m_frame_phase = FramePhase::Begun;
    return true;
}
bool OpenXRRuntime::LocateViews(OpenXRFrame& frame) {
    frame.views_valid = tracking_valid;
    frame.view_state_flags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    for (auto& view : frame.views) {
        view.pose.orientation.w = 1;
        view.pose.position.x = static_cast<float>(frame.serial);
        view.fov.angleLeft = -0.75f;
    }
    return true;
}
bool OpenXRRuntime::EndFrame(const OpenXRFrame& frame,
                             const XrCompositionLayerBaseHeader* const* layers, uint32_t count) {
    Require(m_frame_phase == FramePhase::Begun && frame.serial == m_active_frame_serial);
    Require(frame.predicted_display_time > display_time);
    display_time = frame.predicted_display_time;
    layer_count = count;
    if (count) {
        Require(frame.should_render && count == 1);
        layer_type = layers[0]->type;
        const auto check_image = [](const XrSwapchainSubImage& subimage) {
            const auto& chain = *reinterpret_cast<Swapchain*>(subimage.swapchain);
            Require(chain.released && !chain.acquired && chain.image.content != 0);
            return chain.image.content;
        };
        if (layer_type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(layers[0]);
            Require(projection.viewCount == 2);
            for (const auto& view : {projection.views[0], projection.views[1]}) {
                displayed_content = check_image(view.subImage);
                // Detect a new image paired with an old pose, or a repeated image
                // falsely labelled with the latest compositor pose.
                Require(view.pose.position.x == static_cast<float>(displayed_content));
                Require(view.fov.angleLeft == -0.75f);
            }
        } else {
            Require(layer_type == XR_TYPE_COMPOSITION_LAYER_QUAD);
            const auto& quad = *reinterpret_cast<const XrCompositionLayerQuad*>(layers[0]);
            displayed_content = check_image(quad.subImage);
            quad_pose = quad.pose;
        }
    }
    m_frame_phase = FramePhase::Idle;
    if (fail_end) {
        fail_end = false;
        return false;
    }
    return true;
}
bool OpenXRRuntime::EndFrameWithoutLayers(const OpenXRFrame& frame) {
    return EndFrame(frame, nullptr, 0);
}
}

int main() {
    OpenXRRuntime runtime;
    OpenXRD3D12Backend backend;
    Require(backend.QueryGraphicsRequirements(runtime) && backend.BindAurora(runtime));
    Require(live_swapchains == 4);
    OpenXRD3D12Presentation presentation;
    OpenXRD3D12Frame frame;
    const auto begin = [&] {
        Require(backend.BeginFrame(presentation, frame) == OpenXRD3D12BeginStatus::Ready);
    };
    const auto finish = [&] {
        Complete();
        Require(backend.WaitForSubmission(frame, 0) == OpenXRD3D12SubmissionStatus::Success);
        Require(backend.FinishFrame(frame, true));
    };
    begin();
    Require(backend.RepeatFrame(frame) && layer_count == 0); // No valid image yet.
    finish();
    const auto first = displayed_content;
    begin();
    encoded = true;
    const auto releases_before_stall = releases;
    for (int i = 0; i < 300; ++i) {
        Require(backend.WaitForSubmission(frame, 0) == OpenXRD3D12SubmissionStatus::Timeout);
        Require(!backend.TryCancelPendingFrame(frame));
        Require(backend.RepeatFrame(frame));
        Require(layer_count == 1 && displayed_content == first);
        Require(releases == releases_before_stall);
    }
    const auto second = frame.xr_frame.serial;
    finish(); // A late completion keeps its original render token and poses.
    Require(displayed_content == second);
    begin();
    Require(backend.TryCancelPendingFrame(frame));
    Require(backend.FinishFrame(frame, false));
    Require(layer_count == 1 && displayed_content == second);

    should_render = false;
    begin();
    Require(!frame.expects_gpu_submission && backend.FinishFrame(frame, false));
    Require(layer_count == 0);
    should_render = true;
    tracking_valid = false;
    begin();
    Require(!frame.expects_gpu_submission && backend.FinishFrame(frame, false));
    Require(layer_count == 1 && displayed_content == second);
    tracking_valid = true;

    presentation.mode = OpenXRD3D12FrameMode::VirtualScreen;
    presentation.quad_anchored = true;
    presentation.quad_pose.position.z = -3;
    begin();
    Require(targets.size() == 1);
    finish();
    const auto menu = displayed_content;
    begin();
    Require(backend.RepeatFrame(frame));
    Require(displayed_content == menu && quad_pose.position.z == -3);
    Require(backend.TryCancelPendingFrame(frame) && backend.FinishFrame(frame, false));

    presentation.mode = OpenXRD3D12FrameMode::ImmersiveProjection;
    begin();
    change_space = true;
    Require(backend.RepeatFrame(frame));
    Require(backend.RepeatFrame(frame) && layer_count == 0);
    finish(); // A render from before the space change must remain invalid.
    Require(layer_count == 0);
    begin();
    finish();
    Require(layer_count == 1);
    restart_session = true;
    runtime.PollEvents();
    begin();
    Require(backend.RepeatFrame(frame) && layer_count == 0);
    finish();
    Require(layer_count == 1);

    begin();
    const auto before_failure = releases;
    Complete(false);
    Require(backend.WaitForSubmission(frame, 0) == OpenXRD3D12SubmissionStatus::Failed);
    Require(!backend.FinishFrame(frame, false));
    Require(releases == before_failure); // Never release possibly in-flight GPU work.
    Require(backend.Shutdown() && live_swapchains == 0);

    // A runtime stop or failed xrEndFrame during a stall must still drain the
    // pending target without trying to end an already consumed frame token.
    for (bool fail_submission : {false, true}) {
        display_time = 0;
        OpenXRRuntime next_runtime;
        OpenXRD3D12Backend next_backend;
        Require(next_backend.QueryGraphicsRequirements(next_runtime));
        Require(next_backend.BindAurora(next_runtime));
        Require(next_backend.BeginFrame(presentation, frame) == OpenXRD3D12BeginStatus::Ready);
        encoded = true;
        stop_session = !fail_submission;
        fail_end = fail_submission;
        Require(!next_backend.RepeatFrame(frame));
        Require(next_backend.Shutdown() && live_swapchains == 0);
    }
    std::cout << "OpenXR retained-frame tests passed\n";
}
