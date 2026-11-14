// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runtime glue for room voice chat; see include/voice_chat.h and docs/voice-chat.md.

#include "voice_chat.h"

#include "memory.h"
#include "runtime_config.h"
#include "runtime_product.h"
#include "voicechat/voice_api.h"
#include "voicechat/voice_protocol.h"
#include "voicechat/voice_room_mkw.h"
#include "voicechat/voice_spatial.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/openxr_voice.h"

#include <SDL3/SDL_keyboard.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace VoiceChat {

// Named rather than anonymous: the runtime is a unity build.
namespace voice_chat_internal {

namespace api = mkw::voicechat;

constexpr const char* kSection = "voicechat";
constexpr const char* kHotkeyKeys[kHotkeyCount] = {"push_to_talk_key", "mute_key", "deafen_key"};
constexpr const char* kTalkModes[] = {"voice_activation", "open_mic", "push_to_talk"};

#if defined(__ANDROID__)
// The headset's own voice-call capture already cancels the strap speakers' echo and suppresses
// noise, at no CPU cost to the game; RNNoise stays available but off.
constexpr bool kDefaultNoiseSuppression = false;
constexpr bool kDefaultPlatformProcessing = true;
#else
constexpr bool kDefaultNoiseSuppression = true;
constexpr bool kDefaultPlatformProcessing = false;
#endif

struct State {
    std::mutex mutex;  // settings are read on the game thread and edited by the menus
    api::Settings settings;
    // SDL scancodes; -1 is unbound (PAD_KEY_INVALID, as the other key bindings).
    std::array<int, kHotkeyCount> hotkeys{SDL_SCANCODE_V, -1, -1};
    // MKW-VoiceChat's two overlay parts, under its key names.
    bool overlayLocalStatus = true;
    bool overlayPlayerSpeakers = true;
    bool started = false;
    std::atomic<bool> enabled{false};
    std::uint32_t frame = 0;
    bool spatialPosted = false;
    bool pushToTalkHeld = false;
    // When mute or deafen last changed: the headset badge shows the new state for a moment.
    std::chrono::steady_clock::time_point localStateChangedAt{};
    bool lastMuted = false;
    bool lastDeafened = false;
};

constexpr auto kBadgeLocalStateSeconds = std::chrono::seconds(3);

State& Get() {
    static State state;
    return state;
}

template <typename T>
std::optional<T> FindValue(const toml::value& table, const char* key) {
    try {
        if (!table.contains(key)) return std::nullopt;
        return toml::get<T>(table.at(key));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> FindNumber(const toml::value& table, const char* key) {
    if (const auto value = FindValue<double>(table, key)) return value;
    if (const auto value = FindValue<std::int64_t>(table, key)) return static_cast<double>(*value);
    return std::nullopt;
}

void LoadBool(const toml::value& table, const char* key, bool& out) {
    if (const auto value = FindValue<bool>(table, key)) out = *value;
}

void LoadInt(const toml::value& table, const char* key, int& out, int low, int high) {
    if (const auto value = FindNumber(table, key)) out = std::clamp(static_cast<int>(*value), low, high);
}

void LoadFloat(const toml::value& table, const char* key, float& out, float low, float high) {
    if (const auto value = FindNumber(table, key)) out = std::clamp(static_cast<float>(*value), low, high);
}

void LoadString(const toml::value& table, const char* key, std::string& out) {
    if (const auto value = FindValue<std::string>(table, key)) out = *value;
}

std::string FindCaCertificateBundle() {
    // Same places as network_ssl.cpp's lookup: next to the executable (the APK unpacks its runtime
    // resources there on Android), the Android app's data folder, or a source checkout.
    std::vector<std::filesystem::path> candidates;
    if (const auto directory = RuntimeConfigFile::ExecutableDirectory()) candidates.push_back(*directory / "cacert.pem");
#if defined(__ANDROID__)
    candidates.push_back(RuntimeConfigFile::ApplicationDataDirectory() / "cacert.pem");
#endif
    std::error_code ec;
    for (auto base = std::filesystem::current_path(ec); !ec && !base.empty();) {
        candidates.push_back(base / "runtime" / "assets" / "certs" / "cacert.pem");
        const auto parent = base.parent_path();
        if (parent == base) break;
        base = parent;
    }
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) return RuntimeConfigFile::PathToUtf8(candidate);
    }
    return {};
}

void LoadSettings(State& state) {
    api::Settings& settings = state.settings;
    settings.noiseSuppression = kDefaultNoiseSuppression;
    settings.platformVoiceProcessing = kDefaultPlatformProcessing;
    settings.caCertificateFile = FindCaCertificateBundle();
    const auto& table = RuntimeConfigFile::Get().voiceChatTable;
    if (!table) return;
    LoadBool(*table, "enabled", settings.enabled);
    LoadString(*table, "signaling_url", settings.signalingUrl);
    LoadString(*table, "input_device", settings.inputDevice);
    LoadString(*table, "output_device", settings.outputDevice);
    if (const auto mode = FindValue<std::string>(*table, "talk_mode")) {
        for (int i = 0; i < 3; ++i) {
            if (*mode == kTalkModes[i]) settings.talkMode = static_cast<api::TalkMode>(i);
        }
    }
    LoadInt(*table, "voice_activation_threshold", settings.voiceActivationThreshold, 1, 100);
    LoadBool(*table, "muted", settings.muted);
    LoadBool(*table, "deafened", settings.deafened);
    LoadFloat(*table, "microphone_gain", settings.microphoneGain, 0.25f, 5.0f);
    LoadFloat(*table, "playback_volume", settings.playbackVolume, 0.0f, 3.0f);
    LoadBool(*table, "normalization", settings.normalization);
    LoadBool(*table, "noise_suppression", settings.noiseSuppression);
    LoadInt(*table, "noise_strength", settings.noiseSuppressionStrength, 0, 100);
    LoadBool(*table, "noise_gate", settings.noiseGate);
    LoadInt(*table, "noise_gate_threshold", settings.noiseGateThreshold, 0, 100);
    LoadFloat(*table, "microphone_boost", settings.microphoneBoost, 1.0f, 3.0f);
    LoadBool(*table, "compressor", settings.compressor);
    LoadInt(*table, "compressor_strength", settings.compressorStrength, 0, 100);
    LoadBool(*table, "platform_voice_processing", settings.platformVoiceProcessing);
    LoadBool(*table, "mute_everyone", settings.muteEveryone);
    LoadBool(*table, "mute_only_friends", settings.muteOnlyFriends);
    LoadBool(*table, "mute_everyone_but_friends", settings.muteEveryoneButFriends);
    LoadBool(*table, "mute_teammates", settings.muteTeammates);
    LoadBool(*table, "mute_everyone_but_teammates", settings.muteEveryoneButTeammates);
    LoadBool(*table, "mute_new_players", settings.muteNewPlayers);
    LoadBool(*table, "positional", settings.positional);
    LoadBool(*table, "overlay_local_status", state.overlayLocalStatus);
    LoadBool(*table, "overlay_player_speakers", state.overlayPlayerSpeakers);
    for (int i = 0; i < kHotkeyCount; ++i) {
        int key = state.hotkeys[i];
        LoadInt(*table, kHotkeyKeys[i], key, -1, SDL_SCANCODE_COUNT - 1);
        state.hotkeys[i] = key <= 0 ? -1 : key;
    }
    // Per-player volumes, under MKW-VoiceChat's key names: peer_volume_<profile id>.
    if (table->is_table()) {
        for (const auto& [key, value] : table->as_table()) {
            constexpr std::string_view kPrefix = "peer_volume_";
            if (key.rfind(kPrefix, 0) != 0) continue;
            const std::string pidText = key.substr(kPrefix.size());
            if (pidText.empty() || pidText.size() > 10 ||
                !std::all_of(pidText.begin(), pidText.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
                continue;
            }
            if (const auto volume = FindNumber(*table, key.c_str())) {
                settings.playerVolumes.emplace_back(static_cast<std::uint32_t>(std::stoull(pidText)),
                                                    std::clamp(static_cast<float>(*volume), 0.0f, 3.0f));
            }
        }
    }
}

void Persist(const char* key, bool value) {
    RuntimeConfigFile::WriteSetting(kSection, key, value ? "true" : "false");
}

void Persist(const char* key, int value) { RuntimeConfigFile::WriteSetting(kSection, key, std::to_string(value)); }

void Persist(const char* key, float value) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << value;
    RuntimeConfigFile::WriteSetting(kSection, key, text.str());
}

void Persist(const char* key, const std::string& value) {
    RuntimeConfigFile::WriteSetting(kSection, key, RuntimeConfigFile::FormatString(value));
}

void Apply(State& state) {
    if (state.settings.muted != state.lastMuted || state.settings.deafened != state.lastDeafened) {
        state.lastMuted = state.settings.muted;
        state.lastDeafened = state.settings.deafened;
        state.localStateChangedAt = std::chrono::steady_clock::now();
    }
    state.enabled.store(state.settings.enabled, std::memory_order_relaxed);
    if (state.started) api::ApplySettings(state.settings);
}

float PlayerVolume(const api::Settings& settings, std::uint32_t profileId) {
    for (const auto& [pid, volume] : settings.playerVolumes) {
        if (pid == profileId) return volume;
    }
    return 1.0f;
}

void SetPlayerVolume(api::Settings& settings, std::uint32_t profileId, float volume) {
    for (auto& [pid, value] : settings.playerVolumes) {
        if (pid == profileId) {
            value = volume;
            return;
        }
    }
    settings.playerVolumes.emplace_back(profileId, volume);
}

// ---- game thread -------------------------------------------------------------------------------

void PollPushToTalk(State& state) {
    bool held = false;
    if (state.settings.talkMode == api::TalkMode::PushToTalk) {
        held = mkw::vr::OpenXRVoiceButtonHeld();
        const int key = state.hotkeys[0];
        if (!held && key > SDL_SCANCODE_UNKNOWN && SDL_GetKeyboardFocus() != nullptr) {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            held = keys != nullptr && key < count && keys[key];
        }
    }
    if (held != state.pushToTalkHeld) {
        state.pushToTalkHeld = held;
        api::SetPushToTalkHeld(held);
    }
}

void PostRoom(std::uint32_t profileId) {
    const auto read = api::detail::ReadVoiceRoom<Memory>(profileId);
    api::RoomSnapshot snapshot;
    snapshot.inRoom = read.inRoom;
    snapshot.roomKey = read.roomKey;
    snapshot.roomProfileIds = read.roomProfileIds;
    snapshot.friendProfileIds = read.friendProfileIds;
    snapshot.teamMode = read.teamMode;
    snapshot.teamProfileIds = read.teamProfileIds;
    snapshot.teammateProfileIds = read.teammateProfileIds;
    api::PostRoomSnapshot(snapshot);
}

void Transform(const float* matrix, const float in[3], float out[3]) {
    for (int row = 0; row < 3; ++row) {
        out[row] = matrix[row * 4] * in[0] + matrix[row * 4 + 1] * in[1] + matrix[row * 4 + 2] * in[2] +
                   matrix[row * 4 + 3];
    }
}

// Each remote racer's kart in the listener's head space, in metres: world -> the camera the scene
// is drawn with -> (first person) the seat's anchor -> (immersive VR) the head's rotation.
bool PostSpatial(std::uint32_t profileId, const float* anchorFromScene, float unitsPerMeter) {
    const auto room = api::detail::ReadRkNetRoom<Memory>();
    if (!room.inRoom) return false;
    const auto racers = api::detail::ReadRacerPositions<Memory>(room, profileId);
    if (racers.empty()) return false;
    mkw::vr::Mtx34 view{};
    if (!mkw::vr::MkwVRReadSceneView(view)) return false;
    std::array<float, 9> head{};
    const bool headValid = mkw::vr::OpenXRReadHeadRotation(head);
    const float scale = anchorFromScene != nullptr && unitsPerMeter > 0.0f
                            ? unitsPerMeter
                            : mkw::voicechat::spatial::kWorldUnitsPerMeter;
    api::SpatialSnapshot snapshot;
    for (const auto& racer : racers) {
        float camera[3];
        Transform(view.data(), racer.position, camera);
        float center[3] = {camera[0], camera[1], camera[2]};
        if (anchorFromScene != nullptr) Transform(anchorFromScene, camera, center);
        float listener[3] = {center[0], center[1], center[2]};
        if (headValid) {
            for (int row = 0; row < 3; ++row) {
                listener[row] = head[row * 3] * center[0] + head[row * 3 + 1] * center[1] + head[row * 3 + 2] * center[2];
            }
        }
        api::SpatialEntry entry;
        entry.profileId = racer.profileId;
        for (int axis = 0; axis < 3; ++axis) entry.position[axis] = listener[axis] / scale;
        snapshot.entries.push_back(entry);
    }
    api::PostSpatialSnapshot(snapshot);
    return true;
}

// ---- menus -------------------------------------------------------------------------------------

const char* StateText(api::State state) {
    switch (state) {
    case api::State::Unavailable: return "Unavailable";
    case api::State::Disabled: return "Off";
    case api::State::Offline: return "Not online";
    case api::State::NotInRoom: return "Online, not in a room";
    case api::State::Connecting: return "Connecting";
    case api::State::Admitting: return "Joining the room";
    case api::State::InRoom: return "In the room's voice chat";
    case api::State::Retrying: return "Retrying";
    }
    return "";
}

ImVec4 StateColor(api::State state) {
    switch (state) {
    case api::State::InRoom: return ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
    case api::State::Connecting:
    case api::State::Admitting: return ImVec4(0.95f, 0.8f, 0.3f, 1.0f);
    case api::State::Retrying: return ImVec4(0.95f, 0.45f, 0.3f, 1.0f);
    default: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }
}

void LevelMeter(std::uint32_t peak, bool active) {
    const float level = std::clamp(static_cast<float>(peak) / 32767.0f, 0.0f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                          active ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    ImGui::ProgressBar(level, ImVec2(ImGui::GetFontSize() * 16.0f, ImGui::GetFontSize() * 0.6f), "");
    ImGui::PopStyleColor();
}

void DrawDeviceCombo(const char* label, std::string& value, const std::vector<std::string>& devices,
                     const char* key, State& state) {
    const char* preview = value.empty() ? "System default" : value.c_str();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (!ImGui::BeginCombo(label, preview)) return;
    bool changed = false;
    if (ImGui::Selectable("System default", value.empty())) {
        value.clear();
        changed = true;
    }
    for (const auto& device : devices) {
        if (ImGui::Selectable(device.c_str(), device == value)) {
            value = device;
            changed = true;
        }
    }
    ImGui::EndCombo();
    if (changed) {
        Persist(key, value);
        Apply(state);
    }
}

// Applies live while dragging and saves on release, so the config file is written once.
bool SliderPercent(const char* label, float& value, float low, float high, const char* key, State& state) {
    int percent = static_cast<int>(std::lround(value * 100.0f));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    const bool changed = ImGui::SliderInt(label, &percent, static_cast<int>(low * 100.0f),
                                          static_cast<int>(high * 100.0f), "%d%%");
    if (changed) {
        value = static_cast<float>(percent) / 100.0f;
        Apply(state);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) Persist(key, value);
    return changed;
}

bool SliderInt(const char* label, int& value, int low, int high, const char* key, State& state) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    const bool changed = ImGui::SliderInt(label, &value, low, high);
    if (changed) Apply(state);
    if (ImGui::IsItemDeactivatedAfterEdit()) Persist(key, value);
    return changed;
}

bool Checkbox(const char* label, bool& value, const char* key, State& state) {
    if (!ImGui::Checkbox(label, &value)) return false;
    Persist(key, value);
    Apply(state);
    return true;
}

void Tooltip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void DrawPlayers(State& state, const api::Status& status) {
    if (status.peers.empty() && status.roomPlayers.empty()) {
        ImGui::TextDisabled("Nobody else in the room uses voice chat yet.");
        return;
    }
    for (const auto& peer : status.peers) {
        ImGui::PushID(static_cast<int>(peer.profileId));
        const ImVec4 color = !peer.connected ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                             : peer.speaking ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
                                             : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TextColored(color, "%s %s", peer.speaking ? ">" : " ", peer.displayName.c_str());
        std::string flags;
        if (!peer.connected) flags += " connecting";
        if (peer.isFriend) flags += " friend";
        if (peer.isTeammate) flags += " teammate";
        if (peer.remoteMuted) flags += " muted";
        if (peer.remoteDeafened) flags += " deafened";
        if (peer.policyMuted) flags += " (silenced by a mute rule)";
        if (peer.protocolMismatch) flags += " (needs another voice chat version)";
        if (!flags.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", flags.c_str());
        }
        float volume = PlayerVolume(state.settings, peer.profileId);
        int percent = static_cast<int>(std::lround(volume * 100.0f));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::SliderInt("Volume", &percent, 0, 300, "%d%%")) {
            SetPlayerVolume(state.settings, peer.profileId, static_cast<float>(percent) / 100.0f);
            Apply(state);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            const std::string key = "peer_volume_" + std::to_string(peer.profileId);
            Persist(key.c_str(), PlayerVolume(state.settings, peer.profileId));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(volume > 0.0f ? "Mute" : "Unmute")) {
            SetPlayerVolume(state.settings, peer.profileId, volume > 0.0f ? 0.0f : 1.0f);
            const std::string key = "peer_volume_" + std::to_string(peer.profileId);
            Persist(key.c_str(), PlayerVolume(state.settings, peer.profileId));
            Apply(state);
        }
        ImGui::PopID();
    }
    bool header = false;
    for (const auto& player : status.roomPlayers) {
        if (player.voiceChat) continue;
        if (!header) {
            ImGui::TextDisabled("In the room without voice chat:");
            header = true;
        }
        ImGui::TextDisabled("  %s", player.displayName.c_str());
    }
}

