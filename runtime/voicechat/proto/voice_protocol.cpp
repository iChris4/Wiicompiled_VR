// SPDX-License-Identifier: GPL-3.0-or-later
//
// MKW-VoiceChat's wire format (see voice_protocol.h). The packet layout, the ICE bundle text and
// the signaling replies follow MKW-VoiceChat 0.14.8 (src/VoiceClient.cpp, src/IceSignal.cpp,
// signaling_worker/src/index.js), GPL-3.0.

#include "voicechat/voice_protocol.h"

#include <algorithm>
#include <charconv>

namespace mkw::voicechat::protocol {

namespace {

constexpr std::string_view kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kIceBundleTag = "MKWVC-ICE-1";

std::string Base64Encode(std::string_view input) {
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i < input.size()) {
        const std::size_t count = std::min<std::size_t>(3, input.size() - i);
        const auto a = static_cast<unsigned char>(input[i]);
        const auto b = count > 1 ? static_cast<unsigned char>(input[i + 1]) : 0u;
        const auto c = count > 2 ? static_cast<unsigned char>(input[i + 2]) : 0u;
        i += count;
        const unsigned value = (unsigned{a} << 16) | (unsigned{b} << 8) | unsigned{c};
        out.push_back(kBase64[(value >> 18) & 63]);
        out.push_back(kBase64[(value >> 12) & 63]);
        out.push_back(count > 1 ? kBase64[(value >> 6) & 63] : '=');
        out.push_back(count > 2 ? kBase64[value & 63] : '=');
    }
    return out;
}

int Base64Value(char ch) {
    const auto pos = kBase64.find(ch);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

std::optional<std::string> Base64Decode(std::string_view input) {
    if (input.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(input.size() / 4 * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const bool pad2 = input[i + 2] == '=';
        const bool pad3 = input[i + 3] == '=';
        if (pad2 && !pad3) {
            return std::nullopt;
        }
        const int a = Base64Value(input[i]);
        const int b = Base64Value(input[i + 1]);
        const int c = pad2 ? 0 : Base64Value(input[i + 2]);
        const int d = pad3 ? 0 : Base64Value(input[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || ((pad2 || pad3) && i + 4 != input.size())) {
            return std::nullopt;
        }
        const unsigned value = (unsigned(a) << 18) | (unsigned(b) << 12) | (unsigned(c) << 6) | unsigned(d);
        out.push_back(static_cast<char>((value >> 16) & 0xFF));
        if (!pad2) {
            out.push_back(static_cast<char>((value >> 8) & 0xFF));
        }
        if (!pad3) {
            out.push_back(static_cast<char>(value & 0xFF));
        }
    }
    return out;
}

std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (true) {
        const auto end = text.find('\n');
        lines.push_back(text.substr(0, end));
        if (end == std::string_view::npos) {
            return lines;
        }
        text.remove_prefix(end + 1);
    }
}

std::optional<std::uint32_t> ParseUint32(std::string_view text) {
    if (!IsProfileIdText(text)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 0xFFFFFFFFull) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

std::string_view Field(std::string_view line, std::size_t index) {
    for (std::size_t i = 0; i < index; ++i) {
        const auto tab = line.find('\t');
        if (tab == std::string_view::npos) {
            return {};
        }
        line.remove_prefix(tab + 1);
    }
    return line.substr(0, line.find('\t'));
}

std::size_t FieldCount(std::string_view line) {
    return static_cast<std::size_t>(std::count(line.begin(), line.end(), '\t')) + 1;
}

std::string NameOrDefault(std::string name) {
    // Mii names can hold a NUL ("matt\0ias"); the Worker passes the raw bytes through.
    name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());
    return name.empty() ? std::string("Player") : name;
}

} // namespace

void WritePacketHeader(std::span<std::byte, kPacketHeaderBytes> out, const PacketHeader& header) noexcept {
    out[0] = std::byte{'M'};
    out[1] = std::byte{'K'};
    out[2] = std::byte{'W'};
    out[3] = std::byte{'V'};
    out[4] = std::byte{kPacketVersion};
    out[5] = std::byte{header.flags};
    for (std::size_t i = 0; i < 8; ++i) {
        out[6 + i] = static_cast<std::byte>((header.streamId >> (56 - i * 8)) & 0xFF);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        out[14 + i] = static_cast<std::byte>((header.sequence >> (24 - i * 8)) & 0xFF);
    }
}

PacketCheck CheckPacket(std::span<const std::byte> packet) noexcept {
    if (packet.size() < 5 || packet[0] != std::byte{'M'} || packet[1] != std::byte{'K'} ||
        packet[2] != std::byte{'W'} || packet[3] != std::byte{'V'}) {
        return PacketCheck::NotVoice;
    }
    if (packet[4] != std::byte{kPacketVersion}) {
        return PacketCheck::WrongVersion;
    }
    if (packet.size() <= kPacketHeaderBytes || packet.size() > kMaxVoicePacketBytes) {
        return PacketCheck::NotVoice;
    }
    return PacketCheck::Valid;
}

PacketHeader ReadPacketHeader(std::span<const std::byte> packet) noexcept {
    PacketHeader header;
    header.flags = std::to_integer<std::uint8_t>(packet[5]);
    for (std::size_t i = 0; i < 8; ++i) {
        header.streamId = (header.streamId << 8) | std::to_integer<std::uint64_t>(packet[6 + i]);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        header.sequence = (header.sequence << 8) | std::to_integer<std::uint32_t>(packet[14 + i]);
    }
    return header;
}

std::string EncodeIceBundle(const IceBundle& bundle) {
    std::string out(kIceBundleTag);
    out += "\nTYPE " + bundle.type + "\nSDP " + Base64Encode(bundle.sdp) + "\n";
    for (const auto& candidate : bundle.candidates) {
        out += "CAND " + Base64Encode(candidate.mid) + " " + Base64Encode(candidate.candidate) + "\n";
    }
    return out;
}

std::optional<IceBundle> DecodeIceBundle(std::string_view text) {
    const auto lines = SplitLines(text);
    if (lines.empty() || lines[0] != kIceBundleTag) {
        return std::nullopt;
    }
    IceBundle bundle;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (line.starts_with("TYPE ")) {
            bundle.type = std::string(line.substr(5));
        } else if (line.starts_with("SDP ")) {
            auto sdp = Base64Decode(line.substr(4));
            if (!sdp) {
                return std::nullopt;
            }
            bundle.sdp = std::move(*sdp);
        } else if (line.starts_with("CAND ")) {
            const auto payload = line.substr(5);
            const auto split = payload.find(' ');
            if (split == std::string_view::npos) {
                return std::nullopt;
            }
            auto mid = Base64Decode(payload.substr(0, split));
            auto candidate = Base64Decode(payload.substr(split + 1));
            if (!mid || !candidate) {
                return std::nullopt;
            }
            bundle.candidates.push_back({std::move(*candidate), std::move(*mid)});
        }
    }
    if ((bundle.type != "offer" && bundle.type != "answer") || bundle.sdp.empty()) {
        return std::nullopt;
    }
    return bundle;
}

std::string EncodeHex(std::string_view value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 2);
    for (const unsigned char ch : value) {
        out.push_back(kHex[ch >> 4]);
        out.push_back(kHex[ch & 0x0F]);
    }
    return out;
}

