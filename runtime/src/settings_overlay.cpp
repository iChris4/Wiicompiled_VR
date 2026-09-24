#include "settings_overlay.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "gx_native_wheel.h"
#include "input_bindings.h"
#include "game_graphics_options.h"
#include "log_export.h"
#include "physical_wheel.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "vr/camera_toggle.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/mkw_vr_policy.h"
#include "vr/openxr_diagnostics.h"
#include "vr/openxr_integration.h"
#include "vr/openxr_settings_panel.h"
#include "vr/openxr_wii_remote.h"
#include "wii_remote_input.h"

#include <aurora/imgui.h>
#include <imgui.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include <dolphin/pad.h>

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

// Fixed widths in the menus are written for the desktop bar's 13 px font. The
// headset's settings panel draws the same menus with a larger one.
float Scaled(float pixels) {
    return pixels * ImGui::GetFontSize() / 13.0f;
}

bool g_topBarVisible = false;
bool g_exitPromptOpen = false;
bool g_rumbleEnabled = RuntimeConfigFile::RumbleEnabled(true);
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 120:
        return 1;
    case 180:
        return 2;
    default:
        return 0;
    }
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("windowed");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps();
// The same default the VR path itself takes (kVrEnabledDefault), so the F10 switch
// shows what an unconfigured installation actually starts in.
bool g_vrEnabled = RuntimeConfigFile::VrEnabled(true);
bool g_vrStopAtDisplayCopy = RuntimeConfigFile::VrStopAtDisplayCopy(true);
bool g_vrSkipCopyClears = RuntimeConfigFile::VrSkipCopyClears(true);
bool g_vrSinglePassEyes = RuntimeConfigFile::VrSinglePassEyes(true);
bool g_vrHudVirtualScreen = RuntimeConfigFile::VrHudVirtualScreen(true);
bool g_vrFlatScreen = RuntimeConfigFile::VrFlatScreen();
#if defined(__ANDROID__)
bool g_vrPassthrough = RuntimeConfigFile::VrPassthrough();
#endif
bool g_vrFirstPerson = RuntimeConfigFile::VrFirstPerson(false);
bool g_vrFirstPersonToggleClick = RuntimeConfigFile::VrFirstPersonToggleClick();
// Set from any thread by the right-thumbstick click, applied on the game thread.
std::atomic<bool> g_firstPersonToggleRequested{false};
// Per physical gamepad; the VR controllers keep theirs on the XR side.
std::unordered_map<SDL_JoystickID, mkw::vr::ClickToggle> g_gamepadFirstPersonClicks;
float g_vrFirstPersonUnitsPerMeter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter();
// 0 = cockpit, 1 = custom, matching kVrFirstPersonSeatNames.
constexpr std::array<const char*, 2> kVrFirstPersonSeatNames{"cockpit", "custom"};
int g_vrFirstPersonSeat = RuntimeConfigFile::VrFirstPersonSeat() == "custom" ? 1 : 0;
float g_vrCockpitUnitsPerMeter = RuntimeConfigFile::VrCockpitUnitsPerMeter();
bool g_vrSteeringWheel = RuntimeConfigFile::VrSteeringWheel();
bool g_vrNativeSteeringWheel = RuntimeConfigFile::VrNativeSteeringWheel();
bool g_vrHandSteering = RuntimeConfigFile::VrHandSteering();
mkw::vr::WheelTuning g_vrWheelTuning = RuntimeConfigFile::VrWheelTuning();
float g_vrFirstPersonHeadUp = RuntimeConfigFile::VrFirstPersonHeadUpMeters();
float g_vrFirstPersonHeadForward = RuntimeConfigFile::VrFirstPersonHeadForwardMeters();
float g_vrFirstPersonHeadRight = RuntimeConfigFile::VrFirstPersonHeadRightMeters();
bool g_vrFirstPersonHideDriver = RuntimeConfigFile::VrFirstPersonHideDriver();
constexpr std::array<uint32_t, 5> kVrInterpolationFps{0, 1, 72, 90, 120};
constexpr std::array<const char*, 5> kVrInterpolationLabels{"Off", "Auto", "72", "90", "120"};
int g_vrFrameInterpolationMode = [] {
    const auto value = RuntimeConfigFile::VrFrameInterpolationFps();
    return static_cast<int>(std::find(kVrInterpolationFps.begin(), kVrInterpolationFps.end(), value) -
                            kVrInterpolationFps.begin());
}();
int g_vrFirstPersonHiddenModel = RuntimeConfigFile::VrFirstPersonHiddenModel();
bool g_openxrDiagnosticsLogging = RuntimeConfigFile::DiagnosticsOpenXRLogging(false);
// Config spellings and menu labels for the desktop mirror, index-matched to
// AuroraStereoMirrorView so the combo selection converts to either directly.
constexpr std::array<const char*, 5> kVrMirrorViewNames{"normal", "both", "left", "right", "none"};
constexpr std::array<const char*, 5> kVrMirrorViewLabels{"Normal", "Both eyes", "Left eye", "Right eye", "None"};
static_assert(kVrMirrorViewNames.size() == kVrMirrorViewLabels.size());
static_assert(static_cast<int>(AURORA_STEREO_MIRROR_NORMAL) == 0);
static_assert(static_cast<int>(AURORA_STEREO_MIRROR_BOTH_EYES) == 1);
static_assert(static_cast<int>(AURORA_STEREO_MIRROR_LEFT_EYE) == 2);
static_assert(static_cast<int>(AURORA_STEREO_MIRROR_RIGHT_EYE) == 3);
static_assert(static_cast<int>(AURORA_STEREO_MIRROR_NONE) == 4);
int g_vrMirrorView = [] {
    const std::string mode = RuntimeConfigFile::VrMirrorView();
    for (size_t i = 0; i < kVrMirrorViewNames.size(); ++i) {
        if (mode == kVrMirrorViewNames[i]) {
            return static_cast<int>(i);
        }
    }
    return 0;
}();
// Config spellings and menu labels for the VR controllers, index-matched to
// mkw::vr::OpenXRControllerMode.
constexpr std::array<const char*, 2> kVrControllerModeNames{"wii_remote", "gamepad"};
constexpr std::array<const char*, 2> kVrControllerModeLabels{"Wii Remote + Nunchuk", "Gamepad"};
static_assert(static_cast<int>(mkw::vr::OpenXRControllerMode::WiiRemote) == 0);
static_assert(static_cast<int>(mkw::vr::OpenXRControllerMode::Gamepad) == 1);
int g_vrControllerMode = [] {
    const std::string mode = RuntimeConfigFile::VrControllerMode();
    for (size_t i = 0; i < kVrControllerModeNames.size(); ++i) {
        if (mode == kVrControllerModeNames[i]) {
            return static_cast<int>(i);
        }
    }
    return 0;
}();
constexpr std::array<const char*, 3> kVrFirstPersonRotationNames{"yaw", "yaw_pitch", "full"};
int VrFirstPersonRotationIndex(std::string_view mode) {
    for (size_t i = 0; i < kVrFirstPersonRotationNames.size(); ++i) {
        if (mode == kVrFirstPersonRotationNames[i]) {
            return static_cast<int>(i);
        }
    }
    return 0;
}
int g_vrFirstPersonRotation = VrFirstPersonRotationIndex(RuntimeConfigFile::VrFirstPersonRotation());
// SDL_SCANCODE_UNKNOWN means unbound, which is also what an unrecognised
// name in the config file resolves to rather than silently picking a key.
SDL_Scancode g_vrRecenterScancode = [] {
    const std::string name = RuntimeConfigFile::VrRecenterKey();
    return name.empty() ? SDL_SCANCODE_UNKNOWN
                        : SDL_GetScancodeFromName(name.c_str());
}();
bool g_vrRecenterRebinding = false;
float g_vrLeanBackDegrees = RuntimeConfigFile::VrLeanBackDegrees();
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths();
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// PlayStation layout: bumpers drive the GC triggers, Z moves to Create/Share.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kPlayStationPreset = {
    "south", "east", "west", "north", "start", "back",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;

constexpr std::array<ResolutionItem, 8> kResolutions = {{
    {"Auto (window size)", 0.0f}, {"Native (1x)", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f},
    {"3x", 3.0f}, {"4x", 4.0f}, {"6x", 6.0f}, {"8x", 8.0f},
}};

constexpr std::array<uint32_t, 3> kFrameInterpolationTargetFps{0, 120, 180};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
}

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

using ControllerNames::FindNativeButton;

uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};


// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

using ControllerNames::NativeButtonForValue;

std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}