const ImVec4 kSpeakingColor(0.35f, 0.85f, 0.45f, 1.0f);
const ImVec4 kWarningColor(0.95f, 0.45f, 0.3f, 1.0f);

// MKW-VoiceChat's mute options: one preset at a time, its own config key each, and "new players"
// beside them.
constexpr const char* kMutePresetLabels[] = {"None", "Everyone", "Only friends", "Everyone but friends",
                                             "Teammates", "Everyone but teammates"};
constexpr const char* kMutePresetKeys[] = {nullptr, "mute_everyone", "mute_only_friends", "mute_everyone_but_friends",
                                           "mute_teammates", "mute_everyone_but_teammates"};

bool* MutePresetFlag(api::Settings& settings, int preset) {
    switch (preset) {
    case 1: return &settings.muteEveryone;
    case 2: return &settings.muteOnlyFriends;
    case 3: return &settings.muteEveryoneButFriends;
    case 4: return &settings.muteTeammates;
    case 5: return &settings.muteEveryoneButTeammates;
    default: return nullptr;
    }
}

int CurrentMutePreset(api::Settings& settings) {
    for (int preset = 1; preset < IM_ARRAYSIZE(kMutePresetKeys); ++preset) {
        if (*MutePresetFlag(settings, preset)) return preset;
    }
    return 0;
}

