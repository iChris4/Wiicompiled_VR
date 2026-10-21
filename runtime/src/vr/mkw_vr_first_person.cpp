// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/mkw_vr_first_person.h"

#include "aurora/aurora.h"

#include "gx_native_wheel.h"
#include "hle/dvd_vr_asset.h"
#include "hle_stubs.h"
#include "gx_model_visibility.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "vr/body_hand_profiles.h"
#include "vr/body_ik.h"
#include "vr/cockpit_stabilizer.h"
#include "vr/bullet_bill_model.h"
#include "vr/mkw_vr_instrumentation.h"
#include "vr/mkw_vr_policy.h"
#include "vr/mkw_vr_player.h"
#include "vr/native_wheel_mesh.h"
#include "vr/openxr_driving.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern "C" void func_805A6C58(CpuContext* context);
extern "C" void func_8056A470(CpuContext* context);
extern "C" void func_8056A580(CpuContext* context);
// nw4r::g3d::ScnRoot::CalcView, wrapped by the character body
// (character_body_ik.inl), and the skinning pass ScnMdlSimple's posture
// calculation runs (see kScnMdlSkinByteCodeOffset).
extern "C" void func_8006FA50(CpuContext* context);
extern "C" void func_80067F70(CpuContext* context);

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

// Kart::Link::GetKartPosition (0x8059020C) walks proxy -> accessor -> body ->
// physics -> dynamics. The local racer and its accessor are resolved by
// ReadLocalPlayerKart, shared with driver visibility.
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

// ---------------------------------------------------------------------------
// Cockpit seat and steering wheel, ported from heurazy's mario-kart-wii-VR-port
// (GPL-3.0-or-later). Every offset below is PAL RMCP01 and was re-checked
// against the generated translation (leaf getters) and the mkw decompilation.
// ---------------------------------------------------------------------------

// Kart::Link::GetDriverController (0x80590A40) returns accessor+0x14.
constexpr uint32_t kKartAccessorDriverOffset = 0x14u;
// Killer::Activate (0x8059B7B8) sets bit 27 of KartState+0xC through
// accessor+4: the IN_A_BULLET flag, retained throughout the transformation.
constexpr uint32_t kKartAccessorStateOffset = 0x4u;
constexpr uint32_t kKartStateFlagsOffset = 0xCu;
constexpr uint32_t kKartStateInBullet = 1u << 27;
// Kart::Status::UpdateFromInput (0x8059487C) copies the stick of the input
// state driving the kart to KartState+0x88, and Kart::Movement::UpdateTurn
// (0x8057A8B4) turns by its negation: the steering the vehicle turns with,
// -1..1 with positive to the right, whatever controller it came from. The input
// state has already mirrored it in mirror cups (Input::State::
// SetStickXMirrorCheck, 0x8051E960), as the scene the wheel is drawn in is.
constexpr uint32_t kKartStateStickXOffset = 0x88u;
// Kart::Link::GetKiller (0x80591618): accessor+0x60. Killer::__ct
// (0x8059B658) stores the ModelDirector at +0x34 (+0x38 is its shadow).
constexpr uint32_t kKartAccessorKillerOffset = 0x60u;
constexpr uint32_t kKillerModelOffset = 0x34u;
// Kart::Link::GetMovement (0x8059077C) returns accessor+0x28. Movement's
// driving direction at +0x5C excludes damage spin, trick rotation and visual
// pitch/roll; Kart::Movement::SetScale (0x80581720) stores the player's scale
// (lightning, mega mushroom) at +0x164.
constexpr uint32_t kKartAccessorMovementOffset = 0x28u;
constexpr uint32_t kMovementDirOffset = 0x5Cu;
constexpr uint32_t kMovementScaleOffset = 0x164u;
// Kart::Link::GetDamage (0x80590D20) returns accessor+0x2C; the active damage
// type at +0x1C reads all ones while undamaged.
constexpr uint32_t kKartAccessorDamageOffset = 0x2Cu;
constexpr uint32_t kDamageTypeOffset = 0x1Cu;
// Kart::Link::GetKartPosition (0x8059020C): physics+0x4 -> dynamics, whose
// position is at +0x68.
constexpr uint32_t kKartPhysicsDynamicsOffset = 0x4u;
constexpr uint32_t kDynamicsPositionOffset = 0x68u;
// Kart::Link::IsBike (0x80590A6C) reads accessor+0 -> KartSettings -> +0
// (KartSettings::isBike). KartSettings (0x3C bytes in the decompilation) holds
// KartDriverDispParams* at +0x1C, whose first two floats are the driver's seat
// height and depth on this vehicle.
constexpr uint32_t kKartSettingsIsBikeOffset = 0x0u;
constexpr uint32_t kKartSettingsDriverDispParamsOffset = 0x1Cu;
// Kart::Link::GetKartBodyMtx (0x80590278) returns body+0x1C, the animated body.
constexpr uint32_t kKartBodyMtxOffset = 0x1Cu;
// Body::vf_0x58 (0x8056C500) builds the neutral hand grip frames at +0xA8 and
// +0xD8 from KartDriverDispParams+8 (through 0x80592BF8).
constexpr uint32_t kKartBodyLeftGripOffset = 0xA8u;
constexpr uint32_t kKartBodyRightGripOffset = 0xD8u;
// Kart::BodyBike::__ct (0x8056D858) constructs the BikeHandle at body+0x238 and
// writes its vtable 0x808B5314 at +0xC (Quacker inherits the same handle).
// BodyBike::vf_0x60 (0x8056DA0C) transforms the grip frames by the handle's
// matrix at +0x1C, not by the body's.
constexpr uint32_t kBodyBikeHandleOffset = 0x238u;
constexpr uint32_t kBikeHandleVtableOffset = 0xCu;
constexpr uint32_t kBikeHandleVtable = 0x808B5314u;
constexpr uint32_t kBikeHandleMtxOffset = 0x1Cu;
// A vehicle part's ModelDirector is at +0x7C and a driver's at
// DriverController+0x6C (DriverController::LoadModels 0x807C7828 reads it and
// hands over the placement matrix at +0x78); ModelDirector+0xC is the MDL0.
constexpr uint32_t kPartModelOffset = 0x7Cu;
constexpr uint32_t kDriverModelOffset = 0x6Cu;
constexpr uint32_t kDriverPlacementOffset = 0x78u;
constexpr uint32_t kModelDirectorResMdlOffset = 0xCu;
// DriverController::__ct (0x807C7364) stores the bone table at +0x104, which
// DriverController::GetBoneMatId (0x807D976C) walks: 0x60-byte records, the
// bone's name at +0x14 and its nw4r ResNodeData at +0x18. ResNodeData holds
// mtxId at +0x10 and the bind-pose modelMtx at +0x70.
constexpr uint32_t kDriverBonesOffset = 0x104u;
constexpr uint32_t kDriverBoneRecordBytes = 0x60u;
constexpr uint32_t kDriverBoneCount = 36u;
constexpr uint32_t kDriverBoneNameOffset = 0x14u;
constexpr uint32_t kDriverBoneNodeOffset = 0x18u;
constexpr uint32_t kResNodeMtxIdOffset = 0x10u;
constexpr uint32_t kResNodeModelMtxOffset = 0x70u;
// ModelCalcCallback::GetBoneWorldMtx (0x8055FA90): ModelDirector+0x10 ->
// ScnMdlEx, whose first word is the ScnMdl; ScnMdlSimple::GetScnMtxPos
// (0x80071DC0) returns the world matrix array at +0xEC plus mtxId * 48.
constexpr uint32_t kModelDirectorScnMdlExOffset = 0x10u;
constexpr uint32_t kScnMdlWorldMtxArrayOffset = 0xECu;
// nw4r MDL0 ("MDL0", versions 8-11): the vertex position dictionary's offset
// is at +0x18. ResDic: entry count at +4, 16-byte entries from +8 (entry 0 is
// the root) with name and data offsets at +8/+0xC, relative to the dictionary.
// ResVtxPosData: data offset +8, component count +0x14 (1 = XYZ), type +0x18,
// fraction bits +0x1C, stride +0x1D, count +0x1E, bounds min +0x20, max +0x2C.
constexpr uint32_t kMdl0Magic = 0x4D444C30u;

