// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0); the
// values now come from the shared protocol header.

#pragma once

#include "voicechat/voice_protocol.h"

#include <cstddef>
#include <cstdint>

namespace mkwvc {

struct VoiceFormat {
    static constexpr std::uint32_t SampleRate = mkw::voicechat::protocol::kSampleRate;
    static constexpr std::uint32_t Channels = 1;
    static constexpr std::uint32_t PlaybackChannels = 2;
    static constexpr std::uint32_t FrameDurationMs = mkw::voicechat::protocol::kFrameDurationMs;
    static constexpr std::size_t FrameSamples = mkw::voicechat::protocol::kFrameSamples;
    static constexpr std::size_t PacketHeaderBytes = mkw::voicechat::protocol::kPacketHeaderBytes;
    static constexpr std::size_t MaxOpusPacketBytes = mkw::voicechat::protocol::kMaxOpusPacketBytes;
    static constexpr std::size_t MaxVoicePacketBytes = mkw::voicechat::protocol::kMaxVoicePacketBytes;
};

} // namespace mkwvc
