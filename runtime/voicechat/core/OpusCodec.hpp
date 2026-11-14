// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace mkwvc {

struct OpusCodecSettings {
    int bitrate=48000;
    int complexity=10;
    int expectedPacketLossPercent=5;
    bool inbandFec=true;
    bool dtx=false;

    bool operator==(const OpusCodecSettings&) const = default;
};

class OpusEncoderCodec {
public:
    OpusEncoderCodec();
    ~OpusEncoderCodec();

    OpusEncoderCodec(const OpusEncoderCodec&)=delete;
    OpusEncoderCodec& operator=(const OpusEncoderCodec&)=delete;

    void configure(const OpusCodecSettings& settings);
    std::size_t encode(std::span<const std::int16_t> samples,std::span<std::byte> output);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class OpusDecoderCodec {
public:
    OpusDecoderCodec();
    ~OpusDecoderCodec();

    OpusDecoderCodec(const OpusDecoderCodec&)=delete;
    OpusDecoderCodec& operator=(const OpusDecoderCodec&)=delete;

    void reset();
    std::size_t decode(std::span<const std::byte> packet,std::span<std::int16_t> output,bool decodeFec=false);
    std::size_t conceal(std::span<std::int16_t> output);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
