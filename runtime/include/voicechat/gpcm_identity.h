// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who is logged in to the online service, learned by reading the game's GameSpy presence (GPCM)
// traffic as it passes through the socket HLE. GPCM is plain text on TCP port 29900: messages are
// backslash-separated key/value pairs ending in "\final\". A successful login reply starts
// "\lc\2\" and carries "\profileid\<n>\"; "\logout\" from the game, or the socket closing, ends
// the session.
//
// Read-only by design: the bytes are copied for parsing and never changed, so the game sees
// exactly what it would without voice chat. Only the profile id is kept; the session key in the
// same reply is skipped.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mkw::voicechat {

inline constexpr std::uint16_t kGpcmPort = 29900;

struct GpcmIdentity {
    bool online = false;
    std::uint32_t profileId = 0;
    // Bumped by every login and logout, so a consumer can tell two sessions with the same profile
    // apart.
    std::uint64_t generation = 0;
};

// Not thread-safe; the caller serializes.
class GpcmIdentityTracker {
public:
    // Each returns true when the identity changed.
    bool OnSend(std::uint32_t socket, std::span<const std::uint8_t> bytes);
    bool OnReceive(std::uint32_t socket, std::span<const std::uint8_t> bytes);
    bool OnClosed(std::uint32_t socket);

    const GpcmIdentity& Current() const noexcept { return identity_; }

    // Value of `key` in one GPCM message ("\k1\v1\k2\v2..."), or empty.
    static std::string_view FindValue(std::string_view message, std::string_view key) noexcept;
    // The first key of a message (its command), e.g. "lc", "login", "logout".
    static std::string_view Command(std::string_view message) noexcept;

private:
    struct Stream {
        std::string send;
        std::string receive;
    };

    template <typename Handler>
    static void Feed(std::string& buffer, std::span<const std::uint8_t> bytes, Handler&& handler);
    bool HandleSent(std::uint32_t socket, std::string_view message);
    bool HandleReceived(std::uint32_t socket, std::string_view message);
    bool ClearIdentity();

    std::unordered_map<std::uint32_t, Stream> streams_;
    GpcmIdentity identity_;
    std::uint32_t identitySocket_ = 0;
    bool haveIdentitySocket_ = false;
};

} // namespace mkw::voicechat