std::string DecodeHex(std::string_view value) {
    if (value.size() % 2 != 0) {
        return {};
    }
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int hi = nibble(value[i]);
        const int lo = nibble(value[i + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

bool IsProfileIdText(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 10 &&
           std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

bool IsMemberId(std::string_view value) noexcept {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
           });
}

std::string BuildDevAdmit(std::uint32_t profileId, std::span<const std::uint32_t> roomProfileIds) {
    std::vector<std::uint32_t> pids;
    pids.reserve(roomProfileIds.size() + 1);
    for (const auto pid : roomProfileIds) {
        if (pid != 0) {
            pids.push_back(pid);
        }
    }
    pids.push_back(profileId);
    // The Worker compares decimal strings; MKW-VoiceChat sorts them as strings too.
    std::vector<std::string> text;
    text.reserve(pids.size());
    for (const auto pid : pids) {
        text.push_back(std::to_string(pid));
    }
    std::sort(text.begin(), text.end());
    text.erase(std::unique(text.begin(), text.end()), text.end());
    if (text.size() > kMaxRoomMembers) {
        // Keep our own pid if truncation would drop it.
        const std::string own = std::to_string(profileId);
        text.resize(kMaxRoomMembers);
        if (std::find(text.begin(), text.end(), own) == text.end()) {
            text.back() = own;
            std::sort(text.begin(), text.end());
        }
    }
    std::string command = "RR_DEV_ADMIT " + std::to_string(profileId) + " PIDS=";
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i > 0) {
            command.push_back(',');
        }
        command += text[i];
    }
    return command;
}

std::string BuildSignal(std::string_view targetMemberId, std::string_view bundle) {
    if (!IsMemberId(targetMemberId) || bundle.empty()) {
        return {};
    }
    std::string target(targetMemberId);
    std::transform(target.begin(), target.end(), target.begin(),
                   [](char ch) { return (ch >= 'a' && ch <= 'f') ? static_cast<char>(ch - 'a' + 'A') : ch; });
    return "SIGNAL " + target + "\n" + std::string(bundle);
}

