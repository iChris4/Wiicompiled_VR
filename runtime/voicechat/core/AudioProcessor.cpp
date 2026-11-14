// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#include "AudioProcessor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <rnnoise.h>
#include <mutex>

namespace mkwvc {

namespace {

float rmsOf(std::span<const std::int16_t> samples) {
    if(samples.empty()) return 0.0f;
    double energy=0.0;
    for(const auto sample:samples) energy+=static_cast<double>(sample)*static_cast<double>(sample);
    return static_cast<float>(std::sqrt(energy/static_cast<double>(samples.size())));
}

float peakOf(std::span<const std::int16_t> samples) {
    float peak=0.0f;
    for(const auto sample:samples) peak=std::max(peak,std::abs(static_cast<float>(sample)));
    return peak;
}

float smoothingCoefficient(float milliseconds) {
    if(milliseconds<=0.0f) return 1.0f;
    const float samples=milliseconds*0.001f*static_cast<float>(VoiceFormat::SampleRate);
    return 1.0f-std::exp(-1.0f/std::max(samples,1.0f));
}

float gainFromDb(float db) {
    return std::pow(10.0f,db/20.0f);
}

constexpr std::size_t SuppressionFrameSamples=480;
static_assert(VoiceFormat::SampleRate==48000);
static_assert(VoiceFormat::FrameSamples%SuppressionFrameSamples==0);

void initializeSuppression() {
    static std::once_flag initialized;
    std::call_once(initialized,[] {
        std::unique_ptr<DenoiseState,decltype(&rnnoise_destroy)> state(rnnoise_create(nullptr),rnnoise_destroy);
        std::array<float,SuppressionFrameSamples> silence{};
        rnnoise_process_frame(state.get(),silence.data(),silence.data());
    });
}

}

class AudioProcessor::Impl {
public:
    Impl() {
        initializeSuppression();
        resetSuppressionLocked();
    }

    ~Impl()=default;

    void setSettings(const AudioProcessingSettings& settings) {
        std::scoped_lock lock(mutex_);
        auto next=settings;
        next.noiseSuppressionStrength=std::clamp(next.noiseSuppressionStrength,0,100);
        next.noiseGateThreshold=std::clamp(next.noiseGateThreshold,0,100);
        next.microphoneBoost=std::clamp(next.microphoneBoost,1.0f,3.0f);
        next.compressorStrength=std::clamp(next.compressorStrength,0,100);
        if(next==settings_) return;

        const bool suppressionWasActive=
            settings_.noiseSuppression &&
            settings_.noiseSuppressionStrength>0;
        const bool suppressionWillBeActive=
            next.noiseSuppression &&
            next.noiseSuppressionStrength>0;
        settings_=next;
        if(suppressionWasActive!=suppressionWillBeActive) {
            resetSuppressionLocked();
        }
        if(!settings_.normalization) normalizationGain_=1.0f;
        if(!settings_.noiseGate || settings_.noiseGateThreshold==0) {
            gateGain_=1.0f;
            gateOpen_=true;
            gateHoldFrames_=0;
        }
        if(!settings_.compressor || settings_.compressorStrength==0) {
            compressorEnvelope_=0.0f;
            compressorGain_=1.0f;
        }
    }

    AudioProcessingSettings settings() const {
        std::scoped_lock lock(mutex_);
        return settings_;
    }

