// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#include "OpusCodec.hpp"
#include "VoiceFormat.hpp"

#if __has_include(<opus/opus.h>)
#include <opus/opus.h>
#elif __has_include(<opus.h>)
#include <opus.h>
#else
#error "Opus headers were not found"
#endif

#include <algorithm>
#include <stdexcept>
#include <string>

namespace mkwvc {

namespace {

void requireOpus(int result,const char* action) {
    if(result==OPUS_OK) return;
    throw std::runtime_error(std::string(action)+": "+opus_strerror(result));
}

}

class OpusEncoderCodec::Impl {
public:
    Impl() {
        int error=OPUS_OK;
        encoder_=opus_encoder_create(static_cast<opus_int32>(VoiceFormat::SampleRate),static_cast<int>(VoiceFormat::Channels),OPUS_APPLICATION_VOIP,&error);
        if(!encoder_ || error!=OPUS_OK) throw std::runtime_error(std::string("Failed to create Opus encoder: ")+opus_strerror(error));
        configure({});
    }

    ~Impl() {
        if(encoder_) opus_encoder_destroy(encoder_);
    }

    void configure(const OpusCodecSettings& requested) {
        OpusCodecSettings settings=requested;
        settings.bitrate=std::clamp(settings.bitrate,6000,128000);
        settings.complexity=std::clamp(settings.complexity,0,10);
        settings.expectedPacketLossPercent=std::clamp(settings.expectedPacketLossPercent,0,100);

        requireOpus(opus_encoder_ctl(encoder_,OPUS_SET_BITRATE(settings.bitrate)),"Failed to set Opus bitrate");
        requireOpus(opus_encoder_ctl(encoder_,OPUS_SET_COMPLEXITY(settings.complexity)),"Failed to set Opus complexity");
        requireOpus(opus_encoder_ctl(encoder_,OPUS_SET_VBR(1)),"Failed to enable Opus VBR");
        requireOpus(opus_encoder_ctl(encoder_,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)),"Failed to set Opus voice signal");
        requireOpus(opus_encoder_ctl(encoder_,OPUS_SET_PACKET_LOSS_PERC(settings.expectedPacketLossPercent)),"Failed to set Opus expected packet loss");
        requireOpus(opus_encoder_ctl(encoder_,OPUS_SET_INBAND_FEC(settings.inbandFec ? 1 : 0)),"Failed to configure Opus FEC");
        requireOpus(opus_encoder_ctl(encoder_,OPUS_SET_DTX(settings.dtx ? 1 : 0)),"Failed to configure Opus DTX");
        settings_=settings;
    }

    std::size_t encode(std::span<const std::int16_t> samples,std::span<std::byte> output) {
        if(samples.size()!=VoiceFormat::FrameSamples) throw std::runtime_error("Opus encoder received an invalid frame size");
        if(output.empty()) throw std::runtime_error("Opus encoder output buffer is empty");

        const auto result=opus_encode(
            encoder_,
            reinterpret_cast<const opus_int16*>(samples.data()),
            static_cast<int>(samples.size()),
            reinterpret_cast<unsigned char*>(output.data()),
            static_cast<opus_int32>(output.size())
        );

        if(result<0) throw std::runtime_error(std::string("Opus encode failed: ")+opus_strerror(result));
        return static_cast<std::size_t>(result);
    }

private:
    OpusEncoder* encoder_=nullptr;
    OpusCodecSettings settings_{};
};

class OpusDecoderCodec::Impl {
public:
    Impl() {
        int error=OPUS_OK;
        decoder_=opus_decoder_create(static_cast<opus_int32>(VoiceFormat::SampleRate),static_cast<int>(VoiceFormat::Channels),&error);
        if(!decoder_ || error!=OPUS_OK) throw std::runtime_error(std::string("Failed to create Opus decoder: ")+opus_strerror(error));
    }

    ~Impl() {
        if(decoder_) opus_decoder_destroy(decoder_);
    }

    void reset() {
        requireOpus(opus_decoder_ctl(decoder_,OPUS_RESET_STATE),"Failed to reset Opus decoder");
    }

    std::size_t decode(std::span<const std::byte> packet,std::span<std::int16_t> output,bool decodeFec) {
        if(output.size()<VoiceFormat::FrameSamples) throw std::runtime_error("Opus decoder output buffer is too small");
        if(packet.empty()) throw std::runtime_error("Opus decoder received an empty packet");

        const auto result=opus_decode(
            decoder_,
            reinterpret_cast<const unsigned char*>(packet.data()),
            static_cast<opus_int32>(packet.size()),
            reinterpret_cast<opus_int16*>(output.data()),
            static_cast<int>(VoiceFormat::FrameSamples),
            decodeFec ? 1 : 0
        );

        if(result<0) throw std::runtime_error(std::string("Opus decode failed: ")+opus_strerror(result));
        return static_cast<std::size_t>(result);
    }

    std::size_t conceal(std::span<std::int16_t> output) {
        if(output.size()<VoiceFormat::FrameSamples) throw std::runtime_error("Opus PLC output buffer is too small");

        const auto result=opus_decode(
            decoder_,
            nullptr,
            0,
            reinterpret_cast<opus_int16*>(output.data()),
            static_cast<int>(VoiceFormat::FrameSamples),
            0
        );

        if(result<0) throw std::runtime_error(std::string("Opus PLC failed: ")+opus_strerror(result));
        return static_cast<std::size_t>(result);
    }

private:
    OpusDecoder* decoder_=nullptr;
};

OpusEncoderCodec::OpusEncoderCodec():impl_(std::make_unique<Impl>()) {}
OpusEncoderCodec::~OpusEncoderCodec()=default;
void OpusEncoderCodec::configure(const OpusCodecSettings& settings){impl_->configure(settings);}
std::size_t OpusEncoderCodec::encode(std::span<const std::int16_t> samples,std::span<std::byte> output){return impl_->encode(samples,output);}

OpusDecoderCodec::OpusDecoderCodec():impl_(std::make_unique<Impl>()) {}
OpusDecoderCodec::~OpusDecoderCodec()=default;
void OpusDecoderCodec::reset(){impl_->reset();}
std::size_t OpusDecoderCodec::decode(std::span<const std::byte> packet,std::span<std::int16_t> output,bool decodeFec){return impl_->decode(packet,output,decodeFec);}
std::size_t OpusDecoderCodec::conceal(std::span<std::int16_t> output){return impl_->conceal(output);}

}
