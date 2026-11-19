// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaker_output.h"

#include "runtime_log.h"
#include "shaker_dsp.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace ShakerOutput {
namespace {

// The AX mixer produces 96 samples per frame at 32 kHz. SDL converts to whatever rate the
// chosen device runs at.
constexpr int kSampleRate = 32000;
constexpr uint32_t kMaxBlockFrames = 256;
// New blocks are dropped while this much audio is already queued. That keeps the feed from
// drifting behind when the device clock runs slightly slower than the game's.
constexpr int kQueueLimitMs = 80;

std::string ToLower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_mono{true};
std::atomic<bool> g_includeMusic{false};
std::atomic<bool> g_muted{false};
std::atomic<float> g_lowpassHz{80.0f};
std::atomic<float> g_gainDb{0.0f};

struct State {
    std::mutex mutex;
    SDL_AudioStream* stream = nullptr;
    bool sdlAudioInitialized = false;
    std::string requestedDevice;
    std::string activeDeviceName;

    // One filter per channel; mono uses only the first.
    shaker_dsp::Lowpass4 lowpass[2];
    double designedCutoffHz = -1.0;
    bool lastMono = true;
};

State& GetState() {
    static State state;
    return state;
}

void CloseLocked(State& s) {
    g_active.store(false, std::memory_order_release);
    if (s.stream) {
        SDL_DestroyAudioStream(s.stream);
        s.stream = nullptr;
    }
    if (s.sdlAudioInitialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        s.sdlAudioInitialized = false;
    }
    s.activeDeviceName.clear();
}

// Exact (case-insensitive) match first, then "contains". Returns 0 if nothing matches.
SDL_AudioDeviceID FindDeviceLocked(const std::string& wanted) {
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    if (!devices) {
        return 0;
    }
    const std::string wantedLower = ToLower(wanted);
    SDL_AudioDeviceID exact = 0;
    SDL_AudioDeviceID partial = 0;
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        if (!name) {
            continue;
        }
        const std::string nameLower = ToLower(name);
        if (nameLower == wantedLower) {
            exact = devices[i];
            break;
        }
        if (partial == 0 && nameLower.find(wantedLower) != std::string::npos) {
            partial = devices[i];
        }
    }
    SDL_free(devices);
    return exact != 0 ? exact : partial;
}

bool OpenLocked(State& s) {
    CloseLocked(s);

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        RT_LOG(RT_TAG_AUDIO) << "shaker output: SDL_InitSubSystem failed: " << SDL_GetError() << std::endl;
        return false;
    }
    s.sdlAudioInitialized = true;

    SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!s.requestedDevice.empty()) {
        device = FindDeviceLocked(s.requestedDevice);
        if (device == 0) {
            // Do not fall back to the default device: that is normally the speakers or
            // headphones, and this feed is bass-only audio that should never land there
            // by accident.
            RT_LOG(RT_TAG_AUDIO) << "shaker output: device \"" << s.requestedDevice
                                 << "\" not found; staying off" << std::endl;
            CloseLocked(s);
            return false;
        }
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = kSampleRate;
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(device, &spec, nullptr, nullptr);
    if (!stream) {
        RT_LOG(RT_TAG_AUDIO) << "shaker output: SDL_OpenAudioDeviceStream failed: " << SDL_GetError()
                             << std::endl;
        CloseLocked(s);
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        RT_LOG(RT_TAG_AUDIO) << "shaker output: SDL_ResumeAudioStreamDevice failed: " << SDL_GetError()
                             << std::endl;
        SDL_DestroyAudioStream(stream);
        CloseLocked(s);
        return false;
    }

    s.stream = stream;
    const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream));
    s.activeDeviceName = name ? name : "";
    s.designedCutoffHz = -1.0; // force the filter to be rebuilt
    for (auto& filter : s.lowpass) {
        filter.Reset();
    }
    g_active.store(true, std::memory_order_release);
    RT_LOG(RT_TAG_AUDIO) << "shaker output: playing on \"" << s.activeDeviceName << "\"" << std::endl;
    return true;
}

void ApplyEnabledLocked(State& s) {
    if (g_enabled.load(std::memory_order_acquire)) {
        OpenLocked(s);
    } else {
        CloseLocked(s);
    }
}

} // namespace

void SetEnabled(bool enabled) {
    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    g_enabled.store(enabled, std::memory_order_release);
    ApplyEnabledLocked(s);
}