    void processCapture(std::span<std::int16_t> samples) {
        if(samples.size()!=VoiceFormat::FrameSamples) return;

        std::scoped_lock lock(mutex_);

        if(settings_.normalization) {
            const float rms=rmsOf(samples);
            const float peak=peakOf(samples);
            constexpr float gateRms=350.0f;
            constexpr float targetRms=5200.0f;
            constexpr float ceiling=30000.0f;
            const float previousGain=normalizationGain_;

            if(rms<gateRms) {
                normalizationGain_+=(1.0f-normalizationGain_)*0.035f;
            } else {
                const float targetGain=std::clamp(
                    targetRms/std::max(rms,1.0f),
                    0.55f,
                    2.5f);
                const float smoothing=
                    targetGain<normalizationGain_ ? 0.16f : 0.025f;
                normalizationGain_+=(targetGain-normalizationGain_)*smoothing;
            }

            if(peak>0.0f) {
                normalizationGain_=std::min(
                    normalizationGain_,
                    ceiling/peak);
            }
            normalizationGain_=std::clamp(normalizationGain_,0.35f,2.5f);

            if(std::abs(previousGain-1.0f)>=0.001f ||
               std::abs(normalizationGain_-1.0f)>=0.001f) {
                const float denominator=samples.size()>1
                    ? static_cast<float>(samples.size()-1)
                    : 1.0f;
                for(std::size_t i=0;i<samples.size();++i) {
                    const float t=static_cast<float>(i)/denominator;
                    const float gain=
                        previousGain+(normalizationGain_-previousGain)*t;
                    const auto scaled=static_cast<std::int32_t>(
                        std::lround(static_cast<float>(samples[i])*gain));
                    samples[i]=static_cast<std::int16_t>(
                        std::clamp(scaled,-32768,32767));
                }
            }
        } else {
            normalizationGain_=1.0f;
        }

        const float gateDetectorRms=rmsOf(samples);

        if(settings_.noiseGate && settings_.noiseGateThreshold>0) {
            const float strength=static_cast<float>(settings_.noiseGateThreshold)/100.0f;
            const float rms=gateDetectorRms;
            const float openThreshold=
                4.0f+std::pow(strength,2.2f)*2200.0f;
            const float closeThreshold=openThreshold*0.60f;

            if(gateOpen_) {
                if(rms>=closeThreshold) {
                    gateHoldFrames_=6;
                } else if(gateHoldFrames_>0) {
                    --gateHoldFrames_;
                } else {
                    gateOpen_=false;
                }
            } else if(rms>=openThreshold) {
                gateOpen_=true;
                gateHoldFrames_=6;
            }

            const float target=gateOpen_ ? 1.0f : 0.0f;
            const float attack=smoothingCoefficient(3.0f);
            const float release=smoothingCoefficient(150.0f);
            for(auto& sample:samples) {
                const float coefficient=target>gateGain_ ? attack : release;
                gateGain_+=(target-gateGain_)*coefficient;
                sample=static_cast<std::int16_t>(std::clamp(
                    static_cast<std::int32_t>(
                        std::lround(static_cast<float>(sample)*gateGain_)),
                    -32768,
                    32767));
            }
        } else {
            gateGain_=1.0f;
            gateOpen_=true;
            gateHoldFrames_=0;
        }

        const float boost=settings_.microphoneBoost;
        const bool compress=
            settings_.compressor &&
            settings_.compressorStrength>0;
        if(boost!=1.0f || compress) {
            const float strength=
                static_cast<float>(settings_.compressorStrength)/100.0f;
            const float thresholdDb=-10.0f-20.0f*strength;
            const float threshold=
                32768.0f*gainFromDb(thresholdDb);
            const float ratio=1.0f+7.0f*strength;
            const float makeup=gainFromDb(6.0f*strength);
            const float envelopeAttack=smoothingCoefficient(4.0f);
            const float envelopeRelease=smoothingCoefficient(120.0f);
            const float gainAttack=smoothingCoefficient(8.0f);
            const float gainRelease=smoothingCoefficient(140.0f);

            std::array<float,VoiceFormat::FrameSamples> dynamics{};
            float dynamicsPeak=0.0f;
            for(std::size_t i=0;i<samples.size();++i) {
                float value=static_cast<float>(samples[i])*boost;

                if(compress) {
                    const float magnitude=std::abs(value);
                    const float envelopeCoefficient=
                        magnitude>compressorEnvelope_
                            ? envelopeAttack
                            : envelopeRelease;
                    compressorEnvelope_+=
                        (magnitude-compressorEnvelope_)*envelopeCoefficient;

                    float targetGain=1.0f;
                    if(compressorEnvelope_>threshold) {
                        const float compressedEnvelope=
                            threshold*std::pow(
                                compressorEnvelope_/threshold,
                                1.0f/ratio);
                        targetGain=
                            compressedEnvelope/
                            std::max(compressorEnvelope_,1.0f);
                    }

                    const float gainCoefficient=
                        targetGain<compressorGain_
                            ? gainAttack
                            : gainRelease;
                    compressorGain_+=
                        (targetGain-compressorGain_)*gainCoefficient;
                    value*=compressorGain_*makeup;
                }

                dynamics[i]=value;
                dynamicsPeak=std::max(dynamicsPeak,std::abs(value));
            }

            constexpr float dynamicsCeiling=30000.0f;
            const float limiter=
                dynamicsPeak>dynamicsCeiling
                    ? dynamicsCeiling/dynamicsPeak
                    : 1.0f;
            for(std::size_t i=0;i<samples.size();++i) {
                const auto value=static_cast<std::int32_t>(
                    std::lround(dynamics[i]*limiter));
                samples[i]=static_cast<std::int16_t>(
                    std::clamp(value,-32768,32767));
            }
        } else {
            compressorEnvelope_=0.0f;
            compressorGain_=1.0f;
        }
    }

