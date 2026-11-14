// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#include "AdaptiveJitterBuffer.hpp"

#include <algorithm>
#include <cmath>

namespace mkwvc {

namespace {

constexpr std::uint32_t FrameMs=VoiceFormat::FrameDurationMs;
constexpr std::uint32_t MinDelayFrames=2;
constexpr std::uint32_t MaxDelayFrames=10;
constexpr std::uint32_t MaxRecoverableGap=10;
constexpr std::uint32_t MaxConcealmentFrames=10;
constexpr std::uint32_t StablePacketsPerDecay=100;
constexpr auto TargetDecayInterval=std::chrono::seconds(2);
constexpr auto DelayExpansionInterval=std::chrono::milliseconds(500);

}

std::int32_t AdaptiveJitterBuffer::sequenceDistance(std::uint32_t from,std::uint32_t to) {
    return static_cast<std::int32_t>(to-from);
}

void AdaptiveJitterBuffer::initialize(std::uint32_t sequence,Clock::time_point now) {
    expectedSequence_=sequence;
    highestSequence_=sequence;
    lastArrivalSequence_=sequence;
    lastArrival_=now;
    startupDeadline_=now+std::chrono::milliseconds(targetDelayFrames_*FrameMs);
    nextPlayout_=startupDeadline_;
    currentDelayFrames_=targetDelayFrames_;
    consecutiveMissing_=0;
    initialized_=true;
    haveArrivalReference_=true;
    playoutStarted_=false;
    suspended_=false;
    lastDelayExpansion_=now-DelayExpansionInterval;
    if(lastTargetChange_==Clock::time_point{}) lastTargetChange_=now;
}

void AdaptiveJitterBuffer::updateJitter(std::uint32_t sequence,Clock::time_point now) {
    if(!haveArrivalReference_) {
        lastArrivalSequence_=sequence;
        lastArrival_=now;
        haveArrivalReference_=true;
        return;
    }

    const auto sequenceDelta=sequenceDistance(lastArrivalSequence_,sequence);
    if(sequenceDelta<=0 || sequenceDelta>static_cast<std::int32_t>(MaxRecoverableGap)) return;

    const auto actualMs=std::chrono::duration<float,std::milli>(now-lastArrival_).count();
    const auto expectedMs=static_cast<float>(sequenceDelta*FrameMs);
    const auto variation=std::abs(actualMs-expectedMs);
    estimatedJitterMs_+=(variation-estimatedJitterMs_)/16.0f;
    lastArrivalSequence_=sequence;
    lastArrival_=now;
}

void AdaptiveJitterBuffer::updateTarget(Clock::time_point now,bool allowDecrease) {
    const auto jitterBudgetMs=estimatedJitterMs_*4.0f;
    const auto jitterFrames=static_cast<std::uint32_t>(std::ceil(jitterBudgetMs/static_cast<float>(FrameMs)));
    const auto desired=std::clamp(MinDelayFrames+jitterFrames+lateBoostFrames_,MinDelayFrames,MaxDelayFrames);

    if(desired>targetDelayFrames_) {
        targetDelayFrames_=desired;
        lastTargetChange_=now;
        return;
    }

    if(allowDecrease && desired<targetDelayFrames_ && now-lastTargetChange_>=TargetDecayInterval) {
        --targetDelayFrames_;
        lastTargetChange_=now;
    }
}

JitterBufferPushResult AdaptiveJitterBuffer::push(std::uint32_t sequence,std::span<const std::byte> payload) {
    if(payload.empty() || payload.size()>VoiceFormat::MaxOpusPacketBytes) return JitterBufferPushResult::TooFarAhead;

    const auto now=Clock::now();
    if(!initialized_) initialize(sequence,now);

    const auto distance=sequenceDistance(expectedSequence_,sequence);
    if(distance<0) {
        lateBoostFrames_=std::min<std::uint32_t>(lateBoostFrames_+1,4);
        stablePackets_=0;
        updateTarget(now,false);
        return JitterBufferPushResult::Late;
    }

    if(distance>static_cast<std::int32_t>(MaxRecoverableGap)) return JitterBufferPushResult::TooFarAhead;
    if(packets_.contains(sequence)) return JitterBufferPushResult::Duplicate;

    const bool reordered=sequenceDistance(sequence,highestSequence_)>0;
    if(sequenceDistance(highestSequence_,sequence)>0) highestSequence_=sequence;

    QueuedPacket queued;
    std::copy(payload.begin(),payload.end(),queued.payload.begin());
    queued.size=payload.size();
    packets_.emplace(sequence,std::move(queued));

    updateJitter(sequence,now);

    ++stablePackets_;
    bool allowDecrease=false;
    if(stablePackets_>=StablePacketsPerDecay) {
        stablePackets_=0;
        if(lateBoostFrames_>0) --lateBoostFrames_;
        allowDecrease=true;
    }
    updateTarget(now,allowDecrease);

    if(suspended_) {
        suspended_=false;
        playoutStarted_=false;
        startupDeadline_=now+std::chrono::milliseconds(targetDelayFrames_*FrameMs);
        nextPlayout_=startupDeadline_;
        currentDelayFrames_=targetDelayFrames_;
        consecutiveMissing_=0;
    }

    return reordered ? JitterBufferPushResult::AcceptedReordered : JitterBufferPushResult::Accepted;
}

std::optional<JitterBufferFrame> AdaptiveJitterBuffer::popReady() {
    if(!initialized_ || suspended_) return std::nullopt;

    const auto now=Clock::now();
    if(!playoutStarted_) {
        if(now<startupDeadline_) return std::nullopt;
        playoutStarted_=true;
        nextPlayout_=startupDeadline_;
    }

    if(now<nextPlayout_) return std::nullopt;

    if(currentDelayFrames_<targetDelayFrames_ && now-lastDelayExpansion_>=DelayExpansionInterval) {
        ++currentDelayFrames_;
        lastDelayExpansion_=now;
        nextPlayout_+=std::chrono::milliseconds(FrameMs);

        JitterBufferFrame frame;
        frame.kind=JitterBufferFrameKind::Hold;
        frame.sequence=expectedSequence_;
        return frame;
    }

    JitterBufferFrame frame;
    frame.sequence=expectedSequence_;

    const auto current=packets_.find(expectedSequence_);
    if(current!=packets_.end()) {
        frame.kind=JitterBufferFrameKind::Packet;
        frame.payloadSize=current->second.size;
        std::copy_n(current->second.payload.begin(),current->second.size,frame.payload.begin());
        packets_.erase(current);
        consecutiveMissing_=0;
    } else {
        frame.kind=JitterBufferFrameKind::Missing;
        ++consecutiveMissing_;

        const auto next=packets_.find(expectedSequence_+1);
        if(next!=packets_.end()) {
            frame.fecPayloadSize=next->second.size;
            std::copy_n(next->second.payload.begin(),next->second.size,frame.fecPayload.begin());
        }
    }

    ++expectedSequence_;
    nextPlayout_+=std::chrono::milliseconds(FrameMs);

    if(consecutiveMissing_>=MaxConcealmentFrames && packets_.empty()) {
        suspended_=true;
        playoutStarted_=false;
    }

    return frame;
}

AdaptiveJitterBufferStats AdaptiveJitterBuffer::stats() const {
    return {
        estimatedJitterMs_,
        targetDelayFrames_*FrameMs,
        currentDelayFrames_*FrameMs,
        static_cast<std::uint32_t>(packets_.size())
    };
}

void AdaptiveJitterBuffer::reset(bool preserveAdaptation) {
    packets_.clear();
    expectedSequence_=0;
    highestSequence_=0;
    lastArrivalSequence_=0;
    lastArrival_={};
    startupDeadline_={};
    nextPlayout_={};
    lastDelayExpansion_={};
    consecutiveMissing_=0;
    initialized_=false;
    haveArrivalReference_=false;
    playoutStarted_=false;
    suspended_=false;

    if(!preserveAdaptation) {
        estimatedJitterMs_=0.0f;
        targetDelayFrames_=MinDelayFrames;
        lateBoostFrames_=0;
        stablePackets_=0;
        lastTargetChange_={};
    }

    currentDelayFrames_=targetDelayFrames_;
}

}
