// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).
// Changes: stereo playback (positional voices), Android voice-call capture preset, a
// playback-only fallback when no microphone can be opened, and a built-in test tone.

#pragma once

#include "VoiceFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mkwvc {

struct AudioEngineOptions {
    std::string captureDevice;
    std::string playbackDevice;
    // Android: AAudio's voice-communication input preset (system echo cancellation).
    bool platformVoiceProcessing = false;
};

class AudioEngine {
public:
    static constexpr std::uint32_t SampleRate = VoiceFormat::SampleRate;
    static constexpr std::size_t FrameSamples = VoiceFormat::FrameSamples;

    explicit AudioEngine(AudioEngineOptions options);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    static std::vector<std::string> captureDevices();
    static std::vector<std::string> playbackDevices();

    void start();
    void stop();
    // Opens other devices; on failure the previous ones are reopened and the error rethrown.
    void reopen(AudioEngineOptions options);
    void setPlaybackMuted(bool muted);
    // False when only playback could be opened; captureError() says why.
    bool captureAvailable() const;
    std::string captureError() const;

    // Mono microphone samples.
    std::size_t readCaptured(std::span<std::int16_t> samples);
    // Interleaved stereo frames (two samples each).
    std::size_t queuePlayback(std::span<const std::int16_t> interleavedStereo);
    // Mono loopback for the microphone test, added to both channels.
    std::size_t queueMonitor(std::span<const std::int16_t> samples);
    void playTestTone();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mkwvc
