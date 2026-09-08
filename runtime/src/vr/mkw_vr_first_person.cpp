// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/mkw_vr_first_person.h"

#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "vr/mkw_vr_policy.h"

#include <mutex>
#include <string>

extern "C" void func_805A6C58(CpuContext* context);
extern "C" void func_8056A470(CpuContext* context);
extern "C" void func_8056A580(CpuContext* context);

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
// GetViewMtx also takes a float argument in f1. It scales the positional offset
// the function folds into the camera it builds, so an inherited garbage value
// puts the view somewhere unrelated to the kart while still looking finite.
// The game's own call site (0x80711198) sources it from *(*(0x809C2898)+0x8BC);
// reproduce that exactly, and fall back to zero, which means "no offset".
constexpr uint32_t kRaceCameraBlendOwnerAddress = 0x809C2898u;
constexpr uint32_t kRaceCameraBlendOffset = 0x8BCu;

// nw4r::g3d::G3DState::GetCameraMtxPtr (0x80064180) resolves the matrix the
// scene is actually rendered with, from a static CameraMtxState: a u16 at +2
// selects the live bank and the 3x4 view matrix sits at +52 within it.
//
// This is the matrix the recorded GX draws carry. RaceCamera::GetViewMtx is
// not: measured on device, the camera it returns sits ~155 units directly
// above the kart, with under 5 units of horizontal separation, so a head
// position derived from it has no chase-camera offset in it at all.
constexpr uint32_t kG3DCameraMtxStateAddress = 0x802BBAB4u;
constexpr uint32_t kG3DCameraMtxBankOffset = 0x2u;
constexpr uint32_t kG3DCameraMtxOffset = 52u;

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

// Kart::Link::GetModelsVisibility (0x8059108C) is proxy -> accessor -> +0x58.
// Kart::ModelsVisibility::SetInvisible (0x8056A2F0) is nothing but two stores,
// a u16 at +0x10 and a u8 at +0x12.
//
// Writing them is not enough on its own. UpdateModelsVisibility (0x8056A470)
// is what carries the byte at +0x12 out to the models, looping over them and
// calling a virtual through the secondary vtable at +0x0C, and the game runs it
// during the kart update -- before the draw boundary where this can write. So
// the write has to be followed by running that function again, or nothing ever
// reads it. Measured on device, the mask at +0x10 is already 0 in normal play,
// so it is not the field that decides what draws.
constexpr uint32_t kKartAccessorModelsVisibilityOffset = 0x58u;
constexpr uint32_t kModelsVisibilityMaskOffset = 0x10u;
constexpr uint32_t kModelsVisibilityDrawOffset = 0x12u;

// That one byte reaches every model the loop visits, which is why clearing it
// takes the kart along with the driver. The loop walks an array of models: from
// the holder at *(visibility[0] + 0x14), entries start at +0xD8 with a stride
// of 4 and the count sits at +0xF0. SetModelDraw (0x8056A580) applies the byte
// to a single one of them, so naming an index hides exactly that model.
constexpr uint32_t kModelsVisibilityHolderOffset = 0x14u;
constexpr uint32_t kModelHolderArrayOffset = 0xD8u;
constexpr uint32_t kModelHolderCountOffset = 0xF0u;
constexpr uint32_t kMaxPlayerModels = 32;

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

float ReadRaceCameraBlend() noexcept {
    uint32_t owner = 0;
    if (!ReadGuestPointer(kRaceCameraBlendOwnerAddress, owner) ||
        !Memory::Contains(owner + kRaceCameraBlendOffset, sizeof(float))) {
        return 0.0f;
    }
    try {
        const float value = Memory::ReadFloat32(owner + kRaceCameraBlendOffset);
        return detail::IsFiniteFloat(&value) ? value : 0.0f;
    } catch (const Memory::AccessViolation&) {
        return 0.0f;
    }
}

