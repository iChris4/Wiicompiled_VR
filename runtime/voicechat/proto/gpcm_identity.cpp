// SPDX-License-Identifier: GPL-3.0-or-later

#include "voicechat/gpcm_identity.h"

#include <charconv>

namespace mkw::voicechat {

namespace {

constexpr std::string_view kFinal = "\\final\\";
// A GPCM message is a few hundred bytes; anything without "\final\" after this much is not GPCM.
constexpr std::size_t kMaxBuffered = 16 * 1024;

} // namespace

std::string_view GpcmIdentityTracker::FindValue(std::string_view message, std::string_view key) noexcept {
    // Walk "\k\v\k\v..." pairs from the start so a key is never matched inside a value.
    std::size_t pos = 0;
    while (pos < message.size() && message[pos] == '\\') {
        const std::size_t keyEnd = message.find('\\', pos + 1);
        if (keyEnd == std::string_view::npos) {
            return {};
        }
        const std::size_t valueEnd = message.find('\\', keyEnd + 1);
        const std::string_view currentKey = message.substr(pos + 1, keyEnd - pos - 1);
        const std::string_view value = message.substr(
            keyEnd + 1, valueEnd == std::string_view::npos ? std::string_view::npos : valueEnd - keyEnd - 1);
        if (currentKey == key) {
            return value;
        }
        if (valueEnd == std::string_view::npos) {
            return {};
        }
        pos = valueEnd;
    }
    return {};
}

std::string_view GpcmIdentityTracker::Command(std::string_view message) noexcept {
    if (message.empty() || message[0] != '\\') {
        return {};
    }
    const std::size_t end = message.find('\\', 1);
    return end == std::string_view::npos ? std::string_view{} : message.substr(1, end - 1);
}

template <typename Handler>
void GpcmIdentityTracker::Feed(std::string& buffer, std::span<const std::uint8_t> bytes, Handler&& handler) {
    buffer.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // Every message starts with a backslash; resynchronise on the next one after any junk.
    const std::size_t start = buffer.find('\\');
    if (start == std::string::npos) {
        buffer.clear();
        return;
    }
    buffer.erase(0, start);
    while (true) {
        const std::size_t end = buffer.find(kFinal);
        if (end == std::string::npos) {
            break;
        }
        const std::size_t length = end + kFinal.size();
        handler(std::string_view(buffer.data(), length));
        buffer.erase(0, length);
    }
    if (buffer.size() > kMaxBuffered) {
        buffer.erase(0, buffer.size() - kFinal.size());
    }
}

bool GpcmIdentityTracker::OnSend(std::uint32_t socket, std::span<const std::uint8_t> bytes) {
    bool changed = false;
    Feed(streams_[socket].send, bytes,
         [&](std::string_view message) { changed = HandleSent(socket, message) || changed; });
    return changed;
}

bool GpcmIdentityTracker::OnReceive(std::uint32_t socket, std::span<const std::uint8_t> bytes) {
    bool changed = false;
    Feed(streams_[socket].receive, bytes,
         [&](std::string_view message) { changed = HandleReceived(socket, message) || changed; });
    return changed;
}

bool GpcmIdentityTracker::OnClosed(std::uint32_t socket) {
    streams_.erase(socket);
    if (haveIdentitySocket_ && identitySocket_ == socket) {
        return ClearIdentity();
    }
    return false;
}

bool GpcmIdentityTracker::HandleSent(std::uint32_t socket, std::string_view message) {
    if (Command(message) == "logout" && haveIdentitySocket_ && identitySocket_ == socket) {
        return ClearIdentity();
    }
    return false;
}

bool GpcmIdentityTracker::HandleReceived(std::uint32_t socket, std::string_view message) {
    // "\lc\2\" is the successful login challenge reply (\lc\1\ is the server challenge).
    if (Command(message) != "lc" || FindValue(message, "lc") != "2") {
        return false;
    }
    const std::string_view text = FindValue(message, "profileid");
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || text.size() > 10 || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value == 0 || value > 0xFFFFFFFFull) {
        return false;
    }
    const auto profileId = static_cast<std::uint32_t>(value);
    identitySocket_ = socket;
    haveIdentitySocket_ = true;
    if (identity_.online && identity_.profileId == profileId) {
        return false;
    }
    identity_.online = true;
    identity_.profileId = profileId;
    ++identity_.generation;
    return true;
}

bool GpcmIdentityTracker::ClearIdentity() {
    haveIdentitySocket_ = false;
    if (!identity_.online) {
        return false;
    }
    identity_.online = false;
    identity_.profileId = 0;
    ++identity_.generation;
    return true;
}

} // namespace mkw::voicechat