std::optional<Admission> ParseAdmission(std::string_view payload, std::uint32_t ownProfileId) {
    const auto lines = SplitLines(payload);
    if (lines.size() < 5 || !IsMemberId(lines[0]) || lines[1].size() != 64 || lines[2].empty() ||
        lines[3].empty()) {
        return std::nullopt;
    }
    std::size_t count = 0;
    const auto parsed = std::from_chars(lines[4].data(), lines[4].data() + lines[4].size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != lines[4].data() + lines[4].size() || count > kMaxRoomMembers ||
        lines.size() < 5 + count) {
        return std::nullopt;
    }
    Admission admission;
    admission.memberId = std::string(lines[0]);
    admission.roomInstanceId = std::string(lines[1]);
    admission.roomId = std::string(lines[2]);
    admission.created = std::string(lines[3]);
    bool ownFound = false;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string_view line = lines[5 + i];
        if (FieldCount(line) < 3) {
            return std::nullopt;
        }
        const auto pid = ParseUint32(Field(line, 0));
        if (!pid) {
            return std::nullopt;
        }
        RoomPlayer player;
        player.profileId = *pid;
        player.voiceChat = Field(line, 1) == "1";
        player.displayName = NameOrDefault(DecodeHex(Field(line, 2)));
        if (FieldCount(line) > 3) {
            player.friendCode = std::string(Field(line, 3));
        }
        if (FieldCount(line) > 4) {
            player.openHost = Field(line, 4) == "1";
        }
        ownFound = ownFound || player.profileId == ownProfileId;
        admission.players.push_back(std::move(player));
    }
    if (!ownFound) {
        return std::nullopt;
    }
    return admission;
}

std::optional<PeerInfo> ParsePeerInfo(std::string_view payload, std::string_view roomInstanceId) {
    const auto lines = SplitLines(payload);
    if (lines.size() < 5 || !IsMemberId(lines[0]) || lines[2] != roomInstanceId) {
        return std::nullopt;
    }
    const auto pid = ParseUint32(lines[1]);
    if (!pid) {
        return std::nullopt;
    }
    PeerInfo info;
    info.memberId = std::string(lines[0]);
    info.profileId = *pid;
    info.country = std::string(lines[3]);
    info.displayName = NameOrDefault(DecodeHex(lines[4]));
    if (lines.size() > 5) {
        info.friendCode = std::string(lines[5]);
    }
    if (lines.size() > 6) {
        info.openHost = lines[6] == "1";
    }
    return info;
}

std::vector<std::string> ParseIceServers(std::string_view payload) {
    std::vector<std::string> servers;
    for (const auto line : SplitLines(payload)) {
        if (!line.empty()) {
            servers.emplace_back(line);
        }
    }
    if (servers.empty()) {
        servers.emplace_back(kDefaultStunServer);
    }
    return servers;
}

ServerMessage ParseServerMessage(std::string_view text) {
    ServerMessage message;
    const auto after = [&](std::string_view prefix) { return std::string(text.substr(prefix.size())); };
    if (text.starts_with("ICE_SERVERS\n")) {
        message.kind = MessageKind::IceServers;
        message.payload = after("ICE_SERVERS\n");
    } else if (text.starts_with("RR_DEV_ADMITTED ")) {
        message.kind = MessageKind::DevAdmitted;
        message.payload = after("RR_DEV_ADMITTED ");
    } else if (text.starts_with("RR_DEV_ADMIT_FAIL ")) {
        message.kind = MessageKind::DevAdmitFailed;
        message.payload = after("RR_DEV_ADMIT_FAIL ");
    } else if (text.starts_with("RR_DEV_PEER_INFO\n")) {
        message.kind = MessageKind::DevPeerInfo;
        message.payload = after("RR_DEV_PEER_INFO\n");
    } else if (text.starts_with("PEER_READY ")) {
        message.kind = MessageKind::PeerReady;
        message.memberId = after("PEER_READY ");
    } else if (text.starts_with("SIGNAL ")) {
        const auto split = text.find('\n');
        if (split != std::string_view::npos && split > 7) {
            message.kind = MessageKind::Signal;
            message.memberId = std::string(text.substr(7, split - 7));
            message.payload = std::string(text.substr(split + 1));
        }
    } else if (text.starts_with("PEER_LEFT ")) {
        message.kind = MessageKind::PeerLeft;
        message.memberId = after("PEER_LEFT ");
    } else if (text.starts_with("ERROR ")) {
        message.kind = MessageKind::Error;
        message.payload = after("ERROR ");
    }
    if ((message.kind == MessageKind::PeerReady || message.kind == MessageKind::Signal ||
         message.kind == MessageKind::PeerLeft) &&
        !IsMemberId(message.memberId)) {
        return {};
    }
    return message;
}

} // namespace mkw::voicechat::protocol
