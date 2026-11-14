// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#include "AudioEngine.hpp"
#include "SpscRingBuffer.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

namespace mkwvc {

namespace {

std::vector<std::string> numberedDeviceNames(const ma_device_info* infos, ma_uint32 count) {
    std::unordered_map<std::string, std::size_t> totals;
    for (ma_uint32 i = 0; i < count; ++i) ++totals[infos[i].name];
    std::unordered_map<std::string, std::size_t> seen;
    std::vector<std::string> result;
    result.reserve(count);
    for (ma_uint32 i = 0; i < count; ++i) {
        const std::string raw = infos[i].name;
        const auto occurrence = ++seen[raw];
        result.push_back(totals[raw] > 1 ? raw + " [" + std::to_string(occurrence) + "]" : raw);
    }
    return result;
}

const ma_device_id* resolveDeviceId(const std::string& selected, ma_device_info* infos, ma_uint32 count) {
    if (selected.empty()) return nullptr;
    const auto labels = numberedDeviceNames(infos, count);
    for (ma_uint32 i = 0; i < count; ++i) {
        if (selected == labels[i]) return &infos[i].id;
    }
    for (ma_uint32 i = 0; i < count; ++i) {
        if (selected == infos[i].name) return &infos[i].id;
    }
    return nullptr;
}

enum class DeviceKind { Capture, Playback };

std::vector<std::string> enumerate(DeviceKind kind) {
    ma_context context{};
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) return {};
    ma_device_info* playbackInfos = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info* captureInfos = nullptr;
    ma_uint32 captureCount = 0;
    std::vector<std::string> devices;
    if (ma_context_get_devices(&context, &playbackInfos, &playbackCount, &captureInfos, &captureCount) ==
        MA_SUCCESS) {
        devices = kind == DeviceKind::Capture ? numberedDeviceNames(captureInfos, captureCount)
                                              : numberedDeviceNames(playbackInfos, playbackCount);
    }
    ma_context_uninit(&context);
    return devices;
}

} // namespace

class AudioEngine::Impl {
public:
    explicit Impl(AudioEngineOptions options) {
        if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
            throw std::runtime_error("Failed to initialize the audio context");
        }
        contextInitialized_ = true;
        try {
            initializeDevice(options);
            options_ = std::move(options);
        } catch (...) {
            ma_context_uninit(&context_);
            contextInitialized_ = false;
            throw;
        }
    }

    ~Impl() {
        stop();
        if (deviceInitialized_) ma_device_uninit(&device_);
        if (contextInitialized_) ma_context_uninit(&context_);
    }

    void start() {
        if (running_) return;
        playbackPrimed_ = false;
        monitorPrimed_ = false;
        if (ma_device_start(&device_) != MA_SUCCESS) throw std::runtime_error("Failed to start the audio device");
        running_ = true;
    }

    void stop() {
        if (!running_) return;
        ma_device_stop(&device_);
        running_ = false;
    }

    void reopen(AudioEngineOptions options) {
        const AudioEngineOptions previous = options_;
        const bool wasRunning = running_;
        stop();
        uninitializeDevice();
        try {
            initializeDevice(options);
            options_ = std::move(options);
            if (wasRunning) start();
        } catch (...) {
            const auto error = std::current_exception();
            uninitializeDevice();
            try {
                initializeDevice(previous);
                if (wasRunning) start();
            } catch (...) {
            }
            std::rethrow_exception(error);
        }
    }

    void setPlaybackMuted(bool muted) { playbackMuted_.store(muted, std::memory_order_relaxed); }

    bool captureAvailable() const { return captureAvailable_.load(std::memory_order_relaxed); }

    std::string captureError() const {
        std::scoped_lock lock(errorMutex_);
        return captureError_;
    }

    std::size_t readCaptured(std::span<std::int16_t> samples) { return capture_.pop(samples); }
    std::size_t queuePlayback(std::span<const std::int16_t> samples) { return playback_.pushAll(samples) ? samples.size() : 0; }
    std::size_t queueMonitor(std::span<const std::int16_t> samples) { return monitor_.push(samples); }

    void playTestTone() {
        // 0.6 s of 440 Hz with 20 ms fades, through the monitor path.
        constexpr std::size_t kSamples = SampleRate * 6 / 10;
        constexpr std::size_t kFade = SampleRate / 50;
        std::array<std::int16_t, 960> chunk{};
        std::size_t written = 0;
        while (written < kSamples) {
            const std::size_t count = std::min(chunk.size(), kSamples - written);
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t n = written + i;
                float envelope = 1.0f;
                if (n < kFade) envelope = static_cast<float>(n) / kFade;
                if (kSamples - n < kFade) envelope = static_cast<float>(kSamples - n) / kFade;
                const float phase = 2.0f * std::numbers::pi_v<float> * 440.0f * static_cast<float>(n) / SampleRate;
                chunk[i] = static_cast<std::int16_t>(std::sin(phase) * 7000.0f * envelope);
            }
            if (monitor_.push(std::span<const std::int16_t>(chunk.data(), count)) < count) break;
            written += count;
        }
    }

