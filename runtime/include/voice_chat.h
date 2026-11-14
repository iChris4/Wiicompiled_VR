// SPDX-License-Identifier: GPL-3.0-or-later
//
// The runtime side of room voice chat (voicechat/voice_api.h): the [voicechat] settings, the
// guest reads that tell the engine which room the player is in and where the other karts are,
// push-to-talk and the toggle keys, and the F10 / headset-panel menu and the on-screen speaker
// list. Voice chat is offered in Retro Rewind only, where its rooms live (docs/voice-chat.md).

#pragma once

#include <SDL3/SDL_events.h>

namespace VoiceChat {

// Main thread, once at startup and at exit (beside Discord presence).
void Initialize() noexcept;
void Shutdown() noexcept;

// Game thread, once per presented frame, after the VR scene anchor is published:
// `anchorFromScene` is the first-person anchor (row-major 3x4) or null, `unitsPerMeter` its world
// scale (0 when unknown).
void OnFramePresented(const float* anchorFromScene, float unitsPerMeter) noexcept;

// The overlay's SDL event loop: the mute and deafen toggle keys.
void HandleEvent(const SDL_Event& event) noexcept;

// Hotkeys, rebound through the overlay's prompt. Targets: 0 push to talk, 1 mute, 2 deafen.
inline constexpr int kHotkeyCount = 3;
int Hotkey(int target) noexcept;
void SetHotkey(int target, int scancode) noexcept;

// The menu (desktop F10 bar and the headset panel's tab). `drawHotkey` draws a rebind button for
// one target; the headset panel passes null (no keyboard there).
using HotkeyDrawer = void (*)(const char* label, int scancode, int target);
void DrawSettings(HotkeyDrawer drawHotkey);
// The speaker list on the desktop and on the headset's menu screen.
void DrawOverlay() noexcept;
// The same for immersive races, where the desktop overlay does not reach the eyes: a small badge
// the overlay draws with a context of its own (aurora_imgui_set_stereo_badge). Wanted only while
// someone talks or for a few seconds after mute or deafen changed, so it costs nothing otherwise.
bool HeadsetBadgeWanted() noexcept;
// Fills the current window with the badge's lines.
void DrawHeadsetBadge() noexcept;

} // namespace VoiceChat