    void processPostGainSuppression(std::span<std::int16_t> samples) {
        if(samples.size()!=VoiceFormat::FrameSamples) return;

        std::scoped_lock lock(mutex_);
        if(!settings_.noiseSuppression || settings_.noiseSuppressionStrength<=0) {
            return;
        }

        const float targetMix=static_cast<float>(settings_.noiseSuppressionStrength)/100.0f;
        for(std::size_t offset=0;offset<samples.size();offset+=SuppressionFrameSamples) {
            std::array<float,SuppressionFrameSamples> input{};
            std::array<float,SuppressionFrameSamples> output{};
            for(std::size_t i=0;i<SuppressionFrameSamples;++i) input[i]=static_cast<float>(samples[offset+i]);
            rnnoise_process_frame(denoiser_.get(),output.data(),input.data());
            for(std::size_t i=0;i<SuppressionFrameSamples;++i) {
                const float mix=suppressionMix_+(targetMix-suppressionMix_)*static_cast<float>(i+1)/static_cast<float>(SuppressionFrameSamples);
                const float value=suppressionDry_[i]+mix*(output[i]-suppressionDry_[i]);
                samples[offset+i]=static_cast<std::int16_t>(std::clamp(std::lround(value),-32768L,32767L));
            }
            std::copy_n(suppressionDry_.begin()+SuppressionFrameSamples,SuppressionFrameSamples,suppressionDry_.begin());
            std::copy(input.begin(),input.end(),suppressionDry_.begin()+SuppressionFrameSamples);
            suppressionMix_=targetMix;
        }
    }

private:
    void resetSuppressionLocked() {
        denoiser_.reset(rnnoise_create(nullptr));
        suppressionDry_.fill(0.0f);
        suppressionMix_=static_cast<float>(settings_.noiseSuppressionStrength)/100.0f;
    }

    mutable std::mutex mutex_;
    AudioProcessingSettings settings_{};
    std::unique_ptr<DenoiseState,decltype(&rnnoise_destroy)> denoiser_{nullptr,rnnoise_destroy};
    std::array<float,SuppressionFrameSamples*2> suppressionDry_{};
    float suppressionMix_=0.0f;
    float normalizationGain_=1.0f;
    float gateGain_=1.0f;
    bool gateOpen_=true;
    int gateHoldFrames_=0;
    float compressorEnvelope_=0.0f;
    float compressorGain_=1.0f;
};

AudioProcessor::AudioProcessor():impl_(std::make_unique<Impl>()) {}
AudioProcessor::~AudioProcessor()=default;
void AudioProcessor::setSettings(const AudioProcessingSettings& settings){impl_->setSettings(settings);}
AudioProcessingSettings AudioProcessor::settings() const{return impl_->settings();}
void AudioProcessor::processCapture(std::span<std::int16_t> samples){impl_->processCapture(samples);}
void AudioProcessor::processPostGainSuppression(std::span<std::int16_t> samples){impl_->processPostGainSuppression(samples);}

}

