// SPDX-License-Identifier: GPL-3.0-or-later
// DisplayPhaseLock against a simulated headset: the runtime grants each frame the first refresh at least a minimum
// lead after its eyes are ready, the game's VI timeline runs on another clock, and each frame's work varies. Locked,
// every frame must be shown for the same number of refreshes; unlocked, the drift must reproduce the uneven frames
// the lock exists for.
#include "vr/display_phase_lock.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using mkw::vr::DisplayPhaseLock;

static void Require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::abort();
    }
}

namespace {

struct Outcome {
    uint32_t frames = 0;
    uint32_t uneven = 0; // frames not shown for the expected number of refreshes
    int64_t maxAbsShift = 0;
    int64_t finalErrorNs = 0;
    DisplayPhaseLock::State finalState = DisplayPhaseLock::State::Off;
};

// `retraceNs` is the game's nominal VI interval, `periodNs` the display's, `minLeadNs` the runtime's minimum lead,
// `startNs` where the first retrace falls on the display grid. Work is 14 ms +- 1 ms.
Outcome Simulate(int64_t retraceNs, int64_t periodNs, int64_t minLeadNs, int64_t startNs, bool lockOn,
                 uint32_t settleFrames, uint32_t measuredFrames) {
    DisplayPhaseLock lock;
    uint32_t seed = 12345;
    const auto jitter = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<int64_t>(seed >> 8) % 2'000'001 - 1'000'000;
    };
    const int64_t expected = (retraceNs + periodNs / 2) / periodNs;
    int64_t retrace = startNs + 100'000'000;
    int64_t shift = 0;
    int64_t lastDisplay = -1;
    Outcome out;
    for (uint32_t frame = 0; frame < settleFrames + measuredFrames; ++frame) {
        const int64_t ready = retrace + 14'000'000 + jitter();
        // The first refresh at least the minimum lead away (display times on a grid starting at 0).
        const int64_t display = (ready + minLeadNs + periodNs - 1) / periodNs * periodNs;
        if (frame >= settleFrames && lastDisplay >= 0) {
            ++out.frames;
            if (display - lastDisplay != expected * periodNs) {
                ++out.uneven;
            }
        }
        lastDisplay = display;
        if (lockOn) {
            // The runtime reports the period with a few nanoseconds of jitter (seen on a Quest 3).
            const int64_t reportedPeriod = periodNs + static_cast<int64_t>(frame % 5) - 2;
            shift = lock.Update(display - ready, reportedPeriod, retraceNs);
            if (lock.Locked()) {
                out.maxAbsShift = std::max(out.maxAbsShift, shift < 0 ? -shift : shift);
            }
        }
        retrace += retraceNs + shift;
    }
    out.finalErrorNs = lock.PhaseErrorNs();
    out.finalState = lock.CurrentState();
    return out;
}

} // namespace

int main() {
    constexpr int64_t kRetrace = 16'666'000; // the game's VI interval (vi.cpp IntervalForFormat)
    constexpr int64_t kPeriod120 = 8'333'333;
    constexpr int64_t kPeriod72 = 13'888'889;

    Require(DisplayPhaseLock::RefreshesPerRetrace(kPeriod120, kRetrace) == 2, "120 Hz is two refreshes a retrace");
    Require(DisplayPhaseLock::RefreshesPerRetrace(16'666'667, kRetrace) == 1, "60 Hz is one refresh a retrace");
    Require(DisplayPhaseLock::RefreshesPerRetrace(kPeriod72, kRetrace) == 0, "72 Hz has no shared grid");
    Require(DisplayPhaseLock::RefreshesPerRetrace(11'111'111, kRetrace) == 0, "90 Hz has no shared grid");
    Require(DisplayPhaseLock::RefreshesPerRetrace(0, kRetrace) == 0, "no display period, no lock");

    {
        DisplayPhaseLock lock;
        for (int i = 0; i < 100; ++i) {
            Require(lock.Update(5'000'000, kPeriod72, kRetrace) == 0, "72 Hz never shifts the timeline");
        }
        Require(lock.CurrentState() == DisplayPhaseLock::State::Off, "72 Hz never locks");
    }

    // The problem: unlocked at 120 Hz, the drift carries the ready frames across the deadline within a minute.
    {
        const Outcome free = Simulate(kRetrace, kPeriod120, 5'000'000, 0, false, 0, 60 * 120);
        Require(free.uneven > 0, "the simulated drift reproduces uneven frames");
    }

    // The sweep: a constant, bounded shift until the deadline is found, then the lock.
    {
        DisplayPhaseLock lock;
        Require(lock.Update(9'000'000, kPeriod120, kRetrace) == DisplayPhaseLock::kSweepShiftNs, "the sweep starts");
        Require(lock.CurrentState() == DisplayPhaseLock::State::Calibrating, "calibrating during the sweep");
    }

    // Locked from several starting phases and for several runtime leads: after the sweep and a few seconds, two
    // minutes in which every frame is shown for exactly two refreshes.
    for (int64_t minLead : {int64_t{1'000'000}, int64_t{5'000'000}, int64_t{12'500'000}}) {
        for (int64_t start : {int64_t{0}, int64_t{1'500'000}, int64_t{4'166'666}, int64_t{6'900'000}}) {
            const Outcome locked = Simulate(kRetrace, kPeriod120, minLead, start, true, 10 * 60, 120 * 60);
            Require(locked.finalState == DisplayPhaseLock::State::Locked, "the lock engages after the sweep");
            Require(locked.uneven == 0, "locked at 120 Hz, every frame is shown for two refreshes");
            Require(locked.maxAbsShift <= DisplayPhaseLock::kMaxShiftNs, "the shift stays within its bound");
            Require(locked.finalErrorNs > -500'000 && locked.finalErrorNs < 500'000, "the lead settles mid-period");
        }
    }

    // A display clock a little off its nominal rate (119.8 Hz) is followed too.
    {
        const Outcome locked = Simulate(kRetrace, 8'347'245, 5'000'000, 0, true, 10 * 60, 120 * 60);
        Require(locked.uneven == 0, "a display slightly off 120 Hz is followed");
    }

    // Leaving a lockable rate releases the shift at once; a new rate calibrates again.
    {
        DisplayPhaseLock lock;
        for (int i = 0; i < 400; ++i) {
            lock.Update(4'000'000 + (i % 7) * 10'000, kPeriod120, kRetrace);
        }
        Require(lock.Locked(), "locked at 120 Hz");
        Require(lock.Update(4'000'000, kPeriod72, kRetrace) == 0 && !lock.Locked(), "72 Hz releases the lock");
        Require(lock.Update(4'000'000, kPeriod120, kRetrace) == DisplayPhaseLock::kSweepShiftNs,
                "back at 120 Hz, the deadline is looked for again");
    }

    std::puts("display_phase_lock_tests: all passed");
    return 0;
}
