// SPDX-License-Identifier: GPL-3.0-or-later
//
// The character body: the player's own character drawn in the first-person
// cockpit, its arms reaching for the tracked controllers. Ported from heurazy's
// Wiicompiled_VR-PLUS fork (GPL-3.0-or-later); the math is vr/body_ik.h.
//
// Included inside mkw::vr by mkw_vr_first_person.cpp, after its guest layout
// helpers, which this shares (g_mutex, g_state, the guest reads and offsets).
//
// One native wrapper of nw4r::g3d::ScnRoot::CalcView (0x8006FA50) poses the
// local driver's world matrix palette just before the game turns world matrices
// into view matrices: a seated torso latched once per driver and model in
// kart-local space, the native leg articulation only while it is a driving pose,
// two-bone IK arms, a collapsed head. It re-runs the model's skinning, lets
// CalcView run, and puts the original palette and attribute words back, so only
// this frame's view matrices ever see the pose: physics, the other racers and
// online data never do.

namespace {

struct CharacterRig {
    uint32_t driver = 0, model = 0;
    // World matrices in the palette (the matrix-to-node table's count).
    uint32_t palette_count = 0;
    // Bone index (bone dictionary order) -> world matrix index.
    std::vector<uint32_t> matrices;
    body_ik::RigRegions regions;
    // heurazy's palm profile for this character, or none (body_hand_profiles.h).
    const body_ik::HandProfile* profile = nullptr;
    std::string profile_key;
    // Wrist rotation relative to the controller, taken on the first tracked
    // frame of each hand (without a profile).
    std::array<Mtx34, 2> wrist_offset{};
    std::array<bool, 2> calibrated{};
    // The seated skeleton in kart-local space, by world matrix index.
    std::vector<Mtx34> seated_pose;
    std::array<float, 3> seated_scale{1.0f, 1.0f, 1.0f};
    body_ik::LegAnimationGate leg_animation;
    // This guest frame's pose, for a second CalcView in the same frame.
    std::vector<Mtx34> posed;
    Mtx34 posed_hide = kIdentityMtx34;
    unsigned posed_hands = 0;
    uint64_t posed_frame = 0;
    bool posed_valid = false;
};

CharacterRig g_character_rig;
// The driver and model last found unusable (a Mii, an unknown skeleton), so a
// frame does not walk their bone dictionary again.
uint32_t g_rejected_driver = 0, g_rejected_model = 0;
std::atomic<unsigned> g_body_logged_state{UINT32_MAX};

void ResetCharacterBodyLocked() noexcept {
    g_character_rig = {};
    g_rejected_driver = 0;
    g_rejected_model = 0;
    g_body_logged_state.store(UINT32_MAX, std::memory_order_relaxed);
    g_character_hands.store(0, std::memory_order_release);
}

// Settings changed: pose again on the next CalcView, keeping the rig and its
// seated skeleton.
void InvalidateCharacterPoseLocked() noexcept {
    g_character_rig.posed_valid = false;
}

// The palette as the game left it, envelope matrices included (the skinning
// pass overwrites those), and the same for any part collapsed with the head.
// Restored after CalcView whatever happens inside it.
struct CharacterPalette {
    uint32_t address = 0, attributes = 0;
    std::vector<Mtx34> original;
    std::vector<uint32_t> original_attributes;
    std::vector<std::unique_ptr<CharacterPalette>> attachments;

    void Restore() noexcept {
        for (auto& attachment : attachments) {
            attachment->Restore();
        }
        try {
            for (size_t i = 0; i < original.size(); ++i) {
                for (unsigned k = 0; k < 12; ++k) {
                    Memory::WriteFloat32(address + static_cast<uint32_t>(i) * kMtx34Bytes + k * 4u, original[i][k]);
                }
                Memory::Write32(attributes + static_cast<uint32_t>(i) * 4u, original_attributes[i]);
            }
        } catch (const Memory::AccessViolation&) {
        }
        original.clear();
    }
    ~CharacterPalette() { Restore(); }
};

// Complete or nothing: a palette half read must never be "restored".
bool SavePalette(CharacterPalette& saved, uint32_t palette, uint32_t attributes, uint32_t count) {
    std::vector<Mtx34> original(count);
    std::vector<uint32_t> original_attributes(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!ReadGuestMtx34(palette + i * kMtx34Bytes, original[i]) ||
            !Memory::TryRead32(attributes + i * 4u, original_attributes[i])) {
            return false;
        }
    }
    saved.address = palette;
    saved.attributes = attributes;
    saved.original = std::move(original);
    saved.original_attributes = std::move(original_attributes);
    return true;
}

