// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).
// Trimmed to the Retro Rewind room path this client uses (RR_DEV_ADMIT, SIGNAL, ALIVE); the
// manual rooms, the global presence list and the unfinished authenticated path are left out.
// Leaving a voice room is closing the connection: the Worker then tells the others PEER_LEFT.
// Server messages are parsed by the shared, unit-tested protocol code.

#pragma once

#include "voicechat/voice_protocol.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mkwvc {

struct SignalingEvent {
    enum class Type { Open, Message, TransportError, Closed };
    Type type = Type::Message;
    mkw::voicechat::protocol::ServerMessage message;  // Type::Message
    std::string error;                                // Type::TransportError
};

class SignalingClient {
public:
    // `caCertificateFile`: PEM bundle the WebSocket's TLS verifies against (empty: none passed;
    // libdatachannel skips verification on Windows regardless, see docs/voice-chat.md).
    SignalingClient(std::string serverUrl, std::string caCertificateFile);
    ~SignalingClient();

    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    void admit(std::uint32_t profileId, std::span<const std::uint32_t> roomProfileIds);
    void sendSignal(const std::string& memberId, const std::string& bundle);
    bool open() const;
    // Heartbeat while in a voice room; call regularly.
    void service();
    std::vector<SignalingEvent> takeEvents();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mkwvc
