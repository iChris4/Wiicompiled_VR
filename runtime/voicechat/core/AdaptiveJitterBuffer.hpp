// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#pragma once

#include "VoiceFormat.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>

namespace mkwvc {

enum class JitterBufferPushResult {
    Accepted,
    AcceptedReordered,
    Duplicate,
    Late,
    TooFarAhead
};

enum class JitterBufferFrameKind {
    Packet,
    Missing,
    Hold
};

struct JitterBufferFrame {
    JitterBufferFrameKind kind=JitterBufferFrameKind::Missing;
    std::uint32_t sequence=0;
    std::array<std::byte,VoiceFormat::MaxOpusPacketBytes> payload{};
    std::size_t payloadSize=0;
    std::array<std::byte,VoiceFormat::MaxOpusPacketBytes> fecPayload{};
    std::size_t fecPayloadSize=0;
};

struct AdaptiveJitterBufferStats {
    float estimatedJitterMs=0.0f;
    std::uint32_t targetDelayMs=0;
    std::uint32_t currentDelayMs=0;
    std::uint32_t queuedPackets=0;
};

class AdaptiveJitterBuffer {
public:
    JitterBufferPushResult push(std::uint32_t sequence,std::span<const std::byte> payload);
    std::optional<JitterBufferFrame> popReady();
    AdaptiveJitterBufferStats stats() const;
    void reset(bool preserveAdaptation=false);

private:
    using Clock=std::chrono::steady_clock;

    struct QueuedPacket {
        std::array<std::byte,VoiceFormat::MaxOpusPacketBytes> payload{};
        std::size_t size=0;
    };

    void initialize(std::uint32_t sequence,Clock::time_point now);
    void updateJitter(std::uint32_t sequence,Clock::time_point now);
    void updateTarget(Clock::time_point now,bool allowDecrease);
    static std::int32_t sequenceDistance(std::uint32_t from,std::uint32_t to);

    std::unordered_map<std::uint32_t,QueuedPacket> packets_;
    std::uint32_t expectedSequence_=0;
    std::uint32_t highestSequence_=0;
    std::uint32_t lastArrivalSequence_=0;
    Clock::time_point lastArrival_{};
    Clock::time_point startupDeadline_{};
    Clock::time_point nextPlayout_{};
    Clock::time_point lastTargetChange_{};
    Clock::time_point lastDelayExpansion_{};
    float estimatedJitterMs_=0.0f;
    std::uint32_t targetDelayFrames_=2;
    std::uint32_t currentDelayFrames_=2;
    std::uint32_t lateBoostFrames_=0;
    std::uint32_t stablePackets_=0;
    std::uint32_t consecutiveMissing_=0;
    bool initialized_=false;
    bool haveArrivalReference_=false;
    bool playoutStarted_=false;
    bool suspended_=false;
};

}
