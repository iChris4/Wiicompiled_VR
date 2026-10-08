// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mkw::vr {

// Keeps the game's video timeline (its VI retraces, 60 Hz) in step with a headset display whose refresh is a whole
// multiple of it, such as 120 Hz, so that every game frame is shown for the same number of refreshes.
//
// The two clocks are independent: a retrace is 16.666 ms, two 120 Hz refreshes are 16.667 ms, and displays wander
// a little. Left alone, the moment a game frame is ready drifts across the refresh the runtime can still give it,
// and while it sits within a millisecond of that deadline, the frame's varying work decides whether it gets that
// refresh or the next: frames are then shown one and three refreshes instead of two.
//
// The signal is the lead from the moment a frame's eyes are ready (render-first pacing calls xrWaitFrame then) to
// the display time the runtime grants it. The runtime grants the first refresh at least some minimum lead away, so
// the lead always lies within one display period above that minimum, and a frame ready right at the deadline is
// the one whose lead is either side of the jump. The lock first sweeps the VI timeline once through a little more
// than a period to find the minimum (the frames then cross the deadline once), and from then on steers the
// circular mean of the lead to half a period above it, the point furthest from the deadline either way, by moving
// the VI timeline a few microseconds a retrace. Waits inside xrWaitFrame are no signal: Meta's runtime returns at
// once to a frame this late.
class DisplayPhaseLock {
public:
    static constexpr double kSmoothing = 0.1;         // weight of each new lead in the circular mean
    static constexpr double kGain = 0.05;             // share of the phase error moved per retrace
    static constexpr int64_t kMaxShiftNs = 100'000;   // per retrace, 0.6% of a 60 Hz frame
    static constexpr int64_t kSweepShiftNs = 60'000;  // per retrace while the deadline is looked for
    static constexpr double kSweepPeriods = 1.25;     // display periods the sweep moves the timeline through

    enum class State { Off, Calibrating, Locked };

    // Display refreshes per VI retrace when that is a whole number within 1% (2 at 120 Hz for a 60 Hz game),
    // else 0: at 72 Hz there is no grid both share and nothing to lock to.
    static uint32_t RefreshesPerRetrace(int64_t display_period_ns, int64_t retrace_interval_ns) noexcept {
        if (display_period_ns <= 0 || retrace_interval_ns <= 0) {
            return 0;
        }
        const double ratio = static_cast<double>(retrace_interval_ns) / static_cast<double>(display_period_ns);
        const double whole = std::round(ratio);
        if (whole < 1.0 || whole > 8.0 || std::abs(ratio - whole) > 0.01 * whole) {
            return 0;
        }
        return static_cast<uint32_t>(whole);
    }

    // One rendered frame: the lead from its eyes being ready to the display time granted it, the display period
    // and the VI interval. Returns the shift to apply to every VI retrace until the next update, in nanoseconds
    // (positive is later); zero while the display cannot be locked to.
    int64_t Update(int64_t lead_ns, int64_t display_period_ns, int64_t retrace_interval_ns) noexcept {
        refreshes_ = RefreshesPerRetrace(display_period_ns, retrace_interval_ns);
        if (refreshes_ == 0 || lead_ns <= 0) {
            Reset();
            return 0;
        }
        // The runtime reports the period with a few nanoseconds of jitter: only a new refresh rate calibrates again.
        if (period_ == 0 || std::abs(display_period_ns - period_) > period_ / 200) {
            Reset();
            period_ = display_period_ns;
        }
        const double period = static_cast<double>(display_period_ns);
        if (state_ == State::Off) {
            state_ = State::Calibrating;
            sweep_frames_ = 0;
            min_lead_ = std::numeric_limits<int64_t>::max();
            max_lead_ = std::numeric_limits<int64_t>::min();
        }
        if (state_ == State::Calibrating) {
            min_lead_ = std::min(min_lead_, lead_ns);
            max_lead_ = std::max(max_lead_, lead_ns);
            const auto needed = static_cast<uint32_t>(std::ceil(kSweepPeriods * period / kSweepShiftNs));
            if (++sweep_frames_ < needed) {
                shift_ns_ = kSweepShiftNs;
                return shift_ns_;
            }
            // Leads lie in [minimum, minimum + period); a sweep that saw no jump falls back to its own middle.
            target_ns_ = max_lead_ - min_lead_ >= static_cast<int64_t>(0.7 * period)
                             ? min_lead_ + display_period_ns / 2
                             : (min_lead_ + max_lead_) / 2;
            state_ = State::Locked;
            frames_ = 0;
        }
        const double angle = kTwoPi * static_cast<double>(lead_ns - target_ns_) / period;
        if (frames_ == 0) {
            cos_ = std::cos(angle);
            sin_ = std::sin(angle);
        } else {
            cos_ += kSmoothing * (std::cos(angle) - cos_);
            sin_ += kSmoothing * (std::sin(angle) - sin_);
        }
        ++frames_;
        // The mean lead less the target, within half a period: negative when frames are ready too close to the
        // deadline (move them earlier), positive when they are ready just after the previous one (move them later).
        error_ns_ = static_cast<int64_t>(std::atan2(sin_, cos_) / kTwoPi * period);
        shift_ns_ = std::clamp(static_cast<int64_t>(kGain * static_cast<double>(error_ns_)), -kMaxShiftNs, kMaxShiftNs);
        return shift_ns_;
    }

    void Reset() noexcept {
        state_ = State::Off;
        period_ = 0;
        frames_ = 0;
        sweep_frames_ = 0;
        cos_ = 0.0;
        sin_ = 0.0;
        error_ns_ = 0;
        shift_ns_ = 0;
        target_ns_ = 0;
    }

    State CurrentState() const noexcept { return state_; }
    bool Locked() const noexcept { return state_ == State::Locked; }
    uint32_t RefreshesPerFrame() const noexcept { return refreshes_; }
    // The lead the lock holds frames at, found by the sweep.
    int64_t TargetLeadNs() const noexcept { return target_ns_; }
    // The mean lead's distance from the target (see Update).
    int64_t PhaseErrorNs() const noexcept { return error_ns_; }
    // How alike the leads are: 1 when every frame is ready at the same point.
    double Concentration() const noexcept { return std::hypot(cos_, sin_); }
    int64_t ShiftNs() const noexcept { return shift_ns_; }

private:
    static constexpr double kTwoPi = 6.28318530717958647692;
    State state_ = State::Off;
    uint32_t refreshes_ = 0;
    int64_t period_ = 0;
    uint32_t frames_ = 0;
    uint32_t sweep_frames_ = 0;
    int64_t min_lead_ = 0;
    int64_t max_lead_ = 0;
    int64_t target_ns_ = 0;
    double cos_ = 0.0;
    double sin_ = 0.0;
    int64_t error_ns_ = 0;
    int64_t shift_ns_ = 0;
};

} // namespace mkw::vr
