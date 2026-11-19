// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Signal processing for the secondary (bass shaker) audio output: a 4th-order low-pass
// filter, decibel gain and a soft clip. Header-only and free of SDL and guest access so it
// can be checked on its own (tests/shaker_dsp_tests.cpp).

#include <algorithm>
#include <cmath>

namespace shaker_dsp {

constexpr double kPi = 3.14159265358979323846;

// Cutoffs outside this range are clamped. Below 20 Hz a low-pass at the AX sample rate
// stops being useful for a shaker.
constexpr double kMinCutoffHz = 20.0;
constexpr double kMaxCutoffHz = 400.0;

// One second-order low-pass section (RBJ audio-EQ cookbook), direct form II transposed.
// Double precision: at shaker cutoffs the coefficients sit very close to 0 and 1.
class Biquad {
public:
    void SetLowpass(double cutoffHz, double q, double sampleRate) {
        const double w0 = 2.0 * kPi * cutoffHz / sampleRate;
        const double cosW0 = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        m_b0 = ((1.0 - cosW0) * 0.5) / a0;
        m_b1 = (1.0 - cosW0) / a0;
        m_b2 = m_b0;
        m_a1 = (-2.0 * cosW0) / a0;
        m_a2 = (1.0 - alpha) / a0;
    }

    double Process(double x) {
        const double y = m_b0 * x + m_z1;
        m_z1 = m_b1 * x - m_a1 * y + m_z2;
        m_z2 = m_b2 * x - m_a2 * y;
        return y;
    }

    void Reset() {
        m_z1 = 0.0;
        m_z2 = 0.0;
    }

private:
    double m_b0 = 1.0, m_b1 = 0.0, m_b2 = 0.0, m_a1 = 0.0, m_a2 = 0.0;
    double m_z1 = 0.0, m_z2 = 0.0;
};

// 4th-order Butterworth low-pass (24 dB per octave): two sections in series, with the Q
// values that make the combined response maximally flat.
class Lowpass4 {
public:
    void SetCutoff(double cutoffHz, double sampleRate) {
        m_first.SetLowpass(cutoffHz, 0.5411961001461970, sampleRate);
        m_second.SetLowpass(cutoffHz, 1.3065629648763766, sampleRate);
    }

    double Process(double x) { return m_second.Process(m_first.Process(x)); }

    void Reset() {
        m_first.Reset();
        m_second.Reset();
    }

private:
    Biquad m_first;
    Biquad m_second;
};

inline float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// Linear up to 0.7 of full scale, then eases smoothly toward 1.0, so a loud peak at high
// gain is rounded off instead of hard-clipped. Never returns more than 1.0 in magnitude.
inline float SoftClip(float x) {
    constexpr float kKnee = 0.7f;
    const float magnitude = std::fabs(x);
    if (magnitude <= kKnee) {
        return x;
    }
    const float over = (magnitude - kKnee) / (1.0f - kKnee);
    const float clipped = kKnee + (1.0f - kKnee) * std::tanh(over);
    return x < 0.0f ? -clipped : clipped;
}

} // namespace shaker_dsp