void SetMutePreset(State& state, int chosen) {
    for (int preset = 1; preset < IM_ARRAYSIZE(kMutePresetKeys); ++preset) {
        bool* flag = MutePresetFlag(state.settings, preset);
        if (*flag != (preset == chosen)) {
            *flag = preset == chosen;
            Persist(kMutePresetKeys[preset], *flag);
        }
    }
    Apply(state);
}

bool ToggleButton(const char* label, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.25f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.3f, 0.25f, 1.0f));
    }
    const bool pressed = ImGui::Button(label);
    if (active) ImGui::PopStyleColor(2);
    return pressed;
}

// Devices, levels and processing back to the defaults (the mute state and the rules stay).
void ResetAudioSettings(State& state) {
    const api::Settings defaults;
    auto& settings = state.settings;
    settings.inputDevice.clear();
    settings.outputDevice.clear();
    settings.microphoneGain = defaults.microphoneGain;
    settings.playbackVolume = defaults.playbackVolume;
    settings.voiceActivationThreshold = defaults.voiceActivationThreshold;
    settings.normalization = defaults.normalization;
    settings.noiseSuppression = kDefaultNoiseSuppression;
    settings.noiseSuppressionStrength = defaults.noiseSuppressionStrength;
    settings.noiseGate = defaults.noiseGate;
    settings.noiseGateThreshold = defaults.noiseGateThreshold;
    settings.microphoneBoost = defaults.microphoneBoost;
    settings.compressor = defaults.compressor;
    settings.compressorStrength = defaults.compressorStrength;
    settings.platformVoiceProcessing = kDefaultPlatformProcessing;
    Persist("input_device", settings.inputDevice);
    Persist("output_device", settings.outputDevice);
    Persist("microphone_gain", settings.microphoneGain);
    Persist("playback_volume", settings.playbackVolume);
    Persist("voice_activation_threshold", settings.voiceActivationThreshold);
    Persist("normalization", settings.normalization);
    Persist("noise_suppression", settings.noiseSuppression);
    Persist("noise_strength", settings.noiseSuppressionStrength);
    Persist("noise_gate", settings.noiseGate);
    Persist("noise_gate_threshold", settings.noiseGateThreshold);
    Persist("microphone_boost", settings.microphoneBoost);
    Persist("compressor", settings.compressor);
    Persist("compressor_strength", settings.compressorStrength);
    Persist("platform_voice_processing", settings.platformVoiceProcessing);
    Apply(state);
}

