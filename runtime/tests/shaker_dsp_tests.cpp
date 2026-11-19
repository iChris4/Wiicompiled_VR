// SPDX-License-Identifier: GPL-3.0-or-later
// The secondary (bass shaker) output's signal processing: low-pass response, decibel gain
// and soft clip (shaker_dsp.h).

#include "shaker_dsp.h"

#include <cmath>
#include <iostream>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

constexpr double kSampleRate = 32000.0;

// Level of a sine of `frequencyHz` after the low-pass, in dB relative to the input. The first
// second is discarded so the filter has settled.
double LowpassResponseDb(double cutoffHz, double frequencyHz) {
    shaker_dsp::Lowpass4 filter;
    filter.SetCutoff(cutoffHz, kSampleRate);
    const int total = static_cast<int>(kSampleRate) * 3;
    const int settled = static_cast<int>(kSampleRate);
    double inSquares = 0.0;
    double outSquares = 0.0;
    for (int i = 0; i < total; ++i) {
        const double x = std::sin(2.0 * shaker_dsp::kPi * frequencyHz * i / kSampleRate);
        const double y = filter.Process(x);
        if (i >= settled) {
            inSquares += x * x;
            outSquares += y * y;
        }
    }
    return 10.0 * std::log10(outSquares / inSquares);
}

bool Near(double value, double expected, double tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

void TestLowpassResponse() {
    // 4th-order Butterworth: flat in the passband, -3 dB at the cutoff, 24 dB per octave above.
    Check(Near(LowpassResponseDb(80.0, 20.0), 0.0, 0.3), "passband is flat");
    Check(Near(LowpassResponseDb(80.0, 80.0), -3.0, 0.5), "-3 dB at the cutoff");
    Check(Near(LowpassResponseDb(80.0, 160.0), -24.1, 1.0), "-24 dB one octave above the cutoff");
    Check(LowpassResponseDb(80.0, 400.0) < -45.0, "well above the cutoff is strongly attenuated");
    // Moving the cutoff moves the response: 100 Hz is far down for a 50 Hz cutoff, -3 dB for 100 Hz.
    Check(LowpassResponseDb(50.0, 100.0) < -20.0, "lower cutoff cuts more");
    Check(Near(LowpassResponseDb(100.0, 100.0), -3.0, 0.5), "cutoff follows the setting");
}

void TestLowpassReset() {
    shaker_dsp::Lowpass4 filter;
    filter.SetCutoff(80.0, kSampleRate);
    for (int i = 0; i < 1000; ++i) {
        filter.Process(1.0);
    }
    filter.Reset();
    Check(filter.Process(0.0) == 0.0, "reset clears the filter memory");
}

void TestGain() {
    Check(Near(shaker_dsp::DbToLinear(0.0f), 1.0, 1e-6), "0 dB leaves the level unchanged");
    Check(Near(shaker_dsp::DbToLinear(6.0206f), 2.0, 1e-3), "+6 dB doubles the level");
    Check(Near(shaker_dsp::DbToLinear(-6.0206f), 0.5, 1e-3), "-6 dB halves the level");
    Check(Near(shaker_dsp::DbToLinear(-20.0f), 0.1, 1e-5), "-20 dB is a tenth");
}

void TestSoftClip() {
    Check(shaker_dsp::SoftClip(0.0f) == 0.0f, "zero stays zero");
    Check(shaker_dsp::SoftClip(0.5f) == 0.5f && shaker_dsp::SoftClip(-0.7f) == -0.7f,
          "levels up to the knee pass through unchanged");
    Check(Near(shaker_dsp::SoftClip(0.7001f), 0.7001, 1e-3), "continuous at the knee");
    Check(shaker_dsp::SoftClip(1.0f) < 1.0f && shaker_dsp::SoftClip(1.0f) > 0.7f, "above the knee is rounded off");
    Check(shaker_dsp::SoftClip(1000.0f) <= 1.0f && shaker_dsp::SoftClip(-1000.0f) >= -1.0f,
          "never exceeds full scale, however loud");
    Check(shaker_dsp::SoftClip(-0.9f) == -shaker_dsp::SoftClip(0.9f), "symmetric for negative input");
    float previous = -2.0f;
    bool rising = true;
    for (int i = -400; i <= 400; ++i) {
        const float output = shaker_dsp::SoftClip(static_cast<float>(i) * 0.01f);
        rising = rising && output >= previous;
        previous = output;
    }
    Check(rising, "output never falls as the input rises");
}

} // namespace

int main() {
    TestLowpassResponse();
    TestLowpassReset();
    TestGain();
    TestSoftClip();
    if (g_failures == 0) {
        std::cout << "shaker_dsp_tests passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
