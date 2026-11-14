// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#include "SignalingClient.hpp"

#include <rtc/rtc.hpp>

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>

namespace mkwvc {

namespace protocol = mkw::voicechat::protocol;

class SignalingClient::Impl {
public:
    Impl(std::string serverUrl, std::string caCertificateFile) {
        if (serverUrl.empty()) throw std::invalid_argument("Signaling server URL is empty");

        rtc::WebSocket::Configuration config;
        config.connectionTimeout = std::chrono::seconds(5);
        config.pingInterval = std::chrono::seconds(3);
        config.maxOutstandingPings = 2;
        if (!caCertificateFile.empty()) config.caCertificatePemFile = std::move(caCertificateFile);
        socket_ = std::make_shared<rtc::WebSocket>(std::move(config));

        socket_->onOpen([this] {
            std::vector<std::string> pending;
            std::shared_ptr<rtc::WebSocket> socket;
            {
                std::scoped_lock lock(mutex_);
                if (closed_) return;
                open_ = true;
                socket = socket_;
                pending.swap(pending_);
                events_.push_back({SignalingEvent::Type::Open, {}, {}});
            }
            for (const auto& message : pending) {
                if (!socket->send(message)) pushError("Failed to send a signaling message");
            }
        });
        socket_->onMessage([this](rtc::message_variant message) {
            const auto* text = std::get_if<std::string>(&message);
            if (!text) return;
            auto parsed = protocol::ParseServerMessage(*text);
            if (parsed.kind == protocol::MessageKind::Unknown) return;
            std::scoped_lock lock(mutex_);
            if (parsed.kind == protocol::MessageKind::DevAdmitted) {
                heartbeat_ = true;
                nextHeartbeat_ = std::chrono::steady_clock::now() + kHeartbeat;
            } else if (parsed.kind == protocol::MessageKind::DevAdmitFailed) {
                heartbeat_ = false;
            }
            events_.push_back({SignalingEvent::Type::Message, std::move(parsed), {}});
        });
        socket_->onError([this](std::string error) { pushError(std::move(error)); });
        socket_->onClosed([this] {
            std::scoped_lock lock(mutex_);
            open_ = false;
            closed_ = true;
            events_.push_back({SignalingEvent::Type::Closed, {}, {}});
        });
        socket_->open(serverUrl);
    }

    ~Impl() {
        std::shared_ptr<rtc::WebSocket> socket;
        {
            std::scoped_lock lock(mutex_);
            closed_ = true;
            open_ = false;
            socket = std::move(socket_);
            pending_.clear();
        }
        if (socket) {
            socket->resetCallbacks();
            socket->close();
        }
    }

    void admit(std::uint32_t profileId, std::span<const std::uint32_t> roomProfileIds) {
        send(protocol::BuildDevAdmit(profileId, roomProfileIds));
    }

    void sendSignal(const std::string& memberId, const std::string& bundle) {
        auto message = protocol::BuildSignal(memberId, bundle);
        if (message.empty()) throw std::invalid_argument("Invalid signal target");
        send(std::move(message));
    }

    bool open() const {
        std::scoped_lock lock(mutex_);
        return open_;
    }

    void service() {
        std::shared_ptr<rtc::WebSocket> socket;
        {
            std::scoped_lock lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            if (closed_ || !open_ || !heartbeat_ || now < nextHeartbeat_) return;
            nextHeartbeat_ = now + kHeartbeat;
            socket = socket_;
        }
        // The Worker drops a room member after 90 s without ALIVE.
        if (socket && !socket->send(std::string("ALIVE"))) pushError("Signaling heartbeat failed");
    }

    std::vector<SignalingEvent> takeEvents() {
        std::scoped_lock lock(mutex_);
        return std::exchange(events_, {});
    }

private:
    static constexpr auto kHeartbeat = std::chrono::seconds(60);

    void send(std::string message) {
        std::shared_ptr<rtc::WebSocket> socket;
        {
            std::scoped_lock lock(mutex_);
            if (closed_) throw std::runtime_error("Signaling connection is closed");
            if (!open_) {
                pending_.push_back(std::move(message));
                return;
            }
            socket = socket_;
        }
        if (!socket || !socket->send(message)) throw std::runtime_error("Failed to send a signaling message");
    }

    void pushError(std::string error) {
        std::scoped_lock lock(mutex_);
        events_.push_back({SignalingEvent::Type::TransportError, {}, std::move(error)});
    }

    mutable std::mutex mutex_;
    std::shared_ptr<rtc::WebSocket> socket_;
    std::vector<std::string> pending_;
    std::vector<SignalingEvent> events_;
    bool open_ = false;
    bool closed_ = false;
    bool heartbeat_ = false;
    std::chrono::steady_clock::time_point nextHeartbeat_{};
};

SignalingClient::SignalingClient(std::string serverUrl, std::string caCertificateFile)
    : impl_(std::make_unique<Impl>(std::move(serverUrl), std::move(caCertificateFile))) {}
SignalingClient::~SignalingClient() = default;
void SignalingClient::admit(std::uint32_t profileId, std::span<const std::uint32_t> roomProfileIds) {
    impl_->admit(profileId, roomProfileIds);
}
void SignalingClient::sendSignal(const std::string& memberId, const std::string& bundle) {
    impl_->sendSignal(memberId, bundle);
}
bool SignalingClient::open() const { return impl_->open(); }
void SignalingClient::service() { impl_->service(); }
std::vector<SignalingEvent> SignalingClient::takeEvents() { return impl_->takeEvents(); }

} // namespace mkwvc