void DrawSpeakerLines(const api::Status& status) {
    const ImVec4 idleColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    for (const auto& peer : status.peers) {
        if (!peer.connected) continue;
        const char* suffix = peer.remoteDeafened ? " (deafened)" : peer.remoteMuted ? " (muted)" : "";
        ImGui::TextColored(peer.speaking ? kSpeakingColor : idleColor, "%s %s%s", peer.speaking ? ">" : " ",
                           peer.displayName.c_str(), suffix);
    }
}

void DrawLocalStatusLine(const api::Status& status, bool muted, bool deafened) {
    if (deafened) {
        ImGui::TextColored(kWarningColor, "Voice: deafened");
    } else if (muted) {
        ImGui::TextColored(kWarningColor, "Voice: microphone muted");
    } else if (status.transmitting) {
        ImGui::TextColored(kSpeakingColor, "Voice: talking");
    } else {
        ImGui::TextDisabled("Voice: on");
    }
}

} // namespace voice_chat_internal

using namespace voice_chat_internal;

void Initialize() noexcept {
    auto& state = Get();
    std::scoped_lock lock(state.mutex);
    if (state.started) return;
    LoadSettings(state);
    state.enabled.store(state.settings.enabled, std::memory_order_relaxed);
    // Voice rooms are Retro Rewind rooms (the signaling server resolves them from RWFC's roster).
    if (!RuntimeProduct::IsRetroRewind() || !api::Available()) return;
    api::Start(state.settings);
    state.started = true;
}