// ScnMdlSimple's skinning pass, as ScnMdlSmpl_CalcPosture runs it, over the
// palette just written.
void RunSkinning(const CpuContext* context, uint32_t scn, uint32_t palette, uint32_t attributes) {
    uint32_t byte_code = 0;
    if (context == nullptr || !ReadGuestPointer(scn + kScnMdlSkinByteCodeOffset, byte_code)) {
        return;
    }
    CpuContext call_context = *context;
    call_context.gpr[3] = palette;
    call_context.gpr[4] = attributes;
    // The ResMdl handle is passed by address.
    call_context.gpr[5] = scn + kScnMdlResMdlOffset;
    call_context.gpr[6] = byte_code;
    CpuContextScope scope(&call_context);
    func_80067F70(&call_context);
}

// World matrices in a model's palette, or zero.
uint32_t PaletteCount(uint32_t mdl) {
    if (!Memory::Contains(mdl, 0x90) || Memory::Read32(mdl) != kMdl0Magic) {
        return 0;
    }
    const uint32_t revision = Memory::Read32(mdl + kMdl0RevisionOffset);
    if (revision != 8 && revision != 9 && revision != 11) {
        return 0;
    }
    const uint32_t info = mdl + (revision == 11 ? kMdl0Rev11InfoOffset : kMdl0InfoOffset);
    const uint32_t offset = Memory::Read32(info + kResMdlInfoMtxTableOffset);
    if (offset == 0 || offset > 0x100000 || !Memory::Contains(info + offset, 4)) {
        return 0;
    }
    const uint32_t count = Memory::Read32(info + offset);
    return count > 0 && count <= 256 ? count : 0;
}

// Reads the driver model's bone dictionary into a rig. Only standard driver
// rigs qualify: both arms as arm_*1 -> arm_*2 -> wrist_*1, a head bone unless
// the model is King Boo's, and a name to key the palm profile on.
const char* ReadCharacterRigLocked(uint32_t driver, uint32_t mdl) {
    CharacterRig rig;
    rig.driver = driver;
    rig.model = mdl;
    rig.palette_count = PaletteCount(mdl);
    if (rig.palette_count == 0) {
        return "unsupported MDL0";
    }
    const uint32_t offset = Memory::Read32(mdl + kMdl0BoneDictionaryOffset);
    if (offset == 0 || offset > 0x100000 || !Memory::Contains(mdl + offset, 8)) {
        return "no bone dictionary";
    }
    const uint32_t dictionary = mdl + offset;
    const uint32_t count = Memory::Read32(dictionary + 4);
    if (count == 0 || count > 128 || !Memory::Contains(dictionary, 8 + (count + 1) * 16)) {
        return "bone dictionary size";
    }
    std::vector<uint32_t> nodes;
    std::vector<body_ik::RigBone> bones;
    for (uint32_t i = 1; i <= count; ++i) {
        // ResDic entry i: data offset at +0xC, relative to the dictionary.
        const uint32_t relative = Memory::Read32(dictionary + 8 + i * 16 + 12);
        if (relative == 0 || relative > 0x100000) {
            return "bone entry";
        }
        const uint32_t node = dictionary + relative;
        if (!Memory::Contains(node, kResNodeBytes)) {
            return "bone node";
        }
        const uint32_t matrix = Memory::Read32(node + kResNodeMtxIdOffset);
        if (matrix >= rig.palette_count ||
            std::find(rig.matrices.begin(), rig.matrices.end(), matrix) != rig.matrices.end()) {
            return "bone matrix index";
        }
        nodes.push_back(node);
        rig.matrices.push_back(matrix);
        bones.push_back({-1, ReadGuestName(node + Memory::Read32(node + kResNodeNameOffset), 64)});
    }
    for (size_t i = 0; i < bones.size(); ++i) {
        const auto relative = static_cast<int32_t>(Memory::Read32(nodes[i] + kResNodeParentOffset));
        if (relative == 0) {
            continue;
        }
        const uint32_t parent = nodes[i] + static_cast<uint32_t>(relative);
        const auto found = std::find(nodes.begin(), nodes.end(), parent);
        if (found == nodes.end()) {
            return "bone parent";
        }
        bones[i].parent = static_cast<int>(found - nodes.begin());
    }
    rig.regions = body_ik::ClassifyRig(bones);
    if (!rig.regions.valid) {
        return "no standard arms";
    }
    // The profile key: the name of the first vertex position array.
    const uint32_t positions = mdl + Memory::Read32(mdl + kMdl0VertexPositionsOffset);
    if (!Memory::Contains(positions, 40) || Memory::Read32(positions + 4) == 0) {
        return "no vertex positions";
    }
    const uint32_t array = positions + Memory::Read32(positions + 36);
    if (!Memory::Contains(array, 16)) {
        return "vertex position array";
    }
    rig.profile_key = body_ik::ProfileKey(ReadGuestName(array + Memory::Read32(array + 0xC), 128));
    if (rig.profile_key.empty()) {
        return "no model name";
    }
    // King Boo's head and body are one piece: hands only. Any other headless
    // rig is unknown.
    if (rig.regions.head < 0 && rig.profile_key != "king_teresa_body") {
        return "no head bone";
    }
    rig.profile = body_ik::FindHandProfile(rig.profile_key.c_str());
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] character body: rig ready, " << count << " bones, " << rig.palette_count
                           << " matrices, " << rig.profile_key
                           << (rig.profile != nullptr ? " (palm profile)" : " (no palm profile)") << std::endl;
    g_character_rig = std::move(rig);
    return nullptr;
}