bool ReadSceneViewMatrix(Mtx34& out) noexcept {
    const uint32_t bank_address = kG3DCameraMtxStateAddress + kG3DCameraMtxBankOffset;
    if (!Memory::Contains(bank_address, sizeof(uint16_t))) {
        return false;
    }
    try {
        const uint32_t bank = Memory::Read16(bank_address);
        return ReadGuestMtx34(kG3DCameraMtxStateAddress + bank + kG3DCameraMtxOffset, out);
    } catch (const Memory::AccessViolation&) {
        return false;
    }
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
    // Every argument register has to be set deliberately: the rest of this
    // context belongs to the observed function, not to the one being called.
    call_context.fpr[1].d = static_cast<double>(ReadRaceCameraBlend());
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

// Hiding the player's own models. Like the anchor this only reads the game to
// decide what to write, but unlike the anchor it does modify it, so it owns the
// values it displaced and puts them back when it stops.
struct ModelVisibilityState {
    bool hide_driver = false;
    // -1 hides every model of the player's kart, the vehicle included; a valid
    // index hides only that model. Index 0 is the driver on PAL RMCP01.
    int hidden_model = 0;
    bool saved = false;
    uint32_t saved_object = 0;
    uint16_t original_mask = 0;
    uint8_t original_draw = 0;
    bool logged = false;
    bool logged_models = false;
    bool logged_range = false;
};

struct FirstPersonState {
    bool enabled = false;
    FirstPersonHeadOffsets offsets{};
    float units_per_meter = RuntimeConfigFile::kVrFirstPersonUnitsPerMeterDefault;
    FirstPersonRotation rotation = FirstPersonRotation::YawOnly;

    uint32_t camera_address = 0;
    // Armed by the draw boundary, consumed by the frame seal.
    bool armed = false;
    uint64_t armed_frame = 0;
    // The scene matrix as it stood before this frame's draws, kept only to
    // report how far it had moved by the time the frame was sealed.
    Mtx34 armed_view = kIdentityMtx34;
    bool armed_view_valid = false;

    FirstPersonAnchor anchor{};
    int hold_frames = 0;
    bool ever_valid_this_race = false;
    bool failure_logged = false;
    uint64_t logged_frame = 0;
};

std::mutex g_mutex;
FirstPersonState g_state;
ModelVisibilityState g_visibility;

// Walks to the player's ModelsVisibility, or zero when the race is not up.
uint32_t ResolveModelsVisibility() noexcept {
    uint32_t manager = 0;
    uint32_t players = 0;
    uint32_t proxy = 0;
    uint32_t accessor = 0;
    uint32_t visibility = 0;
    if (!ReadGuestPointer(kKartManagerInstanceAddress, manager) ||
        !ReadGuestPointer(manager + kKartManagerPlayersOffset, players) ||
        !ReadGuestPointer(players + kLocalPlayerIndex * 4u, proxy) ||
        !ReadGuestPointer(proxy + kKartProxyAccessorOffset, accessor) ||
        !ReadGuestPointer(accessor + kKartAccessorModelsVisibilityOffset, visibility)) {
        return 0;
    }
    return visibility;
}

// Carries the visibility fields out to the models, the way the kart update
// does. Without this the fields are just bytes nothing has read.
void ApplyModelsVisibilityToModels(uint32_t visibility) noexcept {
    const CpuContext* context = TryGetCpuContext();
    if (context == nullptr || visibility == 0) {
        return;
    }
    CpuContext call_context = *context;
    call_context.gpr[3] = visibility;
    try {
        CpuContextScope scope(&call_context);
        func_8056A470(&call_context);
    } catch (const Memory::AccessViolation&) {
    }
}

// Applies the draw byte to one model only. Bounded and pointer-checked because
// this ends in a virtual call on a guest object.
bool ApplyModelDraw(uint32_t visibility, uint32_t holder, uint32_t index) noexcept {
    uint32_t model = 0;
    if (!ReadGuestPointer(holder + kModelHolderArrayOffset + index * 4u, model)) {
        return false;
    }
    const CpuContext* context = TryGetCpuContext();
    if (context == nullptr || !Memory::Contains(model, 4)) {
        return false;
    }
    CpuContext call_context = *context;
    call_context.gpr[3] = visibility;
    call_context.gpr[4] = model;
    try {
        CpuContextScope scope(&call_context);
        func_8056A580(&call_context);
    } catch (const Memory::AccessViolation&) {
        return false;
    }
    return true;
}

// The model array the visibility loop walks, or zero when it cannot be reached.
uint32_t ResolveModelHolder(uint32_t visibility, uint32_t& count) noexcept {
    uint32_t holder = 0;
    uint32_t owner = 0;
    count = 0;
    if (!ReadGuestPointer(visibility, owner) ||
        !ReadGuestPointer(owner + kModelsVisibilityHolderOffset, holder) ||
        !Memory::Contains(holder + kModelHolderCountOffset, 4)) {
        return 0;
    }
    try {
        count = Memory::Read32(holder + kModelHolderCountOffset);
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
    if (count == 0 || count > kMaxPlayerModels) {
        count = 0;
        return 0;
    }
    return holder;
}

void RestoreModelVisibilityLocked() noexcept {
    if (!g_visibility.saved) {
        return;
    }
    if (Memory::Contains(g_visibility.saved_object + kModelsVisibilityDrawOffset, 1)) {
        try {
            Memory::Write16(g_visibility.saved_object + kModelsVisibilityMaskOffset,
                            g_visibility.original_mask);
            Memory::Write8(g_visibility.saved_object + kModelsVisibilityDrawOffset,
                           g_visibility.original_draw);
            ApplyModelsVisibilityToModels(g_visibility.saved_object);
        } catch (const Memory::AccessViolation&) {
        }
    }
    g_visibility.saved = false;
    g_visibility.saved_object = 0;
}

// Applied at the draw boundary: the kart update has set these for the frame and
// nothing has drawn yet.
void ApplyModelVisibilityLocked() noexcept {
    if (!g_visibility.hide_driver) {
        RestoreModelVisibilityLocked();
        return;
    }
    const uint32_t visibility = ResolveModelsVisibility();
    if (visibility == 0 ||
        !Memory::Contains(visibility + kModelsVisibilityDrawOffset, 1)) {
        return;
    }
    try {
        if (!g_visibility.saved || g_visibility.saved_object != visibility) {
            RestoreModelVisibilityLocked();
            g_visibility.original_mask =
                Memory::Read16(visibility + kModelsVisibilityMaskOffset);
            g_visibility.original_draw =
                Memory::Read8(visibility + kModelsVisibilityDrawOffset);
            g_visibility.saved_object = visibility;
            g_visibility.saved = true;
            if (!g_visibility.logged) {
                g_visibility.logged = true;
                // The values the game normally holds. If clearing the byte on
                // its own does not remove the driver, these say which bits of
                // the mask are worth trying instead.
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] model visibility: object=0x" << std::hex << visibility
                    << ", mask=0x" << g_visibility.original_mask << ", driver=0x"
                    << static_cast<uint32_t>(g_visibility.original_draw) << std::dec
                    << std::endl;
            }
        }
        uint32_t count = 0;
        const uint32_t holder = ResolveModelHolder(visibility, count);
        if (!g_visibility.logged_models && holder != 0) {
            g_visibility.logged_models = true;
            RT_LOG(RT_TAG_RUNTIME)
                << "[mkw-vr] model visibility: " << count
                << " models; set first_person_hidden_model to one of 0.." << (count - 1)
                << " to hide a single one, or -1 for all of them" << std::endl;
        }
        const bool index_in_range =
            g_visibility.hidden_model >= 0 && holder != 0 &&
            static_cast<uint32_t>(g_visibility.hidden_model) < count;
        if (g_visibility.hidden_model >= 0 && !index_in_range) {
            // Naming a model the kart does not have should leave it alone, not
            // silently fall through to hiding all of them.
            if (!g_visibility.logged_range) {
                g_visibility.logged_range = true;
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] model visibility: model " << g_visibility.hidden_model
                    << " is out of range for this kart's " << count
                    << "; nothing hidden" << std::endl;
            }
        } else if (index_in_range) {
            // Show everything, then take back the one model that is named.
            Memory::Write8(visibility + kModelsVisibilityDrawOffset,
                           g_visibility.original_draw);
            ApplyModelsVisibilityToModels(visibility);
            Memory::Write8(visibility + kModelsVisibilityDrawOffset, 0);
            ApplyModelDraw(visibility, holder,
                           static_cast<uint32_t>(g_visibility.hidden_model));
            Memory::Write8(visibility + kModelsVisibilityDrawOffset,
                           g_visibility.original_draw);
        } else {
            Memory::Write8(visibility + kModelsVisibilityDrawOffset, 0);
            ApplyModelsVisibilityToModels(visibility);
        }
    } catch (const Memory::AccessViolation&) {
    }
}

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
    // Both candidate cameras measured against the kart, so one run says which
    // matrix actually describes the view the frame was rendered from. A real
    // chase camera sits a few hundred units behind and above the kart; a value
    // near zero horizontally means the matrix is kart-centred and unusable.
    const auto eye_report = [&](const char* label, const Mtx34& v) {
        const float cam[3] = {
            -(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]),
            -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
            -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]),
        };
        const float dx = kart_from_local[3] - cam[0];
        const float dy = kart_from_local[7] - cam[1];
        const float dz = kart_from_local[11] - cam[2];
        RT_LOG(RT_TAG_RUNTIME)
            << "[mkw-vr] first-person eye [" << label << "]: camera=(" << cam[0] << ", " << cam[1]
            << ", " << cam[2] << "), kart-camera=(" << dx << ", " << dy << ", " << dz
            << "), horizontal=" << std::sqrt(dx * dx + dz * dz) << std::endl;
    };
    eye_report("scene", view_from_world);
    // The same matrix as it stood before this frame's draws. The gap between
    // the two is the error the old draw-boundary timing was introducing, and
    // it grows with how fast the chase camera is moving.
    if (g_state.armed_view_valid) {
        eye_report("scene at draw entry", g_state.armed_view);
    }
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person pose bits: translation=(0x"
                           << std::hex << std::bit_cast<uint32_t>(kart_from_local[3]) << ", 0x"
                           << std::bit_cast<uint32_t>(kart_from_local[7]) << ", 0x"
                           << std::bit_cast<uint32_t>(kart_from_local[11]) << ")" << std::dec
                           << std::endl;
}

} // namespace