void Shutdown() noexcept {
    auto& state = Get();
    {
        std::scoped_lock lock(state.mutex);
        if (!state.started) return;
        state.started = false;
    }
    api::Shutdown();
}

void OnFramePresented(const float* anchorFromScene, float unitsPerMeter) noexcept {
    auto& state = Get();
    if (!state.enabled.load(std::memory_order_relaxed)) return;
    std::scoped_lock lock(state.mutex);
    if (!state.started) return;
    ++state.frame;
    PollPushToTalk(state);
    // About twice a second: the room changes at human speed, and the reads are a few dozen bytes.
    if (state.frame % 30 == 0 && api::WantsRoomSnapshot()) {
        PostRoom(api::CurrentProfileId());
    }
    // Positions every other frame (30 Hz at 60 fps), only while a voice can be heard.
    if (state.settings.positional && api::WantsSpatialSnapshot()) {
        if (state.frame % 2 == 0) {
            const bool posted = PostSpatial(api::CurrentProfileId(), anchorFromScene, unitsPerMeter);
            if (!posted && state.spatialPosted) api::ClearSpatialSnapshot();
            state.spatialPosted = posted;
        }
    } else if (state.spatialPosted) {
        api::ClearSpatialSnapshot();
        state.spatialPosted = false;
    }
}