void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y. (Wii Remotes with any
        // extension never reach the PAD layer: the game reads them through KPAD.)
        if (WiiRemoteInput::KindForPort(port) == WiiRemoteInput::Kind::WiiUPro) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled(true);
bool g_wiiContinuousScan = RuntimeConfigFile::WiiContinuousScanEnabled(false);

// Accelerometer readout and zero-point calibration for a bare remote / remote + Nunchuk.
void DrawWiiRemoteAccelerometer(uint32_t port) {
    ImGui::SeparatorText("Accelerometer");
    float sdlG[3] = {};
    float kpad[3] = {};
    if (WiiRemoteInput::ReadAccelDebug(port, sdlG, kpad)) {
        ImGui::Text("KPAD acc: x %+.2f  y %+.2f  z %+.2f g", kpad[0], kpad[1], kpad[2]);
        ImGui::TextDisabled("Flat, buttons up: (0, -1, 0). Sideways as a wheel: (1, 0, 0); z follows the turn.");
    } else {
        ImGui::TextDisabled("No accelerometer data yet.");
    }
    // SDL's read of the remote's calibration block often times out over Bluetooth
    // and it falls back to a nominal zero point, leaving a small per-axis bias;
    // measured here with the remote at rest.
    if (WiiRemoteInput::IsAccelCalibrating()) {
        ImGui::ProgressBar(WiiRemoteInput::AccelCalibrationProgress(), ImVec2(Scaled(220.0f), 0.0f), "Hold still...");
    } else if (ImGui::Button("Calibrate (remote lying flat, buttons up)")) {
        WiiRemoteInput::StartAccelCalibration(port);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Put the remote down on a flat surface with the buttons facing up and do not touch it\n"
                          "for about two seconds. Corrects the steering offset of a remote held sideways.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!RuntimeConfigFile::HasWiiAccelOffset() || WiiRemoteInput::IsAccelCalibrating());
    if (ImGui::Button("Clear calibration")) {
        WiiRemoteInput::ClearAccelCalibration();
    }
    ImGui::EndDisabled();
    if (const char* message = WiiRemoteInput::AccelCalibrationMessage()) {
        ImGui::TextWrapped("%s", message);
    } else if (RuntimeConfigFile::HasWiiAccelOffset()) {
        const std::array<double, 3> offset = RuntimeConfigFile::WiiAccelOffset();
        ImGui::TextDisabled("Stored offset: x %+.3f  y %+.3f  z %+.3f g", offset[0], offset[1], offset[2]);
    } else {
        ImGui::TextDisabled("Not calibrated (using SDL's zero point; see console.log for \"fallback accelerometer calibration\").");
    }
}

// Wii Remotes (Bluetooth) menu: driver switch, pairing help, continuous scanning and the port's controller kind.
void DrawWiiRemoteSettings(uint32_t selectedGamePort) {
    if (!ImGui::BeginMenu("Wii Remotes (Bluetooth)")) {
        return;
    }
    if (ImGui::Checkbox("Use Wii Remotes / Wii U Pro Controllers", &g_wiiRemotesEnabled)) {
        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect on the next launch. Turn this off if you use a Mayflash DolphinBar.");
    }
    ImGui::TextDisabled("Pairing: Windows Settings > Bluetooth > Add device, then press 1+2");
    ImGui::TextDisabled("(or the red SYNC button) on the remote. Leave the PIN empty.");
    ImGui::TextDisabled("A remote that was paired before also needs to be turned on with 1+2/SYNC.");
    if (ImGui::Checkbox("Keep scanning for Wii Remotes (like Dolphin's Continuous Scanning)",
                        &g_wiiContinuousScan)) {
        RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While no Wii controller is connected, re-check Bluetooth every 2 seconds so a\n"
                          "remote that dropped out (\"Communications with the controller have been\n"
                          "interrupted\") or was turned on after launch comes back by itself.");
    }
    // The driver hint is only read at launch, so a rescan after the user turned
    // the setting off would still re-enumerate Wii devices in this session.
    ImGui::BeginDisabled(!g_wiiRemotesEnabled);
    if (ImGui::Button("Rescan now")) {
        WiiRemoteInput::RescanNow();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (WiiRemoteInput::IsScanning()) {
        ImGui::TextDisabled("Scanning... (%u so far) - press 1+2 on the remote", WiiRemoteInput::ScanCount());
    } else {
        ImGui::TextDisabled("Not scanning");
    }
    ImGui::Separator();

    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForPort(selectedGamePort);
    ImGui::Text("Port %u: %s", static_cast<unsigned>(selectedGamePort + 1), WiiRemoteInput::KindLabel(kind));
    if (kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        WiiRemoteInput::KpadSample sample;
        if (WiiRemoteInput::ReadKpadSample(selectedGamePort, sample)) {
            // WPAD_CL_BUTTON_* bits, in the game's own layout (no mapping involved).
            const auto held = [&](uint32_t bit, const char* on, const char* off) { return (sample.clHold & bit) ? on : off; };
            ImGui::Text("Classic: %s %s %s %s  %s %s  %s %s  %s %s  %s %s %s %s", held(0x0010, "A", "a"),
                        held(0x0040, "B", "b"), held(0x0008, "X", "x"), held(0x0020, "Y", "y"), held(0x2000, "L", "l"),
                        held(0x0200, "R", "r"), held(0x0080, "ZL", "zl"), held(0x0004, "ZR", "zr"),
                        held(0x0400, "PLUS", "plus"), held(0x1000, "MINUS", "minus"), held(0x0001, "UP", "up"),
                        held(0x4000, "DOWN", "down"), held(0x0002, "LEFT", "left"), held(0x8000, "RIGHT", "right"));
            ImGui::Text("Sticks: L %+.2f %+.2f (WPAD %+d %+d)  R %+.2f %+.2f (WPAD %+d %+d)", sample.clLStick[0],
                        sample.clLStick[1], static_cast<int>(sample.clLStickRaw[0]),
                        static_cast<int>(sample.clLStickRaw[1]), sample.clRStick[0], sample.clRStick[1],
                        static_cast<int>(sample.clRStickRaw[0]), static_cast<int>(sample.clRStickRaw[1]));
            ImGui::TextDisabled("Capitals = held. The game reads this Classic Controller through KPAD, as on the");
            ImGui::TextDisabled("console: its buttons mean what the game says they mean, no mapping applies.");
        }
    }
    if (kind == WiiRemoteInput::Kind::WiiUPro) {
        if (SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(selectedGamePort))) {
            // SDL's Wii driver posts the D-pad as joystick buttons 11-14 (the
            // SDL_GAMEPAD_BUTTON_DPAD_* values) while its default HIDAPI mapping
            // expects a hat, so SDL_GetGamepadButton never sees them; read the
            // joystick directly, like the fallback in aurora's PADRead does.
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            const auto rawButton = [&](int index) {
                return joystick != nullptr && SDL_GetJoystickButton(joystick, index);
            };
            ImGui::Text("Raw D-pad: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ? "UP" : "up",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? "DOWN" : "down",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? "LEFT" : "left",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? "RIGHT" : "right");
            ImGui::Text("Raw face buttons: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_EAST) ? "A" : "a",
                        rawButton(SDL_GAMEPAD_BUTTON_SOUTH) ? "B" : "b", rawButton(SDL_GAMEPAD_BUTTON_NORTH) ? "X" : "x",
                        rawButton(SDL_GAMEPAD_BUTTON_WEST) ? "Y" : "y");
            ImGui::Text("Raw ZL/ZR: %d / %d (pressed above 0)",
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            ImGui::TextDisabled("Capitals = held. If a button never turns to capitals while physically held,");
            ImGui::TextDisabled("that press is not reaching SDL at all (a driver-level issue, not a mapping one).");
            ImGui::TextDisabled("This pad uses Nintendo's own layout (a/b/x/y as labelled); the shared");
            ImGui::TextDisabled("button mapping above does not apply to it.");
        }
    }
    if (kind == WiiRemoteInput::Kind::Remote || kind == WiiRemoteInput::Kind::RemoteWithNunchuk ||
        kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        DrawWiiRemoteAccelerometer(selectedGamePort);
    }

    ImGui::EndMenu();
}

const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller, MuteHotkey };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

void BeginRebind(RebindKind kind, uint16_t target, const char* label, bool secondary = false) {
    g_rebind = {};
    g_rebind.active = true;
    g_rebind.openPopup = true;
    g_rebind.kind = kind;
    g_rebind.port = static_cast<uint32_t>(g_controllerPort);
    g_rebind.target = target;
    g_rebind.secondary = secondary;
    g_rebind.label = label;
    g_rebind.deadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::copy_n(keys, std::min(count, static_cast<int>(g_rebind.keys.size())), g_rebind.keys.begin());
    g_rebind.mouse = SDL_GetMouseState(nullptr, nullptr);
    const int index = PADGetIndexForPort(g_rebind.port);
    if (kind == RebindKind::Controller && index >= 0) {
        if (auto* pad = PADGetSDLGamepadForIndex(index)) {
            g_rebind.instance = SDL_GetGamepadID(pad);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
                g_rebind.buttons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
                g_rebind.axesReady[i] = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i)))) < 8000;
        }
    }
}

void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target) RuntimeConfigFile::SetControllerButton(i, config);
    } else if (capture.kind == RebindKind::MuteHotkey) {
        g_muteHotkey = static_cast<int32_t>(value);
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_rebind.active = false;
        return;
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

void DrawRebindPrompt() {
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }
    if (!ImGui::BeginPopupModal("Rebind input", &g_rebind.active, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_rebind.active = false;
        return;
    }
    if (g_rebind.active) {
        ImGui::Text("Rebind: %s", g_rebind.label.c_str());
        ImGui::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger, or move a stick."
            : g_rebind.kind == RebindKind::MuteHotkey
                ? "Press a keyboard key."
                : "Press a keyboard key or click a mouse button.");
        ImGui::TextUnformatted("Release any held input first. Backspace or Delete clears the mapping.");
        ImGui::TextUnformatted("Escape can be bound. F10 is reserved for settings.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImGui::Text("Unmapped in %d seconds", std::max(0, static_cast<int>(std::ceil(remaining))));
        const bool clear = ImGui::Button("Clear mapping");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) g_rebind.active = false;
        // UI clicks must not become mouse bindings (buttons activate on release).
        const bool overControl = ImGui::IsAnyItemHovered();
        if (g_rebind.active && (clear || remaining <= 0.0f)) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind != RebindKind::Controller) {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_rebind.active; ++i) {
                if (keys[i] && !g_rebind.keys[i] && i != SDL_SCANCODE_F10) CompleteRebind(i);
                g_rebind.keys[i] = keys[i];
            }
            const uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
            for (int i = 1; i <= 5 && g_rebind.active; ++i)
                if (!overControl && g_rebind.kind != RebindKind::MuteHotkey &&
                    (mouse & ~g_rebind.mouse & (1u << (i - 1))) != 0) CompleteRebind(static_cast<uint32_t>(-i - 1));
            g_rebind.mouse = mouse;
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind == RebindKind::Controller) {
            auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
            if (pad != nullptr) {
                for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
                    const bool pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                    if (pressed && !g_rebind.buttons[i]) CompleteRebind(i);
                    g_rebind.buttons[i] = pressed;
                }
                for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
                    const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
                    if (std::abs(value) < 8000) g_rebind.axesReady[i] = true;
                    if (g_rebind.axesReady[i] && std::abs(value) >= 16384)
                        CompleteRebind(PADEncodeAxisButton(i, value < 0));
                }
            }
        }
    }
    if (!g_rebind.active) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawKeyBinding(const char* label, int scancode, RebindKind kind, uint16_t target,
                    float width = 220.0f) {
    const std::string caption = std::string(KeyBindingName(scancode)) + "##binding";
    if (ImGui::Button(caption.c_str(), ImVec2(width, 0.0f))) BeginRebind(kind, target, label);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);

}