// The character body (character_body_ik.inl), ported from heurazy's
// Wiicompiled_VR-PLUS fork (GPL-3.0-or-later).
//
// nw4r::g3d::ScnMdlSimple::ScnMdlSmpl_CalcPosture (0x80070EC0) skins a model by
// calling 0x80067F70 with the world matrix array (+0xEC), its attribute words
// (+0xF0), the address of a copy of the ResMdl handle (+0xE8) and the skinning
// byte code (+0x10C), skipping the call when the byte code is null.
constexpr uint32_t kScnMdlResMdlOffset = 0xE8u;
constexpr uint32_t kScnMdlWorldMtxAttribOffset = 0xF0u;
constexpr uint32_t kScnMdlSkinByteCodeOffset = 0x10Cu;
// MDL0 section offsets follow the header: revisions 8 and 9 have 11 sections
// (ResMdlInfo at +0x40), revision 11 has 14 and its name at +0x48 (ResMdlInfo
// at +0x4C). Section 1 (+0x14) is the bone dictionary. ResMdlInfo+0x24 is the
// offset of the matrix-to-node table, whose first word counts the world
// matrices. ResNodeData: name offset +0x8, mtxId +0x10 (kResNodeMtxIdOffset),
// parent offset +0x5C, all relative to the node.
constexpr uint32_t kMdl0RevisionOffset = 0x8u;
constexpr uint32_t kMdl0BoneDictionaryOffset = 0x14u;
constexpr uint32_t kMdl0VertexPositionsOffset = 0x18u;
constexpr uint32_t kMdl0Rev11NameOffset = 0x48u;
constexpr uint32_t kMdl0Rev11InfoOffset = 0x4Cu;
constexpr uint32_t kMdl0InfoOffset = 0x40u;
constexpr uint32_t kResMdlInfoMtxTableOffset = 0x24u;
constexpr uint32_t kResNodeNameOffset = 0x8u;
constexpr uint32_t kResNodeParentOffset = 0x5Cu;
constexpr uint32_t kResNodeBytes = 0xD0u;
// heurazy's DriverController reads: a Mii driver keeps its separate face model
// at +0x100, and the u16s at +0xF4 and +0xFA are the animation IDs of the
// driver's two animation layers (body_ik::IsDrivingLegAnimation).
constexpr uint32_t kDriverMiiFaceOffset = 0x100u;
constexpr uint32_t kDriverAnimationOffset = 0xF4u;
constexpr uint32_t kDriverSecondAnimationOffset = 0xFAu;
// Kart::KartMove (mkw decompilation): mDriftState is the s16 at +0xFC. The s16
// at +0x10C is the mini-turbo countdown (Movement::Boost at +0x108 holds it at
// +4, per heurazy).
constexpr uint32_t kMovementDriftStateOffset = 0xFCu;
constexpr uint32_t kMovementMiniTurboOffset = 0x10Cu;
// KartState's flag bitfield (mkw decompilation: RKBitField<160> from +0x4,
// flag n in word n/32, bit n%32, as kKartStateInBullet below). Flag 54 (word
// +0x8, bit 22) is set while a trick animates the kart: heurazy reads it
// through Movement+0x258, the KartJump, whose accessor is the kart's own.
constexpr uint32_t kKartStateTrickFlagsOffset = 0x8u;
constexpr uint32_t kKartStateTrickAnimating = 1u << 22;

// System::RaceManager::spInstance (mkw decompilation symbols: StaticR .bss+0x50,
// next to RaceConfig's at 0x809BD728). RaceManager::stage, the u32 at +0x28:
// 0 is the course preview (vr_controls.h RaceIntroOver).
constexpr uint32_t kRaceManagerInstanceAddress = 0x809BD730u;
constexpr uint32_t kRaceManagerStageOffset = 0x28u;
// Frames the stage may stay unreadable before the intro is let go anyway, so a
// wrong address cannot keep first person away for the whole race.
constexpr uint32_t kRaceStageUnreadableFrames = 300;

// Frames of published wheel copies that no draw took before the separate VR
// wheel stands in; it steps aside again as soon as a draw takes one.
constexpr uint32_t kNativeWheelUnmatchedFrames = 30;
// Fallback switches logged per race.
constexpr uint32_t kNativeWheelSwitchLogs = 8;

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