void HandleEvent(const SDL_Event& event) noexcept {
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) return;
    auto& state = Get();
    if (!state.enabled.load(std::memory_order_relaxed)) return;
    std::scoped_lock lock(state.mutex);
    const int scancode = static_cast<int>(event.key.scancode);
    if (state.hotkeys[1] > SDL_SCANCODE_UNKNOWN && scancode == state.hotkeys[1]) {
        state.settings.muted = !state.settings.muted;
        Persist("muted", state.settings.muted);
        Apply(state);
    } else if (state.hotkeys[2] > SDL_SCANCODE_UNKNOWN && scancode == state.hotkeys[2]) {
        state.settings.deafened = !state.settings.deafened;
        Persist("deafened", state.settings.deafened);
        Apply(state);
    }
}

int Hotkey(int target) noexcept {
    if (target < 0 || target >= kHotkeyCount) return -1;
    auto& state = Get();
    std::scoped_lock lock(state.mutex);
    return state.hotkeys[target];
}

void SetHotkey(int target, int scancode) noexcept {
    if (target < 0 || target >= kHotkeyCount) return;
    auto& state = Get();
    std::scoped_lock lock(state.mutex);
    state.hotkeys[target] = scancode <= 0 ? -1 : scancode;
    Persist(kHotkeyKeys[target], state.hotkeys[target]);
}