void MkwVRFirstPersonConfigure(bool enabled, const FirstPersonHeadOffsets& offsets,
                               float units_per_meter, FirstPersonRotation rotation) noexcept {
    std::lock_guard lock(g_mutex);
    g_state.enabled = enabled;
    g_state.offsets = offsets;
    g_state.rotation = rotation;
    if (detail::IsFiniteFloat(&units_per_meter) && units_per_meter > 0.0f) {
        g_state.units_per_meter = units_per_meter;
    }
    if (!enabled) {
        g_state.anchor = {};
        g_state.hold_frames = 0;
    }
}

void MkwVRFirstPersonApplyConfiguredSettings() noexcept {
    const float units_per_meter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter();
    const FirstPersonHeadOffsets offsets{
        RuntimeConfigFile::VrFirstPersonHeadRightMeters(),
        RuntimeConfigFile::VrFirstPersonHeadUpMeters(),
        RuntimeConfigFile::VrFirstPersonHeadForwardMeters(),
    };
    const std::string mode = RuntimeConfigFile::VrFirstPersonRotation();
    const FirstPersonRotation rotation = mode == "full" ? FirstPersonRotation::Full
                                         : mode == "yaw_pitch" ? FirstPersonRotation::YawPitch
                                                               : FirstPersonRotation::YawOnly;
    MkwVRFirstPersonConfigure(RuntimeConfigFile::VrFirstPerson(false), offsets, units_per_meter,
                              rotation);
    MkwVRPolicySetFirstPersonUnitsPerMeter(units_per_meter);
    {
        // Same lock the guest thread applies these under.
        std::lock_guard lock(g_mutex);
        g_visibility.hide_driver = RuntimeConfigFile::VrFirstPersonHideDriver();
        g_visibility.hidden_model = RuntimeConfigFile::VrFirstPersonHiddenModel();
    }
}

