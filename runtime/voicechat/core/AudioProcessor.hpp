// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#pragma once

#include "VoiceFormat.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace mkwvc {

struct AudioProcessingSettings {
    bool normalization=true;
    bool noiseSuppression=true;
    int noiseSuppressionStrength=75;
    bool noiseGate=false;
    int noiseGateThreshold=25;
    float microphoneBoost=1.0f;
    bool compressor=true;
    int compressorStrength=50;

    bool operator==(const AudioProcessingSettings&) const = default;
};

class AudioProcessor {
public:
    AudioProcessor();
    ~AudioProcessor();

    AudioProcessor(const AudioProcessor&)=delete;
    AudioProcessor& operator=(const AudioProcessor&)=delete;

    void setSettings(const AudioProcessingSettings& settings);
    AudioProcessingSettings settings() const;
    void processCapture(std::span<std::int16_t> samples);
    void processPostGainSuppression(std::span<std::int16_t> samples);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
