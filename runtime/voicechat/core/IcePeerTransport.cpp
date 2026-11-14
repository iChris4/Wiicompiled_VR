// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#include "IcePeerTransport.hpp"

#include "voicechat/voice_protocol.h"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

namespace mkwvc {

namespace {

constexpr std::size_t MaxQueuedPackets=256;
constexpr auto VoicePacketLifetime=std::chrono::milliseconds(mkw::voicechat::protocol::kDataChannelLifetimeMs);

}

class IcePeerTransport::Impl {
public:
    Impl(std::vector<std::string> iceServers,bool forceRelay) {
        rtc::Configuration config;
        config.disableAutoNegotiation=true;
        config.iceTransportPolicy=forceRelay ? rtc::TransportPolicy::Relay : rtc::TransportPolicy::All;
        for(auto& server:iceServers) config.iceServers.emplace_back(std::move(server));

        peer_=std::make_shared<rtc::PeerConnection>(std::move(config));

        peer_->onLocalDescription([this](rtc::Description description) {
            std::scoped_lock lock(mutex_);
            localDescription_=IceDescriptionSignal{std::string(description),description.typeString()};
        });

        peer_->onLocalCandidate([this](rtc::Candidate candidate) {
            std::scoped_lock lock(mutex_);
            localCandidates_.push_back({candidate.candidate(),candidate.mid()});
        });

        peer_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            std::scoped_lock lock(mutex_);
            gatheringComplete_=state==rtc::PeerConnection::GatheringState::Complete;
        });

        peer_->onStateChange([this](rtc::PeerConnection::State state) {
            if(state==rtc::PeerConnection::State::Failed ||
               state==rtc::PeerConnection::State::Closed ||
               state==rtc::PeerConnection::State::Disconnected) {
                {
                    std::scoped_lock lock(mutex_);
                    channelOpen_=false;
                }
                receiveCv_.notify_all();
            }
        });

        peer_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
            attachChannel(std::move(channel));
        });
    }

    ~Impl() {
        std::shared_ptr<rtc::DataChannel> channel;
        std::shared_ptr<rtc::PeerConnection> peer;

        {
            std::scoped_lock lock(mutex_);
            closed_=true;
            channelOpen_=false;
            channel=std::move(channel_);
            peer=std::move(peer_);
        }

        receiveCv_.notify_all();

        if(channel) {
            channel->resetCallbacks();
            channel->close();
        }

        if(peer) {
            peer->resetCallbacks();
            peer->close();
        }
    }

    void beginOffer() {
        {
            std::scoped_lock lock(mutex_);
            if(channel_) return;
            gatheringComplete_=false;
        }

        rtc::DataChannelInit init;
        init.reliability.unordered=true;
        init.reliability.maxPacketLifeTime=VoicePacketLifetime;
        init.protocol=std::string(mkw::voicechat::protocol::kDataChannelProtocol);

        auto channel=peer_->createDataChannel(std::string(mkw::voicechat::protocol::kDataChannelLabel),init);
        attachChannel(std::move(channel));
        peer_->setLocalDescription(rtc::Description::Type::Offer);
    }

    void setRemoteDescription(std::string sdp,std::string type) {
        rtc::Description description(sdp,type);
        const bool isOffer=description.type()==rtc::Description::Type::Offer;
        if(isOffer) {
            std::scoped_lock lock(mutex_);
            gatheringComplete_=false;
        }
        peer_->setRemoteDescription(std::move(description));
        if(isOffer) peer_->setLocalDescription(rtc::Description::Type::Answer);
    }

    void addRemoteCandidate(std::string candidate,std::string mid) {
        peer_->addRemoteCandidate(rtc::Candidate(std::move(candidate),std::move(mid)));
    }

    std::optional<IceDescriptionSignal> takeLocalDescription() {
        std::scoped_lock lock(mutex_);
        auto value=std::move(localDescription_);
        localDescription_.reset();
        return value;
    }

    std::vector<IceCandidateSignal> takeLocalCandidates() {
        std::scoped_lock lock(mutex_);
        auto values=std::move(localCandidates_);
        localCandidates_.clear();
        return values;
    }

    bool gatheringComplete() const {
        std::scoped_lock lock(mutex_);
        return gatheringComplete_;
    }

    bool connected() const {
        std::scoped_lock lock(mutex_);
        return channelOpen_;
    }

    bool usingRelay() const {
        auto peer=peerSnapshot();
        if(!peer) return false;
        rtc::Candidate local;
        rtc::Candidate remote;
        if(!peer->getSelectedCandidatePair(&local,&remote)) return false;
        return local.type()==rtc::Candidate::Type::Relayed ||
               remote.type()==rtc::Candidate::Type::Relayed;
    }

    std::optional<std::uint32_t> rttMilliseconds() const {
        auto peer=peerSnapshot();
        if(!peer) return std::nullopt;
        const auto value=peer->rtt();
        if(!value) return std::nullopt;
        return static_cast<std::uint32_t>(std::max<std::int64_t>(0,value->count()));
    }

    std::string localAddress() const {
        auto peer=peerSnapshot();
        if(!peer) return {};
        const auto address=peer->localAddress();
        return address ? *address : std::string{};
    }

    std::string remoteAddress() const {
        auto peer=peerSnapshot();
        if(!peer) return {};
        const auto address=peer->remoteAddress();
        return address ? *address : std::string{};
    }

    bool send(std::span<const std::byte> packet) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::scoped_lock lock(mutex_);
            if(!channelOpen_ || !channel_) return false;
            channel=channel_;
        }
        return channel->send(reinterpret_cast<const rtc::byte*>(packet.data()),packet.size());
    }

    std::size_t receive(std::span<std::byte> packet) {
        std::unique_lock lock(mutex_);
        receiveCv_.wait_for(lock,std::chrono::milliseconds(5),[this] {
            return closed_ || !receivedPackets_.empty();
        });
        return popReceivedLocked(lock,packet);
    }

    std::size_t tryReceive(std::span<std::byte> packet) {
        std::unique_lock lock(mutex_);
        return popReceivedLocked(lock,packet);
    }

