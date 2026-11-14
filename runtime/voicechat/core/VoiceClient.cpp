// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#include "VoiceClient.hpp"
#include "SpscRingBuffer.hpp"

#include "voicechat/voice_protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <random>
#include <span>
#include <stdexcept>

namespace mkwvc {

namespace {

namespace protocol = mkw::voicechat::protocol;

constexpr std::size_t MaxRemotePeers = protocol::kMaxRoomMembers - 1;
using PcmFrame = std::array<std::int16_t, VoiceFormat::FrameSamples>;

std::uint64_t makeStreamId() {
    std::random_device random;
    const auto randomPart = (static_cast<std::uint64_t>(random()) << 32) ^ static_cast<std::uint64_t>(random());
    const auto timePart = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto value = randomPart ^ timePart;
    return value == 0 ? 1 : value;
}

std::uint32_t peakOf(std::span<const std::int16_t> samples) {
    std::uint32_t peak = 0;
    for (const auto sample : samples) {
        peak = std::max(peak, static_cast<std::uint32_t>(std::abs(static_cast<std::int32_t>(sample))));
    }
    return peak;
}

float rmsOf(std::span<const std::int16_t> samples) {
    if (samples.empty()) return 0.0f;
    double energy = 0.0;
    for (const auto sample : samples) energy += static_cast<double>(sample) * static_cast<double>(sample);
    return static_cast<float>(std::sqrt(energy / static_cast<double>(samples.size())));
}

std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// MKW-VoiceChat's level staging, kept so both clients sound alike.
constexpr float BaseMicrophoneGain = 1.35f;
constexpr float BasePlaybackGain = 3.375f;

float softLimit(float value) {
    constexpr float knee = 30000.0f;
    constexpr float ceiling = 32600.0f;
    const float magnitude = std::abs(value);
    if (magnitude <= knee) return value;
    const float compressed = knee + (ceiling - knee) * (1.0f - std::exp(-(magnitude - knee) / (ceiling - knee)));
    return std::copysign(compressed, value);
}

std::int16_t toSample(float value) {
    return static_cast<std::int16_t>(std::clamp(static_cast<std::int32_t>(std::lround(softLimit(value))), -32768, 32767));
}

void applyGain(std::span<std::int16_t> samples, float gain) {
    gain = std::max(0.0f, gain);
    for (auto& sample : samples) sample = toSample(static_cast<float>(sample) * gain);
}

} // namespace

struct VoiceClient::PeerState {
    PeerState(std::string id, std::unique_ptr<VoiceTransport> value) : memberId(std::move(id)), transport(std::move(value)) {}

