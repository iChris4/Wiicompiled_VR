// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/openxr_driving.h"

#include <chrono>
#include <mutex>

namespace mkw::vr {
// Named rather than anonymous: runtime sources are unity-built in groups.
namespace driving_bridge {

struct Published {
    std::mutex mutex;
    DrivingSnapshot snapshot{};
};

Published& Get() {
    static Published published;
    return published;
}

} // namespace driving_bridge

void OpenXRPublishDriving(const DrivingSnapshot& snapshot) noexcept {
    // Stamped here, so every publication (a reset too) carries its own time.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto& published = driving_bridge::Get();
    std::lock_guard lock(published.mutex);
    published.snapshot = snapshot;
    published.snapshot.published_ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

DrivingSnapshot OpenXRReadDriving() noexcept {
    auto& published = driving_bridge::Get();
    std::lock_guard lock(published.mutex);
    return published.snapshot;
}

} // namespace mkw::vr