bool DrawKeyboardSettings(uint32_t port) {
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    bool enabled = buttons != nullptr;
    bool usePreset = false;
    if (ImGui::Checkbox("Keyboard and mouse", &enabled)) {
        PADSetKeyboardActive(port, enabled);
        PADSerializeMappings();
        buttons = PADGetKeyButtonBindings(port, &count);
        usePreset = enabled && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        });
    }
    if (!enabled) return false;
    ImGui::TextDisabled("Replaces the gamepad on this port. F10 opens settings.");
    if (ImGui::Button("Use WASD + mouse preset") || usePreset) {
        const std::array<int, PAD_BUTTON_COUNT> keys = {
            PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q,
            SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        };
        for (size_t i = 0; i < keys.size(); ++i)
            PADSetKeyButtonBinding(port, {keys[i], kControllerButtons[i].padButton});
        const std::array<int, PAD_AXIS_COUNT> axes = {
            SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
            SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
            SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
        };
        uint32_t axisCount = 0;
        auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
        for (uint32_t i = 0; i < axisCount; ++i)
            PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
        PADSerializeMappings();
    }
    ImGui::SeparatorText("Button mapping");
    for (uint32_t i = 0; i < count; ++i) {
        int key = buttons[i].scancode;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(PADGetButtonName(buttons[i].padButton), key, RebindKind::KeyboardButton, buttons[i].padButton);
        ImGui::PopID();
    }
    ImGui::SeparatorText("Stick and trigger mapping");
    uint32_t axisCount = 0;
    auto* axes = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; i < axisCount; ++i) {
        int key = axes[i].scancode;
        ImGui::PushID(static_cast<int>(count + i));
        const char* direction = PADGetAxisDirectionLabel(axes[i].padAxis);
        const std::string label = std::string(PADGetAxisName(axes[i].padAxis)) + " " +
                                  (direction != nullptr ? direction : "");
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(label.c_str(), key, RebindKind::KeyboardAxis, axes[i].padAxis);
        ImGui::PopID();
    }
    return true;
}

// Controller settings menu: port selection, controller assignment and button mapping.
int ExpressionResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void DrawExpressionSettings() {
    ImGui::SeparatorText("Expressions (Dolphin syntax)");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Scaled(440.0f));
    ImGui::TextDisabled(
        "Optional. An expression overrides nothing: its result is combined with the "
        "button mapping above. Operators ! & | ^ and functions if, min, max, clamp, "
        "timer, toggle, hold, tap, pulse, smooth, deadzone behave as they do in Dolphin.");
    ImGui::PopTextWrapPos();

    static std::array<std::string, InputBindings::kControls.size()> errors;
    static std::array<std::string, InputBindings::kControls.size()> buffers;
    static std::string importStatus;
    static int loadedPort = -1;
    static bool reloadBuffers = true;
    const auto port = static_cast<uint32_t>(g_controllerPort);

    if (loadedPort != g_controllerPort || reloadBuffers) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            buffers[i] = InputBindings::GetExpression(port, i);
        }
        errors.fill(std::string());
        loadedPort = g_controllerPort;
        reloadBuffers = false;
    }

    if (ImGui::Button("Import from Dolphin")) {
        const std::string path = InputBindings::DefaultDolphinConfigPath();
        std::string summary;
        std::string error;
        if (InputBindings::ImportDolphinConfig(path, g_controllerPort + 1, port, summary, error) < 0) {
            importStatus = error;
        } else {
            importStatus = summary;
            errors.fill(std::string());
            reloadBuffers = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads [GCPad%d] from %%APPDATA%%\\Dolphin Emulator\\Config\\GCPadNew.ini,\n"
                          "or GCPadNew.ini next to the executable.", g_controllerPort + 1);
    }
    if (!importStatus.empty()) {
        ImGui::TextDisabled("%s", importStatus.c_str());
    }

    for (size_t i = 0; i < InputBindings::kControls.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 2000);
        std::string& text = buffers[i];
        ImGui::SetNextItemWidth(Scaled(300.0f));
        if (ImGui::InputText(InputBindings::kControls[i].label, text.data(), text.capacity() + 1,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                             ExpressionResizeCallback, &text)) {
            std::string error;
            errors[i] = InputBindings::SetExpression(port, i, text, error) ? std::string() : error;
        }
        if (InputBindings::IsActive(port, i)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "active");
        }
        if (!errors[i].empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", errors[i].c_str());
        }
        ImGui::PopID();
    }
}

void DrawRumbleSettings() {
    ImGui::SeparatorText("Vibration");
    if (ImGui::Checkbox("Controller vibration", &g_rumbleEnabled)) {
        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
        if (!g_rumbleEnabled) {
            // Stop whatever is already running: the game will not send another
            // motor command until its own state machine decides to.
            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
                PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
            };
            PADControlAllMotors(stopAll.data());
            mkw::vr::OpenXRSetWiiRemoteRumble(false);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Applies to every port.");
    }
}