// Whether this frame's driver is drawn as the character body. Decided before
// ScnRoot gathers its draw lists (ApplyModelVisibilityLocked), since restoring
// a driver hidden by then is too late for the frame.
bool CanShowCharacterBodyLocked() noexcept {
    if (!g_state.body_ik || !g_state.first_person_active || g_state.bullet_active ||
        g_state.seat != FirstPersonSeat::Cockpit || !g_state.cockpit.valid) {
        return false;
    }
    try {
        const uint32_t driver = LocalDriver(g_state.player_kart.accessor);
        uint32_t director = 0, mdl = 0, mii = 0;
        if (driver == 0 || !ReadGuestPointer(driver + kDriverModelOffset, director) ||
            !ReadGuestPointer(director + kModelDirectorResMdlOffset, mdl)) {
            return false;
        }
        if (g_character_rig.driver == driver && g_character_rig.model == mdl && g_character_rig.regions.valid) {
            return true;
        }
        if (g_rejected_driver == driver && g_rejected_model == mdl) {
            return false;
        }
        g_character_rig = {};
        // A Mii's face is a separate model the head bone does not carry.
        const char* rejected = ReadGuestPointer(driver + kDriverMiiFaceOffset, mii)
                                   ? "a Mii"
                                   : ReadCharacterRigLocked(driver, mdl);
        if (rejected == nullptr) {
            return true;
        }
        g_rejected_driver = driver;
        g_rejected_model = mdl;
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] character body: not for this driver (" << rejected
                               << "); the driver keeps first_person_hide_driver and the VR hands" << std::endl;
    } catch (const Memory::AccessViolation&) {
    }
    return false;
}

void ScaleColumnsLike(Mtx34& pose, const Mtx34& like) {
    for (int column = 0; column < 3; ++column) {
        const float length = body_ik::Length({like[column], like[4 + column], like[8 + column]});
        for (int row = 0; row < 3; ++row) {
            pose[row * 4 + column] *= length;
        }
    }
}

constexpr unsigned kNotPosed = UINT32_MAX;

