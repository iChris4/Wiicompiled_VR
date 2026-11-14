// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).
// Changes: no network simulator or LAN transport; the mix is stereo with a per-peer gain pair
// (positional voices); a client may run without peers (microphone test); the packet header comes
// from the shared protocol code.

#pragma once

#include "AdaptiveJitterBuffer.hpp"
#include "AudioEngine.hpp"
#include "AudioProcessor.hpp"
#include "OpusCodec.hpp"
#include "VoiceTransport.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mkwvc {

struct PeerVoiceState {
    std::string memberId;
    bool receiving = false;      // packets arrived in the last second
    bool speaking = false;
    bool remoteMuted = false;
    bool remoteDeafened = false;
    bool protocolMismatch = false;  // the peer sends another packet version
    std::uint32_t voicePeak = 0;
};

struct LocalVoiceState {
    bool running = false;
    bool transmitting = false;
    bool captureAvailable = false;
    std::string captureError;
    std::uint32_t micPeak = 0;
};

class VoiceClient {
public:
    explicit VoiceClient(AudioEngineOptions audio);
    ~VoiceClient();

    VoiceClient(const VoiceClient&) = delete;
    VoiceClient& operator=(const VoiceClient&) = delete;

    void start();
    void stop();
    void reopenAudio(AudioEngineOptions audio);

    void addPeer(std::string memberId, std::unique_ptr<VoiceTransport> transport);
    void removePeer(const std::string& memberId);
    bool hasPeer(const std::string& memberId) const;
    std::size_t peerCount() const;

    void setMicrophoneGain(float gain);
    void setAudioProcessingSettings(const AudioProcessingSettings& settings);
    void setMicrophoneTestEnabled(bool enabled);
    void setPlaybackVolume(float volume);
    void setRemoteVolume(const std::string& memberId, float volume);
    // Left/right gains for a peer's voice; (1, 1) plays it centred, as MKW-VoiceChat does.
    void setRemoteSpatialGains(const std::string& memberId, float left, float right);
    void setTransmitEnabled(bool enabled);
    void setLocalStatus(bool muted, bool deafened);
    void setVoiceActivation(bool enabled, float threshold);
    void setDeafened(bool deafened);
    void playTestTone();

    LocalVoiceState localState() const;
    std::vector<PeerVoiceState> peerStates() const;

private:
    struct PeerState;

    void senderLoop();
    void receiverLoop(const std::shared_ptr<PeerState>& peer);
    void mixerLoop();
    void startPeerReceiver(const std::shared_ptr<PeerState>& peer);
    std::vector<std::shared_ptr<PeerState>> peerSnapshot() const;
    std::shared_ptr<PeerState> findPeer(const std::string& memberId) const;

    AudioEngine audio_;
    AudioProcessor processor_;
    OpusEncoderCodec encoder_;

    mutable std::mutex peersMutex_;
    std::unordered_map<std::string, std::shared_ptr<PeerState>> peers_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> micPeak_{0};
    std::atomic<float> microphoneGain_{1.0f};
    std::atomic<float> playbackVolume_{1.0f};
    std::atomic<bool> transmitEnabled_{true};
    std::atomic<bool> localMutedStatus_{false};
    std::atomic<bool> localDeafenedStatus_{false};
    std::atomic<bool> voiceActivationEnabled_{false};
    std::atomic<float> voiceActivationThreshold_{0.04f};
    std::atomic<bool> transmitting_{false};
    std::atomic<bool> deafened_{false};
    std::atomic<bool> microphoneTestEnabled_{false};

    std::thread sender_;
    std::thread mixer_;
};

} // namespace mkwvc