// `dolly` is GetViewMtx's f1: the game's own call site passes the blend value
// (ReadRaceCameraBlend); zero asks for the camera without that offset.
bool ReadRaceCameraViewMatrix(const CpuContext* context, uint32_t camera_address, Mtx34& out,
                              float dolly) noexcept {
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
    call_context.fpr[1].d = static_cast<double>(dolly);
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
struct KartPoseRead : detail::LocalPlayerKartRead {
    uint32_t body = 0;
    uint32_t physics = 0;
};

KartPoseRead ReadPlayerKartPose(const detail::LocalPlayerKartRead& player, Mtx34& out) noexcept {
    KartPoseRead read{player};
    if (read.failed_step != nullptr) {
        return read;
    }
    if (!ReadGuestPointer(read.accessor + kKartAccessorBodyOffset, read.body)) {
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
    // First person may run at all (not Flat Screen); race_camera says whether
    // the race in progress shows it.
    bool enabled = false;
    // Camera modes (vr_controls.h).
    RaceCameraChoice race_camera{};
    // [vr] first_person: the camera a race starts on once its intro is over.
    bool default_first_person = false;
    // [vr] bullet_bill_camera = "game".
    bool bullet_game_camera = false;
    bool bullet_active = false;
    // Last Update's verdict, to notice first person starting again.
    bool first_person_active = false;
    uint32_t stage_unread_frames = 0;
    // Whether a switch to first person can show anything in this race: one
    // local screen (the race draw observer reports it) and a local racer the
    // seat can be read from, last seen at the intro's end, in first person, or
    // when a switch was asked for.
    uint32_t local_screens = 1;
    bool local_racer_seen = false;
    // Character body (character_body_ik.inl).
    bool body_ik = RuntimeConfigFile::kVrBodyIkDefault;
    bool body_ik_hands_only = RuntimeConfigFile::kVrBodyIkHandsOnlyDefault;
    // Degrees each hand turns about the forearm, left then right.
    std::array<float, 2> body_ik_hand_roll{RuntimeConfigFile::kVrBodyIkLeftHandRollDefault,
                                           RuntimeConfigFile::kVrBodyIkRightHandRollDefault};
    FirstPersonHeadOffsets offsets{};
    float units_per_meter = RuntimeConfigFile::kVrFirstPersonUnitsPerMeterDefault;
    FirstPersonRotation rotation = FirstPersonRotation::YawOnly;

    uint32_t camera_address = 0;
    detail::LocalPlayerKartRead player_kart{};
    HeldItem held_item{};
    uint64_t race_generation = 0;
    // Armed by the draw boundary, consumed by the frame seal.
    bool armed = false;
    uint64_t armed_frame = 0;
    // Which calls latch; its serial keys the character body's pose.
    SeatLatchClock latch_clock{};
    // The scene matrix as it stood before this frame's draws, kept only to
    // report how far it had moved by the time the frame was sealed.
    Mtx34 armed_view = kIdentityMtx34;
    bool armed_view_valid = false;

    FirstPersonAnchor anchor{};
    int hold_frames = 0;
    bool ever_valid_this_race = false;
    bool failure_logged = false;
    uint64_t logged_frame = 0;

    // Cockpit seat.
    FirstPersonSeat seat = FirstPersonSeat::Cockpit;
    float cockpit_units_per_meter = RuntimeConfigFile::kVrCockpitUnitsPerMeterDefault;
    bool steering_wheel = RuntimeConfigFile::kVrSteeringWheelDefault;
    bool native_steering_wheel = RuntimeConfigFile::kVrNativeSteeringWheelDefault;
    // [diagnostics] first_person_logging: the once-a-second camera lines below.
    bool diagnostic_logging = false;
    // Read at the race draw boundary, consumed at the seal.
    struct CockpitLatch {
        bool valid = false;
        uint32_t body = 0;
        // The level seat frame: simulation position and driving direction.
        Mtx34 stable_body = kIdentityMtx34;
        // The frame the seat rides in, and the wheel with it: the level frame
        // for "yaw", otherwise the kart's own orientation around the same seat
        // position, so the wheel always stays where the view puts it.
        Mtx34 seat_body = kIdentityMtx34;
        std::array<float, 3> player_scale{1.0f, 1.0f, 1.0f};
        Mtx34 body_pose = kIdentityMtx34;
        bool grips_valid = false;
        Mtx34 left_grip = kIdentityMtx34;
        Mtx34 right_grip = kIdentityMtx34;
        bool bike = false;
        bool handle_valid = false;
        Mtx34 handle_pose = kIdentityMtx34;
        bool predicted_view_valid = false;
        Mtx34 predicted_view = kIdentityMtx34;
        bool steering_valid = false;
        float steering = 0.0f;
        bool mesh_published = false;
        // The vehicle's own wheel would have been animated but the XR side has
        // not published its first driving snapshot yet: it shows unturned.
        bool waiting_for_driving = false;
        uint32_t hidden_bullet_arrays = 0;
    } cockpit;
    CockpitStabilizer stabilizer{};
    uint64_t stabilized_frame = 0;
    SeatedEyeReference seated_eye{};
    uint32_t seated_driver = 0;
    std::optional<float> cockpit_forward;
    std::optional<float> cockpit_height;
    // Native wheel copies handed to the GX side and not yet dropped.
    bool wheel_arrays_posted = false;
    bool hidden_arrays_posted = false;
    uint32_t native_wheel_body = 0;
    uint32_t native_wheel_unmatched = 0;
    // The VR wheel is standing in: recent copies have not been taken.
    bool native_wheel_fallback = false;
    uint32_t native_wheel_switch_logs = 0;
};

std::mutex g_mutex;
FirstPersonState g_state;
ModelVisibilityState g_visibility;
// Character body (character_body_ik.inl): bit 0/1 the left/right hand is the
// character's own this frame, bit 2 the body is drawn. Read by the pacing
// thread without the lock.
std::atomic<unsigned> g_character_hands{0};
bool CanShowCharacterBodyLocked() noexcept;
void ResetCharacterBodyLocked() noexcept;
void InvalidateCharacterPoseLocked() noexcept;

// Walks to the player's ModelsVisibility, or zero when the race is not up.
uint32_t ResolveModelsVisibility(uint32_t accessor) noexcept {
    uint32_t visibility = 0;
    if (accessor == 0 ||
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
// nothing has drawn yet. The character body is the driver, so it keeps every
// model drawn; the decision has to be made before ScnRoot gathers its draw
// lists, as a driver hidden by then stays hidden for the frame.
void ApplyModelVisibilityLocked() noexcept {
    if (!g_visibility.hide_driver || CanShowCharacterBodyLocked()) {
        RestoreModelVisibilityLocked();
        return;
    }
    const uint32_t visibility = ResolveModelsVisibility(g_state.player_kart.accessor);
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

// ---------------------------------------------------------------------------
// Cockpit seat and native steering wheel (guest thread, under g_mutex).
// ---------------------------------------------------------------------------

std::array<float, 3> ReadPlayerScale(uint32_t accessor) noexcept {
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    uint32_t movement = 0;
    try {
        if (ReadGuestPointer(accessor + kKartAccessorMovementOffset, movement) &&
            Memory::Contains(movement + kMovementScaleOffset, 12)) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                scale[axis] = ValidPlayerScale(Memory::ReadFloat32(movement + kMovementScaleOffset + axis * 4u));
            }
        }
    } catch (const Memory::AccessViolation&) {
        return {1.0f, 1.0f, 1.0f};
    }
    return scale;
}

uint32_t LocalDriver(uint32_t accessor) noexcept {
    uint32_t driver = 0;
    if (!ReadGuestPointer(accessor + kKartAccessorDriverOffset, driver) ||
        !Memory::Contains(driver, kDriverBonesOffset + 4u)) {
        return 0;
    }
    return driver;
}

bool IsLocalBike(uint32_t accessor) noexcept {
    uint32_t settings = 0, is_bike = 0;
    return ReadGuestPointer(accessor, settings) && Memory::TryRead32(settings + kKartSettingsIsBikeOffset, is_bike) &&
           is_bike != 0;
}

std::string ReadGuestName(uint32_t address, uint32_t limit) {
    std::string text;
    for (uint32_t n = 0; n < limit && Memory::Contains(address + n, 1); ++n) {
        const char c = static_cast<char>(Memory::Read8(address + n));
        if (c == '\0') {
            break;
        }
        text += c;
    }
    return text;
}

// The driver model's "_eye" position array bounds, in the face bone's space.
bool ReadEyeBounds(uint32_t driver, detail::Vec3& minimum, detail::Vec3& maximum) {
    uint32_t model = 0, mdl = 0;
    if (!ReadGuestPointer(driver + kDriverModelOffset, model) ||
        !ReadGuestPointer(model + kModelDirectorResMdlOffset, mdl) || !Memory::Contains(mdl, 0x40) ||
        Memory::Read32(mdl) != kMdl0Magic) {
        return false;
    }
    const uint32_t version = Memory::Read32(mdl + 8), offset = Memory::Read32(mdl + 0x18);
    if (version < 8 || version > 11 || offset == 0 || offset > 0x100000) {
        return false;
    }
    const uint32_t dic = mdl + offset;
    if (!Memory::Contains(dic, 8)) {
        return false;
    }
    const uint32_t count = Memory::Read32(dic + 4);
    if (count > 64 || !Memory::Contains(dic, 8 + (count + 1) * 16)) {
        return false;
    }
    for (uint32_t i = 1; i <= count; ++i) {
        const uint32_t entry = dic + 8 + i * 16;
        const uint32_t name_offset = Memory::Read32(entry + 8), data_offset = Memory::Read32(entry + 12);
        if (name_offset > 0x100000 || data_offset > 0x100000) {
            continue;
        }
        if (ReadGuestName(dic + name_offset, 96).find("_eye") == std::string::npos) {
            continue;
        }
        const uint32_t positions = dic + data_offset;
        if (!Memory::Contains(positions, 0x38) || Memory::Read32(positions + 0x14) != 1) {
            continue;
        }
        minimum = {Memory::ReadFloat32(positions + 0x20), Memory::ReadFloat32(positions + 0x24),
                   Memory::ReadFloat32(positions + 0x28)};
        maximum = {Memory::ReadFloat32(positions + 0x2C), Memory::ReadFloat32(positions + 0x30),
                   Memory::ReadFloat32(positions + 0x34)};
        return true;
    }
    return false;
}

// The seated eye in the vehicle's own frame, in its units. Measured once from
// the driver's head bone while the kart drives straight and undamaged, then
// frozen until the driver/race changes; the bind pose serves until the first
// calibration completes. `observe` false only reads: the character body asks
// for the frame's seat before the seal, which samples it.
//
// The head is taken out of the body matrix latched at the race draw, the one
// the bones were posed from. By the seal the game has stepped the kart again
// but not the bones, so the live matrix would put a head measured at speed a
// whole step (60 to 120 units) behind the seat.
bool ReadDriverEye(const KartPoseRead& kart, std::array<float, 3>& eye, bool observe = true) noexcept {
    const auto SampleEye = [observe](const std::array<float, 3>& measured, bool safe) {
        if (observe) {
            g_state.seated_eye.Observe(measured, safe, true);
        }
    };
    const uint32_t driver = LocalDriver(kart.accessor);
    if (g_state.seated_driver != driver) {
        g_state.seated_driver = driver;
        g_state.seated_eye = {};
        g_state.cockpit_forward.reset();
        g_state.cockpit_height.reset();
    }
    if (driver != 0 && g_state.seated_eye.valid) {
        eye = g_state.seated_eye.value;
        return true;
    }
    uint32_t bones = 0;
    Mtx34 placement{};
    if (driver == 0 || !ReadGuestPointer(driver + kDriverBonesOffset, bones) ||
        !Memory::Contains(bones, kDriverBoneCount * kDriverBoneRecordBytes) ||
        !ReadGuestMtx34(driver + kDriverPlacementOffset, placement)) {
        SampleEye({}, false);
        if (driver != 0 && g_state.seated_eye.valid) {
            eye = g_state.seated_eye.value;
            return true;
        }
        return false;
    }
    bool observed = false;
    try {
        for (uint32_t i = 0; i < kDriverBoneCount; ++i) {
            uint32_t name = 0, node = 0;
            const uint32_t record = bones + i * kDriverBoneRecordBytes;
            if (!ReadGuestPointer(record + kDriverBoneNameOffset, name) ||
                !ReadGuestPointer(record + kDriverBoneNodeOffset, node)) {
                continue;
            }
            // PAL DriverMgr's name table (0x808A7288) calls the head bone face_1.
            const std::string text = ReadGuestName(name, 32);
            if (text != "face_1" && text != "head" && text != "head1" && text != "face") {
                continue;
            }
            Mtx34 bind{};
            if (!ReadGuestMtx34(node + kResNodeModelMtxOffset, bind)) {
                continue;
            }
            detail::Vec3 minimum{}, maximum{};
            const bool bounds_found = ReadEyeBounds(driver, minimum, maximum);
            uint32_t model = 0, ex = 0, scn = 0, palette = 0, mtx_id = 0;
            Mtx34 face_world{};
            const Mtx34& body_world = g_state.cockpit.body_pose;
            if (g_state.cockpit.valid && g_state.cockpit.body == kart.body &&
                ReadGuestPointer(driver + kDriverModelOffset, model) &&
                ReadGuestPointer(model + kModelDirectorScnMdlExOffset, ex) && ReadGuestPointer(ex, scn) &&
                ReadGuestPointer(scn + kScnMdlWorldMtxArrayOffset, palette) &&
                Memory::TryRead32(node + kResNodeMtxIdOffset, mtx_id) && mtx_id < 128 &&
                ReadGuestMtx34(palette + mtx_id * 48u, face_world)) {
                uint32_t damage = 0, damage_type = 0;
                const DrivingSnapshot driving = OpenXRReadDriving();
                // Only a neutral pose may define the seat: straight ahead, at
                // normal size and not being knocked about.
                const bool safe = NeutralPlayerScale(ReadPlayerScale(kart.accessor)) &&
                                  ReadGuestPointer(kart.accessor + kKartAccessorDamageOffset, damage) &&
                                  Memory::TryRead32(damage + kDamageTypeOffset, damage_type) &&
                                  damage_type == UINT32_MAX && std::abs(driving.steering_input) < 0.15f;
                std::array<float, 3> measured{};
                const detail::Vec3 local_eye{(minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f,
                                            (minimum.z + maximum.z) * 0.5f};
                const bool measured_eye =
                    bounds_found ? ComputeSeatedEye(face_world, body_world, local_eye, measured)
                                 : ComputeDriverEyeFromHead(face_world, body_world, measured);
                if (measured_eye) {
                    observed = true;
                    const bool calibrating = !g_state.seated_eye.valid;
                    SampleEye(measured, safe);
                    if (calibrating && g_state.seated_eye.valid) {
                        g_state.cockpit_forward.reset();
                        g_state.cockpit_height.reset();
                        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] cockpit: seated eye calibrated from "
                                               << (bounds_found ? "eye geometry" : "animated head")
                                               << " at (" << measured[0] << ", " << measured[1] << ", "
                                               << measured[2] << ") units"
                                               << std::endl;
                    }
                } else {
                    SampleEye({}, false);
                }
            } else {
                SampleEye({}, false);
            }
            if (g_state.seated_eye.valid) {
                eye = g_state.seated_eye.value;
                return true;
            }
            if (bounds_found && ComputeDriverEyeFromBounds(bind, placement, minimum, maximum, eye)) {
                return true;
            }
            // No eye geometry: a point just above and ahead of the head bone.
            const std::array<float, 3> point{bind[3], bind[7] + 8.0f, bind[11] + 8.0f};
            std::array<float, 3> fallback{};
            for (int row = 0; row < 3; ++row) {
                fallback[row] = placement[row * 4 + 3] + placement[row * 4] * point[0] +
                                placement[row * 4 + 1] * point[1] + placement[row * 4 + 2] * point[2];
            }
            if (ValidSeatedEye(fallback)) {
                eye = fallback;
                return true;
            }
        }
    } catch (const Memory::AccessViolation&) {
    }
    if (!observed) {
        SampleEye({}, false);
    }
    if (driver != 0 && g_state.seated_eye.valid) {
        eye = g_state.seated_eye.value;
        return true;
    }
    return false;
}

void DropNativeWheelLocked() noexcept {
    if (g_state.wheel_arrays_posted) {
        GxNativeWheel::PostClear();
        g_state.wheel_arrays_posted = false;
    }
}

// Decodes a vehicle part's MDL0 position arrays, turns the steering wheel disc
// (or, with `whole_part`, re-seats the whole part) on a copy, and hands each
// changed array to Aurora for the draws carrying `model_view`. The guest's own
// vertices are never written.
bool PublishNativeWheelMesh(uint32_t part, const Mtx34& model_view, const Mtx34& left, const Mtx34& right,
                            float angle, bool whole_part, const Mtx34& correction) noexcept {
    uint32_t model = 0, mdl = 0;
    if (!ReadGuestPointer(part + kPartModelOffset, model) ||
        !ReadGuestPointer(model + kModelDirectorResMdlOffset, mdl) || !Memory::Contains(mdl, 0x40)) {
        return false;
    }
    const detail::Vec3 center{(left[3] + right[3]) * 0.5f, (left[7] + right[7]) * 0.5f,
                              (left[11] + right[11]) * 0.5f};
    const float radius = std::abs(left[3] - right[3]) * 0.5f;
    bool published = false;
    try {
        if (Memory::Read32(mdl) != kMdl0Magic) {
            return false;
        }
        const uint32_t version = Memory::Read32(mdl + 8);
        if (version < 8 || version > 11) {
            return false;
        }
        const uint32_t mdl_size = Memory::Read32(mdl + 4);
        const uint8_t* mdl_bytes = mdl_size <= 0x1000000 ? Memory::GetPointer(mdl, mdl_size) : nullptr;
        Mtx34 body_from_vertices = kIdentityMtx34;
        Mtx34 wheel_model_view = model_view;
        if (!whole_part) {
            // Body::mtx is the model placement, while the bone GX draws the
            // body's node 0 through may have its own authored transform (the
            // Baby Booster's rotated root). GX draws its positions through
            // placement * bone, so both wheel selection and Aurora's
            // local-player matrix match must include it. It is found by node
            // id: the Flame Flyer and Cheep Charger list an nw4r_root first.
            if (!ReadNativeWheelNodeMatrix(mdl_bytes, mdl_size, 0, body_from_vertices.data())) return false;
            wheel_model_view = ComposeMtx(model_view, body_from_vertices);
        }
        const uint32_t dic_offset = Memory::Read32(mdl + 0x18);
        if (dic_offset == 0 || dic_offset > 0x100000) {
            return false;
        }
        const uint32_t dic = mdl + dic_offset;
        if (!Memory::Contains(dic, 8)) {
            return false;
        }
        const uint32_t count = Memory::Read32(dic + 4);
        if (count > 16 || !Memory::Contains(dic, 8 + (count + 1) * 16)) {
            return false;
        }
        for (uint32_t entry = 1; entry <= count; ++entry) {
            const uint32_t offset = Memory::Read32(dic + 8 + entry * 16 + 12);
            if (offset > 0x100000) {
                continue;
            }
            const uint32_t header = dic + offset;
            if (!Memory::Contains(header, 0x40) || Memory::Read32(header + 0x14) != 1) {
                continue;
            }
            const uint32_t data_offset = Memory::Read32(header + 8), type = Memory::Read32(header + 0x18);
            const uint32_t stride = Memory::Read8(header + 0x1D), num = Memory::Read16(header + 0x1E);
            const uint32_t component_size = type == 4 ? 4 : (type == 2 || type == 3 ? 2 : 1);
            if (type > 4 || stride < 3 * component_size || num > 4096 || data_offset > 0x100000) {
                continue;
            }
            const uint32_t data = header + data_offset, size = num * stride;
            if (size == 0 || size > 65536 || !Memory::Contains(data, size)) {
                continue;
            }
            const float scale = std::ldexp(1.0f, -int(Memory::Read8(header + 0x1C)));
            std::vector<detail::Vec3> points(num);
            bool valid = true;
            for (uint32_t i = 0; i < num; ++i) {
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    const uint32_t at = data + i * stride + axis * component_size;
                    float value = type == 4   ? Memory::ReadFloat32(at)
                                  : type == 3 ? float(int16_t(Memory::Read16(at))) * scale
                                  : type == 2 ? float(Memory::Read16(at)) * scale
                                  : type == 1 ? float(int8_t(Memory::Read8(at))) * scale
                                              : float(Memory::Read8(at)) * scale;
                    if (!detail::IsFiniteFloat(&value)) {
                        valid = false;
                    }
                    (axis == 0 ? points[i].x : axis == 1 ? points[i].y : points[i].z) = value;
                }
            }
            if (!valid) {
                continue;
            }
            if (whole_part) {
                for (auto& point : points) {
                    point = detail::TransformPoint(correction, point.x, point.y, point.z);
                }
            } else {
                NativeWheelTopology topology(num);
                if (!ReadNativeWheelTopology(mdl_bytes, mdl_size, Memory::Read32(header + 0x10), topology) ||
                    RotateNativeWheelVertices(points, topology, center, radius, angle, &correction,
                                              body_from_vertices) < 8) {
                    continue;
                }
            }
            const uint8_t* source = Memory::GetPointer(data, size);
            if (source == nullptr) {
                continue;
            }
            std::vector<uint8_t> bytes(source, source + size);
            for (uint32_t i = 0; i < num && valid; ++i) {
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    const float value = axis == 0 ? points[i].x : axis == 1 ? points[i].y : points[i].z;
                    uint32_t encoded = 0;
                    if (type == 4) {
                        std::memcpy(&encoded, &value, 4);
                    } else {
                        const float quantized = std::round(value / scale);
                        const float low = type == 3 ? -32768.0f : type == 1 ? -128.0f : 0.0f;
                        const float high = type == 3 ? 32767.0f : type == 2 ? 65535.0f : type == 1 ? 127.0f : 255.0f;
                        if (!(quantized >= low && quantized <= high)) {
                            valid = false;
                            break;
                        }
                        encoded = uint32_t(int32_t(quantized));
                    }
                    for (uint32_t b = 0; b < component_size; ++b) {
                        bytes[i * stride + axis * component_size + b] =
                            uint8_t(encoded >> ((component_size - b - 1) * 8));
                    }
                }
            }
            if (valid && GxNativeWheel::PostVertices(data, bytes.data(), size, wheel_model_view.data())) {
                published = true;
                g_state.wheel_arrays_posted = true;
            }
        }
    } catch (const Memory::AccessViolation&) {
    }
    return published;
}

void DropHiddenModelsLocked() noexcept {
    if (g_state.hidden_arrays_posted) {
        GxModelVisibility::PostClear();
        g_state.hidden_arrays_posted = false;
    }
}

uint32_t PublishBulletBillBodyLocked(uint32_t accessor, const Mtx34& view) noexcept {
    uint32_t state = 0, flags = 0, killer = 0, model = 0, mdl = 0, ex = 0, scn = 0, palette = 0;
    Mtx34 body_world{};
    if (!ReadGuestPointer(accessor + kKartAccessorStateOffset, state) ||
        !Memory::TryRead32(state + kKartStateFlagsOffset, flags) || !(flags & kKartStateInBullet) ||
        !ReadGuestPointer(accessor + kKartAccessorKillerOffset, killer) ||
        !ReadGuestPointer(killer + kKillerModelOffset, model) ||
        !ReadGuestPointer(model + kModelDirectorResMdlOffset, mdl) || !Memory::Contains(mdl, 0x40) ||
        !ReadGuestPointer(model + kModelDirectorScnMdlExOffset, ex) || !ReadGuestPointer(ex, scn) ||
        !ReadGuestPointer(scn + kScnMdlWorldMtxArrayOffset, palette) || !ReadGuestMtx34(palette, body_world)) {
        return 0;
    }
    uint32_t posted = 0;
    try {
        const uint32_t size = Memory::Read32(mdl + 4);
        if (size > 0x1000000) return 0;
        const uint8_t* bytes = Memory::GetPointer(mdl, size);
        const auto arrays = ReadBulletBillBodyArrays(bytes, size);
        const auto model_view = ComposeMtx(view, body_world);
        for (const auto& array : arrays) {
            if (GxModelVisibility::PostHiddenArray(mdl + array.offset, array.size, model_view.data())) {
                ++posted;
                g_state.hidden_arrays_posted = true;
            }
        }
    } catch (const Memory::AccessViolation&) {
    }
    return posted;
}

// At the race draw boundary, before any of the frame's draws: the kart state
// the cockpit seat needs, and the animated copy of the vehicle's own wheel.
void LatchCockpitLocked(uint64_t guest_frame_index) noexcept {
    auto& latch = g_state.cockpit;
    latch = {};
    DropNativeWheelLocked();
    Mtx34 pose{};
    const KartPoseRead kart = ReadPlayerKartPose(g_state.player_kart, pose);
    if (kart.failed_step != nullptr) {
        return;
    }
    Mtx34 simulation = pose;
    uint32_t damage_type = UINT32_MAX;
    try {
        // The simulation's position and driving direction, never the animated
        // vehicle matrix: damage and tricks spin the chassis, not the seat.
        uint32_t dynamics = 0, movement = 0, damage = 0;
        if (ReadGuestPointer(kart.physics + kKartPhysicsDynamicsOffset, dynamics) &&
            Memory::Contains(dynamics + kDynamicsPositionOffset, 12)) {
            for (uint32_t row = 0; row < 3; ++row) {
                simulation[row * 4 + 3] = Memory::ReadFloat32(dynamics + kDynamicsPositionOffset + row * 4u);
            }
        }
        if (ReadGuestPointer(kart.accessor + kKartAccessorMovementOffset, movement) &&
            Memory::Contains(movement + kMovementDirOffset, 12)) {
            const float x = Memory::ReadFloat32(movement + kMovementDirOffset);
            const float z = Memory::ReadFloat32(movement + kMovementDirOffset + 8u);
            if (detail::IsFiniteFloat(&x) && detail::IsFiniteFloat(&z) && x * x + z * z > 0.01f) {
                const float inverse = 1.0f / std::sqrt(x * x + z * z);
                simulation[2] = x * inverse;
                simulation[10] = z * inverse;
            }
        }
        if (ReadGuestPointer(kart.accessor + kKartAccessorDamageOffset, damage)) {
            Memory::TryRead32(damage + kDamageTypeOffset, damage_type);
        }
        uint32_t state = 0;
        if (ReadGuestPointer(kart.accessor + kKartAccessorStateOffset, state) &&
            Memory::Contains(state + kKartStateStickXOffset, 4)) {
            const float steering = Memory::ReadFloat32(state + kKartStateStickXOffset);
            if (detail::IsFiniteFloat(&steering)) {
                latch.steering = std::clamp(steering, -1.0f, 1.0f);
                latch.steering_valid = true;
            }
        }
    } catch (const Memory::AccessViolation&) {
        return;
    }
    if (!detail::IsFiniteMtx34(simulation) || !ReadGuestMtx34(kart.body + kKartBodyMtxOffset, latch.body_pose)) {
        return;
    }
    const float dt = g_state.stabilized_frame != 0 && guest_frame_index > g_state.stabilized_frame
                         ? float(guest_frame_index - g_state.stabilized_frame) / 60.0f
                         : 1.0f / 60.0f;
    g_state.stabilized_frame = guest_frame_index;
    latch.stable_body = g_state.stabilizer.Update(simulation, damage_type != UINT32_MAX, dt);
    latch.seat_body = latch.stable_body;
    if (g_state.rotation != FirstPersonRotation::YawOnly) {
        latch.seat_body = pose;
        latch.seat_body[3] = latch.stable_body[3];
        latch.seat_body[7] = latch.stable_body[7];
        latch.seat_body[11] = latch.stable_body[11];
    }
    latch.player_scale = ReadPlayerScale(kart.accessor);
    latch.body = kart.body;
    latch.grips_valid = ReadGuestMtx34(kart.body + kKartBodyLeftGripOffset, latch.left_grip) &&
                        ReadGuestMtx34(kart.body + kKartBodyRightGripOffset, latch.right_grip);
    latch.bike = IsLocalBike(kart.accessor);
    if (latch.bike) {
        uint32_t vtable = 0;
        latch.handle_valid =
            Memory::TryRead32(kart.body + kBodyBikeHandleOffset + kBikeHandleVtableOffset, vtable) &&
            vtable == kBikeHandleVtable &&
            ReadGuestMtx34(kart.body + kBodyBikeHandleOffset + kBikeHandleMtxOffset, latch.handle_pose);
    }
    // The G3D scene camera is only set once this frame's draws run, so the
    // wheel copy is matched against the race camera's own view, asked for with
    // no dolly offset; LogCockpitLocked reports how far that is from the
    // scene's at the seal.
    latch.predicted_view_valid =
        g_state.camera_address != 0 &&
        ReadRaceCameraViewMatrix(TryGetCpuContext(), g_state.camera_address, latch.predicted_view, 0.0f);
    latch.valid = true;

    if (g_state.anchor.valid && g_state.anchor.cockpit && latch.predicted_view_valid) {
        latch.hidden_bullet_arrays = PublishBulletBillBodyLocked(kart.accessor, latch.predicted_view);
    }

    if (g_state.native_wheel_body != kart.body) {
        g_state.native_wheel_body = kart.body;
        g_state.native_wheel_unmatched = 0;
        g_state.native_wheel_fallback = false;
    }
    if (!latch.grips_valid || !latch.predicted_view_valid || !g_state.steering_wheel ||
        !g_state.native_steering_wheel) {
        return;
    }
    const DrivingSnapshot driving = OpenXRReadDriving();
    if (!driving.cockpit_active) {
        latch.waiting_for_driving = true;
        return;
    }
    if (latch.bike) {
        // BodyBike::vf_0x60 poses the grips by the handle. The whole handle
        // part is re-seated on the level cockpit frame so the bars stay with the
        // player's hands while the bike banks; the game already turns them.
        Mtx34 inverse_body{}, inverse_handle{};
        if (!latch.handle_valid || !InvertMtx(latch.body_pose, inverse_body)) {
            return;
        }
        const auto stable_handle = ScaleModelBasis(
            ComposeMtx(ComposeMtx(latch.seat_body, inverse_body), latch.handle_pose), latch.player_scale);
        const auto rendered_handle = ScaleModelBasis(latch.handle_pose, latch.player_scale);
        if (!InvertMtx(rendered_handle, inverse_handle)) {
            return;
        }
        latch.mesh_published =
            PublishNativeWheelMesh(kart.body + kBodyBikeHandleOffset, ComposeMtx(latch.predicted_view, rendered_handle),
                                   latch.left_grip, latch.right_grip, driving.visual_angle, true,
                                   ComposeMtx(inverse_handle, stable_handle));
    } else {
        // Karts bake the wheel into the body: turn just its disc, on the level
        // cockpit frame so a spinning chassis does not carry it away.
        const auto rendered_body = ScaleModelBasis(latch.body_pose, latch.player_scale);
        Mtx34 inverse_rendered{};
        if (!InvertMtx(rendered_body, inverse_rendered)) {
            return;
        }
        latch.mesh_published = PublishNativeWheelMesh(
            kart.body, ComposeMtx(latch.predicted_view, rendered_body), latch.left_grip, latch.right_grip,
            driving.visual_angle, false,
            ComposeMtx(inverse_rendered, ScaleModelBasis(latch.seat_body, latch.player_scale)));
    }
}

// At the seal, from the scene camera the frame was drawn with.
bool ComputeCockpitAnchorLocked(const Mtx34& view_from_world, const KartPoseRead& kart, const Mtx34& pose,
                                FirstPersonAnchor& out, const char*& failed_step, bool observe_eye = true) noexcept {
    const auto& latch = g_state.cockpit;
    if (!latch.valid || latch.body != kart.body) {
        failed_step = "cockpit seat (the vehicle was not read at the race draw boundary)";
        return false;
    }
    const float base_units = g_state.cockpit_units_per_meter;
    std::array<float, 3> eye{0.0f, 1.1f * base_units, 0.0f};
    if (!ReadDriverEye(kart, eye, observe_eye)) {
        eye = {0.0f, 1.1f * base_units, 0.0f};
        // KartDriverDispParams: the character's seat height and depth here.
        uint32_t settings = 0, seat = 0;
        try {
            if (ReadGuestPointer(kart.accessor, settings) &&
                ReadGuestPointer(settings + kKartSettingsDriverDispParamsOffset, seat) && Memory::Contains(seat, 8)) {
                const float y = Memory::ReadFloat32(seat), z = Memory::ReadFloat32(seat + 4);
                if (detail::IsFiniteFloat(&y) && detail::IsFiniteFloat(&z) && std::abs(y) < 400.0f &&
                    std::abs(z) < 400.0f) {
                    const std::array<float, 3> fallback{0.0f, eye[1] + y, z};
                    if (ValidSeatedEye(fallback)) {
                        eye = fallback;
                    }
                }
            }
        } catch (const Memory::AccessViolation&) {
        }
    }
    float render_units = base_units * CharacterCockpitScale(eye[1]);
    const auto& scale = latch.player_scale;
    // Keep the controls ahead of the seated player even when a long face or a
    // leaned-forward riding animation puts its eye point over them.
    if (latch.grips_valid && !g_state.cockpit_forward) {
        const auto& left = latch.left_grip;
        const auto& right = latch.right_grip;
        detail::Vec3 center{(left[3] + right[3]) * 0.5f, (left[7] + right[7]) * 0.5f, (left[11] + right[11]) * 0.5f};
        bool valid = true;
        if (latch.bike) {
            Mtx34 inverse_body{};
            valid = latch.handle_valid && InvertMtx(latch.body_pose, inverse_body);
            if (valid) {
                auto local_handle = ComposeMtx(inverse_body, latch.handle_pose);
                for (int row = 0; row < 3; ++row) {
                    local_handle[row * 4 + 3] /= scale[row];
                }
                center = detail::TransformPoint(local_handle, center.x, center.y, center.z);
            }
        }
        if (valid && detail::IsFiniteFloat(&center.z)) {
            g_state.cockpit_forward =
                EyeBehindControls(eye[2], center.z, render_units, std::abs(left[3] - right[3]) * 0.5f);
            g_state.cockpit_height = EyeAboveControls(eye[1], center.y, render_units);
        }
    }
    if (g_state.cockpit_forward) {
        eye[2] = *g_state.cockpit_forward;
    }
    if (g_state.cockpit_height) {
        eye[1] = *g_state.cockpit_height;
    }
    for (int axis = 0; axis < 3; ++axis) {
        eye[axis] *= scale[axis];
    }
    render_units *= scale[1];

    // "yaw" takes the kart's own driving direction from the level seat frame,
    // rather than the chase camera's lagging heading; the other modes take the
    // kart's orientation around the same seat (LatchCockpitLocked).
    (void)pose;
    const FirstPersonRotation rotation =
        g_state.rotation == FirstPersonRotation::YawOnly ? FirstPersonRotation::YawPitch : g_state.rotation;
    Mtx34 anchor{};
    if (!ComputeFirstPersonAnchor(view_from_world, latch.seat_body, eye[0], eye[1], eye[2], rotation, anchor)) {
        failed_step = "cockpit anchor math (degenerate camera or kart frame)";
        return false;
    }
    out = {};
    out.anchor_from_scene = anchor;
    out.valid = true;
    out.cockpit = true;
    out.units_per_meter = render_units;
    out.vehicle_identity = kart.body;
    out.bike = latch.bike;
    out.steering_valid = latch.steering_valid;
    out.steering = latch.steering;
    if (latch.grips_valid) {
        const detail::Vec3 left{latch.left_grip[3], latch.left_grip[7], latch.left_grip[11]};
        const detail::Vec3 right{latch.right_grip[3], latch.right_grip[7], latch.right_grip[11]};
        const auto seat_from_world = ComposeMtx(anchor, view_from_world);
        const auto seat_from_body = ComposeMtx(seat_from_world, ScaleModelBasis(latch.seat_body, scale));
        if (!latch.bike) {
            out.native_wheel = ComputeNativeWheelGeometry(seat_from_body, left, right, render_units);
        } else if (latch.handle_valid) {
            Mtx34 inverse_body{};
            if (InvertMtx(latch.body_pose, inverse_body)) {
                const auto stable_handle = ScaleModelBasis(
                    ComposeMtx(ComposeMtx(latch.seat_body, inverse_body), latch.handle_pose), scale);
                out.native_wheel = ComputeNativeHandlebarGeometry(ComposeMtx(seat_from_world, stable_handle),
                                                                  seat_from_body, left, right, render_units);
            }
        }
    }
    // Waiting counts as prepared, so the separate VR wheel does not flash up for
    // the frame or two the XR side takes to engage.
    out.native_mesh_prepared =
        (latch.mesh_published || latch.waiting_for_driving) && !g_state.native_wheel_fallback;
    return true;
}

// After the frame's draws: drop the wheel copies, and let the placeholder VR
// wheel stand in (when enabled) while no draw takes them (the race's opening
// pan, for one). Copies keep being published, so the vehicle's own wheel
// returns as soon as they match.
void FinishNativeWheelFrameLocked() noexcept {
    if (!g_state.wheel_arrays_posted) {
        return;
    }
    DropNativeWheelLocked();
    if (!g_state.cockpit.mesh_published) {
        return;
    }
    const bool log = g_state.native_wheel_switch_logs < kNativeWheelSwitchLogs;
    // Lags a frame or two with the GX thread on; the threshold allows for it.
    if (GxNativeWheel::LastDrawCount() > 0) {
        if (g_state.native_wheel_fallback && log) {
            ++g_state.native_wheel_switch_logs;
            RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] native steering wheel: draws take the animated copy of vehicle 0x"
                                   << std::hex << g_state.cockpit.body << std::dec
                                   << " again; back to the vehicle's own wheel" << std::endl;
        }
        g_state.native_wheel_fallback = false;
        g_state.native_wheel_unmatched = 0;
        return;
    }
    if (!g_state.native_wheel_fallback && ++g_state.native_wheel_unmatched >= kNativeWheelUnmatchedFrames) {
        g_state.native_wheel_fallback = true;
        if (log) {
            ++g_state.native_wheel_switch_logs;
            RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] native steering wheel: no draw took the animated copy of vehicle 0x"
                                   << std::hex << g_state.cockpit.body << std::dec << " in "
                                   << kNativeWheelUnmatchedFrames << " frames; "
                                   << (RuntimeConfigFile::VrPlaceholderSteeringWheel()
                                           ? "drawing the placeholder VR wheel until one does"
                                           : "the vehicle's own is drawn as the game poses it until one does")
                                   << std::endl;
        }
    }
}