void DrawControllerSettings() {
    if (ImGui::CollapsingHeader("USB wheel and pedals (player 1)")) {
        physical_wheel::DrawSettings();
        ImGui::Separator();
    }
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const std::string label = "Port " + std::to_string(port + 1);
        ImGui::RadioButton(label.c_str(), &g_controllerPort, port);
        if (port + 1 < PAD_MAX_CONTROLLERS) {
            ImGui::SameLine();
        }
    }

    ImGui::Separator();
    const uint32_t selectedGamePort = static_cast<uint32_t>(g_controllerPort);
    if (DrawKeyboardSettings(selectedGamePort)) {
        return;
    }
    ImGui::Separator();
    const char* currentName = PADGetName(selectedGamePort);
    ImGui::Text("Assigned: %s", currentName != nullptr ? currentName : "None");
    if (ImGui::MenuItem("Unassign controller")) {
        PADClearPort(selectedGamePort);
        g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
    }
    ImGui::Separator();
    controller_mapping_wizard::DrawSetupList();
    DrawWiiRemoteSettings(selectedGamePort);
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        ImGui::TextDisabled("No controller connected");
        return;
    }

    if (ImGui::BeginMenu("Assign connected controller")) {
        for (uint32_t index = 0; index < controllerCount; ++index) {
            const char* name = PADGetNameForControllerIndex(index);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::MenuItem(name != nullptr ? name : "Unknown controller")) {
                PADSetPortForIndex(index, selectedGamePort);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        ImGui::TextDisabled("Assign a controller to edit its buttons");
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings =
        PADGetAltButtonMappings(static_cast<uint32_t>(g_controllerPort), &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeBindingConfig(primaryNative);
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeBindingConfig(altNative);
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding combo without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != g_controllerPort) {
        altRowExpandedPort = g_controllerPort;
        altRowExpanded.fill(false);
    }

    ImGui::SeparatorText("Presets");
    if (ImGui::Button("GameCube")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    const auto applyPreset = [&](const std::array<const char*, PAD_BUTTON_COUNT>& preset) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, preset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    };

    ImGui::SameLine();
    if (ImGui::Button("Classic Controller Pro")) {
        applyPreset(kClassicProPreset);
    }
    ImGui::SameLine();
    if (ImGui::Button("PlayStation")) {
        applyPreset(kPlayStationPreset);
    }

    ImGui::SeparatorText("Button mapping");
    ImGui::TextDisabled("LT / L2 = left trigger. RT / R2 = right trigger.");
    ImGui::TextDisabled("LB / L1 = left shoulder. RB / R1 = right shoulder.");
    ImGui::TextDisabled("Click a binding, then press an input. No input for 10 seconds clears it.");
    const float bindingWidth = ImGui::CalcTextSize("Right shoulder (RB / R1)").x +
                               ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        const auto drawThreshold = [&](PADButtonMapping* mapping, bool secondary) {
            if (!PADIsAxisButton(mapping->nativeButton)) return;
            int threshold = static_cast<int>(PADAxisButtonThreshold(mapping->nativeButton));
            ImGui::SetNextItemWidth(bindingWidth);
            if (ImGui::SliderInt(secondary ? "##altThreshold" : "##primaryThreshold", &threshold,
                                 1, 100, "Threshold: %d%%", ImGuiSliderFlags_AlwaysClamp)) {
                const PADButtonMapping updated = {
                    PADAxisButtonIdentity(mapping->nativeButton) | (static_cast<uint32_t>(threshold) << 8),
                    mapping->padButton,
                };
                if (secondary) PADSetAltButtonMapping(selectedGamePort, updated);
                else PADSetButtonMapping(selectedGamePort, updated);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                writeBinding(i, mappingIt->nativeButton,
                             altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                PADSerializeMappings();
            }
        };
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(bindingWidth);
        const std::string primaryCaption = std::string(current.label) + "##primary";
        if (ImGui::Button(primaryCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
            BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label);
        }
        drawThreshold(mappingIt, false);
        ImGui::EndGroup();
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                ImGui::BeginGroup();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                ImGui::SetNextItemWidth(bindingWidth);
                const std::string altCaption = std::string(altLabel) + "##alt";
                if (ImGui::Button(altCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                drawThreshold(altIt, true);
                ImGui::EndGroup();
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::PopID();
    }
    DrawExpressionSettings();
    DrawRumbleSettings();
}

void DrawAudioSettings() {
    ImGui::SetNextItemWidth(Scaled(220.0f));
    if (ImGui::SliderInt("Master", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    if (ImGui::SliderInt("Music", &g_musicVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_musicVolumePercent) / 100.0f;
        MusicAttenuation::SetMusicVolume(volume);
        RuntimeConfigFile::SetMusicVolume(volume);
    }
    if (ImGui::SliderInt("Sound Effects", &g_soundEffectsVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_soundEffectsVolumePercent) / 100.0f;
        MusicAttenuation::SetSoundEffectsVolume(volume);
        RuntimeConfigFile::SetSoundEffectsVolume(volume);
    }
    if (ImGui::SliderInt("UI", &g_uiVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_uiVolumePercent) / 100.0f;
        MusicAttenuation::SetUiVolume(volume);
        RuntimeConfigFile::SetUiVolume(volume);
    }
    if (ImGui::SliderInt("Voices", &g_voicesVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_voicesVolumePercent) / 100.0f;
        MusicAttenuation::SetVoicesVolume(volume);
        RuntimeConfigFile::SetVoicesVolume(volume);
    }
    const float labelColumn = ImGui::GetCursorPosX() + ImGui::CalcItemWidth();
    if (ImGui::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::SameLine();
    DrawKeyBinding("Mute shortcut", g_muteHotkey, RebindKind::MuteHotkey, 0,
                   std::max(60.0f, labelColumn - ImGui::GetCursorPosX()));
    ImGui::Separator();
    if (ImGui::Checkbox("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Runs the AX/DSP voice mix off the game thread. Turn this off if you "
            "suspect an audio problem; the mix then runs inline as it used to.");
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Mute game music while external media is playing",
                        &g_attenuateMusicWhenMediaPlays)) {
        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
    }
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            ImGui::TextDisabled("External media is playing; game music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            ImGui::TextDisabled("Waiting for media controls...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
            ImGui::TextDisabled("Media controls are unavailable.");
        } else {
            ImGui::TextDisabled("No external media is currently playing.");
        }
    }
}

// The virtual screen's placement comes from the launch-time [vr] geometry, the
// same metres the menu quad is built from, converted into the world units the
// eye replay works in. Those units follow the camera: the first-person view
// renders at its own scale, and the screen has to be sized at the same one or
// it would not stay 2 m across in front of the player.
void ApplyVrHudVirtualScreen() {
    const float unitsPerMeter = mkw::vr::MkwVRPolicyGetSnapshot().EffectiveUnitsPerMeter();
    aurora_set_stereo_hud_screen(g_vrHudVirtualScreen,
                                 RuntimeConfigFile::VrHudWidthMeters(2.4f) * unitsPerMeter,
                                 RuntimeConfigFile::VrHudDistanceMeters(2.0f) * unitsPerMeter);
}

// The cockpit's steering wheel and hand steering, under the first-person camera.
void DrawVrSteeringWheelSettings() {
    ImGui::Separator();
    ImGui::Text("Steering wheel");
    ImGui::BeginDisabled(g_vrFirstPersonSeat != 0);
    if (ImGui::Checkbox("Turn the steering wheel", &g_vrSteeringWheel)) {
        RuntimeConfigFile::SetVrSteeringWheel(g_vrSteeringWheel);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("In the cockpit, the kart's steering wheel or the bike's handlebar turns "
                          "with your steering.");
    }
    ImGui::BeginDisabled(!g_vrSteeringWheel);
    if (ImGui::Checkbox("Use the vehicle's own wheel", &g_vrNativeSteeringWheel)) {
        RuntimeConfigFile::SetVrNativeSteeringWheel(g_vrNativeSteeringWheel);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Turns the wheel or handlebar of the vehicle's own model. Off draws a "
                          "separate VR wheel instead, which is also what appears when a vehicle's "
                          "own wheel cannot be animated.");
    }
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Hand steering (by heurazy)", &g_vrHandSteering)) {
        RuntimeConfigFile::SetVrHandSteering(g_vrHandSteering);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Squeeze a grip near the wheel or handlebar to take hold of it, and turn "
                          "it to steer, with one hand or both. Releasing both grips gives steering "
                          "back to the stick, which still aims items. The runtime's hand mesh is "
                          "used when hand steering was on at launch.");
    }
    if (g_vrHandSteering && ImGui::TreeNode("Hand steering tuning")) {
        bool changed = false;
        changed |= ImGui::SliderFloat("Kart full lock (degrees)", &g_vrWheelTuning.kartDegrees,
                                      RuntimeConfigFile::kVrWheelDegreesMin,
                                      RuntimeConfigFile::kVrWheelDegreesMax, "%.0f");
        changed |= ImGui::SliderFloat("Bike full lock (degrees)", &g_vrWheelTuning.bikeDegrees,
                                      RuntimeConfigFile::kVrWheelDegreesMin,
                                      RuntimeConfigFile::kVrWheelDegreesMax, "%.0f");
        changed |= ImGui::SliderFloat("Grab reach (m)", &g_vrWheelTuning.grabDistance,
                                      RuntimeConfigFile::kVrWheelGrabDistanceMin,
                                      RuntimeConfigFile::kVrWheelGrabDistanceMax, "%.2f");
        changed |= ImGui::SliderFloat("Grab assist", &g_vrWheelTuning.grabAssist,
                                      RuntimeConfigFile::kVrWheelGrabAssistMin,
                                      RuntimeConfigFile::kVrWheelGrabAssistMax, "%.2f");
        changed |= ImGui::SliderFloat("Response", &g_vrWheelTuning.response,
                                      RuntimeConfigFile::kVrWheelResponseMin,
                                      RuntimeConfigFile::kVrWheelResponseMax, "%.2f");
        changed |= ImGui::SliderFloat("Tracking-loss grace (s)", &g_vrWheelTuning.trackingGrace,
                                      RuntimeConfigFile::kVrWheelTrackingGraceMin,
                                      RuntimeConfigFile::kVrWheelTrackingGraceMax, "%.2f");
        changed |= ImGui::Checkbox("Grab and release pulse", &g_vrWheelTuning.haptics);
        if (ImGui::Button("Reset hand steering tuning")) {
            g_vrWheelTuning = {};
            changed = true;
        }
        if (changed) {
            RuntimeConfigFile::SetVrWheelTuning(g_vrWheelTuning);
        }
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    // What the cockpit found, for a report when the wheel does not behave.
    const auto anchor = mkw::vr::MkwVRFirstPersonGetAnchor();
    if (anchor.valid && anchor.cockpit) {
        ImGui::TextDisabled("Cockpit: %s, %s, %.0f units/m, animated draws %u",
                            anchor.bike ? "handlebar" : "wheel",
                            !anchor.native_wheel.valid ? "grips not found"
                            : anchor.native_mesh_prepared ? "vehicle's own"
                                                          : "VR wheel",
                            anchor.units_per_meter, GxNativeWheel::LastDrawCount());
    }
}

void DrawGraphicsSettings() {
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    struct EffectFlag {
        const char* label;
        uint32_t flag;
    };
    static constexpr std::array<EffectFlag, 1> kEffectFlags = {{
        {"Disable bloom", RuntimeConfigFile::kPostProcessingBloomPath},
    }};

    for (const auto& effect : kEffectFlags) {
        bool disabled = (g_disabledPostProcessingPaths & effect.flag) != 0;
        if (ImGui::Checkbox(effect.label, &disabled)) {
            if (disabled) {
                g_disabledPostProcessingPaths |= effect.flag;
            } else {
                g_disabledPostProcessingPaths &= ~effect.flag;
            }
            RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
            RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
        }
    }
    ImGui::TextDisabled("Applied when the next scene renderer is created.");
    ImGui::Separator();
    static constexpr const char* kDisplayModes[] = {
        "Windowed",
        "Borderless fullscreen",
        "Exclusive fullscreen",
    };
    if (ImGui::Combo("Display mode", &g_displayMode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)))) {
        const auto mode = static_cast<AuroraDisplayMode>(g_displayMode);
        aurora_set_display_mode(mode);
        const AuroraDisplayMode activeMode = aurora_get_display_mode();
        if (activeMode == mode) {
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        } else {
            g_displayMode = static_cast<int>(activeMode);
        }
    }
    if (g_displayMode == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        ImGui::TextDisabled(
            "Requests the closest native-resolution display mode to the output frame "
            "rate (60 Hz, or the frame interpolation target).");
    }
    constexpr std::array<const char*, 3> kFrameInterpolationModes{
        "Off", "120 FPS", "180 FPS",
    };
    const char* currentFrameInterpolationMode =
        kFrameInterpolationModes[static_cast<size_t>(g_frameInterpolationMode)];
    bool frameInterpolationModeChanged = false;
    if (ImGui::BeginCombo("Race frame interpolation (experimental)", currentFrameInterpolationMode)) {
        for (int mode = 0; mode < static_cast<int>(kFrameInterpolationModes.size()); ++mode) {
            const bool selected = g_frameInterpolationMode == mode;
            if (ImGui::Selectable(kFrameInterpolationModes[static_cast<size_t>(mode)], selected)) {
                g_frameInterpolationMode = mode;
                frameInterpolationModeChanged = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (frameInterpolationModeChanged) {
        const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
        aurora_set_frame_interpolation_fps(targetFps);
        RuntimeConfigFile::SetFrameInterpolationFps(targetFps);
        LimitResolutionForFrameRate();
        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
            // Re-apply exclusive mode so the display refresh tracks the new target.
            aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
        }
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Scaled(380.0f));
    ImGui::TextDisabled("Frame interpolation is experimental, you might find visual artifacts");
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox("Disable copy filter", &g_disableCopyFilter)) {
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (ImGui::Checkbox("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    if (ImGui::Checkbox("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", GraphicsApiDisplayName());
}

// The VR settings live in their own top-bar menu: they are a self-contained
// group, and keeping them out of Graphics stops that menu from running off the
// bottom of the screen.
void DrawVrSettings() {
    const auto xrError = mkw::vr::OpenXRLastError();
    if (!xrError.empty()) {
        ImGui::TextWrapped("OpenXR unavailable: %s", xrError.c_str());
        ImGui::TextWrapped("Playing on the desktop. Check your headset and active OpenXR runtime, then restart.");
    }
    if (ImGui::Checkbox("Enable OpenXR VR", &g_vrEnabled)) {
        RuntimeConfigFile::SetVrEnabled(g_vrEnabled);
    }
    ImGui::TextDisabled("OpenXR mode changes take effect after restarting the game.");
    // Live, unlike the enable toggle above, so it is left usable either way:
    // set before a restart it is simply what the next session starts on.
    if (ImGui::Combo("Desktop view", &g_vrMirrorView, kVrMirrorViewLabels.data(),
                     static_cast<int>(kVrMirrorViewLabels.size()))) {
        aurora_set_stereo_mirror_view(static_cast<AuroraStereoMirrorView>(g_vrMirrorView));
        RuntimeConfigFile::SetVrMirrorView(kVrMirrorViewNames[static_cast<size_t>(g_vrMirrorView)]);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "What this window shows while the headset is running. Normal keeps the ordinary "
            "desktop view, the eye choices mirror what you are actually seeing in the headset, "
            "and None leaves the window black. Menus reach the headset as a screen showing this "
            "same desktop image, so the eye choices only differ from Normal during a race.");
    }
    ImGui::BeginDisabled(!mkw::vr::OpenXRIsRunning());
    bool settingsPanelOpen = mkw::vr::OpenXRSettingsPanelOpen();
    if (ImGui::Checkbox("Show these settings in the headset", &settingsPanelOpen)) {
        mkw::vr::OpenXRSetSettingsPanelOpen(settingsPanelOpen);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Opens these settings on a panel in front of you, in menus and races alike.\n"
            "In the headset, left Y opens and closes it too (with Gamepad VR controllers,\n"
            "click both thumbsticks together instead).\n"
            "Aim at it and pull a trigger to change a setting; push a thumbstick to scroll.\n"
            "While it is open the game does not see the VR controllers.");
    }
    if (ImGui::Combo("VR controllers", &g_vrControllerMode, kVrControllerModeLabels.data(),
                     static_cast<int>(kVrControllerModeLabels.size()))) {
        mkw::vr::OpenXRSetControllerMode(static_cast<mkw::vr::OpenXRControllerMode>(g_vrControllerMode));
        RuntimeConfigFile::SetVrControllerMode(kVrControllerModeNames[static_cast<size_t>(g_vrControllerMode)]);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Wii Remote + Nunchuk: the right controller is a Wii Remote, with motion and a pointer "
            "that lands where you aim on the virtual screen; the left one is the Nunchuk.\n"
            "  Right: A = A, trigger = B, B = C (look behind), stick up/down = 1/2\n"
            "  Left: stick = Nunchuk stick, trigger = Z, X = -, menu = +, Y = settings panel\n"
            "  The grips press nothing; they take hold of the wheel with hand steering.\n"
            "Gamepad: both controllers are one ordinary controller, read as a GameCube pad.\n"
            "Applies immediately; the game sees the controller change as a reconnection.");
    }
    if (mkw::vr::OpenXRIsRunning() &&
        mkw::vr::OpenXRGetControllerMode() == mkw::vr::OpenXRControllerMode::WiiRemote) {
        mkw::vr::OpenXRWiiRemoteSample remote;
        if (mkw::vr::OpenXRReadWiiRemote(remote)) {
            if (remote.pointer_valid) {
                ImGui::TextDisabled("Pointer %+.2f %+.2f | Remote %+.2f %+.2f %+.2f g", remote.pointer[0],
                                    remote.pointer[1], remote.acc[0], remote.acc[1], remote.acc[2]);
            } else {
                ImGui::TextDisabled("Pointer off screen | Remote %+.2f %+.2f %+.2f g", remote.acc[0],
                                    remote.acc[1], remote.acc[2]);
            }
        }
    }

    if (ImGui::Combo("VR frame interpolation (experimental)", &g_vrFrameInterpolationMode,
                     kVrInterpolationLabels.data(), static_cast<int>(kVrInterpolationLabels.size()))) {
        const auto target = kVrInterpolationFps[static_cast<size_t>(g_vrFrameInterpolationMode)];
        mkw::vr::OpenXRSetFrameInterpolationFps(target);
        RuntimeConfigFile::SetVrFrameInterpolationFps(target);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Auto matches the headset refresh rate. 72, 90 and 120 cap the scene rendering rate; "
            "set the headset's refresh rate in Virtual Desktop or your VR runtime. "
            "The game stays at 60 Hz. Adds one game frame of scene latency; head tracking stays current. "
            "Needs GPU headroom and may show interpolation artifacts. Applies immediately.");
    }
    const auto xrTiming = mkw::vr::OpenXRGetFrameTiming();
    if (mkw::vr::OpenXRIsRunning()) {
        ImGui::TextDisabled("Headset: %.1f Hz | New VR frames: %.1f FPS", xrTiming.headset_hz, xrTiming.rendered_fps);
        if (g_vrFrameInterpolationMode != 0 && !mkw::vr::OpenXRFrameInterpolationAvailable()) {
            ImGui::TextWrapped("The OpenXR runtime does not provide the clock conversion needed for interpolation.");
        }
    }

    // Like the mirror above and unlike the enable toggle, these two apply to the
    // very next frame, so they can be compared from inside a running race.
    ImGui::Separator();
    ImGui::Text("VR eye replay (EFB)");
    if (ImGui::Checkbox("Stop eye at display copy", &g_vrStopAtDisplayCopy)) {
        aurora_set_stereo_stop_at_display_copy(g_vrStopAtDisplayCopy);
        RuntimeConfigFile::SetVrStopAtDisplayCopy(g_vrStopAtDisplayCopy);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Ends each eye at the frame's final GXCopyDisp, so an eye holds exactly the "
            "image the game presented. Turn off to replay the whole pass list.");
    }
    if (ImGui::Checkbox("Skip EFB copy clears", &g_vrSkipCopyClears)) {
        aurora_set_stereo_skip_copy_clears(g_vrSkipCopyClears);
        RuntimeConfigFile::SetVrSkipCopyClears(g_vrSkipCopyClears);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Drops the EFB reset a GX copy performs after copying. That reset prepares the "
            "Wii's reused EFB for the next frame; an eye attachment is built fresh, so "
            "replaying it only erases the eye.");
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Scaled(380.0f));
    ImGui::TextDisabled(
        "Both apply on the next frame. Turning either off restores the raw replay and is "
        "expected to black out the eyes.");
    ImGui::PopTextWrapPos();
    // Shows the live state, which the Quest's debug.wiicompiled.eye_passes can override.
    g_vrSinglePassEyes = aurora_get_stereo_single_pass_eyes();
    if (ImGui::Checkbox("One render pass per eye", &g_vrSinglePassEyes)) {
        aurora_set_stereo_single_pass_eyes(g_vrSinglePassEyes);
        RuntimeConfigFile::SetVrSinglePassEyes(g_vrSinglePassEyes);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Keeps drawing each eye in the render pass it has open across the frame's GX "
            "copies, which only the desktop image performs, and skips what a later clear "
            "erases. Same picture, less GPU memory traffic; turn off to compare.");
    }
    ImGui::Separator();
    ImGui::Text("VR 2D layer");
    ImGui::BeginDisabled(g_vrFlatScreen);
    if (ImGui::Checkbox("2D layer on a virtual screen", &g_vrHudVirtualScreen)) {
        ApplyVrHudVirtualScreen();
        RuntimeConfigFile::SetVrHudVirtualScreen(g_vrHudVirtualScreen);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Puts the minimap, race position, item roulette and the rest of the race HUD on a "
            "screen fixed ahead of the kart camera. Turn off to leave them stretched across "
            "the whole view. Its size and distance are the [vr] hud_width_meters and "
            "hud_distance_meters read at launch.");
    }
    ImGui::Separator();
    ImGui::Text("VR view");
    if (ImGui::Button("Recenter view")) {
        mkw::vr::OpenXRRequestRecenter();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Makes where you are sitting right now the centre of the view, and brings "
            "the menu screen back upright in front of you. The race view moves in "
            "position only, so the horizon stays level and forward is unchanged; use "
            "your headset's own recenter to change forward.");
    }
    ImGui::SameLine();
    // Click to arm, then the next key press is captured in HandleEvents.
    if (g_vrRecenterRebinding) {
        if (ImGui::Button("Press a key... (Esc to cancel)###VrRecenterKey")) {
            g_vrRecenterRebinding = false;
        }
    } else {
        const char* name = g_vrRecenterScancode == SDL_SCANCODE_UNKNOWN
                               ? nullptr
                               : SDL_GetScancodeName(g_vrRecenterScancode);
        const std::string label =
            std::string("Hotkey: ") + ((name && *name) ? name : "unbound") +
            "###VrRecenterKey";
        if (ImGui::Button(label.c_str())) {
            g_vrRecenterRebinding = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to rebind. Esc cancels, Backspace unbinds.");
        }
    }
    ImGui::BeginDisabled(g_vrFlatScreen);
    if (ImGui::SliderFloat("Lean back angle (deg)", &g_vrLeanBackDegrees,
                           -RuntimeConfigFile::kVrLeanBackDegreesLimit,
                           RuntimeConfigFile::kVrLeanBackDegreesLimit, "%.1f")) {
        mkw::vr::OpenXRSetLeanBackDegrees(g_vrLeanBackDegrees);
        RuntimeConfigFile::SetVrLeanBackDegrees(g_vrLeanBackDegrees);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Tilts the game camera back with you when you play reclined, so set it to "
            "roughly how far back your seat is and the track comes back in front of you "
            "instead of above. 0 applies no tilt. Unlike recentering, this deliberately "
            "does pitch the view, and looking sideways while it is set will roll the "
            "horizon the way a real recline would.");
    }
