// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace mkw::vr {
inline uint32_t NormalizeFrameInterpolationFps(uint32_t value) noexcept {
    return value == 1 || value == 72 || value == 90 || value == 120 ? value : 0;
}

// A render-rate ceiling on the compositor's own display-time grid. Auto (1)
// renders every tick. Fixed targets cannot increase the physical refresh rate.
class FrameInterpolationPacing {
public:
    bool ShouldRender(int64_t display_time, uint32_t target) noexcept {
        if (target != target_ || display_time <= last_time_) {
            next_time_ = 0;
            target_ = target;
        }
        last_time_ = display_time;
        if (target == 0 || target == 1) {
            next_time_ = 0;
            return true;
        }
        if (next_time_ != 0 && display_time + 1'000 < next_time_) return false;
        const int64_t interval = 1'000'000'000 / target;
        if (next_time_ == 0 || display_time - next_time_ > interval * 2) {
            next_time_ = display_time + interval;
        } else {
            next_time_ += interval;
        }
        return true;
    }
    void Reset() noexcept { *this = {}; }
private:
    int64_t next_time_ = 0;
    int64_t last_time_ = 0;
    uint32_t target_ = 0;
};
} // namespace mkw::vr
