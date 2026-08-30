// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/mkw_vr_policy.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace mkw::vr {
namespace {

constexpr MkwVRPolicyConfig kDefaultConfig{};

struct PolicyState {
    MkwVRPolicyConfig config = kDefaultConfig;
    MkwVRSceneObservation scene{};
    MkwVRCameraObservation camera{};
    uint32_t available_bindings = MkwVRBindingNone;
    bool session_active = false;
};

std::mutex g_policy_mutex;
PolicyState g_policy;

bool IsFinitePositive(float value) noexcept {
    return std::isfinite(value) && value > 0.0f;
}

MkwVRPolicyConfig SanitizeConfig(const MkwVRPolicyConfig& config) noexcept {
    MkwVRPolicyConfig sanitized = config;
    if (!IsFinitePositive(sanitized.world_units_per_meter)) {
        sanitized.world_units_per_meter = kDefaultConfig.world_units_per_meter;
    }
    if (!IsFinitePositive(sanitized.hud_distance_meters)) {
        sanitized.hud_distance_meters = kDefaultConfig.hud_distance_meters;
    }
    if (!IsFinitePositive(sanitized.hud_scale)) {
        sanitized.hud_scale = kDefaultConfig.hud_scale;
    }
    return sanitized;
}

bool IsFiniteCamera(const MkwVRCameraObservation& camera) noexcept {
    return camera.valid &&
           std::all_of(camera.view_from_world.begin(), camera.view_from_world.end(),
                       [](float value) { return std::isfinite(value); });
}

VRPresentationMode SelectPresentation(const PolicyState& state) noexcept {
    if (!state.config.enabled || !state.session_active) {
        return VRPresentationMode::Desktop;
    }

    // A virtual screen is the fail-safe for menus, replays, split-screen, and
    // any incomplete instrumentation. It preserves the unmodified render path.
    if ((state.available_bindings & kMkwVRRequiredImmersiveBindings) !=
            kMkwVRRequiredImmersiveBindings ||
        !state.config.immersive_races || state.scene.mode != VRSceneMode::Race ||
        state.scene.local_player_count != 1 || !IsFiniteCamera(state.camera)) {
        return VRPresentationMode::VirtualScreen;
    }

    return VRPresentationMode::ImmersiveRace;
}

constexpr MkwVRHookPoint kHookPoints[] = {
    {0x80562B34u, "ScnMgr::UpdateCameras", MkwVRHookCapability::SceneState,
     "Observe the active scene camera update boundary."},
    {0x80562BF0u, "GameCamera::GetViewMatrix", MkwVRHookCapability::RaceCamera,
     "Observe the common camera view matrix consumed by ScnMgr."},
    {0x805B2110u, "ScnMgrRace::UpdateCameras", MkwVRHookCapability::SceneState,
     "Identify a race camera update without relying on a scene object layout."},
    {0x805B1CD8u, "ScnMgrRace::Draw", MkwVRHookCapability::DrawClassification,
     "Bracket race-scene drawing for perspective-world classification."},
    {0x805A21D0u, "RaceCamera::Update", MkwVRHookCapability::RaceCamera,
     "Observe the stable post-update race camera for the current guest frame."},
    {0x805A6C58u, "RaceCamera::GetViewMtx", MkwVRHookCapability::RaceCamera,
     "Copy the returned 3x4 view matrix through a future translated observer."},
    {0x805A906Cu, "RaceCameraMgr::ApplyShaking", MkwVRHookCapability::RaceCamera,
     "Separate game camera shake from headset motion when a comfort policy is added."},
    {0x80565DA0u, "GameScreen::SetAndLoadOrthoProj",
     MkwVRHookCapability::DrawClassification,
     "Mark orthographic GameScreen work as HUD or flat-screen content."},
    {0x80566020u, "GameScreen::SetAndLoadProjection",
     MkwVRHookCapability::DrawClassification,
     "Observe GameScreen projection changes used by race and menu UI."},
    {0x805661E8u, "GameScreen::SetProjection", MkwVRHookCapability::DrawClassification,
     "Observe projection setup without assuming GameScreen member offsets."},
    {0x800640D0u, "nw4r::g3d::G3DState::SetCameraProjMtx",
     MkwVRHookCapability::DrawClassification,
     "Provide a renderer-level perspective/orthographic projection boundary."},
    {0x8006AA80u, "nw4r::g3d::Camera::GXSetProjection",
     MkwVRHookCapability::DrawClassification,
     "Observe the final NW4R camera projection submitted to GX."},
    {0x8054F41Cu, "GameScreenEffectsMgr::Draw", MkwVRHookCapability::PostProcess,
     "Bracket screen effects that must be evaluated per eye or composed flat."},
    {0x8054F7A4u, "GameScreenEffectsMgr::DrawCourseFilterEffects",
     MkwVRHookCapability::PostProcess,
     "Classify full-screen course filters as post-processing."},
    {0x8054F8E0u, "GameScreenEffectsMgr::CopyEFBToLensFlareTextures",
     MkwVRHookCapability::PostProcess,
     "Track EFB-dependent lens-flare capture separately from world geometry."},
    {0x802278D0u, "EGG::Frustum::CalcMtxPerspective", MkwVRHookCapability::Culling,
     "Future culling-frustum expansion point for head movement beyond the base camera."},
    {0x80228180u, "EGG::Frustum::CopyToG3D", MkwVRHookCapability::Culling,
     "Observe the frustum handed to NW4R without guessing EGG::Frustum fields."},
};

} // namespace