void MkwVRFirstPersonReset() noexcept {
    std::lock_guard lock(g_mutex);
    RestoreModelVisibilityLocked();
    g_visibility.logged = false;
    g_visibility.logged_models = false;
    g_visibility.logged_range = false;
    g_state.armed = false;
    g_state.armed_view_valid = false;
    g_state.camera_address = 0;
    g_state.anchor = {};
    g_state.hold_frames = 0;
    g_state.ever_valid_this_race = false;
    g_state.failure_logged = false;
    g_state.logged_frame = 0;
}

void MkwVRFirstPersonUpdate(uint64_t guest_frame_index, uint32_t race_camera_address) noexcept {
    std::lock_guard lock(g_mutex);
    g_state.camera_address = race_camera_address;
    if (!g_state.enabled) {
        g_state.anchor = {};
        g_state.hold_frames = 0;
        g_state.armed = false;
        RestoreModelVisibilityLocked();
        return;
    }
    g_state.armed = true;
    g_state.armed_frame = guest_frame_index;
    g_state.armed_view_valid = ReadSceneViewMatrix(g_state.armed_view);
    // Uses last frame's verdict, since this frame's anchor is not computed
    // until the seal. One frame of lag on hiding a model is not visible, and
    // it keeps the player's kart drawn whenever the anchor is not engaged.
    if (g_state.anchor.valid) {
        ApplyModelVisibilityLocked();
    } else {
        RestoreModelVisibilityLocked();
    }
}

void MkwVRFirstPersonCommit() noexcept {
    std::lock_guard lock(g_mutex);
    if (!g_state.armed) {
        return;
    }
    g_state.armed = false;
    const uint64_t guest_frame_index = g_state.armed_frame;

    Mtx34 view_from_world{};
    Mtx34 kart_from_local{};
    Mtx34 anchor{};
    KartPoseRead kart{};
    const char* failed_step = nullptr;
    // The scene's own matrix first: it is what the recorded draws carry. The
    // RaceCamera getter stays as a fallback, but it describes a different
    // camera, so an anchor built from it cannot reach the chase view.
    if (!ReadSceneViewMatrix(view_from_world) &&
        !(g_state.camera_address != 0 &&
          ReadRaceCameraViewMatrix(TryGetCpuContext(), g_state.camera_address, view_from_world))) {
        failed_step = "scene view matrix";
    } else if (kart = ReadPlayerKartPose(kart_from_local); kart.failed_step != nullptr) {
        failed_step = kart.failed_step;
    } else if (!ComputeFirstPersonAnchor(view_from_world, kart_from_local,
                                         g_state.offsets.right * g_state.units_per_meter,
                                         g_state.offsets.up * g_state.units_per_meter,
                                         g_state.offsets.forward * g_state.units_per_meter,
                                         g_state.rotation, anchor)) {
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
            << g_state.camera_address << ", manager=0x" << kart.manager << ", players=0x"
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