#if defined(__ANDROID__)
    if (ImGui::Checkbox("Passthrough around the menu screen", &g_vrPassthrough)) {
        mkw::vr::OpenXRSetPassthrough(g_vrPassthrough);
        RuntimeConfigFile::SetVrPassthrough(g_vrPassthrough);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Shows your room through the headset's cameras around the menu screen and every "
            "other screen outside an immersive race, instead of black. Immersive races stay "
            "fully virtual; the Flat Screen race has the room around it too. "
            "Applies immediately.");
    }
#endif
    ImGui::Separator();
    ImGui::Text("VR camera");
    if (ImGui::Checkbox("Flat Screen mode", &g_vrFlatScreen)) {
        RuntimeConfigFile::SetVrFlatScreen(g_vrFlatScreen);
        mkw::vr::MkwVRPolicySetImmersiveRaces(!g_vrFlatScreen);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Plays races on the same flat screen as the menus, through the game's own camera, "
            "instead of all around you in stereo. The first-person camera, hand steering and "
            "the race view settings do not apply while it is on. Applies immediately.");
    }
    // Everything below shapes the immersive race view, which Flat Screen mode replaces.
    ImGui::BeginDisabled(g_vrFlatScreen);
    if (ImGui::Checkbox("First-person camera", &g_vrFirstPerson)) {
        RuntimeConfigFile::SetVrFirstPerson(g_vrFirstPerson);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Also toggled by clicking the right thumbstick, on the VR controllers or on a gamepad.");
    }
    if (ImGui::Checkbox("Right thumbstick click toggles it", &g_vrFirstPersonToggleClick)) {
        RuntimeConfigFile::SetVrFirstPersonToggleClick(g_vrFirstPersonToggleClick);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Moves the camera to the Player 1 driver's head and keeps the horizon level, "
            "instead of riding behind the kart. Applies during a single-screen race; menus "
            "and split-screen are unaffected.");
    }
    constexpr std::array<const char*, 2> kSeatLabels{"Cockpit", "Custom"};
    if (ImGui::Combo("Seat", &g_vrFirstPersonSeat, kSeatLabels.data(), static_cast<int>(kSeatLabels.size()))) {
        RuntimeConfigFile::SetVrFirstPersonSeat(kVrFirstPersonSeatNames[static_cast<size_t>(g_vrFirstPersonSeat)]);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
        ApplyVrHudVirtualScreen();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Cockpit sits you at the driver's own eyes, behind the steering wheel, at a "
            "life-size scale that allows for the character's height, so the wheel is within "
            "reach. Custom places the head by the world scale and offsets below instead.");
    }
    if (g_vrFirstPersonSeat == 0) {
        if (ImGui::SliderFloat("Cockpit scale (units per metre)", &g_vrCockpitUnitsPerMeter,
                               RuntimeConfigFile::kVrCockpitUnitsPerMeterMin,
                               RuntimeConfigFile::kVrCockpitUnitsPerMeterMax, "%.0f")) {
            RuntimeConfigFile::SetVrCockpitUnitsPerMeter(g_vrCockpitUnitsPerMeter);
            mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
            ApplyVrHudVirtualScreen();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "100 reads life-size for an average-height driver; taller characters raise it "
                "further on their own. Raising it shrinks the world around you.");
        }
    }
    // These are the tuning loop for the anchor: the right head height is a
    // per-taste value that can only really be judged from inside the headset.
    ImGui::BeginDisabled(g_vrFirstPersonSeat == 0);
    if (ImGui::SliderFloat("World units per metre (first person)", &g_vrFirstPersonUnitsPerMeter,
                           1.0f, 200.0f, "%.1f")) {
        RuntimeConfigFile::SetVrFirstPersonUnitsPerMeter(g_vrFirstPersonUnitsPerMeter);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
        // The virtual screen's metres are converted at this same scale.
        ApplyVrHudVirtualScreen();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Mario Kart Wii is authored at about 10 units per metre, which is what makes the "
            "race read life-size. Raising this shrinks the world around you.");
    }
    bool headOffsetsChanged = false;
    headOffsetsChanged |=
        ImGui::SliderFloat("Head height (m)", &g_vrFirstPersonHeadUp, -RuntimeConfigFile::kVrFirstPersonHeadOffsetLimit, RuntimeConfigFile::kVrFirstPersonHeadOffsetLimit, "%.2f");
    headOffsetsChanged |=
        ImGui::SliderFloat("Head forward (m)", &g_vrFirstPersonHeadForward, -20.0f, 20.0f, "%.2f");
    headOffsetsChanged |=
        ImGui::SliderFloat("Head sideways (m)", &g_vrFirstPersonHeadRight, -RuntimeConfigFile::kVrFirstPersonHeadOffsetLimit, RuntimeConfigFile::kVrFirstPersonHeadOffsetLimit, "%.2f");
    if (headOffsetsChanged) {
        RuntimeConfigFile::SetVrFirstPersonHeadUpMeters(g_vrFirstPersonHeadUp);
        RuntimeConfigFile::SetVrFirstPersonHeadForwardMeters(g_vrFirstPersonHeadForward);
        RuntimeConfigFile::SetVrFirstPersonHeadRightMeters(g_vrFirstPersonHeadRight);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Scaled(380.0f));
    ImGui::TextDisabled("Custom seat: where the head sits in the kart's own frame.");
    ImGui::PopTextWrapPos();
    ImGui::EndDisabled();
    constexpr std::array<const char*, 3> kRotationLabels{"Yaw only", "Yaw + Pitch", "Full rotation"};
    if (ImGui::Combo("View rotation", &g_vrFirstPersonRotation, kRotationLabels.data(),
                     static_cast<int>(kRotationLabels.size()))) {
        RuntimeConfigFile::SetVrFirstPersonRotation(
            kVrFirstPersonRotationNames[static_cast<size_t>(g_vrFirstPersonRotation)]);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Where the view's orientation comes from. Yaw only keeps the horizon level "
            "and is the comfortable choice. Yaw + Pitch adds the kart's climb, so slopes "
            "and wheelies tip the view without ever rolling it. Full rotation takes the "
            "kart's whole orientation, banking included. The headset always adds free look "
            "on top.");
    }
    // Two presentations of one setting: which models go, or none at all.
    // Ticking either replaces the other, and unticking both shows everything.
    const auto applyHiding = [](bool enabled, int model) {
        g_vrFirstPersonHideDriver = enabled;
        if (enabled) {
            g_vrFirstPersonHiddenModel = model;
            RuntimeConfigFile::SetVrFirstPersonHiddenModel(model);
        }
        RuntimeConfigFile::SetVrFirstPersonHideDriver(enabled);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    };
    bool hideDriver = g_vrFirstPersonHideDriver && g_vrFirstPersonHiddenModel >= 0;
    bool hideDriverAndKart = g_vrFirstPersonHideDriver && g_vrFirstPersonHiddenModel < 0;
    if (ImGui::Checkbox("Hide driver", &hideDriver)) {
        applyHiding(hideDriver, RuntimeConfigFile::kVrFirstPersonHiddenModelDefault);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Removes your character and leaves the kart around you. Their head would "
            "otherwise be where your eyes are. Other racers are unaffected.");
    }
    if (ImGui::Checkbox("Hide driver and kart", &hideDriverAndKart)) {
        applyHiding(hideDriverAndKart, -1);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Removes the vehicle as well, leaving nothing of your own kart.");
    }
    DrawVrSteeringWheelSettings();
    ImGui::Separator();
    if (ImGui::Button("Reset first-person defaults")) {
        g_vrFirstPersonSeat = 0;
        g_vrCockpitUnitsPerMeter = RuntimeConfigFile::kVrCockpitUnitsPerMeterDefault;
        g_vrSteeringWheel = RuntimeConfigFile::kVrSteeringWheelDefault;
        g_vrNativeSteeringWheel = RuntimeConfigFile::kVrNativeSteeringWheelDefault;
        g_vrHandSteering = RuntimeConfigFile::kVrHandSteeringDefault;
        RuntimeConfigFile::SetVrFirstPersonSeat(RuntimeConfigFile::kVrFirstPersonSeatDefault);
        RuntimeConfigFile::SetVrCockpitUnitsPerMeter(g_vrCockpitUnitsPerMeter);
        RuntimeConfigFile::SetVrSteeringWheel(g_vrSteeringWheel);
        RuntimeConfigFile::SetVrNativeSteeringWheel(g_vrNativeSteeringWheel);
        RuntimeConfigFile::SetVrHandSteering(g_vrHandSteering);
        g_vrFirstPersonUnitsPerMeter = RuntimeConfigFile::kVrFirstPersonUnitsPerMeterDefault;
        g_vrFirstPersonHeadUp = RuntimeConfigFile::kVrFirstPersonHeadUpDefault;
        g_vrFirstPersonHeadForward = RuntimeConfigFile::kVrFirstPersonHeadForwardDefault;
        g_vrFirstPersonHeadRight = RuntimeConfigFile::kVrFirstPersonHeadRightDefault;
        g_vrFirstPersonHideDriver = RuntimeConfigFile::kVrFirstPersonHideDriverDefault;
        g_vrFirstPersonHiddenModel = RuntimeConfigFile::kVrFirstPersonHiddenModelDefault;
        g_vrFirstPersonRotation =
            VrFirstPersonRotationIndex(RuntimeConfigFile::kVrFirstPersonRotationDefault);
        RuntimeConfigFile::SetVrFirstPersonRotation(
            RuntimeConfigFile::kVrFirstPersonRotationDefault);
        RuntimeConfigFile::SetVrFirstPersonUnitsPerMeter(g_vrFirstPersonUnitsPerMeter);
        RuntimeConfigFile::SetVrFirstPersonHeadUpMeters(g_vrFirstPersonHeadUp);
        RuntimeConfigFile::SetVrFirstPersonHeadForwardMeters(g_vrFirstPersonHeadForward);
        RuntimeConfigFile::SetVrFirstPersonHeadRightMeters(g_vrFirstPersonHeadRight);
        RuntimeConfigFile::SetVrFirstPersonHideDriver(g_vrFirstPersonHideDriver);
        RuntimeConfigFile::SetVrFirstPersonHiddenModel(g_vrFirstPersonHiddenModel);
        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
        ApplyVrHudVirtualScreen();
    }
    ImGui::EndDisabled();
}