void LogCockpitLocked(uint64_t frame, const Mtx34& view_from_world) noexcept {
    if (!g_state.diagnostic_logging) {
        return;
    }
    if (g_state.logged_frame != 0 && frame - g_state.logged_frame < 60) {
        return;
    }
    const auto& anchor = g_state.anchor;
    const auto& wheel = anchor.native_wheel;
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] cockpit: units/m=" << anchor.units_per_meter << ", bike=" << anchor.bike
                           << ", wheel valid=" << wheel.valid << " centre m=(" << wheel.center[0] << ", "
                           << wheel.center[1] << ", " << wheel.center[2] << ") radius=" << wheel.radius
                           << ", native mesh=" << anchor.native_mesh_prepared
                           << ", draws animated=" << GxNativeWheel::LastDrawCount() << std::endl;
    const auto& latch = g_state.cockpit;
    if (latch.hidden_bullet_arrays != 0) {
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] cockpit Bullet Bill: " << latch.hidden_bullet_arrays
                               << " body arrays, " << GxModelVisibility::LastDrawCount()
                               << " draws hidden; arms retained" << std::endl;
    }
    if (latch.predicted_view_valid) {
        float rotation = 0.0f, translation = 0.0f;
        for (int i = 0; i < 12; ++i) {
            const float delta = std::abs(latch.predicted_view[i] - view_from_world[i]);
            (i % 4 == 3 ? translation : rotation) = std::max(i % 4 == 3 ? translation : rotation, delta);
        }
        // The wheel copy only matches draws within 0.002 (rotation) and 0.1
        // (translation) of the predicted view.
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] cockpit: race camera view vs scene view: rotation "
                               << rotation << ", translation " << translation << std::endl;
    }
}

