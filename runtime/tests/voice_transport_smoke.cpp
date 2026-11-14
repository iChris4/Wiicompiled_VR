// SPDX-License-Identifier: GPL-3.0-or-later
// Two voice peers in one process, introduced by hand the way the signaling Worker relays them:
// offer and answer as complete MKWVC-ICE-1 bundles, then voice packets both ways over the data
// channel. Exercises libdatachannel, its DTLS on mbed TLS and SCTP, with host candidates only
// (no STUN server), so it needs a network interface but no internet.

#include "IcePeerTransport.hpp"

#include "voicechat/voice_protocol.h"

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

namespace protocol = mkw::voicechat::protocol;
using Clock = std::chrono::steady_clock;

std::string Bundle(mkwvc::IcePeerTransport& transport) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    std::optional<mkwvc::IceDescriptionSignal> description;
    std::vector<mkwvc::IceCandidateSignal> candidates;
    while (Clock::now() < deadline) {
        if (auto taken = transport.takeLocalDescription()) description = std::move(taken);
        auto more = transport.takeLocalCandidates();
        candidates.insert(candidates.end(), more.begin(), more.end());
        if (description && transport.gatheringComplete()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!description) return {};
    protocol::IceBundle bundle;
    bundle.type = description->type;
    bundle.sdp = description->sdp;
    for (const auto& candidate : candidates) bundle.candidates.push_back({candidate.candidate, candidate.mid});
    return protocol::EncodeIceBundle(bundle);
}

bool Apply(mkwvc::IcePeerTransport& transport, const std::string& text) {
    const auto bundle = protocol::DecodeIceBundle(text);
    if (!bundle) return false;
    transport.setRemoteDescription(bundle->sdp, bundle->type);
    for (const auto& candidate : bundle->candidates) transport.addRemoteCandidate(candidate.candidate, candidate.mid);
    return true;
}

bool SendAndReceive(mkwvc::IcePeerTransport& from, mkwvc::IcePeerTransport& to, std::uint32_t sequence) {
    std::array<std::byte, protocol::kPacketHeaderBytes + 4> packet{};
    protocol::PacketHeader header;
    header.streamId = 42;
    header.sequence = sequence;
    protocol::WritePacketHeader(std::span<std::byte, protocol::kPacketHeaderBytes>(packet.data(), 18), header);
    std::memcpy(packet.data() + 18, "opus", 4);
    if (!from.send(packet)) return false;
    std::array<std::byte, protocol::kMaxVoicePacketBytes> received{};
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        const auto size = to.receive(received);
        if (size == 0) continue;
        const auto span = std::span<const std::byte>(received.data(), size);
        return protocol::CheckPacket(span) == protocol::PacketCheck::Valid &&
               protocol::ReadPacketHeader(span).sequence == sequence;
    }
    return false;
}

} // namespace

int main() {
    mkwvc::IcePeerTransport offerer({}, false);
    mkwvc::IcePeerTransport answerer({}, false);
    offerer.beginOffer();
    const std::string offer = Bundle(offerer);
    if (offer.empty() || !Apply(answerer, offer)) {
        std::cerr << "FAILED: no offer bundle\n";
        return 1;
    }
    const std::string answer = Bundle(answerer);
    if (answer.empty() || !Apply(offerer, answer)) {
        std::cerr << "FAILED: no answer bundle\n";
        return 1;
    }
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline && !(offerer.connected() && answerer.connected())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!offerer.connected() || !answerer.connected()) {
        std::cerr << "FAILED: data channel did not open\n";
        return 1;
    }
    if (!SendAndReceive(offerer, answerer, 7) || !SendAndReceive(answerer, offerer, 8)) {
        std::cerr << "FAILED: voice packet lost\n";
        return 1;
    }
    std::cout << "voice transport smoke test passed\n";
    return 0;
}