// The right-thumbstick click: flips the first-person camera exactly as its
// checkbox does, so it does nothing in Flat Screen mode either. Game thread.
void ToggleFirstPersonCamera() {
    if (g_vrFlatScreen) {
        return;
    }
    g_vrFirstPerson = !g_vrFirstPerson;
    RuntimeConfigFile::SetVrFirstPerson(g_vrFirstPerson);
    mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person camera " << (g_vrFirstPerson ? "on" : "off")
                           << " (right thumbstick click)" << std::endl;
}

// A gamepad whose right thumbstick click reaches the game (bound to a
// GameCube control on its port) keeps it; toggling the camera as well would
// fire both.
bool RightStickDrivesGame(SDL_Gamepad* gamepad) {
    if (gamepad == nullptr) {
        return false;
    }
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0 || PADGetSDLGamepadForIndex(static_cast<u32>(index)) != gamepad) {
            continue;
        }
        for (auto* mappings : {&PADGetButtonMappings, &PADGetAltButtonMappings}) {
            u32 count = 0;
            const PADButtonMapping* list = (*mappings)(port, &count);
            for (u32 i = 0; list != nullptr && i < count; ++i) {
                if (list[i].nativeButton == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
                    return true;
                }
            }
        }
        for (size_t control = 0; control < InputBindings::kControls.size(); ++control) {
            const std::string expression = InputBindings::GetExpression(port, control);
            if (expression.find("Thumb R") != std::string::npos || expression.find("Button 11") != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

// A physical gamepad's right-thumbstick click, while VR runs.
void HandleGamepadFirstPersonClick(const SDL_GamepadButtonEvent& event) {
    const bool right = event.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    const bool left = event.button == SDL_GAMEPAD_BUTTON_LEFT_STICK;
    if ((!right && !left) || mkw::vr::OpenXRIsControllerGamepad(event.which)) {
        return;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromID(event.which);
    auto& click = g_gamepadFirstPersonClicks[event.which];
    const bool held = right ? event.down : click.Held();
    const bool partner = left ? event.down : gamepad != nullptr && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    const bool blocked = g_rebind.active || InputBindings::InputBlocked();
    if (click.Update(held, partner, blocked) && g_vrFirstPersonToggleClick && mkw::vr::OpenXRIsRunning() &&
        !RightStickDrivesGame(gamepad)) {
        ToggleFirstPersonCamera();
    }
}

// Export Logs. SDL shows the folder picker without blocking the game and calls
// back on a thread of its choosing (its own dialog thread on Windows), where the
// copy then runs; the menu only reads the outcome through this state.
struct LogExportState {
    std::mutex mutex;
    std::string note;
    std::string message;
    bool failed = false;
};
LogExportState g_logExport;
std::atomic_bool g_logExportInProgress{false};

void SetLogExportMessage(std::string message, bool failed) {
    std::lock_guard lock(g_logExport.mutex);
    g_logExport.message = std::move(message);
    g_logExport.failed = failed;
}

// Captured on the click, so the export describes the moment the player asked.
std::string BuildLogExportNote() {
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    std::ostringstream note;
    note << "Exported by process " << pid << "; its run folder under Logs ends in _pid" << pid << ".\n"
         << "OpenXR diagnostic logging: " << (g_openxrDiagnosticsLogging ? "on" : "off") << '\n'
         << "OpenXR running: " << (mkw::vr::OpenXRIsRunning() ? "yes" : "no") << '\n';
    if (const auto xrError = mkw::vr::OpenXRLastError(); !xrError.empty()) {
        note << "OpenXR last error: " << xrError << '\n';
    }
    return note.str();
}

void SDLCALL OnLogExportFolderChosen(void*, const char* const* filelist, int) {
    // SDL's C caller must never see an exception.
    try {
        if (filelist == nullptr) {
            SetLogExportMessage(std::string("Could not open the folder picker: ") + SDL_GetError(), true);
        } else if (filelist[0] == nullptr) {
            SetLogExportMessage("Export canceled.", false);
        } else {
            std::string note;
            {
                std::lock_guard lock(g_logExport.mutex);
                note = g_logExport.note;
            }
            SetLogExportMessage("Exporting...", false);
            const auto result = log_export::ExportLogs(
                RuntimeConfigFile::ApplicationDataDirectory() / "Logs", RuntimeConfigFile::ResolveConfigPath(),
                RuntimeConfigFile::PathFromUtf8(filelist[0]), note, std::chrono::system_clock::now());
            const std::string destination = RuntimeConfigFile::PathToUtf8(result.destination);
            if (result.Succeeded()) {
                SetLogExportMessage("Exported " + std::to_string(result.files_copied) + " files to " + destination,
                                    false);
            } else if (!result.destination.empty()) {
                SetLogExportMessage("Exported " + std::to_string(result.files_copied) + " files to " + destination +
                                        ", but " + std::to_string(result.files_failed) +
                                        " could not be copied: " + result.error,
                                    true);
            } else {
                SetLogExportMessage("Export failed: " + result.error, true);
            }
            RT_LOG(RT_TAG_RUNTIME) << "Log export to " << destination << ": " << result.files_copied
                                   << " file(s) copied, " << result.files_failed << " failed"
                                   << (result.error.empty() ? "" : " (" + result.error + ")") << std::endl;
        }
    } catch (const std::exception& exception) {
        SetLogExportMessage(std::string("Export failed: ") + exception.what(), true);
    } catch (...) {
        SetLogExportMessage("Export failed.", true);
    }
    g_logExportInProgress.store(false, std::memory_order_release);
}

void StartLogExport() {
    bool expected = false;
    if (!g_logExportInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard lock(g_logExport.mutex);
        g_logExport.note = BuildLogExportNote();
    }
    SetLogExportMessage("Choose the folder to export the logs into.", false);
    // Parented to the game window so the picker opens in front of it. SDL may
    // call back before returning if the dialog cannot be shown at all.
    SDL_ShowOpenFolderDialog(&OnLogExportFolderChosen, nullptr, SDL_GetKeyboardFocus(), nullptr, false);
}

void DrawDiagnosticsSettings() {
    if (ImGui::Checkbox("OpenXR diagnostic logging", &g_openxrDiagnosticsLogging)) {
        mkw::vr::diagnostics::SetEnabled(g_openxrDiagnosticsLogging);
        RuntimeConfigFile::SetDiagnosticsOpenXRLogging(g_openxrDiagnosticsLogging);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Writes VR frame timing to console.log once per second: late, skipped, repeated and\n"
            "empty (black) frames, how long each frame waited for the game and for rendering,\n"
            "head-tracking loss, reference-space changes, and the headset's view layout.\n"
            "Turn it on to report stutter or black frames in VR, and off again afterwards.\n"
            "Off by default. Applies immediately and is remembered.");
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Scaled(380.0f));
    if (g_openxrDiagnosticsLogging && !mkw::vr::OpenXRIsRunning()) {
        ImGui::TextDisabled("OpenXR is not running, so nothing is logged until a VR session starts.");
    }
    ImGui::PopTextWrapPos();

    ImGui::Separator();
    const bool exporting = g_logExportInProgress.load(std::memory_order_acquire);
    ImGui::BeginDisabled(exporting);
    if (ImGui::Button("Export Logs")) {
        StartLogExport();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Choose a folder, and the logs of recent sessions (the last four days, this one\n"
            "included) are copied into a new WiiCompiled-logs folder there, together with\n"
            "Config.toml. Zip that folder to attach it to a bug report.");
    }
    std::string message;
    bool failed = false;
    {
        std::lock_guard lock(g_logExport.mutex);
        message = g_logExport.message;
        failed = g_logExport.failed;
    }
    if (!message.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Scaled(380.0f));
        if (failed) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", message.c_str());
        } else {
            ImGui::TextDisabled("%s", message.c_str());
        }
        ImGui::PopTextWrapPos();
    }
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    if (!g_showFps) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
        if (presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else {
            // Present timing includes the additional frames produced by
            // interpolation, so this remains the actual displayed FPS.
            ImGui::Text("FPS: %.1f", presentTiming.framesPerSecond);
            // Replay-unsafe frames hold the presented cadence with duplicated
            // slots, so the counter alone reads 180 while the motion on screen
            // is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond <
                presentTiming.framesPerSecond * 0.95) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Motion: %.1f",
                                   presentTiming.effectiveFramesPerSecond);
            }
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Wiicompiled Startup", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(1.25f);
        constexpr const char* kTitle = "WiiCompiled";
        const ImVec2 titleSize = ImGui::CalcTextSize(kTitle);
        const float titleX = std::max(0.0f, (viewport->Size.x - titleSize.x) * 0.5f);
        const float startY = std::max(0.0f, (viewport->Size.y - titleSize.y) * 0.5f);
        ImGui::SetCursorPos(ImVec2(titleX, startY));
        ImGui::TextUnformatted(kTitle);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void DrawExitPrompt() {
    constexpr const char* kTitle = "Exit";
    if (g_exitPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &g_exitPromptOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Quit the game?");
    if (ImGui::Button("Exit", ImVec2(120.0f, 0.0f))) ExitForAuroraWindowClose();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) g_exitPromptOpen = false;
    if (!g_exitPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawResolutionMenu() {
    const auto resolutionIt = std::find_if(kResolutions.begin(), kResolutions.end(), [](const ResolutionItem& item) {
        return std::fabs(item.scale - g_resolutionScale) < 0.001f;
    });
    const char* resolutionLabel = resolutionIt != kResolutions.end() ? resolutionIt->label : "Custom";
    const std::string resolutionMenuLabel = std::string("Resolution: ") + resolutionLabel;
    if (ImGui::BeginMenu(resolutionMenuLabel.c_str())) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - g_resolutionScale) < 0.001f;
            const bool disabled = IsHighFrameRateMode() && IsHighResolutionScale(resolution.scale);
            ImGui::BeginDisabled(disabled);
            const bool clicked = ImGui::MenuItem(resolution.label, nullptr, selected);
            ImGui::EndDisabled();
            if (clicked) {
                SetResolutionScale(resolution.scale);
            }
        }
        ImGui::EndMenu();
    }
}