void LogAnchorLocked(uint64_t frame, const Mtx34& anchor, const Mtx34& view_from_world,
                     const KartPoseRead& kart, const Mtx34& kart_from_local) noexcept {
    // One set of lines per second at 60 Hz when [diagnostics] first_person_logging
    // asks for them: enough to confirm the offsets on-device. They are written
    // under the first-person lock, so they stay off otherwise.
    if (!g_state.diagnostic_logging) {
        return;
    }
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
                           << ", local racer=" << kart.player_index
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
    // Flat Screen mode shows the race on the menu screen through the game's own
    // camera, where a hidden driver would only be missing from the kart.
    // Otherwise each race decides, from its intro and [vr] first_person
    // (vr_controls.h RaceCameraChoice).
    MkwVRFirstPersonConfigure(!RuntimeConfigFile::VrFlatScreen(), offsets, units_per_meter, rotation);
    MkwVRPolicySetFirstPersonUnitsPerMeter(units_per_meter);
    const bool cockpit = RuntimeConfigFile::VrFirstPersonSeat() != "custom";
    const float cockpit_units = RuntimeConfigFile::VrCockpitUnitsPerMeter();
    {
        // Same lock the guest thread applies these under.
        std::lock_guard lock(g_mutex);
        g_state.default_first_person = RuntimeConfigFile::VrFirstPerson(false);
        g_state.bullet_game_camera = RuntimeConfigFile::VrBulletBillCamera() == "game";
        g_state.body_ik = RuntimeConfigFile::VrBodyIk();
        g_state.body_ik_hands_only = RuntimeConfigFile::VrBodyIkHandsOnly();
        g_state.body_ik_hand_roll = {RuntimeConfigFile::VrBodyIkHandRoll(0), RuntimeConfigFile::VrBodyIkHandRoll(1)};
        // The next CalcView poses the body under the new settings; the seated
        // skeleton stays latched (re-latching here would take whatever pose
        // the driver happens to be in).
        InvalidateCharacterPoseLocked();
        g_visibility.hide_driver = RuntimeConfigFile::VrFirstPersonHideDriver();
        g_visibility.hidden_model = RuntimeConfigFile::VrFirstPersonHiddenModel();
        const FirstPersonSeat seat = cockpit ? FirstPersonSeat::Cockpit : FirstPersonSeat::Custom;
        if (seat != g_state.seat) {
            // A seat change moves the head: do not hold the other seat's anchor.
            g_state.anchor = {};
            g_state.hold_frames = 0;
        }
        g_state.seat = seat;
        if (g_state.cockpit_units_per_meter != cockpit_units) {
            g_state.cockpit_forward.reset();
            g_state.cockpit_height.reset();
        }
        g_state.cockpit_units_per_meter = cockpit_units;
        g_state.steering_wheel = RuntimeConfigFile::VrSteeringWheel();
        g_state.native_steering_wheel = RuntimeConfigFile::VrNativeSteeringWheel();
        g_state.diagnostic_logging = RuntimeConfigFile::DiagnosticsFirstPersonLogging();
        g_state.native_wheel_fallback = false;
        g_state.native_wheel_unmatched = 0;
    }
    // The cockpit publishes its exact per-frame scale with each anchor; this
    // is the starting point until the first one.
    if (cockpit) {
        MkwVRPolicySetFirstPersonUnitsPerMeter(cockpit_units);
    }
}

