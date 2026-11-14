// SPDX-License-Identifier: GPL-3.0-or-later
//
// The voice chat session behind voice_api.h. It plays the role of MKW-VoiceChat's EmbeddedCore
// (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0) with the same protocol and peer handshake,
// rebuilt around one worker thread: the game thread only posts what it read from guest memory, and
// every WebSocket, peer connection and audio device is created and destroyed here.
//
// Lifecycle, re-evaluated on every tick (~25 ms, sooner on new input):
//   disabled                      -> nothing open
//   enabled, no GPCM login        -> Offline
//   logged in, no RKNet room      -> NotInRoom (signaling closed)
//   in a room                     -> open signaling, RR_DEV_ADMIT, then peers as the Worker
//                                    introduces them; a failed admission is retried
//   room or login changes         -> close everything and start over for the new room
//
// Differences from EmbeddedCore, all deliberate: no global presence socket (signaling is only
// open while in a room), no Open Host rewriting of the player's RWFC profile, no session key, an
// admission retry, positional gains, and listen-only when the microphone cannot be opened.

#include "voicechat/voice_api.h"

#include "voicechat/gpcm_identity.h"
#include "voicechat/voice_protocol.h"
#include "voicechat/voice_spatial.h"

#include "AudioEngine.hpp"
#include "IcePeerTransport.hpp"
#include "SignalingClient.hpp"
#include "VoiceClient.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mkw::voicechat {