// This frame's pose, from the palette as the game left it (`original`): the
// world matrices in `modified`, and in `hide` the matrix that collapses the
// head, for the parts that go with it. Returns the hands posed on a
// controller (bit 0 left, bit 1 right), or kNotPosed.
unsigned PoseCharacterLocked(const std::vector<Mtx34>& original, std::vector<Mtx34>& modified, Mtx34& hide) {
    auto& rig = g_character_rig;
    const auto& regions = rig.regions;
    const uint32_t count = rig.palette_count;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    // Controller poses mapped for this vehicle, and recent: a cockpit the XR
    // side stopped updating leaves the arms seated.
    auto tracking = OpenXRReadDriving();
    if (!tracking.cockpit_active || tracking.vehicle_identity != g_state.cockpit.body || tracking.published_ns == 0 ||
        now < tracking.published_ns || now - tracking.published_ns > 250000000ull) {
        tracking = {};
    }
    // This frame's seat. The seal samples the eye calibration; this only reads
    // it. anchor_from_scene * view does not depend on the view in the cockpit's
    // rotation modes, so the scene camera still holding last frame's matrix is
    // no matter.
    Mtx34 view{}, pose{}, seat_to_world{}, inverse_body{};
    FirstPersonAnchor anchor{};
    const char* failed = nullptr;
    const KartPoseRead kart = ReadPlayerKartPose(g_state.player_kart, pose);
    if (kart.failed_step != nullptr || !ReadSceneViewMatrix(view) ||
        !ComputeCockpitAnchorLocked(view, kart, pose, anchor, failed, false) || !anchor.valid ||
        !(anchor.units_per_meter > 0.0f)) {
        return kNotPosed;
    }
    const float units = anchor.units_per_meter;
    const Mtx34 world_to_seat = ComposeMtx(anchor.anchor_from_scene, view);
    if (!InvertMtx(world_to_seat, seat_to_world) || !InvertMtx(g_state.cockpit.body_pose, inverse_body)) {
        return kNotPosed;
    }
    modified = original;

    // The seated skeleton, latched once per driver and model in kart-local
    // space, rides in the same comfort frame as the view and the wheel: impact
    // and trick animations cannot throw the shoulders away from the hands, and
    // the vehicle's movement and a lightning shrink still apply.
    if (rig.seated_pose.size() != count) {
        rig.seated_pose.assign(count, kIdentityMtx34);
        rig.seated_scale = g_state.cockpit.player_scale;
        for (const uint32_t matrix : rig.matrices) {
            rig.seated_pose[matrix] = ComposeMtx(inverse_body, modified[matrix]);
        }
    }
    for (const uint32_t matrix : rig.matrices) {
        auto local = rig.seated_pose[matrix];
        for (int row = 0; row < 3; ++row) {
            const float ratio = g_state.cockpit.player_scale[row] / rig.seated_scale[row];
            for (int column = 0; column < 4; ++column) {
                local[row * 4 + column] *= ratio;
            }
        }
        modified[matrix] = ComposeMtx(g_state.cockpit.seat_body, local);
    }

    // The legs keep their live articulation relative to the seated pelvis, but
    // only in driving poses: tricks, the mini-turbo bonus and the dash or
    // celebration poses would lift them into view.
    uint32_t state = 0, flags = 0, movement = 0;
    const bool trick = ReadGuestPointer(kart.accessor + kKartAccessorStateOffset, state) &&
                       Memory::TryRead32(state + kKartStateTrickFlagsOffset, flags) &&
                       (flags & kKartStateTrickAnimating) != 0;
    ReadGuestPointer(kart.accessor + kKartAccessorMovementOffset, movement);
    const bool mini_turbo = movement != 0 && Memory::Contains(movement + kMovementMiniTurboOffset, 2) &&
                            static_cast<int16_t>(Memory::Read16(movement + kMovementMiniTurboOffset)) > 0;
    const bool drifting = movement != 0 && Memory::Contains(movement + kMovementDriftStateOffset, 2) &&
                          Memory::Read16(movement + kMovementDriftStateOffset) != 0;
    const bool driving_animation =
        Memory::Contains(rig.driver + kDriverAnimationOffset, 8) &&
        body_ik::IsDrivingLegAnimation(Memory::Read16(rig.driver + kDriverAnimationOffset)) &&
        body_ik::IsDrivingLegAnimation(Memory::Read16(rig.driver + kDriverSecondAnimationOffset));
    if (rig.leg_animation.AllowNative(trick, mini_turbo, drifting, now, !driving_animation)) {
        const auto seated = modified;
        for (const auto& leg : regions.legs) {
            const uint32_t parent = rig.matrices[static_cast<size_t>(leg.parent)];
            for (const int bone : leg.bones) {
                const uint32_t matrix = rig.matrices[static_cast<size_t>(bone)];
                body_ik::AnimateSeatedLimb(seated[parent], original[parent], original[matrix], modified[matrix]);
            }
        }
    }

    const auto matrix_of = [&rig](int bone) { return rig.matrices[static_cast<size_t>(bone)]; };
    const auto bend_spine = [&](const Mtx34& bend) {
        for (const int bone : regions.spine_bones) {
            modified[matrix_of(bone)] = ComposeMtx(bend, modified[matrix_of(bone)]);
        }
    };
    if (regions.spine >= 0) {
        // A wide torso leans back about the spine, just enough for the eyes to
        // clear the shoulders; hips and legs keep their seat.
        const auto left = body_ik::Position(modified[matrix_of(regions.arms[0][0])]);
        const auto right = body_ik::Position(modified[matrix_of(regions.arms[1][0])]);
        const auto l = detail::TransformPoint(world_to_seat, left.x, left.y, left.z);
        const auto r = detail::TransformPoint(world_to_seat, right.x, right.y, right.z);
        const float clearance = std::abs(l.x - r.x) * 0.55f + 0.10f * units;
        const float back = std::max(0.0f, clearance - (l.z + r.z) * 0.5f);
        const auto pivot = body_ik::Position(modified[matrix_of(regions.spine)]);
        const auto seated = detail::TransformPoint(world_to_seat, pivot.x, pivot.y, pivot.z);
        const float lean = std::clamp(back / std::max(-seated.y, 0.35f * units), 0.0f, 0.4f);
        bend_spine(body_ik::Align(pivot, detail::TransformDirection(seat_to_world, {0, 1, 0}),
                                  detail::TransformDirection(seat_to_world, {0, 1, lean})));
        // The headset's lean bends the chest about the same spine, limited
        // rather than stretching the torso.
        if (tracking.head_tracked && detail::IsFiniteMtx34(tracking.seat_from_head)) {
            const auto& head = tracking.seat_from_head;
            const float x = std::clamp(head[3], -0.25f, 0.25f), z = std::clamp(head[11], -0.25f, 0.25f);
            bend_spine(body_ik::Align(body_ik::Position(modified[matrix_of(regions.spine)]),
                                      detail::TransformDirection(seat_to_world, {0, 1, 0}),
                                      detail::TransformDirection(seat_to_world, {x * 0.8f, 1, z * 0.8f})));
        }
    }

    // The arms. The cartoon's short arms are fitted to a human reach (length
    // only, bounded), their sleeves thinned, and the wrist placed on the
    // controller by heurazy's palm profile for the character, or else by a
    // fixed palm offset.
    constexpr float kArmCrossSection = 0.65f;
    const auto rest_pose = modified;
    const float palm_distance = 0.055f * units;
    unsigned hands = 0;
    for (int hand = 0; hand < 2; ++hand) {
        if (!tracking.hands[hand].grip_valid) {
            rig.calibrated[hand] = false;
            continue;
        }
        const auto& arm = regions.arms[hand];
        const Mtx34 shoulder = modified[matrix_of(arm[0])];
        const Mtx34 elbow = modified[matrix_of(arm[1])];
        const Mtx34 wrist = modified[matrix_of(arm[2])];
        auto grip = tracking.hands[hand].seat_from_grip;
        for (int row = 0; row < 3; ++row) {
            grip[row * 4 + 3] *= units;
        }
        Mtx34 target = ComposeMtx(seat_to_world, grip);
        if (!detail::IsFiniteMtx34(target)) {
            continue;
        }
        Mtx34 profile_local{};
        if (rig.profile != nullptr) {
            std::copy_n(rig.profile->wrist_from_grip.begin() + hand * 12, 12, profile_local.begin());
            for (int row = 0; row < 3; ++row) {
                profile_local[row * 4 + 3] *= units;
            }
        } else {
            // Centre the palm on the grip, mirrored per hand, along the
            // controller's own X so it stays right however the hand turns.
            auto lateral = detail::TransformDirection(target, {1, 0, 0});
            if (detail::Normalize(lateral)) {
                const auto offset = body_ik::Mul(lateral, (hand == 0 ? -1.0f : 1.0f) * 0.015f * units);
                target[3] += offset.x;
                target[7] += offset.y;
                target[11] += offset.z;
            }
        }
        const auto pole = detail::TransformDirection(seat_to_world, {hand == 0 ? -0.7f : 0.7f, -1.0f, 0.25f});
        const auto shoulder_at = body_ik::Position(shoulder);
        const auto upper_segment = body_ik::Sub(body_ik::Position(elbow), shoulder_at);
        const auto lower_segment = body_ik::Sub(body_ik::Position(wrist), body_ik::Position(elbow));
        const float natural = body_ik::Length(upper_segment) + body_ik::Length(lower_segment);
        if (natural < 0.001f) {
            continue;
        }
        // Reach is fitted to the wrist, not the palm, so a large glove does not
        // lengthen and fold the whole arm.
        const float reach =
            rig.profile != nullptr
                ? body_ik::Length(body_ik::Sub(body_ik::Position(ComposeMtx(target, profile_local)), shoulder_at))
                : body_ik::Length(body_ik::Sub(body_ik::Position(target), shoulder_at)) + palm_distance;
        const float fit = body_ik::FitArmReach(natural, reach, units);
        const auto fitted_elbow = body_ik::Add(shoulder_at, body_ik::Mul(upper_segment, fit));
        const auto fitted_wrist = body_ik::Add(fitted_elbow, body_ik::Mul(lower_segment, fit));
        auto solved = body_ik::Solve(shoulder_at, fitted_elbow, fitted_wrist, body_ik::Position(target), pole);
        if (!solved.valid) {
            continue;
        }
        Mtx34 upper = body_ik::MapSegment(shoulder_at, shoulder_at, upper_segment,
                                          body_ik::Sub(solved.elbow, shoulder_at), kArmCrossSection);
        Mtx34 lower = body_ik::MapSegment(body_ik::Position(elbow), solved.elbow, lower_segment,
                                          body_ik::Sub(solved.wrist, solved.elbow), kArmCrossSection);
        Mtx34 hand_pose = ComposeMtx(lower, wrist);
        Mtx34 rotation = target;
        rotation[3] = rotation[7] = rotation[11] = 0.0f;
        if (!rig.calibrated[hand]) {
            Mtx34 inverse{};
            if (!InvertMtx(rotation, inverse)) {
                continue;
            }
            auto rest = hand_pose;
            rest[3] = rest[7] = rest[11] = 0.0f;
            for (int column = 0; column < 3; ++column) {
                const float length = body_ik::Length({rest[column], rest[4 + column], rest[8 + column]});
                if (length > 0.0001f) {
                    for (int row = 0; row < 3; ++row) {
                        rest[row * 4 + column] /= length;
                    }
                }
            }
            rig.wrist_offset[hand] = ComposeMtx(inverse, rest);
            rig.calibrated[hand] = true;
        }
        hand_pose = ComposeMtx(rotation, rig.wrist_offset[hand]);
        ScaleColumnsLike(hand_pose, wrist);
        Mtx34 inverse_wrist{};
        if (!InvertMtx(wrist, inverse_wrist)) {
            continue;
        }
        const auto palm_axis = detail::TransformDirection(inverse_wrist, lower_segment);
        auto wrist_target = body_ik::WristForPalm(body_ik::Position(target), hand_pose, palm_axis, palm_distance);
        if (rig.profile != nullptr) {
            hand_pose = ComposeMtx(target, profile_local);
            wrist_target = body_ik::Position(hand_pose);
            ScaleColumnsLike(hand_pose, wrist);
        }
        solved = body_ik::Solve(shoulder_at, fitted_elbow, fitted_wrist, wrist_target, pole);
        if (!solved.valid) {
            continue;
        }
        upper = body_ik::MapSegment(shoulder_at, shoulder_at, upper_segment, body_ik::Sub(solved.elbow, shoulder_at),
                                    kArmCrossSection);
        lower = body_ik::MapSegment(body_ik::Position(elbow), solved.elbow, lower_segment,
                                    body_ik::Sub(solved.wrist, solved.elbow), kArmCrossSection);
        hand_pose[3] = solved.wrist.x;
        hand_pose[7] = solved.wrist.y;
        hand_pose[11] = solved.wrist.z;
        const Mtx34 hand_transform = ComposeMtx(hand_pose, inverse_wrist);
        for (const int bone : regions.hand_bones[hand]) {
            modified[matrix_of(bone)] = ComposeMtx(hand_transform, rest_pose[matrix_of(bone)]);
        }
        for (const int bone : regions.lower_bones[hand]) {
            modified[matrix_of(bone)] = ComposeMtx(lower, rest_pose[matrix_of(bone)]);
        }
        for (const int bone : regions.upper_bones[hand]) {
            modified[matrix_of(bone)] = ComposeMtx(upper, rest_pose[matrix_of(bone)]);
        }
        if (regions.head < 0) {
            // King Boo: the elbow bone carries the hand.
            modified[matrix_of(arm[1])] = ComposeMtx(hand_transform, rest_pose[matrix_of(arm[1])]);
        }
        hands |= 1u << hand;
    }

    // Hands at half size about the wrist, tracked or not, without moving it.
    constexpr float kHandScale = 0.5f;
    for (int hand = 0; hand < 2; ++hand) {
        const auto at = body_ik::Position(modified[matrix_of(regions.arms[hand][2])]);
        const Mtx34 smaller{kHandScale, 0, 0, at.x * (1 - kHandScale), 0, kHandScale, 0, at.y * (1 - kHandScale),
                            0,          0, kHandScale, at.z * (1 - kHandScale)};
        for (const int bone : regions.visible_hand_bones[hand]) {
            modified[matrix_of(bone)] = ComposeMtx(smaller, modified[matrix_of(bone)]);
        }
    }
    // The head and everything under it collapse to its own pivot; a tiny
    // invertible scale keeps nw4r's normal matrices defined.
    hide = kIdentityMtx34;
    if (regions.head >= 0) {
        const auto head = body_ik::Position(modified[matrix_of(regions.head)]);
        hide = {0.0001f, 0, 0, head.x * 0.9999f, 0, 0.0001f, 0, head.y * 0.9999f, 0, 0, 0.0001f, head.z * 0.9999f};
        for (const int bone : regions.head_bones) {
            modified[matrix_of(bone)] = ComposeMtx(hide, modified[matrix_of(bone)]);
        }
    }
    if (g_state.body_ik_hands_only || regions.head < 0) {
        // Each hidden joint collapses onto its arm's wrist or the root, not its
        // own origin, or the old arm would still show.
        const auto body = body_ik::Position(modified[rig.matrices.front()]);
        for (const auto& [bone, arm] : regions.collapsed) {
            const auto pivot = arm < 0 ? body : body_ik::Position(modified[matrix_of(regions.arms[arm][2])]);
            auto& m = modified[matrix_of(bone)];
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    m[row * 4 + column] *= 0.0001f;
                }
            }
            m[3] = pivot.x;
            m[7] = pivot.y;
            m[11] = pivot.z;
        }
    }
    return hands;
}

