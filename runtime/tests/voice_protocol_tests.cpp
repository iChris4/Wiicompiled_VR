// SPDX-License-Identifier: GPL-3.0-or-later
// MKW-VoiceChat wire compatibility: the packet header, the ICE bundle text and the signaling
// messages, checked byte for byte against the format MKW-VoiceChat 0.14.8 produces.

#include "voicechat/voice_protocol.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace protocol = mkw::voicechat::protocol;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

std::vector<std::byte> Bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int value : values) out.push_back(static_cast<std::byte>(value));
    return out;
}

void TestPacketHeader() {
    std::array<std::byte, protocol::kPacketHeaderBytes + 3> packet{};
    protocol::PacketHeader header;
    header.flags = protocol::kFlagMuted | protocol::kFlagDeafened;
    header.streamId = 0x0102030405060708ull;
    header.sequence = 0xA1B2C3D4u;
    protocol::WritePacketHeader(std::span<std::byte, protocol::kPacketHeaderBytes>(packet.data(), 18), header);
    // MKW-VoiceChat's layout: "MKWV", version 3, flags, BE stream id, BE sequence.
    const auto expected = Bytes({'M', 'K', 'W', 'V', 3, 0x03, 1, 2, 3, 4, 5, 6, 7, 8, 0xA1, 0xB2, 0xC3, 0xD4});
    Check(std::memcmp(packet.data(), expected.data(), expected.size()) == 0, "header bytes match MKW-VoiceChat");

    Check(protocol::CheckPacket(packet) == protocol::PacketCheck::Valid, "header plus payload is valid");
    const auto read = protocol::ReadPacketHeader(packet);
    Check(read.flags == header.flags && read.streamId == header.streamId && read.sequence == header.sequence,
          "header round trip");

    Check(protocol::CheckPacket(std::span<const std::byte>(packet.data(), 18)) == protocol::PacketCheck::NotVoice,
          "header without payload is rejected");
    auto other = packet;
    other[4] = std::byte{4};
    Check(protocol::CheckPacket(other) == protocol::PacketCheck::WrongVersion, "other version is reported");
    other = packet;
    other[0] = std::byte{'X'};
    Check(protocol::CheckPacket(other) == protocol::PacketCheck::NotVoice, "no magic is not voice");
    std::vector<std::byte> tooLong(protocol::kMaxVoicePacketBytes + 1);
    std::memcpy(tooLong.data(), packet.data(), 18);
    Check(protocol::CheckPacket(tooLong) == protocol::PacketCheck::NotVoice, "oversized packet is rejected");
}

void TestIceBundle() {
    protocol::IceBundle bundle;
    bundle.type = "offer";
    bundle.sdp = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n";
    bundle.candidates.push_back({"candidate:1 1 UDP 2122317823 192.168.1.2 50000 typ host", "0"});
    const std::string text = protocol::EncodeIceBundle(bundle);
    // The same text MKW-VoiceChat's encodeIceSignal writes for this bundle.
    const std::string expected =
        "MKWVC-ICE-1\nTYPE offer\nSDP dj0wDQpvPS0gMSAyIElOIElQNCAxMjcuMC4wLjENCg==\n"
        "CAND MA== Y2FuZGlkYXRlOjEgMSBVRFAgMjEyMjMxNzgyMyAxOTIuMTY4LjEuMiA1MDAwMCB0eXAgaG9zdA==\n";
    Check(text == expected, "ICE bundle text matches MKW-VoiceChat");
    const auto decoded = protocol::DecodeIceBundle(text);
    Check(decoded && decoded->type == "offer" && decoded->sdp == bundle.sdp && decoded->candidates.size() == 1 &&
              decoded->candidates[0].candidate == bundle.candidates[0].candidate && decoded->candidates[0].mid == "0",
          "ICE bundle round trip");
    Check(!protocol::DecodeIceBundle("MKWVC-ICE-2\nTYPE offer\nSDP AA==\n"), "unknown bundle tag rejected");
    Check(!protocol::DecodeIceBundle("MKWVC-ICE-1\nTYPE pranswer\nSDP AA==\n"), "bad type rejected");
    Check(!protocol::DecodeIceBundle("MKWVC-ICE-1\nTYPE answer\nSDP A\n"), "bad base64 rejected");
    Check(!protocol::DecodeIceBundle("MKWVC-ICE-1\nTYPE answer\n"), "missing SDP rejected");
}

void TestHexAndIds() {
    Check(protocol::EncodeHex("Mario") == "4D6172696F", "hex encode");
    Check(protocol::DecodeHex("4d6172696F") == "Mario", "hex decode, both cases");
    Check(protocol::DecodeHex("4D6").empty() && protocol::DecodeHex("ZZ").empty(), "bad hex is empty");
    Check(protocol::IsProfileIdText("600123456") && !protocol::IsProfileIdText("12345678901") &&
              !protocol::IsProfileIdText("") && !protocol::IsProfileIdText("12a"),
          "profile id text");
    Check(protocol::IsMemberId("0123456789abcdefABCDEF0123456789") && !protocol::IsMemberId("0123") &&
              !protocol::IsMemberId("0123456789abcdefABCDEF012345678G"),
          "member id shape");
}

