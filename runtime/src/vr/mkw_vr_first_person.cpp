// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/mkw_vr_first_person.h"

#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "vr/mkw_vr_policy.h"

#include <mutex>

extern "C" void func_805A6C58(CpuContext* context);

namespace mkw::vr {
namespace {

// ---------------------------------------------------------------------------
// PAL RMCP01 object layout.
//
// Derived from the shipped StaticR.rel and cross-checked against the mkw
// decompilation. Each constant names the accessor that proves it, so a future
// region or a mod that moves these can be re-derived the same way. Keep in
// sync with projects/mkwii/MAP.txt and the generated translations.
// ---------------------------------------------------------------------------

// RaceCamera::GetViewMtx (0x805A6C58) writes the authoritative view matrix to
// its r4 output buffer. The adjacent RaceCamera fields are state vectors, not
// a view matrix, so call the game's getter instead of guessing an object offset.
constexpr uint32_t kRaceCameraScratchBytes = 0x300u;

// Kart::Manager's instance pointer. Its CreateInstance (0x8058FAA8) resolves
// the slot as 0x809C0000 + 6392 in the generated translation. Read directly
// rather than observed from Kart::Manager::Update's r3, so enabling the camera
// needs no change to the translated output: an entry observer only exists in a
// build whose translation was regenerated for it, and its absence is silent.
// This mirrors how the race scene's instance slot is reached in
// mkw_vr_instrumentation.cpp.
constexpr uint32_t kKartManagerInstanceAddress = 0x809C18F8u;
// Kart::Manager::GetKartPlayer (0x80590100): `lwz r3,0x20(r3)` then indexes.
constexpr uint32_t kKartManagerPlayersOffset = 0x20u;
// Kart::Link::GetKartPosition (0x8059020C) walks proxy -> accessor -> body ->
// physics -> dynamics; the first three links are shared by every kart accessor.
constexpr uint32_t kKartProxyAccessorOffset = 0x00u;
constexpr uint32_t kKartAccessorBodyOffset = 0x08u;
constexpr uint32_t kKartBodyPhysicsOffset = 0x90u;
// KartPhysics::pose (Kart::Link::GetMtx 0x80590264). This is the physics-driven
// pose, deliberately not the visual one: an animated frame would bob the
// camera. Kart::Link::GetKartBodyMtx (0x80590278) returns KartBody+0x1C, the
// visual pose, and is the alternative to try if the seat ever looks detached.
constexpr uint32_t kKartPhysicsPoseOffset = 0x9Cu;

// Offline Mario Kart Wii puts the local racer first, and immersive
// presentation already requires exactly one on-screen player.
constexpr uint32_t kLocalPlayerIndex = 0;

// Frames the last good anchor survives a failed read before the camera returns
// to the game's own. Rides out a transient null during a respawn or transition
// without letting a genuinely broken anchor persist.
constexpr int kHoldFrames = 10;

// ---------------------------------------------------------------------------
// Guest reads. Everything is bounds-checked and exception-guarded so a pointer
// caught mid-teardown can only cost this frame's anchor.
// ---------------------------------------------------------------------------

bool ReadGuestPointer(uint32_t address, uint32_t& out) noexcept {
    return Memory::TryRead32(address, out) && out != 0;
}

constexpr uint32_t kMtx34Bytes = 12u * sizeof(float);

bool ReadGuestMtx34(uint32_t address, Mtx34& out) noexcept {
    if (address == 0 || !Memory::Contains(address, kMtx34Bytes)) {
        return false;
    }
    try {
        for (uint32_t i = 0; i < out.size(); ++i) {
            out[i] = Memory::ReadFloat32(address + i * static_cast<uint32_t>(sizeof(float)));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }
    return detail::IsFiniteMtx34(out);
}

bool ReadRaceCameraViewMatrix(const CpuContext* context, uint32_t camera_address,
                              Mtx34& out) noexcept {
    if (context == nullptr || camera_address == 0 ||
        context->gpr[1] < kRaceCameraScratchBytes) {
        return false;
    }

    CpuContext call_context = *context;
    const uint32_t scratch = context->gpr[1] - kRaceCameraScratchBytes;
    call_context.gpr[3] = camera_address;
    call_context.gpr[4] = scratch;
    call_context.gpr[5] = scratch + 48u;
    try {
        CpuContextScope scope(&call_context);
        func_805A6C58(&call_context);
        return ReadGuestMtx34(scratch, out);
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

// The pointer walk, kept inspectable: on failure `failed_step` names the link
// that broke and the resolved pointers before it are still filled in. One log
// line then says exactly which offset needs revisiting.
struct KartPoseRead {
    const char* failed_step = nullptr;
    uint32_t manager = 0;
    uint32_t players = 0;
    uint32_t proxy = 0;
    uint32_t accessor = 0;
    uint32_t body = 0;
    uint32_t physics = 0;
};

KartPoseRead ReadPlayerKartPose(Mtx34& out) noexcept {
    KartPoseRead read{};
    if (!ReadGuestPointer(kKartManagerInstanceAddress, read.manager)) {
        read.failed_step = "Kart::Manager instance";
    } else if (!ReadGuestPointer(read.manager + kKartManagerPlayersOffset, read.players)) {
        read.failed_step = "Kart::Manager players array";
    } else if (!ReadGuestPointer(read.players + kLocalPlayerIndex * 4u, read.proxy)) {
        read.failed_step = "player kart object";
    } else if (!ReadGuestPointer(read.proxy + kKartProxyAccessorOffset, read.accessor)) {
        read.failed_step = "kart accessor";
    } else if (!ReadGuestPointer(read.accessor + kKartAccessorBodyOffset, read.body)) {
        read.failed_step = "kart body";
    } else if (!ReadGuestPointer(read.body + kKartBodyPhysicsOffset, read.physics)) {
        read.failed_step = "kart physics";
    } else if (!ReadGuestMtx34(read.physics + kKartPhysicsPoseOffset, out)) {
        read.failed_step = "kart pose matrix";
    }
    return read;
}

// ---------------------------------------------------------------------------

struct FirstPersonState {
    bool enabled = false;
    FirstPersonHeadOffsets offsets{};
    float units_per_meter = 10.0f;

    uint32_t camera_address = 0;

    FirstPersonAnchor anchor{};
    int hold_frames = 0;
    bool ever_valid_this_race = false;
    bool failure_logged = false;
    uint64_t logged_frame = 0;
};

std::mutex g_mutex;
FirstPersonState g_state;

void LogAnchorLocked(uint64_t frame, const Mtx34& anchor, const Mtx34& view_from_world,
                     const KartPoseRead& kart, const Mtx34& kart_from_local) noexcept {
    // One line per second at 60 Hz: enough to confirm the offsets on-device
    // without drowning the log during a race.
    if (g_state.logged_frame != 0 && frame - g_state.logged_frame < 60) {
        return;
    }
    g_state.logged_frame = frame;
    // The anchor's translation is -R*a, so negating it gives the head's offset
    // from the recorded camera measured in the levelled camera's own axes.
    // While driving it should stay roughly constant: a little to the side, a
    // little below the chase camera, and well in front of it.
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person anchor: frame=" << frame << ", camera=0x"
                           << std::hex << g_state.camera_address << std::dec
                           << ", head from camera (right, up, forward)=(" << -anchor[3] << ", "
                           << -anchor[7] << ", " << anchor[11] << ") units" << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person view: rows=(" << view_from_world[0] << ", "
                           << view_from_world[1] << ", " << view_from_world[2] << "; "
                           << view_from_world[4] << ", " << view_from_world[5] << ", "
                           << view_from_world[6] << "; " << view_from_world[8] << ", "
                           << view_from_world[9] << ", " << view_from_world[10]
                           << "), translation=(" << view_from_world[3] << ", "
                           << view_from_world[7] << ", " << view_from_world[11] << ")"
                           << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person pose: physics=0x" << std::hex << kart.physics
                           << ", pose=0x" << (kart.physics + kKartPhysicsPoseOffset) << std::dec
                           << ", rows=(" << kart_from_local[0] << ", " << kart_from_local[1]
                           << ", " << kart_from_local[2] << "; " << kart_from_local[4] << ", "
                           << kart_from_local[5] << ", " << kart_from_local[6] << "; "
                           << kart_from_local[8] << ", " << kart_from_local[9] << ", "
                           << kart_from_local[10] << "), translation=(" << kart_from_local[3]
                           << ", " << kart_from_local[7] << ", " << kart_from_local[11] << ")"
                           << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person pose bits: translation=(0x"
                           << std::hex << std::bit_cast<uint32_t>(kart_from_local[3]) << ", 0x"
                           << std::bit_cast<uint32_t>(kart_from_local[7]) << ", 0x"
                           << std::bit_cast<uint32_t>(kart_from_local[11]) << ")" << std::dec
                           << std::endl;
}

} // namespace

void MkwVRFirstPersonConfigure(bool enabled, const FirstPersonHeadOffsets& offsets,
                               float units_per_meter) noexcept {
    std::lock_guard lock(g_mutex);
    g_state.enabled = enabled;
    g_state.offsets = offsets;
    if (detail::IsFiniteFloat(&units_per_meter) && units_per_meter > 0.0f) {
        g_state.units_per_meter = units_per_meter;
    }
    if (!enabled) {
        g_state.anchor = {};
        g_state.hold_frames = 0;
    }
}

void MkwVRFirstPersonApplyConfiguredSettings() noexcept {
    const float units_per_meter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter(10.0f);
    const FirstPersonHeadOffsets offsets{
        RuntimeConfigFile::VrFirstPersonHeadRightMeters(0.0f),
        RuntimeConfigFile::VrFirstPersonHeadUpMeters(1.0f),
        RuntimeConfigFile::VrFirstPersonHeadForwardMeters(0.0f),
    };
    MkwVRFirstPersonConfigure(RuntimeConfigFile::VrFirstPerson(false), offsets, units_per_meter);
    MkwVRPolicySetFirstPersonUnitsPerMeter(units_per_meter);
}

void MkwVRFirstPersonReset() noexcept {
    std::lock_guard lock(g_mutex);
    g_state.camera_address = 0;
    g_state.anchor = {};
    g_state.hold_frames = 0;
    g_state.ever_valid_this_race = false;
    g_state.failure_logged = false;
    g_state.logged_frame = 0;
}

void MkwVRFirstPersonUpdate(uint64_t guest_frame_index, uint32_t race_camera_address) noexcept {
    std::lock_guard lock(g_mutex);
    if (!g_state.enabled) {
        g_state.anchor = {};
        g_state.hold_frames = 0;
        return;
    }
    g_state.camera_address = race_camera_address;

    Mtx34 view_from_world{};
    Mtx34 kart_from_local{};
    Mtx34 anchor{};
    KartPoseRead kart{};
    const char* failed_step = nullptr;
    if (race_camera_address == 0) {
        failed_step = "race camera (none updated this frame)";
    } else if (!ReadRaceCameraViewMatrix(TryGetCpuContext(), race_camera_address,
                                         view_from_world)) {
        failed_step = "race camera view matrix";
    } else if (kart = ReadPlayerKartPose(kart_from_local); kart.failed_step != nullptr) {
        failed_step = kart.failed_step;
    } else if (!ComputeFirstPersonAnchor(view_from_world, kart_from_local,
                                         g_state.offsets.right * g_state.units_per_meter,
                                         g_state.offsets.up * g_state.units_per_meter,
                                         g_state.offsets.forward * g_state.units_per_meter,
                                         /*level_horizon=*/true, anchor)) {
        failed_step = "anchor math (degenerate camera or kart frame)";
    }

    if (failed_step == nullptr) {
        g_state.anchor = {anchor, true, guest_frame_index};
        g_state.hold_frames = kHoldFrames;
        g_state.ever_valid_this_race = true;
        LogAnchorLocked(guest_frame_index, anchor, view_from_world, kart, kart_from_local);
        return;
    }

    if (g_state.hold_frames > 0) {
        --g_state.hold_frames;
        g_state.anchor.guest_frame_index = guest_frame_index;
        return;
    }
    if (!g_state.ever_valid_this_race && !g_state.failure_logged) {
        // Once per race, naming the exact link that broke: every address below
        // is a PAL RMCP01 constant, so this is what says which one to revisit.
        g_state.failure_logged = true;
        RT_LOG(RT_TAG_RUNTIME)
            << "[mkw-vr] first-person camera is enabled but could not resolve the "
            << failed_step << "; staying on the game's own camera (camera=0x" << std::hex
            << race_camera_address << ", manager=0x" << kart.manager << ", players=0x"
            << kart.players << ", kart=0x" << kart.proxy << ", accessor=0x" << kart.accessor
            << ", body=0x" << kart.body << ", physics=0x" << kart.physics << std::dec << ")"
            << std::endl;
    }
    g_state.anchor = {};
}

FirstPersonAnchor MkwVRFirstPersonGetAnchor() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state.anchor;
}

} // namespace mkw::vr