private:
    void initializeDevice(const AudioEngineOptions& options) {
        ma_device_info* playbackInfos = nullptr;
        ma_uint32 playbackCount = 0;
        ma_device_info* captureInfos = nullptr;
        ma_uint32 captureCount = 0;
        const ma_device_id* captureId = nullptr;
        const ma_device_id* playbackId = nullptr;
        if (!options.captureDevice.empty() || !options.playbackDevice.empty()) {
            if (ma_context_get_devices(&context_, &playbackInfos, &playbackCount, &captureInfos, &captureCount) !=
                MA_SUCCESS) {
                throw std::runtime_error("Failed to enumerate audio devices");
            }
            // A device that went away falls back to the system default rather than failing.
            captureId = resolveDeviceId(options.captureDevice, captureInfos, captureCount);
            playbackId = resolveDeviceId(options.playbackDevice, playbackInfos, playbackCount);
        }

        const auto configure = [&](ma_device_type type) {
            auto config = ma_device_config_init(type);
            config.capture.format = ma_format_s16;
            config.capture.channels = VoiceFormat::Channels;
            config.capture.pDeviceID = captureId;
            config.playback.format = ma_format_s16;
            config.playback.channels = VoiceFormat::PlaybackChannels;
            config.playback.pDeviceID = playbackId;
            config.sampleRate = SampleRate;
            config.dataCallback = audioCallback;
            config.pUserData = this;
            // Mixed with the game's own stream, never routed or ducked as a phone call.
            config.aaudio.usage = ma_aaudio_usage_game;
            config.aaudio.contentType = ma_aaudio_content_type_speech;
            if (options.platformVoiceProcessing) {
                config.aaudio.inputPreset = ma_aaudio_input_preset_voice_communication;
            }
            return config;
        };

        auto duplex = configure(ma_device_type_duplex);
        if (ma_device_init(&context_, &duplex, &device_) == MA_SUCCESS) {
            deviceInitialized_ = true;
            setCaptureState(true, {});
            return;
        }
        // No microphone (absent, or the Android permission was not granted): still hear the room.
        auto playback = configure(ma_device_type_playback);
        if (ma_device_init(&context_, &playback, &device_) != MA_SUCCESS) {
            throw std::runtime_error("Failed to open the audio output");
        }
        deviceInitialized_ = true;
        setCaptureState(false, "The microphone could not be opened");
    }

    void uninitializeDevice() {
        if (deviceInitialized_) {
            ma_device_uninit(&device_);
            deviceInitialized_ = false;
            device_ = {};
        }
    }

    void setCaptureState(bool available, std::string error) {
        captureAvailable_.store(available, std::memory_order_relaxed);
        std::scoped_lock lock(errorMutex_);
        captureError_ = std::move(error);
    }

    static void audioCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
        static_cast<Impl*>(device->pUserData)
            ->process(static_cast<std::int16_t*>(output), static_cast<const std::int16_t*>(input), frameCount);
    }

    void process(std::int16_t* output, const std::int16_t* input, std::size_t frameCount) {
        if (input) capture_.push(std::span<const std::int16_t>(input, frameCount));
        if (!output) return;

        const std::size_t samples = frameCount * VoiceFormat::PlaybackChannels;
        std::fill(output, output + samples, 0);

        if (!playbackPrimed_ && playback_.available() >= kPlaybackPrebufferSamples) playbackPrimed_ = true;
        if (playbackPrimed_) {
            const auto read = playback_.pop(std::span<std::int16_t>(output, samples));
            if (read < samples) {
                std::fill(output + read, output + samples, 0);
                playbackPrimed_ = false;
            }
        }
        if (playbackMuted_.load(std::memory_order_relaxed)) std::fill(output, output + samples, 0);

        if (!monitorPrimed_ && monitor_.available() >= kMonitorPrebufferSamples) monitorPrimed_ = true;
        if (!monitorPrimed_) return;
        std::array<std::int16_t, 1024> monitorSamples{};
        std::size_t frame = 0;
        while (frame < frameCount) {
            const auto count = std::min<std::size_t>(monitorSamples.size(), frameCount - frame);
            const auto read = monitor_.pop(std::span<std::int16_t>(monitorSamples.data(), count));
            for (std::size_t i = 0; i < read; ++i) {
                for (std::size_t channel = 0; channel < VoiceFormat::PlaybackChannels; ++channel) {
                    auto& sample = output[(frame + i) * VoiceFormat::PlaybackChannels + channel];
                    sample = static_cast<std::int16_t>(std::clamp(
                        static_cast<std::int32_t>(sample) + static_cast<std::int32_t>(monitorSamples[i]), -32768,
                        32767));
                }
            }
            if (read < count) {
                monitorPrimed_ = false;
                break;
            }
            frame += count;
        }
    }

    static constexpr std::size_t kCaptureBufferSamples = SampleRate * 2 + 1;
    static constexpr std::size_t kPlaybackBufferSamples = SampleRate * 2 * VoiceFormat::PlaybackChannels + 1;
    static constexpr std::size_t kPlaybackPrebufferSamples = SampleRate * 60 / 1000 * VoiceFormat::PlaybackChannels;
    static constexpr std::size_t kMonitorPrebufferSamples = SampleRate * 40 / 1000;
    SpscRingBuffer<std::int16_t, kCaptureBufferSamples> capture_;
    SpscRingBuffer<std::int16_t, kPlaybackBufferSamples> playback_;
    SpscRingBuffer<std::int16_t, kCaptureBufferSamples> monitor_;
    ma_context context_{};
    ma_device device_{};
    AudioEngineOptions options_;
    std::atomic<bool> playbackMuted_{false};
    std::atomic<bool> captureAvailable_{false};
    mutable std::mutex errorMutex_;
    std::string captureError_;
    bool playbackPrimed_ = false;
    bool monitorPrimed_ = false;
    bool contextInitialized_ = false;
    bool deviceInitialized_ = false;
    bool running_ = false;
};

