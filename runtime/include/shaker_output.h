// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Secondary audio output for bass shakers.
//
// The AX mixer (src/hle/audio/ax_mix.cpp) hands this module one block of game sound per AX
// frame (everything except streamed music, unless music is included). This module low-pass
// filters it, applies gain and a soft clip, optionally folds it to mono, and plays it on a
// second audio device chosen by the user. The normal game output is never touched: if
// anything here fails, the game keeps playing exactly as before.

#include <cstdint>
#include <string>
#include <vector>

namespace ShakerOutput {

// Master switch. Opens (true) or closes (false) the second output stream.
void SetEnabled(bool enabled);

// Name (or part of the name) of the playback device. Empty means the system default.
void SetDevice(const std::string& deviceName);

// true: left and right are averaged and sent identically to both channels.
// false: left and right are kept separate.
void SetMono(bool mono);

// Low-pass cutoff in Hz. 0 (or less) bypasses the filter.
void SetLowpassHz(float hz);

// Gain in decibels, applied after the filter.
void SetGainDb(float db);

// true: the music is sent too, and the output stays on outside of races (menus, the
// title-screen demo). false: music is left out, and the output is silent unless a human
// is racing. The mixer reads this; this module only stores it.
void SetIncludeMusic(bool includeMusic);
bool IncludeMusic();

// Follows the game's Mute setting.
void SetMuted(bool muted);

// True only while the second output is enabled and its stream is open. The mixer checks
// this once per voice, so there is no cost while the feature is off.
bool IsActive();

// Called by the mixer once per AX frame with `frames` left/right pairs of signed 16-bit
// audio at 32 kHz.
void PushFrame(const int16_t* interleavedLR, uint32_t frames);

// Names of the playback devices SDL can see right now (for the settings menu).
std::vector<std::string> ListDevices();

// Name of the device currently in use, or an empty string if none.
std::string ActiveDeviceName();

void Shutdown();

} // namespace ShakerOutput