    std::string memberId;
    std::unique_ptr<VoiceTransport> transport;
    OpusDecoderCodec decoder;
    AdaptiveJitterBuffer jitterBuffer;
    SpscRingBuffer<PcmFrame, 32> pcmFrames;
    std::vector<std::uint64_t> retiredStreams;
    std::uint64_t currentStreamId = 0;
    bool haveStream = false;
    std::atomic<bool> running{false};
    std::atomic<float> volume{1.0f};
    std::atomic<float> gainLeft{1.0f};
    std::atomic<float> gainRight{1.0f};
    // Mixer-thread only: the gains the last frame ended on, ramped toward the targets.
    float appliedLeft = 1.0f;
    float appliedRight = 1.0f;
    std::atomic<std::uint32_t> voicePeak{0};
    std::atomic<std::int64_t> speakingUntilMs{0};
    std::atomic<std::int64_t> receivingUntilMs{0};
    std::atomic<bool> remoteMuted{false};
    std::atomic<bool> remoteDeafened{false};
    std::atomic<bool> protocolMismatch{false};
    std::thread receiver;
};

VoiceClient::VoiceClient(AudioEngineOptions audio) : audio_(std::move(audio)) {}

VoiceClient::~VoiceClient() { stop(); }

std::vector<std::shared_ptr<VoiceClient::PeerState>> VoiceClient::peerSnapshot() const {
    std::scoped_lock lock(peersMutex_);
    std::vector<std::shared_ptr<PeerState>> peers;
    peers.reserve(peers_.size());
    for (const auto& [id, peer] : peers_) peers.push_back(peer);
    return peers;
}

std::shared_ptr<VoiceClient::PeerState> VoiceClient::findPeer(const std::string& memberId) const {
    std::scoped_lock lock(peersMutex_);
    const auto found = peers_.find(memberId);
    return found == peers_.end() ? nullptr : found->second;
}

void VoiceClient::startPeerReceiver(const std::shared_ptr<PeerState>& peer) {
    if (!peer || peer->running.exchange(true)) return;
    if (peer->receiver.joinable()) peer->receiver.join();
    peer->receiver = std::thread(&VoiceClient::receiverLoop, this, peer);
}

void VoiceClient::start() {
    if (running_.exchange(true)) return;
    try {
        audio_.start();
        for (const auto& peer : peerSnapshot()) startPeerReceiver(peer);
        sender_ = std::thread(&VoiceClient::senderLoop, this);
        mixer_ = std::thread(&VoiceClient::mixerLoop, this);
    } catch (...) {
        stop();
        throw;
    }
}

void VoiceClient::stop() {
    running_ = false;
    const auto peers = peerSnapshot();
    for (const auto& peer : peers) peer->running = false;
    if (sender_.joinable()) sender_.join();
    if (mixer_.joinable()) mixer_.join();
    for (const auto& peer : peers) {
        if (peer->receiver.joinable()) peer->receiver.join();
    }
    transmitting_ = false;
    audio_.stop();
}

void VoiceClient::reopenAudio(AudioEngineOptions audio) { audio_.reopen(std::move(audio)); }

void VoiceClient::addPeer(std::string memberId, std::unique_ptr<VoiceTransport> transport) {
    if (memberId.empty()) throw std::invalid_argument("Voice peer member ID is empty");
    if (!transport) throw std::invalid_argument("Voice peer transport is required");
    auto peer = std::make_shared<PeerState>(memberId, std::move(transport));
    {
        std::scoped_lock lock(peersMutex_);
        if (peers_.contains(memberId)) throw std::invalid_argument("Voice peer already exists");
        if (peers_.size() >= MaxRemotePeers) throw std::runtime_error("Voice session already has 11 remote peers");
        peers_.emplace(memberId, peer);
    }
    if (running_.load(std::memory_order_relaxed)) {
        try {
            startPeerReceiver(peer);
        } catch (...) {
            std::scoped_lock lock(peersMutex_);
            peers_.erase(memberId);
            throw;
        }
    }
}

void VoiceClient::removePeer(const std::string& memberId) {
    std::shared_ptr<PeerState> peer;
    {
        std::scoped_lock lock(peersMutex_);
        const auto found = peers_.find(memberId);
        if (found == peers_.end()) return;
        peer = found->second;
        peers_.erase(found);
    }
    peer->running = false;
    if (peer->receiver.joinable()) peer->receiver.join();
}

bool VoiceClient::hasPeer(const std::string& memberId) const {
    std::scoped_lock lock(peersMutex_);
    return peers_.contains(memberId);
}

std::size_t VoiceClient::peerCount() const {
    std::scoped_lock lock(peersMutex_);
    return peers_.size();
}

void VoiceClient::setMicrophoneGain(float gain) {
    microphoneGain_.store(std::clamp(gain, 0.25f, 5.0f), std::memory_order_relaxed);
}

void VoiceClient::setAudioProcessingSettings(const AudioProcessingSettings& settings) { processor_.setSettings(settings); }

void VoiceClient::setMicrophoneTestEnabled(bool enabled) {
    microphoneTestEnabled_.store(enabled, std::memory_order_relaxed);
}

void VoiceClient::setPlaybackVolume(float volume) {
    playbackVolume_.store(std::clamp(volume, 0.0f, 3.0f), std::memory_order_relaxed);
}

void VoiceClient::setRemoteVolume(const std::string& memberId, float volume) {
    if (const auto peer = findPeer(memberId)) peer->volume.store(std::clamp(volume, 0.0f, 3.0f), std::memory_order_relaxed);
}

void VoiceClient::setRemoteSpatialGains(const std::string& memberId, float left, float right) {
    if (const auto peer = findPeer(memberId)) {
        peer->gainLeft.store(std::clamp(left, 0.0f, 2.0f), std::memory_order_relaxed);
        peer->gainRight.store(std::clamp(right, 0.0f, 2.0f), std::memory_order_relaxed);
    }
}

void VoiceClient::setTransmitEnabled(bool enabled) {
    transmitEnabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) transmitting_.store(false, std::memory_order_relaxed);
}