void DrawSettings(HotkeyDrawer drawHotkey) {
    if (!api::Available()) {
        ImGui::TextDisabled("This build has no voice chat.");
        return;
    }
    if (!RuntimeProduct::IsRetroRewind()) {
        ImGui::TextDisabled("Voice chat works in Retro Rewind online rooms.");
        return;
    }
    auto& state = Get();
    std::unique_lock lock(state.mutex);
    auto& settings = state.settings;
    const float wrap = ImGui::GetFontSize() * 30.0f;

    if (Checkbox("Enabled", settings.enabled, "enabled", state) && settings.enabled) api::RefreshDevices();
    ImGui::TextUnformatted("Retro Rewind voice chat");
    ImGui::TextDisabled("MKW-VoiceChat protocol | voice packets v%u",
                        static_cast<unsigned>(mkw::voicechat::protocol::kPacketVersion));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextDisabled(
        "Talks with the players in your Retro Rewind room who use WiiCompiled VR or MKW-VoiceChat. "
        "Voice goes directly between players, so they can see your IP address, and anyone can join "
        "a room's voice chat by claiming to be in it.");
    ImGui::PopTextWrapPos();
    if (!settings.enabled) return;

    const api::Status status = api::GetStatus();
    ImGui::TextColored(StateColor(status.state), "%s", StateText(status.state));
    if (!status.message.empty() && status.state != api::State::InRoom) {
        ImGui::SameLine();
        ImGui::TextDisabled("- %s", status.message.c_str());
    }
    ImGui::Text("Display name: %s", status.displayName.empty() ? "-" : status.displayName.c_str());
    if (!status.audioError.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextColored(kWarningColor, "%s", status.audioError.c_str());
#if defined(__ANDROID__)
        ImGui::TextDisabled("Allow the microphone in the launcher's Settings > Voice, then start the game again.");
#endif
        ImGui::PopTextWrapPos();
    }

    ImGui::TextUnformatted("Audio input");
    DrawDeviceCombo("##voice_input", settings.inputDevice, status.inputDevices, "input_device", state);
    ImGui::SameLine();
    if (ImGui::Button("Refresh##voice_input")) api::RefreshDevices();
    ImGui::TextUnformatted("Audio output");
    DrawDeviceCombo("##voice_output", settings.outputDevice, status.outputDevices, "output_device", state);
    ImGui::SameLine();
    if (ImGui::Button("Refresh##voice_output")) api::RefreshDevices();
    ImGui::TextUnformatted("Output volume");
    SliderPercent("##voice_output_volume", settings.playbackVolume, 0.0f, 3.0f, "playback_volume", state);
    ImGui::TextUnformatted("Microphone volume");
    SliderPercent("##voice_microphone_volume", settings.microphoneGain, 0.25f, 5.0f, "microphone_gain", state);
    if (ImGui::Button("Test output tone")) api::PlayTestTone();
    ImGui::SameLine();
    if (ImGui::Button("Reset audio settings")) ResetAudioSettings(state);
    ImGui::TextUnformatted("Microphone test");
    bool test = status.microphoneTest;
    if (ImGui::Checkbox("Hear my processed microphone", &test)) api::SetMicrophoneTest(test);
    Tooltip("Plays your processed microphone back to you; the room hears nothing meanwhile.");

    ImGui::SeparatorText("Status");
    if (ToggleButton(settings.muted ? "Unmute" : "Mute", settings.muted)) {
        settings.muted = !settings.muted;
        Persist("muted", settings.muted);
        Apply(state);
    }
    ImGui::SameLine();
    if (ToggleButton(settings.deafened ? "Undeafen" : "Deafen", settings.deafened)) {
        settings.deafened = !settings.deafened;
        Persist("deafened", settings.deafened);
        Apply(state);
    }
    Tooltip("Deafen: hear nobody, and tell the room you can't hear them.");
    ImGui::TextUnformatted("Voice activity");
    LevelMeter(status.microphonePeak, status.transmitting);
    int mode = static_cast<int>(settings.talkMode);
    ImGui::TextUnformatted("Talk mode");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::Combo("##voice_talk_mode", &mode, "Voice activation\0Open mic\0Push to talk\0")) {
        settings.talkMode = static_cast<api::TalkMode>(mode);
        Persist("talk_mode", std::string(kTalkModes[mode]));
        Apply(state);
    }
    if (settings.talkMode == api::TalkMode::VoiceActivation) {
        SliderInt("Activation level", settings.voiceActivationThreshold, 1, 100, "voice_activation_threshold", state);
        Tooltip("How loud you must speak to be heard; raise it if noise opens your microphone.");
    } else if (settings.talkMode == api::TalkMode::PushToTalk) {
        ImGui::TextDisabled("Hold the push-to-talk key (Hotkeys below); in VR, hold the left thumbstick in.");
    }
    Checkbox("Positional voices in races", settings.positional, "positional", state);
    Tooltip("Each player's voice comes from their kart, around your head in VR.");

    ImGui::TextUnformatted("Overlay options");
    int overlay = (state.overlayLocalStatus ? 1 : 0) | (state.overlayPlayerSpeakers ? 2 : 0);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::Combo("##voice_overlay", &overlay, "Off\0Local status\0Player speakers\0Local status + player speakers\0")) {
        state.overlayLocalStatus = (overlay & 1) != 0;
        state.overlayPlayerSpeakers = (overlay & 2) != 0;
        Persist("overlay_local_status", state.overlayLocalStatus);
        Persist("overlay_player_speakers", state.overlayPlayerSpeakers);
    }
    Tooltip("On screen and in the menus' headset screen; in VR races, the badge at the left of the race.");
    ImGui::TextUnformatted("Mute options");
    int preset = CurrentMutePreset(settings);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::Combo("##voice_mute_preset", &preset, kMutePresetLabels, IM_ARRAYSIZE(kMutePresetLabels))) {
        SetMutePreset(state, preset);
    }
    Checkbox("Mute players who join after me", settings.muteNewPlayers, "mute_new_players", state);

    if (ImGui::CollapsingHeader("Advanced")) {
#if defined(__ANDROID__)
        Checkbox("Headset echo cancellation", settings.platformVoiceProcessing, "platform_voice_processing", state);
        Tooltip("The headset's voice-call processing: keeps the game's sound out of your microphone.");
#endif
        Checkbox("Automatic level", settings.normalization, "normalization", state);
        Checkbox("Noise suppression", settings.noiseSuppression, "noise_suppression", state);
        if (settings.noiseSuppression) {
            SliderInt("Suppression strength", settings.noiseSuppressionStrength, 0, 100, "noise_strength", state);
        }
        Checkbox("Noise gate", settings.noiseGate, "noise_gate", state);
        if (settings.noiseGate) {
            SliderInt("Gate threshold", settings.noiseGateThreshold, 0, 100, "noise_gate_threshold", state);
        }
        SliderPercent("Microphone boost", settings.microphoneBoost, 1.0f, 3.0f, "microphone_boost", state);
        Checkbox("Compressor", settings.compressor, "compressor", state);
        if (settings.compressor) {
            SliderInt("Compressor strength", settings.compressorStrength, 0, 100, "compressor_strength", state);
        }
        ImGui::TextUnformatted("Signaling server");
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextDisabled("Voice rooms are arranged by MKW-VoiceChat's server unless another is set here; "
                            "players on different servers do not meet.");
        ImGui::PopTextWrapPos();
        static std::string editing;
        static bool editingLoaded = false;
        if (!editingLoaded) {
            editing = settings.signalingUrl;
            editingLoaded = true;
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        char buffer[256] = {};
        std::snprintf(buffer, sizeof(buffer), "%s", editing.c_str());
        if (ImGui::InputTextWithHint("##signaling", api::DefaultSignalingUrl(), buffer, sizeof(buffer))) {
            editing = buffer;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && editing != settings.signalingUrl) {
            settings.signalingUrl = editing;
            Persist("signaling_url", settings.signalingUrl);
            Apply(state);
        }
    }
    if (ImGui::CollapsingHeader("Hotkeys")) {
        if (drawHotkey != nullptr) {
            const auto keys = state.hotkeys;
            lock.unlock();
            drawHotkey("Push to talk", keys[0], 0);
            drawHotkey("Mute", keys[1], 1);
            drawHotkey("Deafen", keys[2], 2);
            lock.lock();
        } else {
            ImGui::TextDisabled("Keyboard keys are set in the desktop window's F10 > Voice Chat.");
        }
        ImGui::TextDisabled("In VR, push to talk is holding the left thumbstick in.");
    }

    ImGui::SeparatorText("Voice room");
    if (status.state != api::State::InRoom) {
        ImGui::TextDisabled("Not connected.");
    } else {
        DrawPlayers(state, status);
    }

    if (ImGui::CollapsingHeader("Debug")) {
        ImGui::Text("State: %s", StateText(status.state));
        ImGui::Text("Profile id: %u", status.profileId);
        ImGui::Text("Room: %s", status.roomId.empty() ? "-" : status.roomId.c_str());
        ImGui::Text("Member: %s", status.memberId.empty() ? "-" : status.memberId.substr(0, 8).c_str());
        ImGui::Text("ICE servers: %zu", status.iceServers);
        ImGui::Text("Microphone: %s", status.microphoneOpen ? "open" : "closed");
        for (const auto& peer : status.peers) {
            ImGui::Text("%s (%u): %s%s%s", peer.displayName.c_str(), peer.profileId,
                        peer.connected ? "connected" : "connecting", peer.receiving ? ", receiving" : "",
                        peer.protocolMismatch ? ", other packet version" : "");
        }
    }
}