void MkwVRFirstPersonReset() noexcept {
    std::lock_guard lock(g_mutex);
    RestoreModelVisibilityLocked();
    // Race entry and exit: the next race starts from its intro again. The exit
    // observer also fires early in a race the game then goes on drawing (run
    // logs: "exited RaceScene" before "race intro over"), so the policy is told
    // the intro is pending on both.
    g_state.race_camera.Reset();
    MkwVRPolicySetRaceIntroPending(true);
    g_state.bullet_active = false;
    g_state.first_person_active = false;
    g_state.stage_unread_frames = 0;
    g_state.local_racer_seen = false;
    ResetCharacterBodyLocked();
    g_visibility.logged = false;
    g_visibility.logged_models = false;
    g_visibility.logged_range = false;
    g_state.armed = false;
    g_state.armed_view_valid = false;
    g_state.camera_address = 0;
    g_state.player_kart = {};
    g_state.held_item = {};
    ++g_state.race_generation;
    g_state.held_item.race_generation = g_state.race_generation;
    g_state.anchor = {};
    g_state.hold_frames = 0;
    g_state.ever_valid_this_race = false;
    g_state.failure_logged = false;
    g_state.logged_frame = 0;
    DropNativeWheelLocked();
    g_state.cockpit = {};
    DropHiddenModelsLocked();
    g_state.stabilizer = {};
    g_state.stabilized_frame = 0;
    g_state.seated_eye = {};
    g_state.seated_driver = 0;
    g_state.cockpit_forward.reset();
    g_state.cockpit_height.reset();
    g_state.native_wheel_body = 0;
    g_state.native_wheel_unmatched = 0;
    g_state.native_wheel_fallback = false;
    g_state.native_wheel_switch_logs = 0;
}