void SetDevice(const std::string& deviceName) {
    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.requestedDevice = deviceName;
    if (g_enabled.load(std::memory_order_acquire)) {
        ApplyEnabledLocked(s);
    }
}

void SetMono(bool mono) {
    g_mono.store(mono, std::memory_order_release);
}

void SetLowpassHz(float hz) {
    g_lowpassHz.store(std::isfinite(hz) ? hz : 0.0f, std::memory_order_release);
}

void SetGainDb(float db) {
    g_gainDb.store(std::isfinite(db) ? std::clamp(db, -60.0f, 30.0f) : 0.0f, std::memory_order_release);
}

void SetIncludeMusic(bool includeMusic) {
    g_includeMusic.store(includeMusic, std::memory_order_release);
}

bool IncludeMusic() {
    return g_includeMusic.load(std::memory_order_acquire);
}

void SetMuted(bool muted) {
    g_muted.store(muted, std::memory_order_release);
}

bool IsActive() {
    return g_active.load(std::memory_order_acquire);
}

void PushFrame(const int16_t* interleavedLR, uint32_t frames) {
    if (!interleavedLR || frames == 0 || frames > kMaxBlockFrames || !IsActive()) {
        return;
    }

    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.stream || g_muted.load(std::memory_order_acquire)) {
        return; // muted: send nothing, the device plays silence
    }

    const bool mono = g_mono.load(std::memory_order_acquire);
    const double requestedHz = static_cast<double>(g_lowpassHz.load(std::memory_order_acquire));
    const bool filterOn = requestedHz > 0.0;
    const double cutoffHz =
        filterOn ? std::clamp(requestedHz, shaker_dsp::kMinCutoffHz, shaker_dsp::kMaxCutoffHz) : 0.0;

    // Rebuild the filter when the cutoff changed, and clear stale filter memory when
    // switching between mono and stereo.
    if (filterOn && cutoffHz != s.designedCutoffHz) {
        for (auto& filter : s.lowpass) {
            filter.SetCutoff(cutoffHz, kSampleRate);
        }
        s.designedCutoffHz = cutoffHz;
    }
    if (mono != s.lastMono) {
        for (auto& filter : s.lowpass) {
            filter.Reset();
        }
        s.lastMono = mono;
    }

    const float gain = shaker_dsp::DbToLinear(g_gainDb.load(std::memory_order_acquire));

    std::array<float, kMaxBlockFrames * 2> out{};
    for (uint32_t i = 0; i < frames; ++i) {
        double left = static_cast<double>(interleavedLR[i * 2]) / 32768.0;
        double right = static_cast<double>(interleavedLR[i * 2 + 1]) / 32768.0;
        if (mono) {
            left = (left + right) * 0.5;
            if (filterOn) {
                left = s.lowpass[0].Process(left);
            }
            right = left;
        } else if (filterOn) {
            left = s.lowpass[0].Process(left);
            right = s.lowpass[1].Process(right);
        }
        out[i * 2] = shaker_dsp::SoftClip(static_cast<float>(left) * gain);
        out[i * 2 + 1] = shaker_dsp::SoftClip(static_cast<float>(right) * gain);
    }

    const int lengthBytes = static_cast<int>(frames * 2 * sizeof(float));
    const int limitBytes =
        static_cast<int>(static_cast<int64_t>(kSampleRate) * 2 * sizeof(float) * kQueueLimitMs / 1000);
    const int queuedBytes = SDL_GetAudioStreamQueued(s.stream);
    if (queuedBytes >= 0 && queuedBytes + lengthBytes > limitBytes) {
        return; // queue already full: drop this block
    }
    if (!SDL_PutAudioStreamData(s.stream, out.data(), lengthBytes)) {
        RT_LOG(RT_TAG_AUDIO) << "shaker output: SDL_PutAudioStreamData failed: " << SDL_GetError() << std::endl;
    }
}

std::vector<std::string> ListDevices() {
    std::vector<std::string> names;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return names;
    }
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    if (devices) {
        for (int i = 0; i < count; ++i) {
            if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
                names.emplace_back(name);
            }
        }
        SDL_free(devices);
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return names;
}

std::string ActiveDeviceName() {
    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.activeDeviceName;
}

void Shutdown() {
    State& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    g_enabled.store(false, std::memory_order_release);
    CloseLocked(s);
}

} // namespace ShakerOutput