void MkwVRPolicyReset() noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    g_policy = PolicyState{};
}

void MkwVRPolicyConfigure(const MkwVRPolicyConfig& config) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    g_policy.config = SanitizeConfig(config);
}

void MkwVRPolicySetSessionActive(bool active) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    g_policy.session_active = active;
}

void MkwVRPolicySetAvailableBindings(uint32_t bindings) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    g_policy.available_bindings = bindings;
}

void MkwVRPolicyPublishScene(const MkwVRSceneObservation& scene) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    if (scene.mode != VRSceneMode::Race || g_policy.scene.mode != VRSceneMode::Race) {
        // Never carry a camera sample across a menu/replay-to-race transition.
        // A fresh RaceCamera observation must arrive before immersive mode can
        // become active again.
        g_policy.camera.valid = false;
    }
    g_policy.scene = scene;
}

void MkwVRPolicyPublishRaceCamera(const MkwVRCameraObservation& camera) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    g_policy.camera = camera;
    g_policy.camera.valid = IsFiniteCamera(camera);
}

void MkwVRPolicyInvalidateRaceCamera() noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    g_policy.camera.valid = false;
}

MkwVRPolicySnapshot MkwVRPolicyGetSnapshot() noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    MkwVRPolicySnapshot snapshot;
    snapshot.presentation = SelectPresentation(g_policy);
    snapshot.config = g_policy.config;
    snapshot.scene = g_policy.scene;
    snapshot.camera = g_policy.camera;
    snapshot.available_bindings = g_policy.available_bindings;
    snapshot.session_active = g_policy.session_active;
    return snapshot;
}

VRDrawClass MkwVRPolicyClassifyDraw(const MkwVRDrawObservation& draw) noexcept {
    if (draw.pass == VRDrawPass::PostProcess) {
        return VRDrawClass::PostProcess;
    }
    switch (draw.projection) {
    case VRProjectionKind::Perspective:
        return VRDrawClass::PerspectiveWorld;
    case VRProjectionKind::Orthographic:
        return VRDrawClass::OrthographicHud;
    case VRProjectionKind::Unknown:
        return VRDrawClass::Unknown;
    }
    return VRDrawClass::Unknown;
}

VRDrawRoute MkwVRPolicyRouteDraw(VRDrawClass draw_class,
                                 const MkwVRPolicySnapshot& snapshot) noexcept {
    switch (snapshot.presentation) {
    case VRPresentationMode::Desktop:
        return VRDrawRoute::DesktopPassthrough;
    case VRPresentationMode::VirtualScreen:
        return VRDrawRoute::VirtualScreen;
    case VRPresentationMode::ImmersiveRace:
        break;
    }

    switch (draw_class) {
    case VRDrawClass::PerspectiveWorld:
        return VRDrawRoute::StereoWorld;
    case VRDrawClass::OrthographicHud:
        return VRDrawRoute::HeadLockedHud;
    case VRDrawClass::PostProcess:
        return VRDrawRoute::PerEyePostProcess;
    case VRDrawClass::Unknown:
        return VRDrawRoute::Unclassified;
    }
    return VRDrawRoute::Unclassified;
}

const MkwVRHookPoint* MkwVRPolicyHookPoints(std::size_t* count) noexcept {
    if (count != nullptr) {
        *count = sizeof(kHookPoints) / sizeof(kHookPoints[0]);
    }
    return kHookPoints;
}

} // namespace mkw::vr
