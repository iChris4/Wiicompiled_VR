// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <aurora/aurora.h>

#include <string>

namespace mkw::vr {

enum class OpenXRStartupResult {
    Disabled,
    Prepared,
    Unavailable,
};

// Performs the OpenXR instance/system and graphics-requirements work that must
// happen before Aurora selects an adapter. On success this may force the
// backend and populate AuroraConfig's XR interop fields.
OpenXRStartupResult OpenXRPrepareAurora(AuroraConfig& config);

// Completes the graphics binding and starts the asynchronous XR pacing thread.
// Call after aurora_initialize(), while Aurora's frame worker is idle.
bool OpenXRStartAfterAurora(AuroraBackend active_backend);

// Stops publishing stereo work, drains the pacing thread, and destroys the XR
// session before aurora_shutdown(). Safe to call after partial initialization.
void OpenXRShutdownBeforeAurora() noexcept;

// Services an XR-owned teardown request at the producer's safe frame boundary:
// after aurora_begin_frame() has granted the worker's prepare phase and before
// aurora_end_frame_tagged() seals the current frame. No-op unless the pacing
// thread has fallen back to desktop rendering.
void OpenXRServiceProducerFrameBoundary() noexcept;

bool OpenXRIsRunning() noexcept;
std::string OpenXRLastError();

// Recenters on where the player is sitting now: it moves the immersive race
// view's origin, and re-places the anchored menu screen upright in front of
// them. Position only for the race view: the forward direction and the horizon
// come from the OpenXR reference space and are never relatched from the
// headset, so recentering cannot tilt the game's horizon. Use the runtime's own
// recenter gesture to change forward. Callable from any thread; serviced once
// per frame on the XR pacing thread.
void OpenXRRequestRecenter() noexcept;

// Sets a fixed pitch of the game camera for a player sitting reclined, in
// degrees, clamped to RuntimeConfigFile::kVrLeanBackDegreesLimit. Positive
// tilts the camera back with the player, so an angle matching how far they are
// reclined puts the track back in front of them. 0 applies no pitch at all.
// Applies to the immersive race view only; callable from any thread and read
// once per published frame.
void OpenXRSetLeanBackDegrees(float degrees) noexcept;

// Live pacing choice. Disabled: submit fresh frames promptly and repeat during
// stalls. Enabled: repeat at each headset display deadline while waiting.
void OpenXRSetEagerFrameHeartbeat(bool enabled) noexcept;

} // namespace mkw::vr