// Poses the palette for this CalcView. Returns the hands the character draws
// (bit 0 left, bit 1 right) with bit 2 set, or zero when the body is not drawn.
unsigned PrepareCharacterBody(const CpuContext* context, CharacterPalette& saved) {
    std::lock_guard lock(g_mutex);
    if (!g_state.armed || !CanShowCharacterBodyLocked()) {
        return 0;
    }
    auto& rig = g_character_rig;
    uint32_t director = 0, ex = 0, scn = 0, palette = 0, attributes = 0;
    if (!ReadGuestPointer(rig.driver + kDriverModelOffset, director) ||
        !ReadGuestPointer(director + kModelDirectorScnMdlExOffset, ex) || !ReadGuestPointer(ex, scn) ||
        !ReadGuestPointer(scn + kScnMdlWorldMtxArrayOffset, palette) ||
        !ReadGuestPointer(scn + kScnMdlWorldMtxAttribOffset, attributes)) {
        return 0;
    }
    const uint32_t count = rig.palette_count;
    if (!Memory::Contains(palette, count * kMtx34Bytes) || !Memory::Contains(attributes, count * 4u) ||
        !SavePalette(saved, palette, attributes, count)) {
        return 0;
    }
    // CalcView can run more than once in a guest frame (another scene root or
    // camera): the pose is the same, only the palette is set again.
    if (!rig.posed_valid || rig.posed_frame != g_state.armed_frame || rig.posed.size() != count) {
        const unsigned posed = PoseCharacterLocked(saved.original, rig.posed, rig.posed_hide);
        if (posed == kNotPosed) {
            rig.posed_valid = false;
            // Nothing has been written: there is nothing to put back.
            saved.original.clear();
            return 0;
        }
        rig.posed_valid = true;
        rig.posed_frame = g_state.armed_frame;
        rig.posed_hands = posed;
    }
    const auto& modified = rig.posed;
    const Mtx34& hide = rig.posed_hide;
    const unsigned hands = rig.posed_hands;
    // Only the bones' matrices change; the skinning pass derives the rest.
    for (const uint32_t matrix : rig.matrices) {
        for (unsigned k = 0; k < 12; ++k) {
            Memory::WriteFloat32(palette + matrix * kMtx34Bytes + k * 4u, modified[matrix][k]);
        }
    }
    RunSkinning(context, scn, palette, attributes);

    // Toadette's pigtails are a separate model with their own skeleton, which
    // would hang across the eyes: collapse validated hair parts with the head,
    // never vehicle parts or unknown accessories.
    uint32_t part_count = 0;
    if (Memory::TryRead32(rig.driver + kModelHolderCountOffset, part_count) && part_count <= 6) {
        for (uint32_t index = 0; index < part_count; ++index) {
            uint32_t part = 0, part_mdl = 0, part_ex = 0, part_scn = 0, part_palette = 0, part_attributes = 0;
            if (!ReadGuestPointer(rig.driver + kModelHolderArrayOffset + index * 4u, part) || part == director ||
                !ReadGuestPointer(part + kModelDirectorResMdlOffset, part_mdl) || !Memory::Contains(part_mdl, 0x90) ||
                Memory::Read32(part_mdl) != kMdl0Magic || Memory::Read32(part_mdl + kMdl0RevisionOffset) != 11) {
                continue;
            }
            const auto name = ReadGuestName(part_mdl + Memory::Read32(part_mdl + kMdl0Rev11NameOffset), 64);
            if (name.find("hair") == std::string::npos) {
                continue;
            }
            if (!ReadGuestPointer(part + kModelDirectorScnMdlExOffset, part_ex) ||
                !ReadGuestPointer(part_ex, part_scn) ||
                !ReadGuestPointer(part_scn + kScnMdlWorldMtxArrayOffset, part_palette) ||
                !ReadGuestPointer(part_scn + kScnMdlWorldMtxAttribOffset, part_attributes)) {
                continue;
            }
            const uint32_t matrices = PaletteCount(part_mdl);
            if (matrices == 0 || matrices > 64 || !Memory::Contains(part_palette, matrices * kMtx34Bytes) ||
                !Memory::Contains(part_attributes, matrices * 4u)) {
                continue;
            }
            auto copy = std::make_unique<CharacterPalette>();
            if (!SavePalette(*copy, part_palette, part_attributes, matrices)) {
                continue;
            }
            for (uint32_t i = 0; i < matrices; ++i) {
                const auto matrix = ComposeMtx(hide, copy->original[i]);
                for (unsigned k = 0; k < 12; ++k) {
                    Memory::WriteFloat32(part_palette + i * kMtx34Bytes + k * 4u, matrix[k]);
                }
            }
            saved.attachments.push_back(std::move(copy));
            RunSkinning(context, part_scn, part_palette, part_attributes);
        }
    }
    RestoreModelVisibilityLocked();
    return hands | 4u;
}

} // namespace