void TestAdmitCommand() {
    const std::array<std::uint32_t, 4> pids = {900000002u, 100000001u, 0u, 900000002u};
    // String-sorted, unique, own pid included, zero dropped.
    Check(protocol::BuildDevAdmit(500000000u, pids) == "RR_DEV_ADMIT 500000000 PIDS=100000001,500000000,900000002",
          "admit command shape");
    std::vector<std::uint32_t> many;
    for (std::uint32_t i = 0; i < 14; ++i) many.push_back(100000000u + i);
    const std::string capped = protocol::BuildDevAdmit(999999999u, many);
    Check(std::count(capped.begin(), capped.end(), ',') == 11, "at most 12 pids");
    Check(capped.find(",999999999") != std::string::npos, "own pid survives the cap");
    Check(protocol::BuildSignal("0123456789abcdef0123456789ABCDEF", "x") == "SIGNAL 0123456789ABCDEF0123456789ABCDEF\nx",
          "signal upper-cases the target");
    Check(protocol::BuildSignal("nope", "x").empty(), "signal rejects a bad target");
}

void TestAdmission() {
    const std::string member = "0123456789ABCDEF0123456789ABCDEF";
    const std::string room(64, 'A');
    const std::string payload = member + "\n" + room + "\nroom-1\n2026-11-12T10:00:00Z\n2\n" +
                                "500000000\t1\t" + protocol::EncodeHex("Chris") + "\t1234-5678-9012\t0\n" +
                                "600000000\t0\t" + protocol::EncodeHex(std::string("matt\0ias", 8)) + "\t\t1";
    const auto admission = protocol::ParseAdmission(payload, 500000000u);
    Check(admission.has_value(), "admission parses");
    if (admission) {
        Check(admission->memberId == member && admission->roomInstanceId == room && admission->roomId == "room-1",
              "admission identity");
        Check(admission->players.size() == 2 && admission->players[0].voiceChat &&
                  admission->players[0].friendCode == "1234-5678-9012" && admission->players[1].openHost,
              "admission players");
        Check(admission->players[1].displayName == "mattias", "NUL stripped from Mii names");
    }
    Check(!protocol::ParseAdmission(payload, 700000000u), "admission without us is rejected");
    Check(!protocol::ParseAdmission(member + "\nshort\nroom\nc\n0", 1u), "bad room instance rejected");
    Check(!protocol::ParseAdmission(member + "\n" + room + "\nroom\nc\n3\n1\t1\tAA", 1u), "short player list rejected");
}

void TestPeerInfoAndMessages() {
    const std::string member = "FEDCBA9876543210FEDCBA9876543210";
    const std::string room(64, 'B');
    const std::string info = member + "\n600000000\n" + room + "\nFR\n" + protocol::EncodeHex("Luigi") + "\n1111\n1";
    const auto peer = protocol::ParsePeerInfo(info, room);
    Check(peer && peer->profileId == 600000000u && peer->displayName == "Luigi" && peer->country == "FR" &&
              peer->friendCode == "1111" && peer->openHost,
          "peer info parses");
    Check(!protocol::ParsePeerInfo(info, std::string(64, 'C')), "peer info for another room rejected");

    auto message = protocol::ParseServerMessage("RR_DEV_PEER_INFO\n" + info);
    Check(message.kind == protocol::MessageKind::DevPeerInfo && message.payload == info, "peer info message");
    message = protocol::ParseServerMessage("PEER_READY " + member);
    Check(message.kind == protocol::MessageKind::PeerReady && message.memberId == member, "peer ready");
    message = protocol::ParseServerMessage("SIGNAL " + member + "\nMKWVC-ICE-1\nTYPE offer");
    Check(message.kind == protocol::MessageKind::Signal && message.memberId == member &&
              message.payload == "MKWVC-ICE-1\nTYPE offer",
          "signal from a peer");
    message = protocol::ParseServerMessage("PEER_LEFT " + member);
    Check(message.kind == protocol::MessageKind::PeerLeft && message.memberId == member, "peer left");
    message = protocol::ParseServerMessage("PEER_LEFT nonsense");
    Check(message.kind == protocol::MessageKind::Unknown, "peer left with a bad id is ignored");
    message = protocol::ParseServerMessage("RR_DEV_ADMIT_FAIL PID is absent from public RR roster");
    Check(message.kind == protocol::MessageKind::DevAdmitFailed && message.payload == "PID is absent from public RR roster",
          "admission failure");
    message = protocol::ParseServerMessage("ICE_SERVERS\nstun:a\nturn:b");
    Check(message.kind == protocol::MessageKind::IceServers, "ice servers message");
    const auto servers = protocol::ParseIceServers(message.payload);
    Check(servers.size() == 2 && servers[1] == "turn:b", "ice server list");
    Check(protocol::ParseIceServers("").size() == 1, "default STUN when the list is empty");
    Check(protocol::ParseServerMessage("RR_ONLINE_COUNT 3").kind == protocol::MessageKind::Unknown,
          "presence traffic is ignored");
}

} // namespace

int main() {
    TestPacketHeader();
    TestIceBundle();
    TestHexAndIds();
    TestAdmitCommand();
    TestAdmission();
    TestPeerInfoAndMessages();
    if (g_failures == 0) std::cout << "voice protocol tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
