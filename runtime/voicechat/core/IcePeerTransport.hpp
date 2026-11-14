// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#pragma once

#include "VoiceTransport.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mkwvc {

struct IceDescriptionSignal {
    std::string sdp;
    std::string type;
};

struct IceCandidateSignal {
    std::string candidate;
    std::string mid;
};

class IcePeerTransport final : public VoiceTransport {
public:
    explicit IcePeerTransport(std::vector<std::string> iceServers,bool forceRelay=false);
    ~IcePeerTransport();

    IcePeerTransport(const IcePeerTransport&)=delete;
    IcePeerTransport& operator=(const IcePeerTransport&)=delete;

    void beginOffer();
    void setRemoteDescription(std::string sdp,std::string type);
    void addRemoteCandidate(std::string candidate,std::string mid);

    std::optional<IceDescriptionSignal> takeLocalDescription();
    std::vector<IceCandidateSignal> takeLocalCandidates();

    bool gatheringComplete() const;
    bool connected() const;
    bool usingRelay() const;
    std::optional<std::uint32_t> rttMilliseconds() const;
    std::string remoteAddress() const;

    bool send(std::span<const std::byte> packet) override;
    std::size_t receive(std::span<std::byte> packet) override;
    std::size_t tryReceive(std::span<std::byte> packet);
    std::string localAddress() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
