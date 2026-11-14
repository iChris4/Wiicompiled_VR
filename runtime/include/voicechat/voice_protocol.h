// SPDX-License-Identifier: GPL-3.0-or-later
//
// The wire format of MKW-VoiceChat (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0), which
// this client speaks so its players and ours share Retro Rewind voice rooms. Everything here is a
// pure function over bytes or text: no sockets, no threads, no exceptions escape.
//
// Voice packets travel over a WebRTC data channel and carry one 20 ms Opus frame each:
//   [0..3]  "MKWV"
//   [4]     packet version (3)
//   [5]     flags: 0x01 sender muted, 0x02 sender deafened
//   [6..13] stream id, big-endian u64 (random per sender start; a new id restarts decoding)
//   [14..17] sequence, big-endian u32
//   [18..]  Opus payload (48 kHz mono, at most 1275 bytes)
//
// Signaling is text over one WebSocket to MKW-VoiceChat's signaling Worker. A client in a Retro
// Rewind room sends RR_DEV_ADMIT, the Worker resolves the room from the public RWFC roster and
// introduces every voice member to every other one (RR_DEV_PEER_INFO + PEER_READY); the members
// then exchange one complete ICE bundle each way through SIGNAL.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mkw::voicechat::protocol {

inline constexpr std::uint8_t kPacketVersion = 3;
inline constexpr std::size_t kPacketHeaderBytes = 18;
inline constexpr std::size_t kMaxOpusPacketBytes = 1275;
inline constexpr std::size_t kMaxVoicePacketBytes = kPacketHeaderBytes + kMaxOpusPacketBytes;
inline constexpr std::uint8_t kFlagMuted = 0x01;
inline constexpr std::uint8_t kFlagDeafened = 0x02;

inline constexpr std::uint32_t kSampleRate = 48000;
inline constexpr std::uint32_t kFrameDurationMs = 20;
inline constexpr std::size_t kFrameSamples = 960;

inline constexpr std::string_view kDataChannelLabel = "mkw-voice";
inline constexpr std::string_view kDataChannelProtocol = "mkwvc-opus-v1";
inline constexpr std::uint32_t kDataChannelLifetimeMs = 100;
inline constexpr std::size_t kMaxRoomMembers = 12;
inline constexpr std::string_view kDefaultStunServer = "stun:stun.l.google.com:19302";

struct PacketHeader {
    std::uint8_t flags = 0;
    std::uint64_t streamId = 0;
    std::uint32_t sequence = 0;
};

void WritePacketHeader(std::span<std::byte, kPacketHeaderBytes> out, const PacketHeader& header) noexcept;

enum class PacketCheck : std::uint8_t {
    Valid,
    NotVoice,      // too short, too long or no "MKWV" magic
    WrongVersion,  // MKW-VoiceChat packet of another version: the sender needs another client
};

// A valid packet has the magic, our version and at least one payload byte.
PacketCheck CheckPacket(std::span<const std::byte> packet) noexcept;
// Precondition: CheckPacket(packet) == Valid.
PacketHeader ReadPacketHeader(std::span<const std::byte> packet) noexcept;

struct IceCandidate {
    std::string candidate;
    std::string mid;
};

struct IceBundle {
    std::string type;  // "offer" or "answer"
    std::string sdp;
    std::vector<IceCandidate> candidates;
};

// "MKWVC-ICE-1\nTYPE <type>\nSDP <base64>\nCAND <base64 mid> <base64 candidate>\n..."
std::string EncodeIceBundle(const IceBundle& bundle);
std::optional<IceBundle> DecodeIceBundle(std::string_view text);

std::string EncodeHex(std::string_view value);
// Empty on odd length or a non-hex digit.
std::string DecodeHex(std::string_view value);

// RWFC profile ids travel as decimal uint32 text.
bool IsProfileIdText(std::string_view value) noexcept;
// A member id is 32 hex digits (the Worker hashes it per connection and room).
bool IsMemberId(std::string_view value) noexcept;

// "RR_DEV_ADMIT <pid> PIDS=<sorted unique pids incl. own, at most 12>". The Worker resolves the
// room from the public roster by <pid>; PIDS= lets it fall back to a voice room that already
// holds one of the local RKNet room's players while the roster has not caught up.
std::string BuildDevAdmit(std::uint32_t profileId, std::span<const std::uint32_t> roomProfileIds);
// "SIGNAL <member>\n<bundle>"; empty when the member id is malformed.
std::string BuildSignal(std::string_view targetMemberId, std::string_view bundle);

struct RoomPlayer {
    std::uint32_t profileId = 0;
    std::string displayName;
    std::string friendCode;
    bool voiceChat = false;
    bool openHost = false;
};

struct Admission {
    std::string memberId;        // ours in this voice room
    std::string roomInstanceId;  // SHA-256 of the RWFC room id and creation time
    std::string roomId;
    std::string created;
    std::vector<RoomPlayer> players;
};

// The payload after "RR_DEV_ADMITTED ". Rejected unless it lists `ownProfileId`.
std::optional<Admission> ParseAdmission(std::string_view payload, std::uint32_t ownProfileId);

struct PeerInfo {
    std::string memberId;
    std::uint32_t profileId = 0;
    std::string country;
    std::string displayName;
    std::string friendCode;
    bool openHost = false;
};

// The payload after "RR_DEV_PEER_INFO\n". Rejected for another room instance.
std::optional<PeerInfo> ParsePeerInfo(std::string_view payload, std::string_view roomInstanceId);

// The payload after "ICE_SERVERS\n": one URL a line; the default STUN server when empty.
std::vector<std::string> ParseIceServers(std::string_view payload);

enum class MessageKind : std::uint8_t {
    Unknown,
    IceServers,
    DevAdmitted,
    DevAdmitFailed,
    DevPeerInfo,
    PeerReady,
    Signal,
    PeerLeft,
    Error,
};

struct ServerMessage {
    MessageKind kind = MessageKind::Unknown;
    std::string payload;
    std::string memberId;  // PeerReady, Signal (sender) and PeerLeft
};

ServerMessage ParseServerMessage(std::string_view text);

} // namespace mkw::voicechat::protocol
