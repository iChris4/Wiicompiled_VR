// SPDX-License-Identifier: GPL-3.0-or-later
//
// What voice chat needs from the headset, published by the OpenXR threads and read on the game
// thread: the optional push-to-talk button (the left thumbstick click) and the listener's head
// rotation for positional voices. Header-only and free of OpenXR, so the glue compiles everywhere.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace mkw::vr {

namespace voice_detail {

inline std::atomic<bool> g_leftStickClick{false};

struct HeadRotation {
    std::mutex mutex;
    std::array<float, 9> rotation{};  // centre space -> head, row-major
    bool valid = false;
    std::chrono::steady_clock::time_point at{};
};

inline HeadRotation& Head() {
    static HeadRotation head;
    return head;
}

} // namespace voice_detail

// Input thread: the left thumbstick click, already false while the panel has the controllers.
inline void OpenXRPublishVoiceButton(bool held) noexcept {
    voice_detail::g_leftStickClick.store(held, std::memory_order_relaxed);
}

inline bool OpenXRVoiceButtonHeld() noexcept {
    return voice_detail::g_leftStickClick.load(std::memory_order_relaxed);
}

// Pacing thread, once per immersive frame: the rotation part of an eye's viewFromCenter (both eyes
// share it). Pass nullptr when the frame is not immersive.
inline void OpenXRPublishHeadRotation(const float* viewFromCenter) noexcept {
    auto& head = voice_detail::Head();
    std::scoped_lock lock(head.mutex);
    head.valid = viewFromCenter != nullptr;
    if (head.valid) {
        // viewFromCenter is a row-major 3x4; drop the translation column.
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) head.rotation[row * 3 + col] = viewFromCenter[row * 4 + col];
        }
        head.at = std::chrono::steady_clock::now();
    }
}

// The latest immersive head rotation, if one was published in the last quarter second.
inline bool OpenXRReadHeadRotation(std::array<float, 9>& rotation) noexcept {
    auto& head = voice_detail::Head();
    std::scoped_lock lock(head.mutex);
    if (!head.valid || std::chrono::steady_clock::now() - head.at > std::chrono::milliseconds(250)) return false;
    rotation = head.rotation;
    return true;
}

} // namespace mkw::vr