bool MkwVRReadSceneView(Mtx34& view) noexcept {
    return ReadSceneViewMatrix(view);
}

void MkwVRFirstPersonUpdate(uint64_t guest_frame_index, uint32_t race_camera_address,
                            bool draw_observer) noexcept {
    std::lock_guard lock(g_mutex);
    // The character body's ScnRoot::CalcView wrapper latches the frame before
    // the race draw observer gets here; the observer's call for that draw must
    // not run the comfort filters again or drop the wheel copies, hidden arrays
    // and item already prepared for it. A frame the game catches up on calls
    // CalcView twice, and the seat is latched again after the step its draws
    // show: keeping the first, the seat sat a whole kart step (up to a metre in
    // the cockpit) away from the drawn kart, which VR frame interpolation then
    // blended into a jump.
    if (!g_state.latch_clock.Observe(draw_observer, g_state.armed,
                                     g_state.camera_address == race_camera_address)) {
        return;
    }
    g_state.held_item = {};
    g_state.held_item.race_generation = g_state.race_generation;
    DropHiddenModelsLocked();
    g_state.camera_address = race_camera_address;

    // The race's intro shows the game's own camera; the configured default
    // takes over once RaceManager leaves the course preview.
    if (g_state.race_camera.IntroPending()) {
        uint32_t manager = 0, stage = 0;
        bool stage_valid = ReadGuestPointer(kRaceManagerInstanceAddress, manager) &&
                           Memory::TryRead32(manager + kRaceManagerStageOffset, stage);
        if (!stage_valid && ++g_state.stage_unread_frames >= kRaceStageUnreadableFrames) {
            RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] race stage unreadable at 0x" << std::hex
                                   << kRaceManagerInstanceAddress << "+0x" << kRaceManagerStageOffset << std::dec
                                   << " for " << kRaceStageUnreadableFrames
                                   << " frames; ending the race intro without it" << std::endl;
            stage_valid = true;
            stage = 1;
        }
        if (g_state.race_camera.Observe(stage_valid, stage, g_state.default_first_person)) {
            // [vr] flat_race_intro turns the race immersive here, with the
            // camera. The policy's lock is a leaf, so taking it under ours is safe.
            MkwVRPolicySetRaceIntroPending(false);
            g_state.local_racer_seen = detail::ReadLocalPlayerKart<Memory>().failed_step == nullptr;
            RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] race intro over (stage " << stage << "): camera = "
                                   << (g_state.race_camera.Selected() == CameraMode::FirstPerson ? "first person"
                                                                                                 : "game")
                                   << (g_state.local_screens == 1 && g_state.local_racer_seen
                                           ? ""
                                           : " (first person cannot show: no single local racer)")
                                   << std::endl;
        }
    } else {
        g_state.race_camera.Observe(false, 0, g_state.default_first_person);
    }
    // The player and the Bullet Bill state are only needed for first person.
    const bool wants_first_person =
        g_state.enabled && g_state.race_camera.Effective(false) == CameraMode::FirstPerson;
    detail::LocalPlayerKartRead player{};
    player.failed_step = "first person not selected";
    bool bullet = false;
    if (wants_first_person) {
        player = detail::ReadLocalPlayerKart<Memory>();
        bullet = detail::LocalPlayerInBullet<Memory>(player);
    }
    if (wants_first_person && bullet != g_state.bullet_active && g_state.bullet_game_camera) {
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] Bullet Bill: "
                               << (bullet ? "game camera until it ends" : "back to first person") << std::endl;
    }
    g_state.bullet_active = bullet;
    if (!wants_first_person || g_state.race_camera.Effective(g_state.bullet_game_camera && bullet) !=
                                   CameraMode::FirstPerson) {
        g_state.first_person_active = false;
        g_character_hands.store(0, std::memory_order_release);
        g_state.anchor = {};
        g_state.hold_frames = 0;
        g_state.armed = false;
        g_state.cockpit = {};
        DropNativeWheelLocked();
        RestoreModelVisibilityLocked();
        return;
    }
    if (!g_state.first_person_active) {
        // First person starting (the intro over, a click, a Bullet Bill gone):
        // the comfort filter starts afresh instead of from where it stopped.
        g_state.first_person_active = true;
        g_state.stabilizer = {};
        g_state.stabilized_frame = 0;
    }
    g_state.local_racer_seen = player.failed_step == nullptr;
    if (player.failed_step != nullptr || player.accessor != g_state.player_kart.accessor) {
        // Do not carry a held anchor or hidden models across an ownership
        // change, including entering spectator mode or an online roster reset.
        ResetCharacterBodyLocked();
        RestoreModelVisibilityLocked();
        g_state.anchor = {};
        g_state.hold_frames = 0;
    }
    g_state.player_kart = player;
    if (player.failed_step == nullptr && g_state.seat == FirstPersonSeat::Cockpit) {
        static bool item_archive_attempted = false;
        if (!item_archive_attempted && RuntimeConfigFile::VrCockpitItemHand() != "off") {
            item_archive_attempted = true;
            const auto archive = DVDReadVrAsset("/Race/Common.szs");
            if (!archive.empty()) aurora_set_cockpit_item_archive(archive.data(), static_cast<uint32_t>(archive.size()));
        }
        g_state.held_item = detail::ReadHeldItem<Memory>(player.player_index, g_state.race_generation);
    }
    g_state.armed = true;
    g_state.armed_frame = guest_frame_index;
    g_state.armed_view_valid = ReadSceneViewMatrix(g_state.armed_view);
    if (g_state.seat == FirstPersonSeat::Cockpit) {
        LatchCockpitLocked(guest_frame_index);
    } else {
        g_state.cockpit = {};
        DropNativeWheelLocked();
    }
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
    // After this frame's draws, whatever the anchor makes of it.
    struct FinishWheel {
        ~FinishWheel() {
            FinishNativeWheelFrameLocked();
            DropHiddenModelsLocked();
        }
    } finish_wheel;
    if (!g_state.armed) {
        return;
    }
    g_state.armed = false;
    const uint64_t guest_frame_index = g_state.armed_frame;

    Mtx34 view_from_world{};
    Mtx34 kart_from_local{};
    Mtx34 anchor{};
    FirstPersonAnchor cockpit_anchor{};
    KartPoseRead kart{};
    const char* failed_step = nullptr;
    // The scene's own matrix first: it is what the recorded draws carry. The
    // RaceCamera getter stays as a fallback, but it describes a different
    // camera, so an anchor built from it cannot reach the chase view.
    if (!ReadSceneViewMatrix(view_from_world) &&
        !(g_state.camera_address != 0 &&
          ReadRaceCameraViewMatrix(TryGetCpuContext(), g_state.camera_address, view_from_world,
                                   ReadRaceCameraBlend()))) {
        failed_step = "scene view matrix";
    } else if (kart = ReadPlayerKartPose(g_state.player_kart, kart_from_local);
               kart.failed_step != nullptr) {
        failed_step = kart.failed_step;
    } else if (g_state.seat == FirstPersonSeat::Cockpit) {
        ComputeCockpitAnchorLocked(view_from_world, kart, kart_from_local, cockpit_anchor, failed_step);
    } else if (!ComputeFirstPersonAnchor(view_from_world, kart_from_local,
                                         g_state.offsets.right * g_state.units_per_meter,
                                         g_state.offsets.up * g_state.units_per_meter,
                                         g_state.offsets.forward * g_state.units_per_meter,
                                         g_state.rotation, anchor)) {
        failed_step = "anchor math (degenerate camera or kart frame)";
    }

    if (failed_step == nullptr) {
        if (g_state.seat == FirstPersonSeat::Cockpit) {
            g_state.anchor = cockpit_anchor;
            g_state.anchor.guest_frame_index = guest_frame_index;
            anchor = cockpit_anchor.anchor_from_scene;
            LogCockpitLocked(guest_frame_index, view_from_world);
        } else {
            g_state.anchor = {anchor, true, guest_frame_index};
        }
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
            << g_state.camera_address << ", race data=0x" << kart.race_data
            << ", local racer=" << std::dec << kart.player_index << std::hex
            << ", manager=0x" << kart.manager << ", players=0x"
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

HeldItem MkwVRFirstPersonGetHeldItem() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state.held_item;
}