void VoiceClient::setLocalStatus(bool muted, bool deafened) {
    localMutedStatus_.store(muted, std::memory_order_relaxed);
    localDeafenedStatus_.store(deafened, std::memory_order_relaxed);
}

void VoiceClient::setVoiceActivation(bool enabled, float threshold) {
    voiceActivationEnabled_.store(enabled, std::memory_order_relaxed);
    voiceActivationThreshold_.store(std::clamp(threshold, 0.005f, 0.25f), std::memory_order_relaxed);
}

void VoiceClient::setDeafened(bool deafened) {
    deafened_.store(deafened, std::memory_order_relaxed);
    audio_.setPlaybackMuted(deafened);
}

void VoiceClient::playTestTone() { audio_.playTestTone(); }

LocalVoiceState VoiceClient::localState() const {
    LocalVoiceState state;
    state.running = running_.load(std::memory_order_relaxed);
    state.transmitting = transmitting_.load(std::memory_order_relaxed);
    state.captureAvailable = audio_.captureAvailable();
    state.captureError = audio_.captureError();
    state.micPeak = micPeak_.load(std::memory_order_relaxed);
    return state;
}

std::vector<PeerVoiceState> VoiceClient::peerStates() const {
    const auto now = steadyNowMs();
    std::vector<PeerVoiceState> result;
    for (const auto& peer : peerSnapshot()) {
        PeerVoiceState state;
        state.memberId = peer->memberId;
        state.receiving = now < peer->receivingUntilMs.load(std::memory_order_relaxed);
        state.speaking = now < peer->speakingUntilMs.load(std::memory_order_relaxed);
        state.remoteMuted = peer->remoteMuted.load(std::memory_order_relaxed);
        state.remoteDeafened = peer->remoteDeafened.load(std::memory_order_relaxed);
        state.protocolMismatch = peer->protocolMismatch.load(std::memory_order_relaxed);
        state.voicePeak = peer->voicePeak.load(std::memory_order_relaxed);
        result.push_back(std::move(state));
    }
    return result;
}