private:
    std::size_t popReceivedLocked(std::unique_lock<std::mutex>& lock,std::span<std::byte> packet) {
        if(receivedPackets_.empty()) return 0;

        auto received=std::move(receivedPackets_.front());
        receivedPackets_.pop_front();
        lock.unlock();

        if(received.size()>packet.size()) return 0;
        std::copy(received.begin(),received.end(),packet.begin());
        return received.size();
    }

    std::shared_ptr<rtc::PeerConnection> peerSnapshot() const {
        std::scoped_lock lock(mutex_);
        return peer_;
    }

    void attachChannel(std::shared_ptr<rtc::DataChannel> channel) {
        if(!channel) return;

        std::shared_ptr<rtc::DataChannel> previous;
        {
            std::scoped_lock lock(mutex_);
            previous=std::exchange(channel_,channel);
            channelOpen_=channel->isOpen();
        }

        if(previous && previous!=channel) previous->resetCallbacks();

        channel->onOpen([this] {
            std::scoped_lock lock(mutex_);
            if(!closed_) channelOpen_=true;
        });

        channel->onClosed([this] {
            {
                std::scoped_lock lock(mutex_);
                channelOpen_=false;
            }
            receiveCv_.notify_all();
        });

        channel->onError([this](std::string) {
            {
                std::scoped_lock lock(mutex_);
                channelOpen_=false;
            }
            receiveCv_.notify_all();
        });

        channel->onMessage([this](rtc::message_variant message) {
            const auto* binary=std::get_if<rtc::binary>(&message);
            if(!binary || binary->empty()) return;

            std::vector<std::byte> packet(binary->begin(),binary->end());
            {
                std::scoped_lock lock(mutex_);
                if(closed_) return;
                if(receivedPackets_.size()>=MaxQueuedPackets) receivedPackets_.pop_front();
                receivedPackets_.push_back(std::move(packet));
            }
            receiveCv_.notify_one();
        });
    }

    mutable std::mutex mutex_;
    std::condition_variable receiveCv_;
    std::shared_ptr<rtc::PeerConnection> peer_;
    std::shared_ptr<rtc::DataChannel> channel_;
    std::deque<std::vector<std::byte>> receivedPackets_;
    std::optional<IceDescriptionSignal> localDescription_;
    std::vector<IceCandidateSignal> localCandidates_;
    bool gatheringComplete_=false;
    bool channelOpen_=false;
    bool closed_=false;
};

IcePeerTransport::IcePeerTransport(std::vector<std::string> iceServers,bool forceRelay):
    impl_(std::make_unique<Impl>(std::move(iceServers),forceRelay)) {}

IcePeerTransport::~IcePeerTransport()=default;
void IcePeerTransport::beginOffer(){impl_->beginOffer();}
void IcePeerTransport::setRemoteDescription(std::string sdp,std::string type){impl_->setRemoteDescription(std::move(sdp),std::move(type));}
void IcePeerTransport::addRemoteCandidate(std::string candidate,std::string mid){impl_->addRemoteCandidate(std::move(candidate),std::move(mid));}
std::optional<IceDescriptionSignal> IcePeerTransport::takeLocalDescription(){return impl_->takeLocalDescription();}
std::vector<IceCandidateSignal> IcePeerTransport::takeLocalCandidates(){return impl_->takeLocalCandidates();}
bool IcePeerTransport::gatheringComplete() const{return impl_->gatheringComplete();}
bool IcePeerTransport::connected() const{return impl_->connected();}
bool IcePeerTransport::usingRelay() const{return impl_->usingRelay();}
std::optional<std::uint32_t> IcePeerTransport::rttMilliseconds() const{return impl_->rttMilliseconds();}
std::string IcePeerTransport::remoteAddress() const{return impl_->remoteAddress();}
bool IcePeerTransport::send(std::span<const std::byte> packet){return impl_->send(packet);}
std::size_t IcePeerTransport::receive(std::span<std::byte> packet){return impl_->receive(packet);}
std::size_t IcePeerTransport::tryReceive(std::span<std::byte> packet){return impl_->tryReceive(packet);}
std::string IcePeerTransport::localAddress() const{return impl_->localAddress();}

}