AudioEngine::AudioEngine(AudioEngineOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
AudioEngine::~AudioEngine() = default;

std::vector<std::string> AudioEngine::captureDevices() { return enumerate(DeviceKind::Capture); }
std::vector<std::string> AudioEngine::playbackDevices() { return enumerate(DeviceKind::Playback); }

void AudioEngine::start() { impl_->start(); }
void AudioEngine::stop() { impl_->stop(); }
void AudioEngine::reopen(AudioEngineOptions options) { impl_->reopen(std::move(options)); }
void AudioEngine::setPlaybackMuted(bool muted) { impl_->setPlaybackMuted(muted); }
bool AudioEngine::captureAvailable() const { return impl_->captureAvailable(); }
std::string AudioEngine::captureError() const { return impl_->captureError(); }
std::size_t AudioEngine::readCaptured(std::span<std::int16_t> samples) { return impl_->readCaptured(samples); }
std::size_t AudioEngine::queuePlayback(std::span<const std::int16_t> interleavedStereo) {
    return impl_->queuePlayback(interleavedStereo);
}
std::size_t AudioEngine::queueMonitor(std::span<const std::int16_t> samples) { return impl_->queueMonitor(samples); }
void AudioEngine::playTestTone() { impl_->playTestTone(); }

} // namespace mkwvc