namespace {

namespace protocol = mkw::voicechat::protocol;
using Clock = std::chrono::steady_clock;

constexpr const char* kDefaultSignalingUrl = "wss://mkw-voicechat-signaling.mkwvoicechat.workers.dev/";
constexpr auto kTick = std::chrono::milliseconds(25);
// Off or outside a room nothing is time-critical; new input still wakes the worker at once.
constexpr auto kIdleTick = std::chrono::milliseconds(250);
constexpr auto kReconnectDelay = std::chrono::seconds(5);
constexpr auto kAdmitRetryDelay = std::chrono::seconds(10);
constexpr auto kSpatialMaxAge = std::chrono::milliseconds(500);
// MKW-VoiceChat waits up to this long for a full ICE gathering before giving up on a peer.
constexpr auto kPeerSetupTimeout = std::chrono::seconds(30);

void Log(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::fputs("[voice] ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

float ThresholdFromPercent(int percent) {
    const float normalized = static_cast<float>(std::clamp(percent, 1, 100)) / 100.0f;
    return 0.005f + normalized * 0.195f;
}

mkwvc::AudioProcessingSettings ProcessingFrom(const Settings& settings) {
    mkwvc::AudioProcessingSettings processing;
    processing.normalization = settings.normalization;
    processing.noiseSuppression = settings.noiseSuppression;
    processing.noiseSuppressionStrength = settings.noiseSuppressionStrength;
    processing.noiseGate = settings.noiseGate;
    processing.noiseGateThreshold = settings.noiseGateThreshold;
    processing.microphoneBoost = settings.microphoneBoost;
    processing.compressor = settings.compressor;
    processing.compressorStrength = settings.compressorStrength;
    return processing;
}

mkwvc::AudioEngineOptions AudioOptionsFrom(const Settings& settings) {
    return {settings.inputDevice, settings.outputDevice, settings.platformVoiceProcessing};
}

struct PeerMetadata {
    std::uint32_t profileId = 0;
    std::string displayName;
    std::string friendCode;
};

struct PeerLink {
    std::unique_ptr<mkwvc::IcePeerTransport> setup;
    std::optional<mkwvc::IceDescriptionSignal> localDescription;
    std::vector<mkwvc::IceCandidateSignal> localCandidates;
    bool signalSent = false;
    bool offerer = false;
    Clock::time_point started{};
};

struct SpatialState {
    std::unordered_map<std::uint32_t, spatial::Gains> gains;
    Clock::time_point postedAt{};
};

class Service {
public:
    void Start(const Settings& settings) {
        std::scoped_lock lock(lifecycleMutex_);
        {
            std::scoped_lock inputLock(inputMutex_);
            settings_ = settings;
            settingsGeneration_++;
        }
        wantsRoom_.store(false, std::memory_order_relaxed);
        if (worker_.joinable()) {
            Wake();
            return;
        }
        stop_ = false;
        worker_ = std::thread([this] { Run(); });
    }

    void Shutdown() {
        std::scoped_lock lock(lifecycleMutex_);
        if (!worker_.joinable()) return;
        {
            std::scoped_lock inputLock(inputMutex_);
            stop_ = true;
        }
        Wake();
        worker_.join();
    }

    void ApplySettings(const Settings& settings) {
        {
            std::scoped_lock lock(inputMutex_);
            settings_ = settings;
            settingsGeneration_++;
        }
        Wake();
    }

    void ObserveSend(std::uint32_t socket, const std::uint8_t* data, std::size_t size) {
        std::scoped_lock lock(identityMutex_);
        if (identity_.OnSend(socket, std::span<const std::uint8_t>(data, size))) IdentityChangedLocked();
    }

    void ObserveReceive(std::uint32_t socket, const std::uint8_t* data, std::size_t size) {
        std::scoped_lock lock(identityMutex_);
        if (identity_.OnReceive(socket, std::span<const std::uint8_t>(data, size))) IdentityChangedLocked();
    }

    void ObserveClosed(std::uint32_t socket) {
        std::scoped_lock lock(identityMutex_);
        if (identity_.OnClosed(socket)) IdentityChangedLocked();
    }

    bool WantsRoom() const { return wantsRoom_.load(std::memory_order_relaxed); }
    std::uint32_t ProfileId() const { return profileId_.load(std::memory_order_relaxed); }
    bool WantsSpatial() const { return wantsSpatial_.load(std::memory_order_relaxed); }

    void PostRoom(const RoomSnapshot& snapshot) {
        {
            std::scoped_lock lock(inputMutex_);
            room_ = snapshot;
        }
        Wake();
    }

    void PostSpatial(const SpatialSnapshot& snapshot) {
        SpatialState state;
        state.postedAt = Clock::now();
        for (const auto& entry : snapshot.entries) {
            state.gains[entry.profileId] = spatial::GainsForPosition(
                entry.position[0], entry.position[1], entry.position[2]);
        }
        std::scoped_lock lock(inputMutex_);
        spatial_ = std::move(state);
    }

    void ClearSpatial() {
        std::scoped_lock lock(inputMutex_);
        spatial_ = {};
    }

    void SetPushToTalkHeld(bool held) {
        if (pushToTalkHeld_.exchange(held, std::memory_order_relaxed) != held) Wake();
    }

    void RequestDeviceRefresh() {
        refreshDevices_.store(true, std::memory_order_relaxed);
        Wake();
    }

    void SetMicrophoneTest(bool enabled) {
        microphoneTest_.store(enabled, std::memory_order_relaxed);
        Wake();
    }

    void RequestTestTone() {
        testTone_.store(true, std::memory_order_relaxed);
        Wake();
    }

    Status GetStatus() {
        std::scoped_lock lock(statusMutex_);
        return status_;
    }

private:
    void Wake() {
        {
            std::scoped_lock lock(wakeMutex_);
            woken_ = true;
        }
        wakeCv_.notify_one();
    }

    void IdentityChangedLocked() {
        const auto& identity = identity_.Current();
        profileId_.store(identity.online ? identity.profileId : 0, std::memory_order_relaxed);
        {
            // A room read under the previous login says nothing about the next one.
            std::scoped_lock lock(inputMutex_);
            room_ = {};
        }
        if (identity.online) {
            Log("online login seen (profile %u)", identity.profileId);
        } else {
            Log("online session ended");
        }
        Wake();
    }

    // ---- worker thread --------------------------------------------------------------------------

    void Run() {
        Log("worker started");
        while (true) {
            {
                const bool busy = signaling_ != nullptr || client_ != nullptr || toneEngine_ != nullptr;
                std::unique_lock lock(wakeMutex_);
                wakeCv_.wait_for(lock, busy ? kTick : kIdleTick, [this] { return woken_; });
                woken_ = false;
            }
            Settings settings;
            RoomSnapshot room;
            SpatialState spatialState;
            bool stop = false;
            {
                std::scoped_lock lock(inputMutex_);
                stop = stop_;
                settings = settings_;
                room = room_;
                spatialState = spatial_;
                if (settingsGeneration_ != appliedSettingsGeneration_) {
                    settingsChanged_ = true;
                    appliedSettingsGeneration_ = settingsGeneration_;
                }
            }
            if (stop) break;
            GpcmIdentity identity;
            {
                std::scoped_lock lock(identityMutex_);
                identity = identity_.Current();
            }
            try {
                Tick(settings, room, spatialState, identity);
            } catch (const std::exception& error) {
                message_ = std::string("Voice chat error: ") + error.what();
                Log("%s", message_.c_str());
            } catch (...) {
                message_ = "Voice chat error";
            }
            PublishStatus(settings, identity);
        }
        CloseSession("Stopped");
        StopClient();
        toneEngine_.reset();
        Log("worker stopped");
    }

    void Tick(const Settings& settings, const RoomSnapshot& room, const SpatialState& spatialState,
              const GpcmIdentity& identity) {
        if (std::exchange(settingsChanged_, false)) ApplySettingsToClient(settings);
        if (refreshDevices_.exchange(false, std::memory_order_relaxed) || (settings.enabled && !devicesListed_)) {
            inputDevices_ = mkwvc::AudioEngine::captureDevices();
            outputDevices_ = mkwvc::AudioEngine::playbackDevices();
            devicesListed_ = true;
        }
        ServiceTestTone(settings);

        if (!settings.enabled) {
            wantsRoom_.store(false, std::memory_order_relaxed);
            wantsSpatial_.store(false, std::memory_order_relaxed);
            CloseSession("Disabled");
            ForgetSessionRoom();
            state_ = State::Disabled;
            message_ = "Voice chat is off";
            ApplyControls(settings);
            return;
        }

        wantsRoom_.store(identity.online, std::memory_order_relaxed);
        const bool active = identity.online && room.inRoom;
        if (!active) {
            CloseSession(identity.online ? "Not in a room" : "Offline");
            ForgetSessionRoom();
            state_ = identity.online ? State::NotInRoom : State::Offline;
            message_ = identity.online ? "Waiting for an online room" : "Not connected to Retro Rewind online";
            ApplyControls(settings);
            ApplyPolicy(settings, room);
            wantsSpatial_.store(false, std::memory_order_relaxed);
            return;
        }

        if (identity.profileId != sessionProfileId_ || identity.generation != sessionGeneration_ ||
            room.roomKey != sessionRoomKey_) {
            if (signaling_) Log("room changed; reconnecting");
            CloseSession("Room changed");
            ForgetSessionRoom();
            sessionProfileId_ = identity.profileId;
            sessionGeneration_ = identity.generation;
            sessionRoomKey_ = room.roomKey;
        }
        roomProfileIds_ = room.roomProfileIds;

        const auto now = Clock::now();
        if (!signaling_ && (nextConnect_ == Clock::time_point{} || now >= nextConnect_)) {
            const std::string url = settings.signalingUrl.empty() ? kDefaultSignalingUrl : settings.signalingUrl;
            try {
                signaling_ = std::make_unique<mkwvc::SignalingClient>(url, settings.caCertificateFile);
                state_ = State::Connecting;
                message_ = "Connecting to the voice server";
                Log("connecting to %s", url.c_str());
            } catch (const std::exception& error) {
                ScheduleReconnect(std::string("Could not reach the voice server: ") + error.what());
            }
        }
        if (signaling_) {
            ServiceSignaling(settings, identity);
        }
        if (signaling_ && !admitted_ && !admitPending_ && admitRetryAt_ != Clock::time_point{} && now >= admitRetryAt_) {
            SendAdmit(identity.profileId);
        }
        PollPeerLinks(settings);
        ApplyControls(settings);
        ApplyPolicy(settings, room);
        ApplySpatial(settings, spatialState);
    }

    void ScheduleReconnect(std::string why) {
        CloseSession(why);
        state_ = State::Retrying;
        message_ = std::move(why);
        nextConnect_ = Clock::now() + kReconnectDelay;
        Log("%s; retrying in 5 s", message_.c_str());
    }

    void SendAdmit(std::uint32_t profileId) {
        signaling_->admit(profileId, roomProfileIds_);
        admitPending_ = true;
        admitRetryAt_ = {};
        state_ = State::Admitting;
        message_ = "Joining the room's voice chat";
    }

    void ServiceSignaling(const Settings& settings, const GpcmIdentity& identity) {
        signaling_->service();
        for (auto& event : signaling_->takeEvents()) {
            switch (event.type) {
            case mkwvc::SignalingEvent::Type::Open:
                ClearPeers();
                admitted_ = false;
                SendAdmit(identity.profileId);
                break;
            case mkwvc::SignalingEvent::Type::TransportError:
                Log("signaling error: %s", event.error.c_str());
                ScheduleReconnect("Lost the voice server: " + event.error);
                return;
            case mkwvc::SignalingEvent::Type::Closed:
                ScheduleReconnect("The voice server closed the connection");
                return;
            case mkwvc::SignalingEvent::Type::Message:
                HandleMessage(settings, identity, event.message);
                break;
            }
            if (!signaling_) return;
        }
    }

    void HandleMessage(const Settings& settings, const GpcmIdentity& identity, const protocol::ServerMessage& message) {
        using protocol::MessageKind;
        switch (message.kind) {
        case MessageKind::IceServers:
            iceServers_ = protocol::ParseIceServers(message.payload);
            break;
        case MessageKind::DevAdmitted: {
            auto admission = protocol::ParseAdmission(message.payload, identity.profileId);
            admitPending_ = false;
            if (!admission) {
                admitted_ = false;
                state_ = State::Retrying;
                message_ = "The voice server sent a malformed room";
                admitRetryAt_ = Clock::now() + kAdmitRetryDelay;
                break;
            }
            if (admitted_ && admission->roomInstanceId != roomInstanceId_) ClearPeers();
            admitted_ = true;
            localMemberId_ = admission->memberId;
            roomInstanceId_ = admission->roomInstanceId;
            roomId_ = admission->roomId;
            roomPlayers_ = std::move(admission->players);
            state_ = State::InRoom;
            message_ = "In the room's voice chat";
            Log("admitted to voice room %s (%zu players)", roomInstanceId_.substr(0, 12).c_str(), roomPlayers_.size());
            break;
        }
        case MessageKind::DevAdmitFailed:
            admitPending_ = false;
            admitted_ = false;
            ClearPeers();
            state_ = State::Retrying;
            message_ = "Room not found yet: " + message.payload;
            // The public roster refreshes every few seconds; a room just joined is often absent.
            admitRetryAt_ = Clock::now() + kAdmitRetryDelay;
            Log("admission failed: %s", message.payload.c_str());
            break;
        case MessageKind::DevPeerInfo: {
            if (!admitted_) break;
            const auto info = protocol::ParsePeerInfo(message.payload, roomInstanceId_);
            if (!info || info->profileId == identity.profileId) break;
            const bool known = std::any_of(peers_.begin(), peers_.end(), [&](const auto& entry) {
                return entry.second.profileId == info->profileId;
            });
            if (settings.muteNewPlayers && !known && !everSeenProfiles_.contains(info->profileId)) {
                autoMutedProfiles_.insert(info->profileId);
            }
            everSeenProfiles_.insert(info->profileId);
            peers_[info->memberId] = {info->profileId, info->displayName, info->friendCode};
            bool listed = false;
            for (auto& player : roomPlayers_) {
                if (player.profileId == info->profileId) {
                    player.voiceChat = true;
                    player.displayName = info->displayName;
                    if (!info->friendCode.empty()) player.friendCode = info->friendCode;
                    listed = true;
                }
            }
            if (!listed) {
                protocol::RoomPlayer player;
                player.profileId = info->profileId;
                player.displayName = info->displayName;
                player.friendCode = info->friendCode;
                player.voiceChat = true;
                roomPlayers_.push_back(std::move(player));
            }
            break;
        }
        case MessageKind::PeerReady:
            if (!admitted_ || localMemberId_.empty() || !peers_.contains(message.memberId)) break;
            ResetPeerTransport(message.memberId);
            EnsurePeerLink(message.memberId, localMemberId_ < message.memberId);
            break;
        case MessageKind::Signal:
            ApplyRemoteSignal(message);
            break;
        case MessageKind::PeerLeft:
            RemovePeer(message.memberId);
            break;
        case MessageKind::Error:
            Log("voice server error: %s", message.payload.c_str());
            message_ = "Voice server: " + message.payload;
            break;
        case MessageKind::Unknown:
            break;
        }
    }

    void EnsurePeerLink(const std::string& memberId, bool offerer) {
        if (links_.contains(memberId)) return;
        PeerLink link;
        link.offerer = offerer;
        link.started = Clock::now();
        link.setup = std::make_unique<mkwvc::IcePeerTransport>(iceServers_, false);
        if (offerer) link.setup->beginOffer();
        links_.emplace(memberId, std::move(link));
    }

    void ApplyRemoteSignal(const protocol::ServerMessage& message) {
        if (!admitted_ || !peers_.contains(message.memberId)) return;
        const auto bundle = protocol::DecodeIceBundle(message.payload);
        if (!bundle) {
            Log("malformed ICE bundle from a peer");
            return;
        }
        auto found = links_.find(message.memberId);
        if (found == links_.end()) {
            if (bundle->type != "offer") return;
            EnsurePeerLink(message.memberId, false);
            found = links_.find(message.memberId);
        }
        auto& link = found->second;
        if (!link.setup) return;
        if (bundle->type != (link.offerer ? "answer" : "offer")) {
            Log("unexpected ICE %s from a peer", bundle->type.c_str());
            return;
        }
        link.setup->setRemoteDescription(bundle->sdp, bundle->type);
        for (const auto& candidate : bundle->candidates) {
            link.setup->addRemoteCandidate(candidate.candidate, candidate.mid);
        }
    }

    void PollPeerLinks(const Settings& settings) {
        if (!signaling_ || !admitted_) return;
        std::vector<std::string> failed;
        const auto now = Clock::now();
        for (auto& [memberId, link] : links_) {
            if (!link.setup) continue;
            try {
                if (auto description = link.setup->takeLocalDescription()) link.localDescription = std::move(*description);
                auto candidates = link.setup->takeLocalCandidates();
                link.localCandidates.insert(link.localCandidates.end(), std::make_move_iterator(candidates.begin()),
                                            std::make_move_iterator(candidates.end()));
                if (link.localDescription && link.setup->gatheringComplete() && !link.signalSent) {
                    protocol::IceBundle bundle;
                    bundle.type = link.localDescription->type;
                    bundle.sdp = link.localDescription->sdp;
                    for (const auto& candidate : link.localCandidates) {
                        bundle.candidates.push_back({candidate.candidate, candidate.mid});
                    }
                    signaling_->sendSignal(memberId, protocol::EncodeIceBundle(bundle));
                    link.signalSent = true;
                }
                if (!link.setup->connected() || !link.localDescription || !link.setup->gatheringComplete()) {
                    if (now - link.started > kPeerSetupTimeout) failed.push_back(memberId);
                    continue;
                }
                if (client_ && client_->hasPeer(memberId)) continue;
                auto transport = std::unique_ptr<mkwvc::VoiceTransport>(link.setup.release());
                EnsureClient(settings);
                client_->addPeer(memberId, std::move(transport));
                const auto meta = peers_.find(memberId);
                Log("voice connected to %s", meta != peers_.end() ? meta->second.displayName.c_str() : "a peer");
            } catch (const std::exception& error) {
                Log("peer setup failed: %s", error.what());
                failed.push_back(memberId);
            }
        }
        for (const auto& memberId : failed) {
            ResetPeerTransport(memberId);
        }
        if (client_ && client_->peerCount() == 0 && !microphoneTest_.load(std::memory_order_relaxed)) StopClient();
    }

    void EnsureClient(const Settings& settings) {
        if (client_) return;
        client_ = std::make_unique<mkwvc::VoiceClient>(AudioOptionsFrom(settings));
        appliedAudio_ = AudioOptionsFrom(settings);
        ApplySettingsToClient(settings);
        ApplyControlsToClient(settings);
        client_->start();
        const auto local = client_->localState();
        if (!local.captureAvailable) Log("listen-only: %s", local.captureError.c_str());
    }

    void StopClient() {
        if (!client_) return;
        client_->stop();
        client_.reset();
    }

    void ResetPeerTransport(const std::string& memberId) {
        if (client_) client_->removePeer(memberId);
        links_.erase(memberId);
    }

    void RemovePeer(const std::string& memberId) {
        ResetPeerTransport(memberId);
        if (const auto found = peers_.find(memberId); found != peers_.end()) {
            for (auto& player : roomPlayers_) {
                if (player.profileId == found->second.profileId) player.voiceChat = false;
            }
            Log("%s left the voice chat", found->second.displayName.c_str());
            peers_.erase(found);
        }
    }

    void ClearPeers() {
        for (const auto& [memberId, link] : links_) {
            if (client_) client_->removePeer(memberId);
        }
        links_.clear();
        if (client_) {
            for (const auto& [memberId, meta] : peers_) client_->removePeer(memberId);
        }
        peers_.clear();
        if (client_ && !microphoneTest_.load(std::memory_order_relaxed)) StopClient();
    }

    void CloseSession(const std::string& why) {
        if (signaling_ || admitted_ || !peers_.empty()) Log("voice session closed (%s)", why.c_str());
        ClearPeers();
        signaling_.reset();
        admitted_ = false;
        admitPending_ = false;
        admitRetryAt_ = {};
        localMemberId_.clear();
        roomInstanceId_.clear();
        roomId_.clear();
        roomPlayers_.clear();
        iceServers_ = {std::string(protocol::kDefaultStunServer)};
        autoMutedProfiles_.clear();
        everSeenProfiles_.clear();
    }

    // Forget which room the session was for, so the next room starts a fresh connection at once.
    void ForgetSessionRoom() {
        sessionProfileId_ = 0;
        sessionGeneration_ = 0;
        sessionRoomKey_ = 0;
        nextConnect_ = {};
    }

    void ApplySettingsToClient(const Settings& settings) {
        if (!client_) return;
        const auto audio = AudioOptionsFrom(settings);
        if (audio.captureDevice != appliedAudio_.captureDevice || audio.playbackDevice != appliedAudio_.playbackDevice ||
            audio.platformVoiceProcessing != appliedAudio_.platformVoiceProcessing) {
            try {
                client_->reopenAudio(audio);
                appliedAudio_ = audio;
            } catch (const std::exception& error) {
                audioError_ = error.what();
            }
        }
        client_->setMicrophoneGain(settings.microphoneGain);
        client_->setAudioProcessingSettings(ProcessingFrom(settings));
        client_->setPlaybackVolume(settings.playbackVolume);
    }

    // The client lives while it has peers or the microphone test runs.
    void ApplyControls(const Settings& settings) {
        const bool microphoneTest = settings.enabled && microphoneTest_.load(std::memory_order_relaxed);
        if (microphoneTest) {
            try {
                EnsureClient(settings);
            } catch (const std::exception& error) {
                audioError_ = error.what();
                microphoneTest_.store(false, std::memory_order_relaxed);
            }
        } else if (client_ && client_->peerCount() == 0) {
            StopClient();
        }
        ApplyControlsToClient(settings);
    }

    void ApplyControlsToClient(const Settings& settings) {
        if (!client_) return;
        const bool microphoneTest = settings.enabled && microphoneTest_.load(std::memory_order_relaxed);
        const bool pushToTalk = settings.talkMode == TalkMode::PushToTalk;
        const bool held = !pushToTalk || pushToTalkHeld_.load(std::memory_order_relaxed);
        client_->setLocalStatus(settings.muted, settings.deafened);
        client_->setVoiceActivation(settings.talkMode == TalkMode::VoiceActivation,
                                    ThresholdFromPercent(settings.voiceActivationThreshold));
        client_->setTransmitEnabled(!microphoneTest && !settings.muted && !settings.deafened && held);
        client_->setDeafened(settings.deafened || microphoneTest);
        client_->setMicrophoneTestEnabled(microphoneTest && held);
    }

    bool PolicyMuted(const Settings& settings, std::uint32_t profileId) const {
        const bool isFriend = friends_.contains(profileId);
        const bool teamKnown = teamMode_ && teamKnown_.contains(profileId);
        const bool teammate = teamKnown && teammates_.contains(profileId);
        if (settings.muteEveryone) return true;
        if (settings.muteOnlyFriends && isFriend) return true;
        if (settings.muteEveryoneButFriends && !isFriend) return true;
        if (settings.muteTeammates && teammate) return true;
        if (settings.muteEveryoneButTeammates && teamKnown && !teammate) return true;
        return autoMutedProfiles_.contains(profileId);
    }

    float PlayerVolume(const Settings& settings, std::uint32_t profileId) const {
        for (const auto& [pid, volume] : settings.playerVolumes) {
            if (pid == profileId) return volume;
        }
        return 1.0f;
    }

    void ApplyPolicy(const Settings& settings, const RoomSnapshot& room) {
        friends_ = std::unordered_set<std::uint32_t>(room.friendProfileIds.begin(), room.friendProfileIds.end());
        teamMode_ = room.teamMode;
        teamKnown_ = std::unordered_set<std::uint32_t>(room.teamProfileIds.begin(), room.teamProfileIds.end());
        teammates_ = std::unordered_set<std::uint32_t>(room.teammateProfileIds.begin(), room.teammateProfileIds.end());
        if (!settings.muteNewPlayers) autoMutedProfiles_.clear();
        if (!client_) return;
        for (const auto& [memberId, meta] : peers_) {
            client_->setRemoteVolume(memberId, PolicyMuted(settings, meta.profileId) ? 0.0f : PlayerVolume(settings, meta.profileId));
        }
    }

    void ApplySpatial(const Settings& settings, const SpatialState& state) {
        wantsSpatial_.store(settings.positional && client_ && client_->peerCount() > 0, std::memory_order_relaxed);
        if (!client_) return;
        const bool fresh = settings.positional && state.postedAt != Clock::time_point{} &&
                           Clock::now() - state.postedAt < kSpatialMaxAge;
        for (const auto& [memberId, meta] : peers_) {
            spatial::Gains gains;
            if (fresh) {
                if (const auto found = state.gains.find(meta.profileId); found != state.gains.end()) gains = found->second;
            }
            client_->setRemoteSpatialGains(memberId, gains.left, gains.right);
        }
    }

    void ServiceTestTone(const Settings& settings) {
        if (!testTone_.exchange(false, std::memory_order_relaxed)) {
            if (toneEngine_ && Clock::now() > toneUntil_) toneEngine_.reset();
            return;
        }
        try {
            if (client_) {
                client_->playTestTone();
                return;
            }
            toneEngine_ = std::make_unique<mkwvc::AudioEngine>(AudioOptionsFrom(settings));
            toneEngine_->start();
            toneEngine_->playTestTone();
            toneUntil_ = Clock::now() + std::chrono::seconds(2);
        } catch (const std::exception& error) {
            audioError_ = error.what();
            toneEngine_.reset();
        }
    }

    void PublishStatus(const Settings& settings, const GpcmIdentity& identity) {
        Status status;
        status.state = state_;
        status.message = message_;
        status.profileId = identity.online ? identity.profileId : 0;
        status.inputDevices = inputDevices_;
        status.outputDevices = outputDevices_;
        status.microphoneTest = microphoneTest_.load(std::memory_order_relaxed);
        status.audioError = audioError_;
        if (client_) {
            const auto local = client_->localState();
            status.microphoneOpen = local.captureAvailable;
            status.transmitting = local.transmitting;
            status.microphonePeak = local.micPeak;
            if (!local.captureAvailable) status.audioError = local.captureError;
            const auto states = client_->peerStates();
            for (const auto& state : states) {
                const auto meta = peers_.find(state.memberId);
                if (meta == peers_.end()) continue;
                PeerStatus peer;
                peer.profileId = meta->second.profileId;
                peer.displayName = meta->second.displayName;
                peer.friendCode = meta->second.friendCode;
                peer.connected = true;
                peer.receiving = state.receiving;
                peer.speaking = state.speaking;
                peer.remoteMuted = state.remoteMuted;
                peer.remoteDeafened = state.remoteDeafened;
                peer.protocolMismatch = state.protocolMismatch;
                status.peers.push_back(std::move(peer));
            }
        }
        // Voice members still connecting are listed too.
        for (const auto& [memberId, meta] : peers_) {
            const bool listed = std::any_of(status.peers.begin(), status.peers.end(),
                                            [&](const PeerStatus& peer) { return peer.profileId == meta.profileId; });
            if (listed) continue;
            PeerStatus peer;
            peer.profileId = meta.profileId;
            peer.displayName = meta.displayName;
            peer.friendCode = meta.friendCode;
            status.peers.push_back(std::move(peer));
        }
        for (auto& peer : status.peers) {
            peer.isFriend = friends_.contains(peer.profileId);
            peer.isTeammate = teamMode_ && teammates_.contains(peer.profileId);
            peer.policyMuted = PolicyMuted(settings, peer.profileId);
            peer.volume = PlayerVolume(settings, peer.profileId);
        }
        std::sort(status.peers.begin(), status.peers.end(),
                  [](const PeerStatus& a, const PeerStatus& b) { return a.displayName < b.displayName; });
        for (const auto& player : roomPlayers_) {
            if (player.profileId == identity.profileId) {
                status.displayName = player.displayName;
                continue;
            }
            status.roomPlayers.push_back({player.profileId, player.displayName, player.friendCode, player.voiceChat});
        }
        if (admitted_) {
            status.roomId = roomId_;
            status.memberId = localMemberId_;
        }
        status.iceServers = iceServers_.size();
        std::scoped_lock lock(statusMutex_);
        status_ = std::move(status);
    }

    // Inputs, written by other threads.
    std::mutex lifecycleMutex_;
    std::mutex inputMutex_;
    Settings settings_;
    std::uint64_t settingsGeneration_ = 0;
    RoomSnapshot room_;
    SpatialState spatial_;
    bool stop_ = false;
    std::mutex identityMutex_;
    GpcmIdentityTracker identity_;
    std::atomic<bool> wantsRoom_{false};
    std::atomic<std::uint32_t> profileId_{0};
    std::atomic<bool> wantsSpatial_{false};
    std::atomic<bool> pushToTalkHeld_{false};
    std::atomic<bool> refreshDevices_{false};
    std::atomic<bool> microphoneTest_{false};
    std::atomic<bool> testTone_{false};
    std::mutex statusMutex_;
    Status status_;
    std::mutex wakeMutex_;
    std::condition_variable wakeCv_;
    bool woken_ = false;
    std::thread worker_;

    // Worker-thread state.
    std::uint64_t appliedSettingsGeneration_ = ~0ull;
    bool settingsChanged_ = false;
    std::unique_ptr<mkwvc::SignalingClient> signaling_;
    std::unique_ptr<mkwvc::VoiceClient> client_;
    std::unique_ptr<mkwvc::AudioEngine> toneEngine_;
    Clock::time_point toneUntil_{};
    mkwvc::AudioEngineOptions appliedAudio_;
    State state_ = State::Disabled;
    std::string message_;
    std::string audioError_;
    std::vector<std::string> inputDevices_;
    std::vector<std::string> outputDevices_;
    bool devicesListed_ = false;
    std::uint32_t sessionProfileId_ = 0;
    std::uint64_t sessionGeneration_ = 0;
    std::uint32_t sessionRoomKey_ = 0;
    std::vector<std::uint32_t> roomProfileIds_;
    Clock::time_point nextConnect_{};
    bool admitted_ = false;
    bool admitPending_ = false;
    Clock::time_point admitRetryAt_{};
    std::string localMemberId_;
    std::string roomInstanceId_;
    std::string roomId_;
    std::vector<protocol::RoomPlayer> roomPlayers_;
    std::vector<std::string> iceServers_{std::string(protocol::kDefaultStunServer)};
    std::map<std::string, PeerMetadata> peers_;
    std::map<std::string, PeerLink> links_;
    std::unordered_set<std::uint32_t> friends_;
    bool teamMode_ = false;
    std::unordered_set<std::uint32_t> teamKnown_;
    std::unordered_set<std::uint32_t> teammates_;
    std::set<std::uint32_t> autoMutedProfiles_;
    std::set<std::uint32_t> everSeenProfiles_;
};

Service& GetService() {
    // Never destroyed: Shutdown() joins the worker, and nothing may run after static teardown.
    static auto* service = new Service();
    return *service;
}

} // namespace

bool Available() noexcept { return true; }
const char* DefaultSignalingUrl() noexcept { return kDefaultSignalingUrl; }

void Start(const Settings& settings) noexcept {
    try {
        GetService().Start(settings);
    } catch (...) {
    }
}

void Shutdown() noexcept {
    try {
        GetService().Shutdown();
    } catch (...) {
    }
}

void ApplySettings(const Settings& settings) noexcept {
    try {
        GetService().ApplySettings(settings);
    } catch (...) {
    }
}

namespace {

// Guest sockets whose connect aimed at the GPCM port, one bit per socket (the HLE has 24). Read by
// every TCP send and receive, so the check for any other socket is one load.
std::atomic<std::uint64_t> g_gpcmSockets{0};

bool IsGpcmSocket(std::uint32_t socket, std::uint16_t peerPort) noexcept {
    if (peerPort == kGpcmPort) return true;
    return socket < 64 && ((g_gpcmSockets.load(std::memory_order_relaxed) >> socket) & 1u) != 0;
}

} // namespace

void ObserveTcpConnect(std::uint32_t socket, std::uint16_t peerPort) noexcept {
    if (socket >= 64) return;
    const std::uint64_t bit = std::uint64_t{1} << socket;
    if (peerPort == kGpcmPort) {
        g_gpcmSockets.fetch_or(bit, std::memory_order_relaxed);
    } else {
        g_gpcmSockets.fetch_and(~bit, std::memory_order_relaxed);
    }
}

void ObserveTcpSend(std::uint32_t socket, std::uint16_t peerPort, const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size == 0 || !IsGpcmSocket(socket, peerPort)) return;
    try {
        GetService().ObserveSend(socket, data, size);
    } catch (...) {
    }
}

void ObserveTcpReceive(std::uint32_t socket, std::uint16_t peerPort, const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size == 0 || !IsGpcmSocket(socket, peerPort)) return;
    try {
        GetService().ObserveReceive(socket, data, size);
    } catch (...) {
    }
}