void DrawTopBar() {
    if (!g_topBarVisible) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 70));
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                 viewport->Pos.y + viewport->Size.y - 24.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("Settings input hint", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextUnformatted("Settings open - game controls disabled. Press F10 to return to the game.");
    }
    ImGui::End();
    if (!ImGui::BeginMainMenuBar()) return;

    ImGui::TextUnformatted("WiiCompiled");
    ImGui::Separator();
    DrawResolutionMenu();

    if (ImGui::BeginMenu("Graphics")) {
        DrawGraphicsSettings();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("VR")) {
        DrawVrSettings();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Controller settings")) {
        DrawControllerSettings();
        // Nest capture under this menu so opening/closing the modal preserves
        // the settings popup and its current port and scroll position.
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    const std::string audioLabel = g_audioMuted
        ? "Audio: Muted"
        : "Audio: " + std::to_string(g_audioVolumePercent) + "%";
    // Keep the popup ID stable while the Master slider changes the visible
    // label. Without the ### suffix, ImGui treats every new percentage as a
    // different menu and closes the popup on the first drag update.
    const std::string audioMenuLabel = audioLabel + "###AudioSettingsMenu";
    if (ImGui::BeginMenu(audioMenuLabel.c_str())) {
        DrawAudioSettings();
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Diagnostics")) {
        DrawDiagnosticsSettings();
        ImGui::EndMenu();
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float hideWidth = ImGui::CalcTextSize("Hide (F10)").x + style.FramePadding.x * 2.0f;
    const float exitWidth = ImGui::CalcTextSize("X").x + style.FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                  ImGui::GetWindowWidth() - hideWidth - exitWidth - style.ItemSpacing.x - 8.0f));
    if (ImGui::MenuItem("Hide (F10)")) {
        SetTopBarVisible(false);
    }
    if (ImGui::MenuItem("X")) {
        g_exitPromptOpen = true;
    }
    ImGui::EndMainMenuBar();
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

// Consumes the next key press into the recenter binding while the menu
// button is armed. Esc leaves the existing binding alone, Backspace clears
// it; anything else becomes the new hotkey.
bool CaptureVrRecenterBinding(const SDL_Event& event) {
    if (!g_vrRecenterRebinding || event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return false;
    }
    g_vrRecenterRebinding = false;
    if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
        return true;
    }
    g_vrRecenterScancode = event.key.scancode == SDL_SCANCODE_BACKSPACE
                               ? SDL_SCANCODE_UNKNOWN
                               : event.key.scancode;
    const char* name = g_vrRecenterScancode == SDL_SCANCODE_UNKNOWN
                           ? nullptr
                           : SDL_GetScancodeName(g_vrRecenterScancode);
    RuntimeConfigFile::SetVrRecenterKey(name ? name : "");
    return true;
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
void UpdateCursorAutoHide() {
#if defined(__ANDROID__)
    // No pointer icon to manage on a headset, and SDL changes it through Java,
    // which ART aborts on from a guest fiber's stack: Draw runs on whichever
    // guest thread advances the retrace. InitializeRuntimeSettings keeps
    // ImGui's SDL backend off the cursor for the same reason.
#else
    const bool shouldHide =
        !g_topBarVisible && Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    // ImGui_ImplSDL3_NewFrame calls SDL_ShowCursor every frame unless this flag is set.
    if (shouldHide) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_HideCursor();
    } else {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    }
#endif
}

// Alt+Enter toggles the display mode inside aurora without going through the
// F10 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}

