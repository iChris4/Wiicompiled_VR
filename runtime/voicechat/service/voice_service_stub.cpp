// SPDX-License-Identifier: GPL-3.0-or-later
//
// voice_api.h for builds without MKW_ENABLE_VOICECHAT: nothing runs, the menu says so.

#include "voicechat/voice_api.h"

namespace mkw::voicechat {

bool Available() noexcept { return false; }
const char* DefaultSignalingUrl() noexcept { return ""; }
void Start(const Settings&) noexcept {}
void Shutdown() noexcept {}
void ApplySettings(const Settings&) noexcept {}
void ObserveTcpConnect(std::uint32_t, std::uint16_t) noexcept {}
void ObserveTcpSend(std::uint32_t, std::uint16_t, const std::uint8_t*, std::size_t) noexcept {}
void ObserveTcpReceive(std::uint32_t, std::uint16_t, const std::uint8_t*, std::size_t) noexcept {}
void ObserveSocketClosed(std::uint32_t) noexcept {}
std::uint32_t CurrentProfileId() noexcept { return 0; }
bool WantsRoomSnapshot() noexcept { return false; }
void PostRoomSnapshot(const RoomSnapshot&) noexcept {}
bool WantsSpatialSnapshot() noexcept { return false; }
void PostSpatialSnapshot(const SpatialSnapshot&) noexcept {}
void ClearSpatialSnapshot() noexcept {}
void SetPushToTalkHeld(bool) noexcept {}

Status GetStatus() {
    Status status;
    status.state = State::Unavailable;
    status.message = "This build has no voice chat";
    return status;
}

void RefreshDevices() noexcept {}
void SetMicrophoneTest(bool) noexcept {}
void PlayTestTone() noexcept {}

} // namespace mkw::voicechat