bool MkwVRCharacterHandActive(size_t hand) noexcept {
    return hand < 2 && (g_character_hands.load(std::memory_order_acquire) & (1u << hand)) != 0;
}

extern "C" void CharacterBodyCalcView(CpuContext* context) {
    bool latch_frame = false;
    {
        std::lock_guard lock(g_mutex);
        latch_frame = g_state.body_ik && g_state.enabled && g_state.first_person_active &&
                      g_state.seat == FirstPersonSeat::Cockpit;
    }
    // CalcView runs before the race draw observer, which is where the seat is
    // normally latched; this frame's has to be ready now.
    if (latch_frame) {
        MkwVRFirstPersonUpdateForCurrentFrame();
    }
    CharacterPalette saved;
    unsigned active = 0;
    try {
        active = PrepareCharacterBody(context, saved);
    } catch (const Memory::AccessViolation&) {
        saved.Restore();
        active = 0;
    }
    if (active != 0 && g_body_logged_state.exchange(active, std::memory_order_relaxed) != active) {
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] character body: drawn, left hand "
                               << ((active & 1u) != 0 ? "tracked" : "seated") << ", right hand "
                               << ((active & 2u) != 0 ? "tracked" : "seated") << std::endl;
    }
    g_character_hands.store(active, std::memory_order_release);
    func_8006FA50(context);
    // `saved` puts the original palettes back here, even if CalcView throws.
}