// The headset's settings panel (vr/openxr_settings_panel.h): the same menus as
// the F10 bar, drawn with an ImGui context of its own at a size fixed in panel
// pixels, operated by the VR controllers' pointer and handed to Aurora, which
// lays it over the eyes. The desktop context is untouched, so the F10 bar and
// the panel can both be open.
struct VrSettingsPanel {
    ImGuiContext* context = nullptr;
    Clock::time_point lastFrame{};
    bool selectHeld = false;
};
VrSettingsPanel g_vrSettingsPanel;

ImGuiContext* CreateVrSettingsPanelContext() {
    ImGuiContext* const desktop = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // No platform backend draws a cursor for it, and nothing else shows where
    // the controller is aiming.
    io.MouseDrawCursor = true;
    // Aurora's WebGPU backend, which renders this context's draw data, honours it.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    // The default font rasterised at the panel's scale rather than magnified,
    // with an atlas of its own so the desktop font texture is left alone.
    ImFontConfig font;
    font.SizePixels = 13.0f * mkw::vr::kSettingsPanelUiScale;
    io.Fonts->AddFontDefault(&font);
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(aurora_imgui_add_texture(static_cast<uint32_t>(width), static_cast<uint32_t>(height), pixels));
    io.Fonts->ClearTexData();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(mkw::vr::kSettingsPanelUiScale);
    // Nothing behind the panel is meant to be read through it.
    style.Colors[ImGuiCol_WindowBg].w = 0.97f;
    style.Colors[ImGuiCol_PopupBg].w = 0.98f;
    ImGui::SetCurrentContext(desktop);
    return context;
}

void DrawVrSettingsPanelWindow() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("WiiCompiled settings", nullptr, kFlags)) {
        ImGui::TextUnformatted("WiiCompiled settings");
        const ImGuiStyle& style = ImGui::GetStyle();
        const float recenterWidth = ImGui::CalcTextSize("Recenter view").x + style.FramePadding.x * 2.0f;
        const float closeWidth = ImGui::CalcTextSize("Close").x + style.FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - recenterWidth - closeWidth - style.ItemSpacing.x);
        if (ImGui::Button("Recenter view")) {
            mkw::vr::OpenXRRequestRecenter();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            mkw::vr::OpenXRSetSettingsPanelOpen(false);
        }
        ImGui::TextDisabled("Aim and pull a trigger to change a setting, push a thumbstick to scroll.");
        ImGui::TextDisabled("%s to close. The game does not see the controllers meanwhile.",
                            mkw::vr::OpenXRGetControllerMode() == mkw::vr::OpenXRControllerMode::WiiRemote
                                ? "Press left Y or Menu"
                                : "Click both thumbsticks or press Menu");
        ImGui::Separator();
        if (ImGui::BeginTabBar("Settings")) {
            const auto tab = [](const char* label, void (*draw)()) {
                if (ImGui::BeginTabItem(label)) {
                    // Its own scrolling region, so the header stays in view.
                    ImGui::BeginChild("Contents");
                    draw();
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            };
            tab("VR", DrawVrSettings);
            tab("Graphics", [] {
                DrawResolutionMenu();
                ImGui::Separator();
                DrawGraphicsSettings();
            });
            tab("Controllers", DrawControllerSettings);
            tab("Audio", DrawAudioSettings);
            tab("Diagnostics", DrawDiagnosticsSettings);
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// Called once per presented frame after the desktop menus, with the frame
// worker done: the draw data handed to Aurora must stay put until the next
// frame is encoded, and only the next call here rebuilds it.
void DrawVrSettingsPanel() {
    if (!mkw::vr::OpenXRIsRunning() || !mkw::vr::OpenXRSettingsPanelOpen()) {
        aurora_imgui_set_stereo_overlay(nullptr, 0.0f);
        return;
    }
    VrSettingsPanel& panel = g_vrSettingsPanel;
    if (panel.context == nullptr) {
        panel.context = CreateVrSettingsPanelContext();
    }
    const mkw::vr::OpenXRSettingsPanelPointer pointer = mkw::vr::OpenXRTakeSettingsPanelPointer();
    const Clock::time_point now = Clock::now();
    float deltaSeconds = 1.0f / 60.0f;
    if (panel.lastFrame != Clock::time_point{}) {
        deltaSeconds = std::clamp(std::chrono::duration<float>(now - panel.lastFrame).count(), 1.0e-4f, 0.25f);
    }
    panel.lastFrame = now;

    ImGuiContext* const desktop = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(panel.context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(mkw::vr::kSettingsPanelWidthPixels, mkw::vr::kSettingsPanelHeightPixels);
    io.DeltaTime = deltaSeconds;
    if (pointer.valid) {
        io.AddMousePosEvent(pointer.x, pointer.y);
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    if (pointer.select != panel.selectHeld) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, pointer.select);
        panel.selectHeld = pointer.select;
    }
    if (pointer.wheel != 0.0f) {
        io.AddMouseWheelEvent(0.0f, pointer.wheel);
    }
    ImGui::NewFrame();
    DrawVrSettingsPanelWindow();
    ImGui::Render();
    ImDrawData* const drawData = ImGui::GetDrawData();
    ImGui::SetCurrentContext(desktop);
    aurora_imgui_set_stereo_overlay(drawData, mkw::vr::kSettingsPanelWidthFraction);
}
} // namespace

void InitializeRuntimeSettings() noexcept {
#if defined(__ANDROID__)
    // ImGui_ImplSDL3_NewFrame would otherwise call SDL_SetCursor/SDL_HideCursor
    // (Java on Android) from the guest fiber that starts the next host frame.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
#endif
    PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
    InputBindings::Reload();
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(static_cast<float>(g_musicVolumePercent) / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(static_cast<float>(g_soundEffectsVolumePercent) / 100.0f);
    MusicAttenuation::SetUiVolume(static_cast<float>(g_uiVolumePercent) / 100.0f);
    MusicAttenuation::SetVoicesVolume(static_cast<float>(g_voicesVolumePercent) / 100.0f);
    MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(targetFps);
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_stereo_stop_at_display_copy(g_vrStopAtDisplayCopy);
    aurora_set_stereo_skip_copy_clears(g_vrSkipCopyClears);
    aurora_set_stereo_single_pass_eyes(g_vrSinglePassEyes);
    aurora_set_stereo_mirror_view(static_cast<AuroraStereoMirrorView>(g_vrMirrorView));
    mkw::vr::OpenXRSetControllerMode(static_cast<mkw::vr::OpenXRControllerMode>(g_vrControllerMode));
    ApplyVrHudVirtualScreen();
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    mkw::vr::diagnostics::SetEnabled(g_openxrDiagnosticsLogging);
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
}

void RefreshVrHudVirtualScreen() noexcept { ApplyVrHudVirtualScreen(); }

void RequestFirstPersonToggle() noexcept { g_firstPersonToggleRequested.store(true, std::memory_order_release); }

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (CaptureVrRecenterBinding(ev->sdl)) {
            continue;
        }
        if (g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_BACKSPACE) ||
                                IsToggleKey(ev->sdl, SDL_SCANCODE_DELETE))) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        }
        if (ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            HandleGamepadFirstPersonClick(ev->sdl.gbutton);
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F10)) {
            SetTopBarVisible(!g_topBarVisible);
        }
        if (!g_rebind.active && g_vrRecenterScancode != SDL_SCANCODE_UNKNOWN &&
            IsToggleKey(ev->sdl, g_vrRecenterScancode)) {
            mkw::vr::OpenXRRequestRecenter();
        }
        if (!g_rebind.active && g_muteHotkey != PAD_KEY_INVALID &&
            IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        if (!g_rebind.active && !g_topBarVisible && IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE)) {
            g_exitPromptOpen = true;
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

void ReleaseControllers() noexcept {
    // Aurora drives the LED white on first PADRead and never clears it, and the
    // exit paths terminate the process outright, so do it here.
    bool queued = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) continue;
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
            SDL_SetGamepadLED(pad, 0, 0, 0);
            queued = true;
        }
    }
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
    PADControlAllMotors(stopAll.data());
    // SDL hands LED and rumble reports to its own HIDAPI sender thread rather
    // than writing them here, so without this the process dies before the
    // controller ever receives them.
    if (queued) SDL_Delay(120);
}

void Draw() noexcept {
    // The overlay draws into the game thread's own ImGui frame
    // (aurora_imgui_host_frame_begin); the worker replays a copy of the draw
    // data, so there is nothing to wait for here.
    // Explain fallback without interrupting gameplay or capturing input.
    static std::string shownXrError;
    static double xrNoticeUntil = 0.0;
    const auto xrError = mkw::vr::OpenXRLastError();
    if (!xrError.empty() && xrError != shownXrError) {
        shownXrError = xrError;
        xrNoticeUntil = ImGui::GetTime() + 15.0;
    }
    if (!shownXrError.empty() && ImGui::GetTime() < xrNoticeUntil) {
        ImGui::SetNextWindowPos(ImVec2(16.0f, 60.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.9f);
        if (ImGui::Begin("OpenXR desktop fallback", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::PushTextWrapPos(440.0f);
            ImGui::TextUnformatted("OpenXR unavailable - playing on the desktop.");
            ImGui::TextUnformatted(shownXrError.c_str());
            ImGui::TextUnformatted("Check your headset and active OpenXR runtime, then restart. Details: F10 > VR.");
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
    }
    // Also drive the Wii Remote rescan from here: PADRead runs it too, but this
    // runs once per presented frame whatever the game is doing (e.g. sitting in
    // its "communications interrupted" prompt without polling pads). Same guest
    // thread as PADRead, so no concurrent access to the scanner's state.
    WiiRemoteInput::Poll();
    // Likewise for the VR controllers' gamepad, so it keeps being written even
    // while the game is not reading pads.
    mkw::vr::OpenXRApplyControllerState();
    if (g_firstPersonToggleRequested.exchange(false, std::memory_order_acq_rel)) {
        ToggleFirstPersonCamera();
    }
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    DrawFpsOverlay();
    DrawTopBar();
    DrawExitPrompt();
    controller_mapping_wizard::Draw();
    // The wizard captures raw presses; keep them out of the game.
    const bool inputBlocked = controller_mapping_wizard::IsActive() || g_rebind.active;
    PADBlockInput(inputBlocked);
    InputBindings::SetInputBlocked(inputBlocked);
    DrawStartupScreen();
    DrawVrSettingsPanel();
}

bool StartupScreenVisible() noexcept {
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept { ++g_presentedFrame; }
} // namespace settings_overlay