void VoiceClient::senderLoop() {
    std::array<std::int16_t, VoiceFormat::FrameSamples> samples{};
    std::array<std::byte, VoiceFormat::MaxVoicePacketBytes> packet{};
    std::array<std::int16_t, VoiceFormat::FrameSamples> monitor{};
    protocol::PacketHeader header;
    header.streamId = makeStreamId();
    std::size_t filled = 0;
    bool voiceActivationOpen = false;
    std::int64_t voiceActivationUntilMs = 0;
    std::int64_t transmittingUntilMs = 0;
    // MKW-VoiceChat's codec settings (48 kbit/s VOIP, complexity 10, in-band FEC, no DTX).
    encoder_.configure({});

    while (running_.load(std::memory_order_relaxed)) {
        try {
            filled += audio_.readCaptured(std::span<std::int16_t>(samples).subspan(filled));
            if (filled < VoiceFormat::FrameSamples) {
                // A frame is 20 ms; 4 ms polling keeps the added latency small at a fifth of the
                // wakeups of MKW-VoiceChat's 1 ms.
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            filled = 0;

            processor_.processCapture(samples);
            applyGain(samples, microphoneGain_.load(std::memory_order_relaxed) * BaseMicrophoneGain);
            processor_.processPostGainSuppression(samples);

            const bool microphoneTest = microphoneTestEnabled_.load(std::memory_order_relaxed);
            const auto processedPeak = peakOf(samples);
            const auto processedRms = rmsOf(samples);
            if (microphoneTest) {
                std::copy(samples.begin(), samples.end(), monitor.begin());
                applyGain(monitor, playbackVolume_.load(std::memory_order_relaxed) * BasePlaybackGain);
                audio_.queueMonitor(monitor);
            }

            const auto now = steadyNowMs();
            bool voiceActive = true;
            if (voiceActivationEnabled_.load(std::memory_order_relaxed)) {
                const auto threshold = voiceActivationThreshold_.load(std::memory_order_relaxed) * 32768.0f;
                if (processedRms >= threshold) {
                    voiceActivationOpen = true;
                    voiceActivationUntilMs = now + 280;
                } else if (voiceActivationOpen && processedRms < threshold * 0.65f && now >= voiceActivationUntilMs) {
                    voiceActivationOpen = false;
                }
                voiceActive = voiceActivationOpen;
            } else {
                voiceActivationOpen = false;
            }

            const bool transmit = transmitEnabled_.load(std::memory_order_relaxed) && voiceActive;
            if (transmit && processedPeak > 500) transmittingUntilMs = now + 300;
            transmitting_.store(now < transmittingUntilMs, std::memory_order_relaxed);
            if (!transmit) std::fill(samples.begin(), samples.end(), std::int16_t{0});
            micPeak_.store(microphoneTest || transmit ? processedPeak : 0, std::memory_order_relaxed);

            const auto peers = peerSnapshot();
            if (peers.empty()) continue;

            // Silence is still sent: the stream and the mute/deafen flags stay current, as the
            // other client expects.
            header.flags = 0;
            if (localMutedStatus_.load(std::memory_order_relaxed)) header.flags |= protocol::kFlagMuted;
            if (localDeafenedStatus_.load(std::memory_order_relaxed)) header.flags |= protocol::kFlagDeafened;
            protocol::WritePacketHeader(
                std::span<std::byte, protocol::kPacketHeaderBytes>(packet.data(), protocol::kPacketHeaderBytes), header);
            ++header.sequence;
            const auto payload =
                encoder_.encode(samples, std::span<std::byte>(packet).subspan(VoiceFormat::PacketHeaderBytes));
            const auto packetSize = VoiceFormat::PacketHeaderBytes + payload;
            for (const auto& peer : peers) {
                if (!peer->running.load(std::memory_order_relaxed)) continue;
                try {
                    peer->transport->send(std::span<const std::byte>(packet.data(), packetSize));
                } catch (...) {
                    peer->running = false;
                }
            }
        } catch (...) {
            transmitting_ = false;
            running_ = false;
            break;
        }
    }
    transmitting_ = false;
}

void VoiceClient::receiverLoop(const std::shared_ptr<PeerState>& peer) {
    std::array<std::byte, VoiceFormat::MaxVoicePacketBytes> packet{};
    std::array<std::int16_t, VoiceFormat::FrameSamples> samples{};

    const auto queueDecoded = [&](std::size_t count, bool observeVoice) {
        PcmFrame frame{};
        const auto copyCount = std::min(count, frame.size());
        std::copy_n(samples.begin(), copyCount, frame.begin());
        peer->pcmFrames.push(std::span<const PcmFrame>(&frame, 1));
        if (observeVoice) {
            const auto peak = peakOf(std::span<const std::int16_t>(frame.data(), copyCount));
            peer->voicePeak.store(peak, std::memory_order_relaxed);
            if (peak > 600) peer->speakingUntilMs.store(steadyNowMs() + 300, std::memory_order_relaxed);
        }
    };
    const auto conceal = [&] {
        try {
            queueDecoded(peer->decoder.conceal(samples), false);
        } catch (...) {
        }
    };
    const auto processReadyFrames = [&] {
        for (int drained = 0; drained < 16; ++drained) {
            const auto ready = peer->jitterBuffer.popReady();
            if (!ready) break;
            if (ready->kind == JitterBufferFrameKind::Hold) {
                conceal();
                continue;
            }
            if (ready->kind == JitterBufferFrameKind::Missing) {
                if (ready->fecPayloadSize > 0) {
                    try {
                        queueDecoded(peer->decoder.decode(
                                         std::span<const std::byte>(ready->fecPayload.data(), ready->fecPayloadSize),
                                         samples, true),
                                     true);
                        continue;
                    } catch (...) {
                    }
                }
                conceal();
                continue;
            }
            try {
                queueDecoded(peer->decoder.decode(std::span<const std::byte>(ready->payload.data(), ready->payloadSize),
                                                  samples, false),
                             true);
            } catch (...) {
                conceal();
            }
        }
    };

    while (running_.load(std::memory_order_relaxed) && peer->running.load(std::memory_order_relaxed)) {
        const auto size = peer->transport->receive(packet);
        if (size > 0) {
            const auto received = std::span<const std::byte>(packet.data(), size);
            const auto check = protocol::CheckPacket(received);
            if (check != protocol::PacketCheck::Valid) {
                if (check == protocol::PacketCheck::WrongVersion) {
                    peer->protocolMismatch.store(true, std::memory_order_relaxed);
                }
                processReadyFrames();
                continue;
            }
            const auto header = protocol::ReadPacketHeader(received);
            peer->protocolMismatch.store(false, std::memory_order_relaxed);
            peer->remoteMuted.store((header.flags & protocol::kFlagMuted) != 0, std::memory_order_relaxed);
            peer->remoteDeafened.store((header.flags & protocol::kFlagDeafened) != 0, std::memory_order_relaxed);
            peer->receivingUntilMs.store(steadyNowMs() + 1000, std::memory_order_relaxed);

            if (!peer->haveStream) {
                peer->currentStreamId = header.streamId;
                peer->haveStream = true;
                peer->decoder.reset();
                peer->jitterBuffer.reset(false);
            } else if (header.streamId != peer->currentStreamId) {
                if (std::find(peer->retiredStreams.begin(), peer->retiredStreams.end(), header.streamId) !=
                    peer->retiredStreams.end()) {
                    processReadyFrames();
                    continue;
                }
                peer->retiredStreams.push_back(peer->currentStreamId);
                if (peer->retiredStreams.size() > 16) peer->retiredStreams.erase(peer->retiredStreams.begin());
                peer->currentStreamId = header.streamId;
                peer->decoder.reset();
                peer->jitterBuffer.reset(false);
            }

            const auto payload = received.subspan(VoiceFormat::PacketHeaderBytes);
            if (peer->jitterBuffer.push(header.sequence, payload) == JitterBufferPushResult::TooFarAhead) {
                peer->decoder.reset();
                peer->jitterBuffer.reset(true);
                peer->jitterBuffer.push(header.sequence, payload);
            }
        }
        processReadyFrames();
    }
    peer->voicePeak = 0;
    peer->speakingUntilMs = 0;
    peer->running = false;
}

void VoiceClient::mixerLoop() {
    constexpr std::size_t Channels = VoiceFormat::PlaybackChannels;
    constexpr std::size_t FrameSamples = VoiceFormat::FrameSamples;
    auto nextMix = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        nextMix += std::chrono::milliseconds(VoiceFormat::FrameDurationMs);

        std::array<float, FrameSamples * Channels> mixed{};
        PcmFrame frame{};
        bool anyFrame = false;
        for (const auto& peer : peerSnapshot()) {
            if (peer->pcmFrames.pop(std::span<PcmFrame>(&frame, 1)) == 0) continue;
            anyFrame = true;
            const float volume = peer->volume.load(std::memory_order_relaxed);
            const float targetLeft = peer->gainLeft.load(std::memory_order_relaxed) * volume;
            const float targetRight = peer->gainRight.load(std::memory_order_relaxed) * volume;
            // Ramp across the frame so a moving voice never clicks.
            const float stepLeft = (targetLeft - peer->appliedLeft) / static_cast<float>(FrameSamples);
            const float stepRight = (targetRight - peer->appliedRight) / static_cast<float>(FrameSamples);
            float left = peer->appliedLeft;
            float right = peer->appliedRight;
            for (std::size_t i = 0; i < FrameSamples; ++i) {
                left += stepLeft;
                right += stepRight;
                const float sample = static_cast<float>(frame[i]);
                mixed[i * Channels] += sample * left;
                mixed[i * Channels + 1] += sample * right;
            }
            peer->appliedLeft = targetLeft;
            peer->appliedRight = targetRight;
        }

        // Silence is queued too: a continuous stream keeps the output primed, so speech starts
        // without the 60 ms prebuffer again.
        std::array<std::int16_t, FrameSamples * Channels> output{};
        if (anyFrame && !deafened_.load(std::memory_order_relaxed)) {
            const float globalVolume = playbackVolume_.load(std::memory_order_relaxed) * BasePlaybackGain;
            for (std::size_t i = 0; i < output.size(); ++i) output[i] = toSample(mixed[i] * globalVolume);
        }
        audio_.queuePlayback(output);

        std::this_thread::sleep_until(nextMix);
        const auto now = std::chrono::steady_clock::now();
        if (nextMix + std::chrono::milliseconds(VoiceFormat::FrameDurationMs * 4) < now) nextMix = now;
    }
}

} // namespace mkwvc
