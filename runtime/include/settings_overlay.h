#pragma once

#include <aurora/aurora.h>
#include <aurora/event.h>

namespace settings_overlay {
// Apply persistent controller settings once Aurora has discovered host devices.
void InitializeRuntimeSettings() noexcept;
// Draw the F10 settings bar before each Aurora present.
void HandleEvents(const AuroraEvent* events) noexcept;
void Draw() noexcept;
bool StartupScreenVisible() noexcept;
void NotifyStrapInputAccepted() noexcept;
void AdvancePresentedFrame() noexcept;
// Re-sends the VR virtual screen's placement to Aurora. Its metres are
// converted with the world scale currently in effect, so switching the
// first-person camera on or off has to repeat it.
void RefreshVrHudVirtualScreen() noexcept;
} // namespace settings_overlay
