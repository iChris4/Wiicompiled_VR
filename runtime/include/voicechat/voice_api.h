// SPDX-License-Identifier: GPL-3.0-or-later
//
// Room voice chat, as the runtime sees it. The engine (runtime/voicechat) speaks MKW-VoiceChat's
// protocol, so players of both clients in the same Retro Rewind room hear each other; see
// docs/voice-chat.md for the design and its privacy limits.
//
// Threads:
//  * Start/Shutdown/ApplySettings and the UI calls: the host thread that owns the settings
//    (the game thread, from the overlay).
//  * Observe*: the guest thread running the socket HLE. They copy the bytes and return.
//  * Post*/Wants*: the game thread, once a frame at most. Wants* are a single atomic load.
//  * Everything that touches the network or an audio device runs on the engine's own threads,
//    never on a guest fiber (Android aborts on JNI from a fiber, and a blocking connect would
//    stall the frame).
//
// Without MKW_ENABLE_VOICECHAT every call is a no-op and Available() is false.
//
// Plain C++17 types only: the runtime includes this header, the engine is C++23.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mkw::voicechat {

enum class TalkMode : std::uint8_t {
    VoiceActivation,  // transmit while the processed microphone level is over the threshold
    OpenMic,          // transmit continuously
    PushToTalk,       // transmit while the push-to-talk input is held
};

struct Settings {
    bool enabled = false;
    std::string signalingUrl;       // empty: the built-in default
    std::string caCertificateFile;  // PEM bundle for the signaling TLS (Android/Linux)
    std::string inputDevice;        // empty: system default
    std::string outputDevice;
    TalkMode talkMode = TalkMode::VoiceActivation;
    int voiceActivationThreshold = 25;  // 1..100
    bool muted = false;
    bool deafened = false;
    float microphoneGain = 1.0f;  // 0.25..5
    float playbackVolume = 1.0f;  // 0..3
    bool normalization = true;
    bool noiseSuppression = true;  // RNNoise
    int noiseSuppressionStrength = 50;
    bool noiseGate = false;
    int noiseGateThreshold = 25;
    float microphoneBoost = 1.0f;
    bool compressor = false;
    int compressorStrength = 50;
    // Ask the platform for its voice-call capture path (Android: AAudio's voice-communication
    // preset, with the system's echo canceller and noise suppressor).
    bool platformVoiceProcessing = false;
    bool muteEveryone = false;
    bool muteOnlyFriends = false;
    bool muteEveryoneButFriends = false;
    bool muteTeammates = false;
    bool muteEveryoneButTeammates = false;
    bool muteNewPlayers = false;
    // In races, place each player's voice at their kart (PostSpatialSnapshot).
    bool positional = true;
    std::vector<std::pair<std::uint32_t, float>> playerVolumes;  // profile id -> 0..3
};

bool Available() noexcept;
const char* DefaultSignalingUrl() noexcept;

void Start(const Settings& settings) noexcept;
void Shutdown() noexcept;
void ApplySettings(const Settings& settings) noexcept;

// Socket HLE hooks. Only TCP sockets to the GPCM port are parsed; anything else returns at once.
// ObserveTcpConnect gives the port a connect aims at whatever it returned: the game's GameSpy
// sockets connect nonblocking, and the HLE records a peer port only for a connect that succeeds at
// once, so the send/receive hooks see port 0 for them.
void ObserveTcpConnect(std::uint32_t socket, std::uint16_t peerPort) noexcept;
void ObserveTcpSend(std::uint32_t socket, std::uint16_t peerPort, const std::uint8_t* data, std::size_t size) noexcept;
void ObserveTcpReceive(std::uint32_t socket, std::uint16_t peerPort, const std::uint8_t* data,
                       std::size_t size) noexcept;
void ObserveSocketClosed(std::uint32_t socket) noexcept;

// The online room as the game sees it, read from guest memory on the game thread.
struct RoomSnapshot {
    bool inRoom = false;
    std::uint32_t roomKey = 0;  // changes with the RKNet group, i.e. a different room
    std::vector<std::uint32_t> roomProfileIds;
    std::vector<std::uint32_t> friendProfileIds;
    bool teamMode = false;
    std::vector<std::uint32_t> teamProfileIds;      // players whose team is known
    std::vector<std::uint32_t> teammateProfileIds;  // ... and on ours
};

// The online login's profile id, 0 while offline. One atomic load.
std::uint32_t CurrentProfileId() noexcept;

// True while voice chat is enabled and a GPCM login is known: the room is worth reading.
bool WantsRoomSnapshot() noexcept;
void PostRoomSnapshot(const RoomSnapshot& snapshot) noexcept;

// Where each player's voice comes from, relative to the listener's head: metres, +x right, +y up,
// -z ahead. Posted during races only; an entry missing from a fresh snapshot plays centred.
struct SpatialEntry {
    std::uint32_t profileId = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
};
struct SpatialSnapshot {
    std::vector<SpatialEntry> entries;
};

// True while positional voice could be heard: enabled, positional, and a peer connected.
bool WantsSpatialSnapshot() noexcept;
void PostSpatialSnapshot(const SpatialSnapshot& snapshot) noexcept;
// Drop positions at once (race over); voices play centred again.
void ClearSpatialSnapshot() noexcept;

// Push-to-talk input, polled by the host each frame.
void SetPushToTalkHeld(bool held) noexcept;

enum class State : std::uint8_t {
    Unavailable,  // built without voice chat
    Disabled,
    Offline,       // no online login seen
    NotInRoom,     // logged in, not in a room with others
    Connecting,    // signaling WebSocket opening
    Admitting,     // RR_DEV_ADMIT sent
    InRoom,        // admitted; peers connect as they come
    Retrying,      // failed; next attempt scheduled
};

struct PeerStatus {
    std::uint32_t profileId = 0;
    std::string displayName;
    std::string friendCode;
    bool connected = false;  // data channel open
    bool receiving = false;  // their voice packets arrived in the last second
    bool speaking = false;
    bool remoteMuted = false;
    bool remoteDeafened = false;
    bool isFriend = false;
    bool isTeammate = false;
    bool policyMuted = false;  // silenced by a mute preset
    float volume = 1.0f;       // the player's own setting
    bool protocolMismatch = false;
};

struct RoomPlayerStatus {
    std::uint32_t profileId = 0;
    std::string displayName;
    std::string friendCode;
    bool voiceChat = false;
};

struct Status {
    State state = State::Unavailable;
    std::string message;
    std::uint32_t profileId = 0;
    bool microphoneOpen = false;   // capture device running
    bool transmitting = false;     // a frame went out unmuted in the last ~300 ms
    std::uint32_t microphonePeak = 0;  // 0..32767, processed
    std::string audioError;
    std::vector<PeerStatus> peers;
    std::vector<RoomPlayerStatus> roomPlayers;
    std::vector<std::string> inputDevices;
    std::vector<std::string> outputDevices;
    bool microphoneTest = false;
    // The voice room, once admitted: our name as the room lists it, and identifiers for the
    // menu's debug section.
    std::string displayName;
    std::string roomId;
    std::string memberId;
    std::size_t iceServers = 0;
};

Status GetStatus();

void RefreshDevices() noexcept;
void SetMicrophoneTest(bool enabled) noexcept;
void PlayTestTone() noexcept;

} // namespace mkw::voicechat