void DrawOverlay() noexcept {
    auto& state = Get();
    if (!state.enabled.load(std::memory_order_relaxed)) return;
    bool localStatus = false;
    bool playerSpeakers = false;
    bool muted = false;
    bool deafened = false;
    {
        std::scoped_lock lock(state.mutex);
        if (!state.started) return;
        localStatus = state.overlayLocalStatus;
        playerSpeakers = state.overlayPlayerSpeakers;
        muted = state.settings.muted;
        deafened = state.settings.deafened;
    }
    if (!localStatus && !playerSpeakers) return;
    const api::Status status = api::GetStatus();
    if (status.state != api::State::InRoom) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (playerSpeakers && std::any_of(status.peers.begin(), status.peers.end(),
                                      [](const api::PeerStatus& peer) { return peer.connected; })) {
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f,
                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        if (ImGui::Begin("##VoiceChatSpeakers", nullptr, kFlags)) {
            DrawSpeakerLines(status);
        }
        ImGui::End();
    }
    if (localStatus) {
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f,
                                       viewport->WorkPos.y + viewport->WorkSize.y - 12.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        if (ImGui::Begin("##VoiceChatLocalStatus", nullptr, kFlags)) {
            DrawLocalStatusLine(status, muted, deafened);
        }
        ImGui::End();
    }
}

bool HeadsetBadgeWanted() noexcept {
    auto& state = Get();
    if (!state.enabled.load(std::memory_order_relaxed)) return false;
    bool localStatus = false;
    bool playerSpeakers = false;
    {
        std::scoped_lock lock(state.mutex);
        if (!state.started) return false;
        localStatus = state.overlayLocalStatus;
        playerSpeakers = state.overlayPlayerSpeakers;
        if (localStatus && std::chrono::steady_clock::now() - state.localStateChangedAt < kBadgeLocalStateSeconds) {
            return true;
        }
    }
    if (!localStatus && !playerSpeakers) return false;
    const api::Status status = api::GetStatus();
    if (status.state != api::State::InRoom) return false;
    if (localStatus && status.transmitting) return true;
    return playerSpeakers && std::any_of(status.peers.begin(), status.peers.end(), [](const api::PeerStatus& peer) {
               return peer.connected && peer.speaking;
           });
}

void DrawHeadsetBadge() noexcept {
    auto& state = Get();
    bool localStatus = false;
    bool playerSpeakers = false;
    bool muted = false;
    bool deafened = false;
    {
        std::scoped_lock lock(state.mutex);
        localStatus = state.overlayLocalStatus;
        playerSpeakers = state.overlayPlayerSpeakers;
        muted = state.settings.muted;
        deafened = state.settings.deafened;
    }
    const api::Status status = api::GetStatus();
    if (localStatus) DrawLocalStatusLine(status, muted, deafened);
    if (!playerSpeakers) return;
    int shown = 0;
    for (const auto& peer : status.peers) {
        if (!peer.connected || !peer.speaking) continue;
        if (++shown > 4) break;
        ImGui::TextColored(kSpeakingColor, "> %s", peer.displayName.c_str());
    }
}

} // namespace VoiceChat