void ObserveSocketClosed(std::uint32_t socket) noexcept {
    ObserveTcpConnect(socket, 0);
    try {
        GetService().ObserveClosed(socket);
    } catch (...) {
    }
}

std::uint32_t CurrentProfileId() noexcept { return GetService().ProfileId(); }

bool WantsRoomSnapshot() noexcept { return GetService().WantsRoom(); }

void PostRoomSnapshot(const RoomSnapshot& snapshot) noexcept {
    try {
        GetService().PostRoom(snapshot);
    } catch (...) {
    }
}

bool WantsSpatialSnapshot() noexcept { return GetService().WantsSpatial(); }

void PostSpatialSnapshot(const SpatialSnapshot& snapshot) noexcept {
    try {
        GetService().PostSpatial(snapshot);
    } catch (...) {
    }
}

void ClearSpatialSnapshot() noexcept {
    try {
        GetService().ClearSpatial();
    } catch (...) {
    }
}

void SetPushToTalkHeld(bool held) noexcept { GetService().SetPushToTalkHeld(held); }

Status GetStatus() { return GetService().GetStatus(); }

void RefreshDevices() noexcept { GetService().RequestDeviceRefresh(); }
void SetMicrophoneTest(bool enabled) noexcept { GetService().SetMicrophoneTest(enabled); }
void PlayTestTone() noexcept { GetService().RequestTestTone(); }

} // namespace mkw::voicechat