CameraMode MkwVRGetCameraMode() noexcept {
    std::lock_guard lock(g_mutex);
    if (!g_state.enabled || !g_state.race_camera.InRace()) {
        return CameraMode::Game;
    }
    return g_state.race_camera.Effective(g_state.bullet_game_camera && g_state.bullet_active);
}

CameraMode MkwVRSelectedCameraMode() noexcept {
    std::lock_guard lock(g_mutex);
    // Through the intro, the camera the race will start on.
    if (g_state.race_camera.IntroPending()) {
        return g_state.default_first_person ? CameraMode::FirstPerson : CameraMode::Game;
    }
    return g_state.race_camera.Selected();
}

bool MkwVRSetCameraMode(CameraMode mode) noexcept {
    std::lock_guard lock(g_mutex);
    if (mode == CameraMode::FirstPerson && g_state.race_camera.InRace() && !g_state.race_camera.IntroPending()) {
        // Asked for on the game thread: look again rather than trust a reading
        // from the intro's end.
        g_state.local_racer_seen = detail::ReadLocalPlayerKart<Memory>().failed_step == nullptr;
        if (g_state.local_screens != 1 || !g_state.local_racer_seen) {
            return false;
        }
    }
    return g_state.race_camera.Select(mode);
}

bool MkwVRCameraSwitchAvailable() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state.enabled && g_state.race_camera.InRace() && !g_state.race_camera.IntroPending() &&
           g_state.local_screens == 1 && g_state.local_racer_seen;
}

void MkwVRFirstPersonObserveRaceScreens(uint32_t local_screens) noexcept {
    std::lock_guard lock(g_mutex);
    g_state.local_screens = local_screens;
}

bool MkwVRInRace() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state.race_camera.InRace();
}

bool MkwVRRaceIntroActive() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state.race_camera.InRace() && g_state.race_camera.IntroPending();
}

// The character body: CharacterBodyCalcView and the rig it poses.
#include "character_body_ik.inl"
REGISTER_NATIVE_FUNCTION_AS(0x8006FA50, CharacterBodyCalcView, "nw4r::g3d::ScnRoot::CalcView character body IK");

} // namespace mkw::vr
