#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <toml.hpp>
#include "platform/host_platform.h"
#include "vr/frame_interpolation_pacing.h"
#include "vr/steering_wheel.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

struct RuntimeUserConfig {
    std::optional<bool> widescreen;
    std::optional<int32_t> windowPosX;
    std::optional<int32_t> windowPosY;
    std::optional<uint32_t> windowWidth;
    std::optional<uint32_t> windowHeight;
    std::optional<float> resolutionMultiplier;
    std::optional<std::string> graphicsApi;
    std::optional<std::string> displayMode;
    std::optional<uint32_t> frameInterpolationFps;
    std::optional<bool> skipUnreadyPipelines;
    std::optional<bool> disableCopyFilter;
    std::optional<bool> textureReplacements;
    std::optional<bool> textureDumps;
    std::optional<bool> showFps;
    std::optional<bool> gxThread;
    std::optional<uint32_t> disabledPostProcessingPaths;
    std::optional<bool> vrEnabled;
    std::optional<bool> vrRequired;
    std::optional<float> vrRenderScale;
    std::optional<float> vrWorldUnitsPerMeter;
    std::optional<float> vrHudDistanceMeters;
    std::optional<float> vrHudWidthMeters;
    std::optional<bool> vrHudVirtualScreen;
    std::optional<bool> vrFlatScreen;
    std::optional<bool> vrPassthrough;
    std::optional<bool> vrStopAtDisplayCopy;
    std::optional<bool> vrSkipCopyClears;
    std::optional<bool> vrSinglePassEyes;
    std::optional<std::string> vrMirrorView;
    std::optional<std::string> vrControllerMode;
    std::optional<uint32_t> vrFrameInterpolationFps;
    std::optional<bool> vrFirstPerson;
    std::optional<bool> vrFirstPersonToggleClick;
    std::optional<float> vrFirstPersonUnitsPerMeter;
    std::optional<float> vrFirstPersonHeadUpMeters;
    std::optional<float> vrFirstPersonHeadForwardMeters;
    std::optional<float> vrFirstPersonHeadRightMeters;
    std::optional<bool> vrFirstPersonHideDriver;
    std::optional<int32_t> vrFirstPersonHiddenModel;
    std::optional<std::string> vrFirstPersonRotation;
    std::optional<std::string> vrFirstPersonSeat;
    std::optional<float> vrCockpitUnitsPerMeter;
    std::optional<bool> vrSteeringWheel;
    std::optional<bool> vrNativeSteeringWheel;
    std::optional<bool> vrHandSteering;
    std::optional<float> vrWheelKartDegrees;
    std::optional<float> vrWheelBikeDegrees;
    std::optional<float> vrWheelGrabDistance;
    std::optional<float> vrWheelGrabAssist;
    std::optional<float> vrWheelResponse;
    std::optional<float> vrWheelTrackingGrace;
    std::optional<bool> vrWheelHaptics;
    std::optional<std::string> vrPerformanceLevel;
    std::optional<std::string> vrFoveation;
    std::optional<std::string> vrRecenterKey;
    std::optional<float> vrLeanBackDegrees;
    // F10 > Diagnostics: OpenXR pacing and presentation logging in console.log.
    // Off unless set; it is a bug-report aid, not something to leave running.
    std::optional<bool> diagnosticsOpenXRLogging;
    std::optional<float> audioVolume;
    std::optional<float> audioMusicVolume;
    std::optional<float> audioSoundEffectsVolume;
    std::optional<float> audioUiVolume;
    std::optional<float> audioVoicesVolume;
    std::optional<bool> audioMuted;
    std::optional<bool> audioMixWorker;
    std::optional<bool> attenuateMusicWhenMediaPlays;
    // Real Wii Remotes (with or without Nunchuk / Classic Controller) and Wii U Pro
    // Controllers paired over Bluetooth, driven by SDL's HIDAPI Wii driver. The driver
    // is opt-in on SDL's side, so this decides whether the runtime turns it on.
    std::optional<bool> wiiRemotes;
    // Keep re-enumerating Bluetooth HID devices while no Wii controller is connected
    // (Dolphin's "continuous scanning"), so a remote that dropped or was switched on
    // after launch shows up without restarting.
    std::optional<bool> wiiContinuousScan;
    // Accelerometer zero-point correction for the Bluetooth Wii Remote, in g and in
    // SDL's sensor frame (x right, y out of the button face, z towards the user).
    // SDL's Wii driver falls back to a nominal zero point when its read of the
    // remote's calibration block times out (common over Bluetooth), so this is
    // measured in the overlay with the remote at rest.
    std::optional<double> wiiAccelOffsetX;
    std::optional<double> wiiAccelOffsetY;
    std::optional<double> wiiAccelOffsetZ;
    // Debugging aid: append every KPAD sample of the Bluetooth remote (raw and
    // corrected accelerometer, buttons) to wii_accel_trace.csv next to Config.toml.
    std::optional<bool> wiiAccelTrace;
    std::optional<bool> networkEnabled;
    std::optional<bool> discordPresenceEnabled;
    // The application ID of the WiiCompiled Discord application. This is only
    // used by the base product; Retro Rewind supplies its own ID through the
    // standard Dolphin /dev/dolphin interface.
    std::optional<std::string> discordClientId;
    std::optional<std::string> nandRoot;
    std::optional<std::string> dvdRoot;
    // The one canonical Retro Rewind installation, owned and updated by the frontend. Setup records
    // it here instead of copying the pack, so an asset-only update is visible on the next launch.
    std::optional<std::string> retroRewindRoot;
    std::vector<std::string> overlayRoots;
    // Controller mappings use Wii/GameCube button names as keys and up to two
    // comma-separated SDL-style physical button names ("south", or
    // "dpad_up,left_shoulder") as values; pressing either bound button counts.
    std::array<std::optional<std::string>, 12> controllerButtons;
    std::optional<bool> rumbleEnabled;
    std::optional<int32_t> muteHotkey;
    std::map<std::string, std::string> controllerExpressions;
};

namespace RuntimeConfigFile {

// Narrow path strings are UTF-8 everywhere in the runtime; string() and the
// char path constructor would use the ANSI codepage on Windows, which drops
// characters the codepage cannot represent.
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline constexpr const char* kConfigFileName = "Config.toml";
#if defined(_WIN32) || defined(__ANDROID__)
// The Quest activity (android/.../QuestActivity.kt) creates this directory
// under the app's external files dir and writes Config.toml into it.
inline constexpr const char* kApplicationDirectoryName = "WiiCompiledOpenXRVR";
#else
inline constexpr const char* kApplicationDirectoryName = "WiiCompiled";
#endif

// Portable layout. A directory holding kPortableMarkerFileName is a portable root; every piece of
// runtime user state (Config.toml, NAND, Cache, Logs) lives in <root>/UserData instead of
// %LOCALAPPDATA%. The marker is searched for from the executable's directory upwards, which is what
// makes an installation survive being moved or carried on removable media.
inline constexpr const char* kPortableMarkerFileName = "portable.txt";
inline constexpr const char* kPortableUserDataDirectoryName = "UserData";

// The installed layout puts products two levels below the root (<root>/Install/Base/game.exe). The
// bound is deliberately small so an unrelated marker far up a drive can never capture an ordinary
// installation.
inline constexpr int kPortableSearchDepth = 4;

// Post-processing paths the game is allowed to skip, as a mask of the engine's
// own path bits. Bloom is the only one exposed, and it starts off: its bright
// bleed is the effect that reads worst in a headset, and the F10 bar's tick box
// is the way back to it.
inline constexpr uint32_t kPostProcessingBloomPath = 0x10u;
inline constexpr uint32_t kDisabledPostProcessingPathsDefault = kPostProcessingBloomPath;

// Headset eye size as a fraction of what the OpenXR runtime recommends. A
// standalone headset renders on a mobile GPU, so the Quest starts below it;
// the launcher's first Config.toml (GameStorage.kt) writes the same value.
#if defined(__ANDROID__)
inline constexpr float kVrRenderScaleDefault = 0.8f;
#define MKW_VR_RENDER_SCALE_DEFAULT_TEXT "0.8"
#else
inline constexpr float kVrRenderScaleDefault = 1.0f;
#define MKW_VR_RENDER_SCALE_DEFAULT_TEXT "1.0"
#endif

// First-person camera defaults and the range its head offsets accept, in one
// place: the config getters, the on-disk template and the F10 bar's reset all
// read them from here, so they cannot drift apart again.
inline constexpr float kVrFirstPersonUnitsPerMeterDefault = 50.0f;
inline constexpr float kVrFirstPersonHeadUpDefault = 1.50f;
inline constexpr float kVrFirstPersonHeadForwardDefault = 0.0f;
inline constexpr float kVrFirstPersonHeadRightDefault = 0.0f;
inline constexpr bool kVrFirstPersonHideDriverDefault = true;
inline constexpr int32_t kVrFirstPersonHiddenModelDefault = 0;
inline constexpr float kVrFirstPersonHeadOffsetLimit = 10.0f;
// "yaw", "yaw_pitch" or "full", matching FirstPersonRotation. The cockpit seat
// is the first-person default, and sitting in the vehicle reads better with its
// climb than with a level horizon, so "yaw_pitch" is the default anchor.
inline constexpr const char* kVrFirstPersonRotationDefault = "yaw_pitch";

inline bool IsSupportedVrFirstPersonRotation(std::string_view value) {
    return value == "yaw" || value == "yaw_pitch" || value == "full";
}
// Where the first-person head sits, matching FirstPersonSeat: "cockpit" at the
// driver's own eyes behind the wheel, "custom" at the head offsets above.
inline constexpr const char* kVrFirstPersonSeatDefault = "cockpit";

inline bool IsSupportedVrFirstPersonSeat(std::string_view value) {
    return value == "cockpit" || value == "custom";
}
// The cockpit seat's world scale before the character's height is allowed for.
inline constexpr float kVrCockpitUnitsPerMeterDefault = 100.0f;
inline constexpr float kVrCockpitUnitsPerMeterMin = 20.0f;
inline constexpr float kVrCockpitUnitsPerMeterMax = 400.0f;
// The vehicle's steering wheel or handlebar turns with the steering; the
// vehicle's own model is animated unless native_steering_wheel is off, which
// draws a separate VR wheel instead. Hand steering (grabbing that wheel with
// the tracked controllers, by heurazy) comes with it: the stick still steers
// until a grip actually takes hold of the wheel. Both launchers register
// hand_steering with this same default.
inline constexpr bool kVrSteeringWheelDefault = true;
inline constexpr bool kVrNativeSteeringWheelDefault = true;
inline constexpr bool kVrHandSteeringDefault = true;
// Hand steering tuning ranges; the defaults are mkw::vr::WheelTuning's.
inline constexpr float kVrWheelDegreesMin = 20.0f, kVrWheelDegreesMax = 180.0f;
inline constexpr float kVrWheelGrabDistanceMin = 0.15f, kVrWheelGrabDistanceMax = 0.8f;
inline constexpr float kVrWheelGrabAssistMin = 0.7f, kVrWheelGrabAssistMax = 2.0f;
inline constexpr float kVrWheelResponseMin = 0.5f, kVrWheelResponseMax = 2.0f;
inline constexpr float kVrWheelTrackingGraceMin = 0.05f, kVrWheelTrackingGraceMax = 0.5f;
// The performance level asked of the OpenXR runtime (XR_EXT_performance_settings) for its CPU and
// GPU domains. Standalone headsets clock their cores by this request: a Quest 3 ran the game
// thread at 1.92 GHz with the runtime's own choice while its fast cores reach 2.36 GHz. "default"
// leaves the runtime's choice; desktop runtimes without the extension ignore the setting.
inline constexpr const char* kVrPerformanceLevelDefault = "boost";

inline bool IsSupportedVrPerformanceLevel(std::string_view value) {
    return value == "default" || value == "power_savings" || value == "sustained_low" ||
           value == "sustained_high" || value == "boost";
}
// Fixed foveated rendering of the immersive eyes on the Quest, in the order of
// aurora_set_stereo_foveation's levels: the periphery is shaded in 2x2, then
// 4x4 pixel blocks, the higher the level the closer to the centre. Whether the
// GPU device gets fragment density maps at all is decided at launch, so going
// from "off" to a level takes a restart; between levels and back to "off" it is
// live.
inline constexpr const char* kVrFoveationDefault = "off";
inline constexpr std::array<std::string_view, 4> kVrFoveationLevels{"off", "low", "medium", "high"};

inline bool IsSupportedVrFoveation(std::string_view value) {
    return std::find(kVrFoveationLevels.begin(), kVrFoveationLevels.end(), value) != kVrFoveationLevels.end();
}

// The level aurora_set_stereo_foveation takes; anything unknown is off.
inline uint32_t VrFoveationLevelIndex(std::string_view value) {
    const auto it = std::find(kVrFoveationLevels.begin(), kVrFoveationLevels.end(), value);
    return it == kVrFoveationLevels.end() ? 0u : static_cast<uint32_t>(it - kVrFoveationLevels.begin());
}
// What the desktop window shows while the headset is running: "normal" leaves
// the ordinary desktop view alone, "both", "left" and "right" mirror the
// headset's eyes, and "none" blacks the window out. Matches
// AuroraStereoMirrorView.
inline constexpr const char* kVrMirrorViewDefault = "normal";

inline bool IsSupportedVrMirrorView(std::string_view value) {
    return value == "normal" || value == "both" || value == "left" || value == "right" || value == "none";
}
// What the tracked VR controllers are to the game: "wii_remote" is a Wii
// Remote with a Nunchuk (motion and pointer included), "gamepad" one ordinary
// controller read as a GameCube pad. Matches mkw::vr::OpenXRControllerMode.
inline constexpr const char* kVrControllerModeDefault = "wii_remote";

inline bool IsSupportedVrControllerMode(std::string_view value) {
    return value == "wii_remote" || value == "gamepad";
}
// SDL scancode name, spelled the way SDL_GetScancodeName produces it. An
// empty string leaves the recenter hotkey unbound, menu button only.
inline constexpr std::string_view kVrRecenterKeyDefault = "F9";
// Fixed pitch of the game camera for a player sitting reclined, in degrees.
// Positive leans the camera back with you; 0 disables it entirely.
inline constexpr float kVrLeanBackDegreesDefault = 0.0f;
inline constexpr float kVrLeanBackDegreesLimit = 45.0f;

inline std::string Trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

inline std::string RemoveComment(std::string_view line) {
    bool inSingle = false;
    bool inDouble = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inDouble && ch == '\\' && !escaped) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (ch == '"' && !inSingle && !escaped) {
            inDouble = !inDouble;
        } else if (ch == '#' && !inSingle && !inDouble) {
            return std::string(line.substr(0, i));
        }
        escaped = false;
    }
    return std::string(line);
}

inline bool IsSupportedResolutionMultiplier(float value) {
    static constexpr std::array values{0.0f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Must stay in step with the backend table in main.cpp, which is what actually
// maps these to AuroraBackend.
inline bool IsSupportedGraphicsApi(std::string_view value) {
#if defined(__APPLE__)
    static constexpr std::array<std::string_view, 2> values{"auto", "metal"};
// only vulkan for linux
#elif defined(__linux__)
    static constexpr std::array<std::string_view, 2> values{"auto", "vulkan"};
#elif defined(_WIN32)
    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};
#endif
    return std::find(values.begin(), values.end(), value) != values.end();
}

inline bool IsSupportedDisplayMode(std::string_view value) {
    static constexpr std::array<std::string_view, 3> values{
        "windowed", "borderless", "exclusive",
    };
    return std::find(values.begin(), values.end(), value) != values.end();
}

// 240 was offered by an early build and is no longer supported; a saved 240 is
// migrated to 180 at the parse site.
inline bool IsSupportedFrameInterpolationFps(uint32_t value) {
    return value == 0 || value == 120 || value == 180;
}

inline std::optional<std::filesystem::path> ExecutableDirectory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__) || defined(__ANDROID__)
    // Android has no executable next to which assets could sit; the platform
    // layer answers with the directory the activity unpacked the bundled
    // runtime resources into, so the adjacent-file lookups below keep working.
    return RuntimePlatform::ExecutableDirectory();
#else
    // /proc/self/exe is a Linux-specific magic symlink to the running executable; readlink()
    // does not NUL-terminate and silently truncates if the buffer is too small, so this grows
    // the buffer until the result no longer fills it completely, the same doubling strategy as
    // the Windows branch above uses for GetModuleFileNameW.
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<size_t>(length));
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

// The portable root this executable lives under, or nullopt for a normal installation. The answer
// cannot change while the process runs, so it is resolved exactly once: every user-state path
// derives from it and they must not disagree with each other.
inline const std::optional<std::filesystem::path>& PortableRootDirectory() {
    static const std::optional<std::filesystem::path> root = []() -> std::optional<std::filesystem::path> {
        const auto executableDirectory = ExecutableDirectory();
        if (!executableDirectory) {
            return std::nullopt;
        }
        std::filesystem::path current = *executableDirectory;
        for (int level = 0; level <= kPortableSearchDepth; ++level) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(current / kPortableMarkerFileName, ec)) {
                return current;
            }
            const auto parent = current.parent_path();
            if (parent.empty() || parent == current) {
                break;
            }
            current = parent;
        }
        return std::nullopt;
    }();
    return root;
}

inline std::filesystem::path ApplicationDataDirectory() {
    if (const auto& portableRoot = PortableRootDirectory()) {
        return *portableRoot / kPortableUserDataDirectoryName;
    }
#ifdef _WIN32
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath) {
        const std::filesystem::path directory = std::filesystem::path(rawPath) / kApplicationDirectoryName;
        CoTaskMemFree(rawPath);
        return directory;
    }
#elif defined(__APPLE__) || defined(__ANDROID__)
    return RuntimePlatform::ApplicationDataDirectory(kApplicationDirectoryName);
#else
    // XDG Base Directory spec equivalent of FOLDERID_LocalAppData: $XDG_DATA_HOME if set and
    // non-empty, otherwise its default of $HOME/.local/share.
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"); xdgDataHome && *xdgDataHome) {
        return std::filesystem::path(xdgDataHome) / kApplicationDirectoryName;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / kApplicationDirectoryName;
    }
#endif
    return std::filesystem::current_path() / kApplicationDirectoryName;
}

inline std::filesystem::path ResolveConfigPath() {
    return ApplicationDataDirectory() / kConfigFileName;
}

inline void EnsureConfigFile() {
    const std::filesystem::path path = ResolveConfigPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec || std::filesystem::exists(path, ec)) {
        return;
    }

    std::ofstream output(path);
    if (!output) {
        return;
    }
    output << "# WiiCompiled user configuration\n"
              "# Set paths.dvd_root to an extracted Mario Kart Wii DATA directory.\n\n"
              "[video]\n"
              "widescreen = true\n"
              "resolution_multiplier = 1.0\n"
              "frame_interpolation_fps = 0\n"
              "display_mode = \"windowed\"\n"
              "graphics_api = \"auto\"\n"
              "skip_unready_pipelines = true\n"
              "disable_copy_filter = true\n"
              "show_fps = false\n"
              "# Run the host side of the GX pipeline (state tracking, FIFO parsing,\n"
              "# texture uploads) on its own thread. On by default on the Quest, where\n"
              "# the game thread is the bottleneck; opt-in elsewhere.\n"
              "# gx_thread = true\n"
              "# Dolphin-style custom textures. When enabled, the renderer indexes\n"
              "# texture_replacements/ next to this file at startup and substitutes\n"
              "# any tex1_<W>x<H>_<hash>[_<tlut hash>]_<format>.dds or .png it finds\n"
              "# there for the matching game texture. texture_dumps writes every\n"
              "# unmatched texture to Cache/texture_dumps under the name a\n"
              "# replacement would need. Both are read once, at startup.\n"
              "texture_replacements = false\n"
              "texture_dumps = false\n\n"
              "[vr]\n"
              "# This is the OpenXR VR build, so the headset path is on. If required\n"
              "# is false, startup failures (no runtime, no headset, an unsupported\n"
              "# GPU) fall back to the ordinary desktop renderer, so turning enabled\n"
              "# off is a preference, not a fix. These values are read at launch.\n"
              "enabled = true\n"
              "required = false\n"
              "# What the desktop window shows while the headset is running:\n"
              "# \"normal\" keeps the ordinary desktop view, \"both\", \"left\" and\n"
              "# \"right\" mirror the headset's eyes, and \"none\" blacks the window\n"
              "# out. Changeable live from the F10 menu.\n"
              "mirror_view = \"normal\"\n"
              "# What the headset's controllers are to the game: \"wii_remote\"\n"
              "# is a Wii Remote (right hand, with motion and a pointer aimed at\n"
              "# the virtual screen) plus a Nunchuk (left hand); \"gamepad\" is one\n"
              "# ordinary controller read as a GameCube pad. Changeable live from\n"
              "# the F10 menu.\n"
              "controller_mode = \"wii_remote\"\n"
              "# VR interpolation: 0 = Off, 1 = Auto, or 72/90/120 FPS. Live.\n"
              "frame_interpolation_fps = 0\n"
              "render_scale = " MKW_VR_RENDER_SCALE_DEFAULT_TEXT "\n"
              "world_units_per_meter = 500.0\n"
              "hud_distance_meters = 2.0\n"
              "hud_width_meters = 2.4\n"
              "# During a race, put the game's 2D layer (minimap, position,\n"
              "# item roulette, lap times) on a virtual screen fixed in front of\n"
              "# the kart camera instead of stretching it across the whole view.\n"
              "# Changeable live from the F10 menu; the two sizes above place\n"
              "# that screen and the menu screen alike and are read at launch.\n"
              "hud_virtual_screen = true\n"
              "# Flat Screen mode keeps races on that same screen, as the\n"
              "# menus are, instead of all around you: no stereo race view, no\n"
              "# first-person camera or hand steering. Changeable live from the\n"
              "# F10 menu.\n"
              "flat_screen = false\n"
              "# EFB replay controls for the per-eye views, changeable live\n"
              "# from the F10 menu. stop_at_display_copy ends each eye at the\n"
              "# frame's final GXCopyDisp; skip_copy_clears drops the EFB\n"
              "# reset a GX copy performs afterwards. Both keep that reset\n"
              "# from erasing the eye, and both are safe to turn off.\n"
              "stop_at_display_copy = true\n"
              "skip_copy_clears = true\n"
              "# Draw each eye in one render pass across the frame's GX copies,\n"
              "# which only the desktop image performs: the same picture with\n"
              "# less GPU memory traffic. Changeable live from the F10 menu.\n"
              "single_pass_eyes = true\n"
              "# Put the camera at the Player 1 driver's head instead of behind\n"
              "# the kart, with the horizon kept level. Changeable live from the\n"
              "# F10 menu, and only during a single-screen race.\n"
              "first_person = false\n"
              "# Clicking the right thumbstick, on the VR controllers or on any\n"
              "# gamepad while VR runs, toggles first_person as the F10 checkbox does.\n"
              "first_person_toggle_click = true\n"
              "# Where the head sits: \"cockpit\" puts it at the driver's own eyes,\n"
              "# behind the steering wheel, at a life-size scale that allows for\n"
              "# the character's height, so the wheel is within reach.\n"
              "# \"custom\" uses the world scale and head offsets below instead.\n"
              "first_person_seat = \"cockpit\"\n"
              "cockpit_units_per_meter = 100.0\n"
              "# The custom seat's world scale, replacing world_units_per_meter\n"
              "# while first person is engaged; the 500 above makes the race a\n"
              "# small diorama.\n"
              "first_person_units_per_meter = 50.0\n"
              "# Where the custom seat's head sits in the kart's own frame, in metres.\n"
              "first_person_head_up_meters = 1.5\n"
              "first_person_head_forward_meters = 0.0\n"
              "first_person_head_right_meters = 0.0\n"
              "# In first person the driver sits where your eyes are. Hiding\n"
              "# the driver removes the head that would otherwise be in the\n"
              "# way; hiding the kart removes the vehicle around you too.\n"
              "first_person_hide_driver = true\n"
              "# 0 is the driver, which is the usual choice. -1 hides every\n"
              "# model of your kart, the vehicle included.\n"
              "first_person_hidden_model = 0\n"
              "# Where the view's orientation comes from: \"yaw\" levels the\n"
              "# horizon, \"yaw_pitch\" adds the kart's climb but no roll, and\n"
              "# \"full\" takes the kart's whole orientation so the view banks.\n"
              "first_person_rotation = \"yaw_pitch\"\n"
              "# In the cockpit the vehicle's steering wheel or handlebar turns\n"
              "# with the steering. native_steering_wheel animates the vehicle's\n"
              "# own model; false draws a separate VR wheel instead.\n"
              "steering_wheel = true\n"
              "native_steering_wheel = true\n"
              "# Hand steering (by heurazy): squeeze a grip near the wheel or\n"
              "# handlebar to take hold of it with the tracked controllers, and\n"
              "# turn it to steer. Releasing both grips gives steering back to the\n"
              "# stick. The tuning below: degrees of turn for full lock on karts\n"
              "# and bikes, how far (metres) and how generously a grip reaches\n"
              "# the wheel, how quickly the wheel follows the hands, how long\n"
              "# (seconds) a hand that loses tracking keeps hold, and a short\n"
              "# pulse on grab and release. All changeable live from the F10 menu.\n"
              "hand_steering = true\n"
              "wheel_kart_degrees = 90.0\n"
              "wheel_bike_degrees = 45.0\n"
              "wheel_grab_distance = 0.35\n"
              "wheel_grab_assist = 1.0\n"
              "wheel_response = 1.0\n"
              "wheel_tracking_grace = 0.2\n"
              "wheel_haptics = true\n\n"
              "# Performance level asked of the headset's runtime for its CPU and\n"
              "# GPU: \"boost\", \"sustained_high\", \"sustained_low\", \"power_savings\",\n"
              "# or \"default\" to leave the runtime's own choice. Standalone headsets\n"
              "# clock their cores by this request; desktop runtimes ignore it.\n"
              "performance_level = \"boost\"\n\n"
              "# Keyboard shortcut that recenters the VR view, naming the key the\n"
              "# way SDL does (F9, Home, Keypad 5, ...). It moves the race view to\n"
              "# where you are sitting now and brings the menu screen back upright in\n"
              "# front of you. The race view moves in position only: forward and the\n"
              "# horizon come from the headset's own reference space, so recentering\n"
              "# cannot tilt the game. Rebindable from the F10 menu under VR. Leave\n"
              "# empty to unbind.\n"
              "recenter_key = \"F9\"\n"
              "# Fixed pitch of the game camera, in degrees, for a player sitting\n"
              "# reclined. Positive tilts the camera back with you, so 20 here cancels\n"
              "# reclining about 20 degrees and puts the track back in front of you.\n"
              "# 0 disables it. Changeable live from the F10 menu under VR.\n"
              "lean_back_degrees = 0.0\n\n"
              "[audio]\n"
              "volume = 1.0\n"
              "music_volume = 1.0\n"
              "sound_effects_volume = 1.0\n"
              "ui_volume = 1.0\n"
              "voices_volume = 1.0\n"
              "muted = false\n"
              "attenuate_music_when_media_plays = false\n"
              "# Runs the AX/DSP voice mix on its own thread, joined before the\n"
              "# guest can observe it. Set to false to mix inline on the guest\n"
              "# thread exactly as the runtime did before.\n"
              "mix_worker = true\n\n"
              "[network]\n"
              "enabled = true\n\n"
              "[discord]\n"
              "# Rich Presence talks only to a locally-running Discord client.\n"
              "# Retro Rewind supplies its official app ID automatically. Set this\n"
              "# to WiiCompiled's Discord application ID for basic base-game presence.\n"
              "enabled = true\n"
              "# client_id = \"123456789012345678\"\n\n"
              "[paths]\n"
              "# dvd_root = \"D:\\\\MarioKartWii\\\\DATA\"\n"
              "# nand_root = \"D:\\\\WiiNand\"\n"
              "# retro_rewind_root = \"D:\\\\RetroRewind\\\\RetroRewind6\"\n"
              "# overlay_roots = [\"D:\\\\RetroRewind\"]\n";
}

template <typename T>
inline std::optional<T> FindConfigValue(
    const toml::value& document, std::string_view section, std::string_view key) {
    try {
        return toml::find<T>(document, std::string(section), std::string(key));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::optional<uint32_t> FindConfigUint(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < 0 || static_cast<uint64_t>(*value) > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

inline std::optional<int32_t> FindConfigInt(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < INT32_MIN || *value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(*value);
}

inline std::optional<float> FindConfigFloat(
    const toml::value& document, std::string_view section, std::string_view key) {
    std::optional<double> value = FindConfigValue<double>(document, section, key);
    if (!value) {
        if (const auto integer = FindConfigValue<int64_t>(document, section, key)) {
            value = static_cast<double>(*integer);
        }
    }
    if (!value || !std::isfinite(*value) ||
        *value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        *value > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(*value);
}

inline void AppendOverlayRoots(RuntimeUserConfig& config, const std::string& roots) {
    size_t begin = 0;
    while (begin < roots.size()) {
        const size_t end = roots.find(';', begin);
        std::string root = Trim(std::string_view(roots).substr(begin, end - begin));
        if (!root.empty()) {
            config.overlayRoots.push_back(std::move(root));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

// Reads every supported setting out of a parsed Config.toml document.
inline RuntimeUserConfig ParseConfigDocument(const toml::value& document) {
    RuntimeUserConfig config;

    static constexpr std::array<std::string_view, 12> buttonKeys = {
        "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
    };
    for (size_t index = 0; index < buttonKeys.size(); ++index) {
        config.controllerButtons[index] =
            FindConfigValue<std::string>(document, "controller", buttonKeys[index]);
    }

    config.rumbleEnabled = FindConfigValue<bool>(document, "controller", "rumble");
    if (auto value = FindConfigInt(document, "audio", "mute_key")) {
        config.muteHotkey = *value;
    }

    if (const auto* section = document.contains("controller") ? &document.at("controller") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (key.rfind("expr_", 0) == 0 && value.is_string()) {
                config.controllerExpressions[key] = value.as_string();
            }
        }
    }

    config.widescreen = FindConfigValue<bool>(document, "video", "widescreen");
    config.windowPosX = FindConfigInt(document, "video", "window_x");
    config.windowPosY = FindConfigInt(document, "video", "window_y");
    if (auto value = FindConfigUint(document, "video", "window_width"); value && *value != 0) {
        config.windowWidth = *value;
    }
    if (auto value = FindConfigUint(document, "video", "window_height"); value && *value != 0) {
        config.windowHeight = *value;
    }
    if (auto value = FindConfigFloat(document, "video", "resolution_multiplier");
        value && IsSupportedResolutionMultiplier(*value)) {
        config.resolutionMultiplier = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "graphics_api")) {
        if (IsSupportedGraphicsApi(*value)) {
            config.graphicsApi = *value;
        } else {
            std::cerr << "[runtime] Unknown video.graphics_api=\"" << *value
                      << "\", using the automatic backend" << std::endl;
        }
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "display_mode");
        value && IsSupportedDisplayMode(*value)) {
        config.displayMode = *value;
    }
    if (auto value = FindConfigUint(document, "video", "frame_interpolation_fps")) {
        const uint32_t migrated = *value == 240u ? 180u : *value;
        if (IsSupportedFrameInterpolationFps(migrated)) {
            config.frameInterpolationFps = migrated;
        }
    }
    config.skipUnreadyPipelines = FindConfigValue<bool>(document, "video", "skip_unready_pipelines");
    config.disableCopyFilter = FindConfigValue<bool>(document, "video", "disable_copy_filter");
    config.showFps = FindConfigValue<bool>(document, "video", "show_fps");
    config.gxThread = FindConfigValue<bool>(document, "video", "gx_thread");
    config.textureReplacements = FindConfigValue<bool>(document, "video", "texture_replacements");
    config.textureDumps = FindConfigValue<bool>(document, "video", "texture_dumps");
    if (auto value = FindConfigUint(document, "video", "disabled_post_processing_paths");
        value && (*value & ~kPostProcessingBloomPath) == 0) {
        config.disabledPostProcessingPaths = *value & kPostProcessingBloomPath;
    }

    config.vrEnabled = FindConfigValue<bool>(document, "vr", "enabled");
    config.vrRequired = FindConfigValue<bool>(document, "vr", "required");
    if (auto value = FindConfigFloat(document, "vr", "render_scale");
        value && *value >= 0.25f && *value <= 2.0f) {
        config.vrRenderScale = *value;
    }
    if (auto value = FindConfigFloat(document, "vr", "world_units_per_meter");
        value && *value >= 1.0f && *value <= 10000.0f) {
        config.vrWorldUnitsPerMeter = *value;
    }
    if (auto value = FindConfigFloat(document, "vr", "hud_distance_meters");
        value && *value >= 0.25f && *value <= 10.0f) {
        config.vrHudDistanceMeters = *value;
    }
    if (auto value = FindConfigFloat(document, "vr", "hud_width_meters");
        value && *value >= 0.25f && *value <= 20.0f) {
        config.vrHudWidthMeters = *value;
    }
    config.vrHudVirtualScreen = FindConfigValue<bool>(document, "vr", "hud_virtual_screen");
    config.vrFlatScreen = FindConfigValue<bool>(document, "vr", "flat_screen");
    config.vrPassthrough = FindConfigValue<bool>(document, "vr", "passthrough");
    config.vrStopAtDisplayCopy = FindConfigValue<bool>(document, "vr", "stop_at_display_copy");
    config.vrSkipCopyClears = FindConfigValue<bool>(document, "vr", "skip_copy_clears");
    config.vrSinglePassEyes = FindConfigValue<bool>(document, "vr", "single_pass_eyes");
    config.vrFirstPerson = FindConfigValue<bool>(document, "vr", "first_person");
    config.vrFirstPersonToggleClick = FindConfigValue<bool>(document, "vr", "first_person_toggle_click");
    if (auto value = FindConfigFloat(document, "vr", "first_person_units_per_meter");
        value && *value >= 1.0f && *value <= 10000.0f) {
        config.vrFirstPersonUnitsPerMeter = *value;
    }
    if (auto value = FindConfigFloat(document, "vr", "first_person_head_up_meters");
        value && *value >= -kVrFirstPersonHeadOffsetLimit &&
        *value <= kVrFirstPersonHeadOffsetLimit) {
        config.vrFirstPersonHeadUpMeters = *value;
    }
    if (auto value = FindConfigFloat(document, "vr", "first_person_head_forward_meters");
        value && *value >= -kVrFirstPersonHeadOffsetLimit &&
        *value <= kVrFirstPersonHeadOffsetLimit) {
        config.vrFirstPersonHeadForwardMeters = *value;
    }
    if (auto value = FindConfigFloat(document, "vr", "first_person_head_right_meters");
        value && *value >= -kVrFirstPersonHeadOffsetLimit &&
        *value <= kVrFirstPersonHeadOffsetLimit) {
        config.vrFirstPersonHeadRightMeters = *value;
    }
    config.vrFirstPersonHideDriver =
        FindConfigValue<bool>(document, "vr", "first_person_hide_driver");
    if (auto value = FindConfigValue<std::string>(document, "vr", "recenter_key")) {
        config.vrRecenterKey = *value;
    }
    if (auto value = FindConfigFloat(document, "vr", "lean_back_degrees");
        value && *value >= -kVrLeanBackDegreesLimit && *value <= kVrLeanBackDegreesLimit) {
        config.vrLeanBackDegrees = static_cast<float>(*value);
    }
    if (auto value = FindConfigValue<std::string>(document, "vr", "first_person_rotation");
        value && IsSupportedVrFirstPersonRotation(*value)) {
        config.vrFirstPersonRotation = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "vr", "performance_level");
        value && IsSupportedVrPerformanceLevel(*value)) {
        config.vrPerformanceLevel = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "vr", "foveation");
        value && IsSupportedVrFoveation(*value)) {
        config.vrFoveation = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "vr", "mirror_view");
        value && IsSupportedVrMirrorView(*value)) {
        config.vrMirrorView = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "vr", "controller_mode");
        value && IsSupportedVrControllerMode(*value)) {
        config.vrControllerMode = *value;
    }
    config.vrFrameInterpolationFps = FindConfigValue<uint32_t>(document, "vr", "frame_interpolation_fps");
    if (!config.vrFrameInterpolationFps) {
        // Migrate the initial experimental checkbox to Auto.
        if (const auto legacy = FindConfigValue<bool>(document, "vr", "frame_interpolation"))
            config.vrFrameInterpolationFps = *legacy ? 1u : 0u;
    }
    if (auto value = FindConfigInt(document, "vr", "first_person_hidden_model");
        value && *value >= -1 && *value <= 31) {
        config.vrFirstPersonHiddenModel = static_cast<int32_t>(*value);
    }
    if (auto value = FindConfigValue<std::string>(document, "vr", "first_person_seat");
        value && IsSupportedVrFirstPersonSeat(*value)) {
        config.vrFirstPersonSeat = *value;
    }
    const auto readRangedFloat = [&](std::string_view key, float low, float high) -> std::optional<float> {
        auto value = FindConfigFloat(document, "vr", key);
        return value && *value >= low && *value <= high ? value : std::nullopt;
    };
    config.vrCockpitUnitsPerMeter =
        readRangedFloat("cockpit_units_per_meter", kVrCockpitUnitsPerMeterMin, kVrCockpitUnitsPerMeterMax);
    config.vrSteeringWheel = FindConfigValue<bool>(document, "vr", "steering_wheel");
    config.vrNativeSteeringWheel = FindConfigValue<bool>(document, "vr", "native_steering_wheel");
    config.vrHandSteering = FindConfigValue<bool>(document, "vr", "hand_steering");
    config.vrWheelKartDegrees = readRangedFloat("wheel_kart_degrees", kVrWheelDegreesMin, kVrWheelDegreesMax);
    config.vrWheelBikeDegrees = readRangedFloat("wheel_bike_degrees", kVrWheelDegreesMin, kVrWheelDegreesMax);
    config.vrWheelGrabDistance =
        readRangedFloat("wheel_grab_distance", kVrWheelGrabDistanceMin, kVrWheelGrabDistanceMax);
    config.vrWheelGrabAssist = readRangedFloat("wheel_grab_assist", kVrWheelGrabAssistMin, kVrWheelGrabAssistMax);
    config.vrWheelResponse = readRangedFloat("wheel_response", kVrWheelResponseMin, kVrWheelResponseMax);
    config.vrWheelTrackingGrace =
        readRangedFloat("wheel_tracking_grace", kVrWheelTrackingGraceMin, kVrWheelTrackingGraceMax);
    config.vrWheelHaptics = FindConfigValue<bool>(document, "vr", "wheel_haptics");
    config.diagnosticsOpenXRLogging = FindConfigValue<bool>(document, "diagnostics", "openxr_logging");

    auto readVolume = [&](std::string_view key) -> std::optional<float> {
        auto value = FindConfigFloat(document, "audio", key);
        return value && *value >= 0.0f && *value <= 1.0f ? value : std::nullopt;
    };
    config.audioVolume = readVolume("volume");
    config.audioMusicVolume = readVolume("music_volume");
    config.audioSoundEffectsVolume = readVolume("sound_effects_volume");
    config.audioUiVolume = readVolume("ui_volume");
    config.audioVoicesVolume = readVolume("voices_volume");
    config.audioMuted = FindConfigValue<bool>(document, "audio", "muted");
    config.audioMixWorker = FindConfigValue<bool>(document, "audio", "mix_worker");
    config.attenuateMusicWhenMediaPlays =
        FindConfigValue<bool>(document, "audio", "attenuate_music_when_media_plays");
    config.wiiRemotes = FindConfigValue<bool>(document, "controller", "wii_remotes");
    config.wiiContinuousScan = FindConfigValue<bool>(document, "controller", "wii_continuous_scan");
    config.wiiAccelOffsetX = FindConfigValue<double>(document, "controller", "wii_accel_offset_x");
    config.wiiAccelOffsetY = FindConfigValue<double>(document, "controller", "wii_accel_offset_y");
    config.wiiAccelOffsetZ = FindConfigValue<double>(document, "controller", "wii_accel_offset_z");
    config.wiiAccelTrace = FindConfigValue<bool>(document, "controller", "wii_accel_trace");
    config.networkEnabled = FindConfigValue<bool>(document, "network", "enabled");
    config.discordPresenceEnabled = FindConfigValue<bool>(document, "discord", "enabled");
    config.discordClientId = FindConfigValue<std::string>(document, "discord", "client_id");

    config.nandRoot = FindConfigValue<std::string>(document, "paths", "nand_root");
    config.dvdRoot = FindConfigValue<std::string>(document, "paths", "dvd_root");
    config.retroRewindRoot = FindConfigValue<std::string>(document, "paths", "retro_rewind_root");
    if (auto roots = FindConfigValue<std::vector<std::string>>(document, "paths", "overlay_roots")) {
        for (auto& root : *roots) {
            root = Trim(root);
            if (!root.empty()) {
                config.overlayRoots.push_back(std::move(root));
            }
        }
    } else if (auto roots = FindConfigValue<std::string>(document, "paths", "overlay_roots")) {
        AppendOverlayRoots(config, *roots);
    }

    return config;
}

inline RuntimeUserConfig ParseConfig(std::istream& input, std::string sourceName = "Config.toml") {
    try {
        return ParseConfigDocument(toml::parse(input, std::move(sourceName)));
    } catch (const std::exception& exception) {
        std::cerr << "[runtime-config] Invalid TOML; using built-in defaults: "
                  << exception.what() << std::endl;
        return {};
    }
}

inline RuntimeUserConfig LoadConfigFile() {
    EnsureConfigFile();
    std::ifstream file(ResolveConfigPath(), std::ios::binary);
    return file ? ParseConfig(file, PathToUtf8(ResolveConfigPath())) : RuntimeUserConfig{};
}

inline const RuntimeUserConfig& Get() {
    static RuntimeUserConfig config = LoadConfigFile();
    return config;
}

inline RuntimeUserConfig& Mutable() {
    return const_cast<RuntimeUserConfig&>(Get());
}

inline constexpr std::array<std::string_view, 12> kControllerButtonKeys = {
    "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
};

inline const std::optional<std::string>& ControllerButton(size_t index) {
    static const std::optional<std::string> empty;
    return index < Get().controllerButtons.size() ? Get().controllerButtons[index] : empty;
}

// Update one TOML value without discarding comments, unrelated settings, or
// user-specific paths. This is used by the in-game F10 settings bar.
inline bool WriteSetting(std::string_view section, std::string_view key, std::string_view value) {
    const auto path = ResolveConfigPath();
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }

    const std::string normalizedSection = Trim(section);
    const std::string normalizedKey = Trim(key);
    size_t sectionStart = lines.size();
    size_t sectionEnd = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(RemoveComment(lines[i]));
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string found = Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
            if (sectionStart != lines.size()) {
                sectionEnd = i;
                break;
            }
            if (found == normalizedSection) {
                sectionStart = i;
            }
        }
    }

    const std::string replacement = normalizedKey + " = " + std::string(value);
    if (sectionStart == lines.size()) {
        if (!lines.empty() && !lines.back().empty()) {
            lines.emplace_back();
        }
        lines.emplace_back("[" + normalizedSection + "]");
        lines.push_back(replacement);
    } else {
        bool replaced = false;
        for (size_t i = sectionStart + 1; i < sectionEnd; ++i) {
            const std::string uncommented = Trim(RemoveComment(lines[i]));
            const size_t equals = uncommented.find('=');
            if (equals != std::string::npos && Trim(std::string_view(uncommented).substr(0, equals)) == normalizedKey) {
                lines[i] = replacement;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            // Append after the section's last real line rather than after the blank line that
            // separates it from the next header: this file is edited by hand and by the host
            // installer as well, and a key parked below the separator reads as if it belonged to
            // the next section. The host writer (Launcher/WiiCompiled.Setup/RuntimeConfiguration.cs)
            // applies exactly this rule.
            size_t insertAt = sectionEnd;
            while (insertAt > sectionStart + 1 && Trim(lines[insertAt - 1]).empty()) {
                --insertAt;
            }
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), replacement);
        }
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "[runtime-config] Unable to write " << PathToUtf8(path) << std::endl;
        return false;
    }
    for (const auto& outputLine : lines) {
        output << outputLine << '\n';
    }
    return static_cast<bool>(output);
}

inline std::string FormatString(std::string_view value) {
    return toml::format(toml::value(std::string(value)));
}

inline bool SetResolutionMultiplier(float value) {
    Mutable().resolutionMultiplier = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("video", "resolution_multiplier", formatted.str());
}

inline bool SetWindowSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    Mutable().windowWidth = width;
    Mutable().windowHeight = height;
    const bool wroteWidth = WriteSetting("video", "window_width", std::to_string(width));
    const bool wroteHeight = WriteSetting("video", "window_height", std::to_string(height));
    return wroteWidth && wroteHeight;
}

inline bool SetWindowPosition(int32_t x, int32_t y) {
    Mutable().windowPosX = x;
    Mutable().windowPosY = y;
    const bool wroteX = WriteSetting("video", "window_x", std::to_string(x));
    const bool wroteY = WriteSetting("video", "window_y", std::to_string(y));
    return wroteX && wroteY;
}

inline bool SetFrameInterpolationFps(uint32_t value) {
    if (!IsSupportedFrameInterpolationFps(value)) {
        return false;
    }
    Mutable().frameInterpolationFps = value;
    return WriteSetting("video", "frame_interpolation_fps", std::to_string(value));
}

inline bool SetDisplayMode(std::string value) {
    if (!IsSupportedDisplayMode(value)) {
        return false;
    }
    Mutable().displayMode = value;
    return WriteSetting("video", "display_mode", FormatString(value));
}

inline bool SetSkipUnreadyPipelines(bool value) {
    Mutable().skipUnreadyPipelines = value;
    return WriteSetting("video", "skip_unready_pipelines", value ? "true" : "false");
}

inline bool SetDisableCopyFilter(bool value) {
    Mutable().disableCopyFilter = value;
    return WriteSetting("video", "disable_copy_filter", value ? "true" : "false");
}

inline bool SetShowFps(bool value) {
    Mutable().showFps = value;
    return WriteSetting("video", "show_fps", value ? "true" : "false");
}

inline bool SetDisabledPostProcessingPaths(uint32_t value) {
    Mutable().disabledPostProcessingPaths = value;
    std::ostringstream formatted;
    formatted << "0x" << std::hex << std::uppercase << value;
    return WriteSetting("video", "disabled_post_processing_paths", formatted.str());
}

inline bool SetVrEnabled(bool value) {
    Mutable().vrEnabled = value;
    return WriteSetting("vr", "enabled", value ? "true" : "false");
}

inline bool SetVrHudVirtualScreen(bool value) {
    Mutable().vrHudVirtualScreen = value;
    return WriteSetting("vr", "hud_virtual_screen", value ? "true" : "false");
}

inline bool SetVrFlatScreen(bool value) {
    Mutable().vrFlatScreen = value;
    return WriteSetting("vr", "flat_screen", value ? "true" : "false");
}

inline bool SetVrPassthrough(bool value) {
    Mutable().vrPassthrough = value;
    return WriteSetting("vr", "passthrough", value ? "true" : "false");
}

inline bool SetVrStopAtDisplayCopy(bool value) {
    Mutable().vrStopAtDisplayCopy = value;
    return WriteSetting("vr", "stop_at_display_copy", value ? "true" : "false");
}

inline bool SetVrSkipCopyClears(bool value) {
    Mutable().vrSkipCopyClears = value;
    return WriteSetting("vr", "skip_copy_clears", value ? "true" : "false");
}

inline bool SetVrSinglePassEyes(bool value) {
    Mutable().vrSinglePassEyes = value;
    return WriteSetting("vr", "single_pass_eyes", value ? "true" : "false");
}

inline bool SetVrFirstPerson(bool value) {
    Mutable().vrFirstPerson = value;
    return WriteSetting("vr", "first_person", value ? "true" : "false");
}

inline bool SetVrFirstPersonToggleClick(bool value) {
    Mutable().vrFirstPersonToggleClick = value;
    return WriteSetting("vr", "first_person_toggle_click", value ? "true" : "false");
}

inline bool SetVrFirstPersonUnitsPerMeter(float value) {
    value = std::clamp(value, 1.0f, 10000.0f);
    Mutable().vrFirstPersonUnitsPerMeter = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("vr", "first_person_units_per_meter", formatted.str());
}

inline bool SetVrFirstPersonHeadUpMeters(float value) {
    value = std::clamp(value, -kVrFirstPersonHeadOffsetLimit, kVrFirstPersonHeadOffsetLimit);
    Mutable().vrFirstPersonHeadUpMeters = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("vr", "first_person_head_up_meters", formatted.str());
}

inline bool SetVrFirstPersonHeadForwardMeters(float value) {
    value = std::clamp(value, -kVrFirstPersonHeadOffsetLimit, kVrFirstPersonHeadOffsetLimit);
    Mutable().vrFirstPersonHeadForwardMeters = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("vr", "first_person_head_forward_meters", formatted.str());
}

inline bool SetVrRecenterKey(std::string value) {
    Mutable().vrRecenterKey = value;
    return WriteSetting("vr", "recenter_key", FormatString(value));
}

inline bool SetVrLeanBackDegrees(float value) {
    value = std::clamp(value, -kVrLeanBackDegreesLimit, kVrLeanBackDegreesLimit);
    Mutable().vrLeanBackDegrees = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("vr", "lean_back_degrees", formatted.str());
}

inline bool SetVrFirstPersonHideDriver(bool value) {
    Mutable().vrFirstPersonHideDriver = value;
    return WriteSetting("vr", "first_person_hide_driver", value ? "true" : "false");
}

inline bool SetVrMirrorView(std::string value) {
    if (!IsSupportedVrMirrorView(value)) {
        return false;
    }
    Mutable().vrMirrorView = value;
    return WriteSetting("vr", "mirror_view", FormatString(value));
}

inline bool SetVrControllerMode(std::string value) {
    if (!IsSupportedVrControllerMode(value)) {
        return false;
    }
    Mutable().vrControllerMode = value;
    return WriteSetting("vr", "controller_mode", FormatString(value));
}

inline bool SetVrFrameInterpolationFps(uint32_t value) {
    value = mkw::vr::NormalizeFrameInterpolationFps(value);
    Mutable().vrFrameInterpolationFps = value;
    return WriteSetting("vr", "frame_interpolation_fps", std::to_string(value));
}

inline bool SetVrFirstPersonRotation(std::string value) {
    if (!IsSupportedVrFirstPersonRotation(value)) {
        return false;
    }
    Mutable().vrFirstPersonRotation = value;
    return WriteSetting("vr", "first_person_rotation", FormatString(value));
}

inline bool SetVrPerformanceLevel(std::string value) {
    if (!IsSupportedVrPerformanceLevel(value)) {
        return false;
    }
    Mutable().vrPerformanceLevel = value;
    return WriteSetting("vr", "performance_level", FormatString(value));
}

inline bool SetVrFoveation(std::string value) {
    if (!IsSupportedVrFoveation(value)) {
        return false;
    }
    Mutable().vrFoveation = value;
    return WriteSetting("vr", "foveation", FormatString(value));
}

inline bool SetVrFirstPersonSeat(std::string value) {
    if (!IsSupportedVrFirstPersonSeat(value)) {
        return false;
    }
    Mutable().vrFirstPersonSeat = value;
    return WriteSetting("vr", "first_person_seat", FormatString(value));
}

inline bool SetVrCockpitUnitsPerMeter(float value) {
    value = std::clamp(value, kVrCockpitUnitsPerMeterMin, kVrCockpitUnitsPerMeterMax);
    Mutable().vrCockpitUnitsPerMeter = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("vr", "cockpit_units_per_meter", formatted.str());
}

inline bool SetVrSteeringWheel(bool value) {
    Mutable().vrSteeringWheel = value;
    return WriteSetting("vr", "steering_wheel", value ? "true" : "false");
}

inline bool SetVrNativeSteeringWheel(bool value) {
    Mutable().vrNativeSteeringWheel = value;
    return WriteSetting("vr", "native_steering_wheel", value ? "true" : "false");
}

inline bool SetVrHandSteering(bool value) {
    Mutable().vrHandSteering = value;
    return WriteSetting("vr", "hand_steering", value ? "true" : "false");
}

inline bool SetVrWheelTuning(const mkw::vr::WheelTuning& tuning) {
    auto& config = Mutable();
    const auto write = [](const char* key, std::optional<float>& slot, float value, float low, float high) {
        value = std::clamp(value, low, high);
        slot = value;
        std::ostringstream formatted;
        formatted << value;
        return WriteSetting("vr", key, formatted.str());
    };
    bool ok = write("wheel_kart_degrees", config.vrWheelKartDegrees, tuning.kartDegrees, kVrWheelDegreesMin,
                    kVrWheelDegreesMax);
    ok = write("wheel_bike_degrees", config.vrWheelBikeDegrees, tuning.bikeDegrees, kVrWheelDegreesMin,
               kVrWheelDegreesMax) && ok;
    ok = write("wheel_grab_distance", config.vrWheelGrabDistance, tuning.grabDistance, kVrWheelGrabDistanceMin,
               kVrWheelGrabDistanceMax) && ok;
    ok = write("wheel_grab_assist", config.vrWheelGrabAssist, tuning.grabAssist, kVrWheelGrabAssistMin,
               kVrWheelGrabAssistMax) && ok;
    ok = write("wheel_response", config.vrWheelResponse, tuning.response, kVrWheelResponseMin,
               kVrWheelResponseMax) && ok;
    ok = write("wheel_tracking_grace", config.vrWheelTrackingGrace, tuning.trackingGrace,
               kVrWheelTrackingGraceMin, kVrWheelTrackingGraceMax) && ok;
    config.vrWheelHaptics = tuning.haptics;
    return WriteSetting("vr", "wheel_haptics", tuning.haptics ? "true" : "false") && ok;
}

inline bool SetVrFirstPersonHiddenModel(int32_t value) {
    value = std::clamp(value, -1, 31);
    Mutable().vrFirstPersonHiddenModel = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("vr", "first_person_hidden_model", formatted.str());
}

inline bool SetVrFirstPersonHeadRightMeters(float value) {
    value = std::clamp(value, -kVrFirstPersonHeadOffsetLimit, kVrFirstPersonHeadOffsetLimit);
    Mutable().vrFirstPersonHeadRightMeters = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("vr", "first_person_head_right_meters", formatted.str());
}

inline bool SetControllerButton(size_t index, std::string value) {
    if (index >= kControllerButtonKeys.size()) {
        return false;
    }
    Mutable().controllerButtons[index] = value;
    return WriteSetting("controller", kControllerButtonKeys[index], FormatString(value));
}

inline std::string ControllerExpression(const std::string& key) {
    const auto it = Get().controllerExpressions.find(key);
    return it == Get().controllerExpressions.end() ? std::string() : it->second;
}

inline bool SetControllerExpression(const std::string& key, const std::string& value) {
    Mutable().controllerExpressions[key] = value;
    return WriteSetting("controller", key, FormatString(value));
}

inline bool RumbleEnabled(bool fallback = true) {
    return Get().rumbleEnabled.value_or(fallback);
}

inline bool SetRumbleEnabled(bool value) {
    Mutable().rumbleEnabled = value;
    return WriteSetting("controller", "rumble", value ? "true" : "false");
}

inline int32_t MuteHotkey(int32_t fallback) {
    return Get().muteHotkey.value_or(fallback);
}

inline bool SetMuteHotkey(int32_t value) {
    Mutable().muteHotkey = value;
    return WriteSetting("audio", "mute_key", std::to_string(value));
}

inline bool SetAudioVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "volume", formatted.str());
}

inline bool SetMusicVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioMusicVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "music_volume", formatted.str());
}

inline bool SetSoundEffectsVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioSoundEffectsVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "sound_effects_volume", formatted.str());
}

inline bool SetUiVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioUiVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "ui_volume", formatted.str());
}

inline bool SetVoicesVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVoicesVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "voices_volume", formatted.str());
}

inline bool SetAudioMuted(bool value) {
    Mutable().audioMuted = value;
    return WriteSetting("audio", "muted", value ? "true" : "false");
}

inline bool SetAudioMixWorker(bool value) {
    Mutable().audioMixWorker = value;
    return WriteSetting("audio", "mix_worker", value ? "true" : "false");
}

inline bool SetAttenuateMusicWhenMediaPlays(bool value) {
    Mutable().attenuateMusicWhenMediaPlays = value;
    return WriteSetting("audio", "attenuate_music_when_media_plays", value ? "true" : "false");
}

inline bool WidescreenEnabled(bool fallback = false) {
    return Get().widescreen.value_or(fallback);
}

inline bool WindowPosition(int32_t& x, int32_t& y) {
    if (!Get().windowPosX || !Get().windowPosY) {
        return false;
    }
    x = *Get().windowPosX;
    y = *Get().windowPosY;
    return true;
}

inline uint32_t WindowWidth(uint32_t fallback) {
    return Get().windowWidth.value_or(fallback);
}

inline uint32_t WindowHeight(uint32_t fallback) {
    return Get().windowHeight.value_or(fallback);
}

inline float ResolutionMultiplier(float fallback = 1.0f) {
    return std::max(0.0f, Get().resolutionMultiplier.value_or(fallback));
}

inline float AudioVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float MusicVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioMusicVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float SoundEffectsVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioSoundEffectsVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float UiVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioUiVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float VoicesVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVoicesVolume.value_or(fallback), 0.0f, 1.0f);
}

inline bool AudioMuted(bool fallback = false) {
    return Get().audioMuted.value_or(fallback);
}

// Off-thread AX/DSP mix. Default on; false restores the fully synchronous mix.
inline bool AudioMixWorkerEnabled(bool fallback = true) {
    return Get().audioMixWorker.value_or(fallback);
}

// Whether background music should duck automatically for other media playback.
inline bool AttenuateMusicWhenMediaPlays(bool fallback = false) {
    return Get().attenuateMusicWhenMediaPlays.value_or(fallback);
}

// Bluetooth Wii Remotes / Wii U Pro Controllers. Read once before SDL's joystick
// subsystem comes up, so a change only takes effect on the next launch.
inline bool WiiRemotesEnabled(bool fallback = true) {
    return Get().wiiRemotes.value_or(fallback);
}

// Persists the Bluetooth Wii Remote driver switch.
inline bool SetWiiRemotesEnabled(bool value) {
    Mutable().wiiRemotes = value;
    return WriteSetting("controller", "wii_remotes", value ? "true" : "false");
}

// Whether to keep rescanning Bluetooth while no Wii controller is connected.
inline bool WiiContinuousScanEnabled(bool fallback = false) {
    return Get().wiiContinuousScan.value_or(fallback);
}

// Persists the continuous scanning switch.
inline bool SetWiiContinuousScanEnabled(bool value) {
    Mutable().wiiContinuousScan = value;
    return WriteSetting("controller", "wii_continuous_scan", value ? "true" : "false");
}

// Wii Remote accelerometer zero-point correction (g, SDL sensor frame); all zero
// when the remote has not been calibrated.
inline std::array<double, 3> WiiAccelOffset() {
    const RuntimeUserConfig& config = Get();
    return {config.wiiAccelOffsetX.value_or(0.0), config.wiiAccelOffsetY.value_or(0.0),
            config.wiiAccelOffsetZ.value_or(0.0)};
}

// Whether to write the per-frame accelerometer trace (off unless asked for).
inline bool WiiAccelTraceEnabled(bool fallback = false) {
    return Get().wiiAccelTrace.value_or(fallback);
}

// True while a non-zero correction is stored ("Clear calibration" writes zeros).
inline bool HasWiiAccelOffset() {
    const std::array<double, 3> offset = WiiAccelOffset();
    return offset[0] != 0.0 || offset[1] != 0.0 || offset[2] != 0.0;
}

// Persists the accelerometer correction measured by the overlay's calibration.
inline bool SetWiiAccelOffset(const std::array<double, 3>& offset) {
    Mutable().wiiAccelOffsetX = offset[0];
    Mutable().wiiAccelOffsetY = offset[1];
    Mutable().wiiAccelOffsetZ = offset[2];
    bool ok = true;
    const char* keys[3] = {"wii_accel_offset_x", "wii_accel_offset_y", "wii_accel_offset_z"};
    for (size_t i = 0; i < 3; ++i) {
        // Always a float literal, so a whole-number offset does not come back as a TOML integer.
        std::ostringstream formatted;
        formatted << std::fixed << std::setprecision(4) << offset[i];
        ok = WriteSetting("controller", keys[i], formatted.str()) && ok;
    }
    return ok;
}

// Target frame rate for frame interpolation, or 0 to disable it.
inline uint32_t FrameInterpolationFps(uint32_t fallback = 0) {
    return Get().frameInterpolationFps.value_or(fallback);
}

// Whether to skip draws whose graphics pipeline has not finished compiling yet.
inline bool SkipUnreadyPipelines(bool fallback = true) {
    return Get().skipUnreadyPipelines.value_or(fallback);
}

inline bool DisableCopyFilter(bool fallback = true) {
    return Get().disableCopyFilter.value_or(fallback);
}

// The counter is a diagnostic, so it starts off and the F10 bar turns it on.
inline bool ShowFps(bool fallback = false) {
    return Get().showFps.value_or(fallback);
}

// Runs the host side of the GX pipeline on its own thread (gx_thread.h). On by
// default on the Quest, where the game thread is the bottleneck; opt-in elsewhere.
inline bool GxThread() {
#if defined(__ANDROID__)
    return Get().gxThread.value_or(true);
#else
    return Get().gxThread.value_or(false);
#endif
}

inline bool TextureReplacements(bool fallback = false) {
    return Get().textureReplacements.value_or(fallback);
}

// Dumping only produces the names a replacement would need, so main.cpp
// gates it on TextureReplacements() as well.
inline bool TextureDumps(bool fallback = false) {
    return Get().textureDumps.value_or(fallback);
}

// An absent setting means the default mask, not "nothing disabled": a fresh
// install has no [video] section at all and still starts without bloom.
inline uint32_t DisabledPostProcessingPaths(uint32_t fallback = kDisabledPostProcessingPathsDefault) {
    return Get().disabledPostProcessingPaths.value_or(fallback) & kPostProcessingBloomPath;
}

inline bool VrEnabled(bool fallback = false) {
    return Get().vrEnabled.value_or(fallback);
}

inline bool VrRequired(bool fallback = false) {
    return Get().vrRequired.value_or(fallback);
}

inline float VrRenderScale(float fallback = kVrRenderScaleDefault) {
    return std::clamp(Get().vrRenderScale.value_or(fallback), 0.25f, 2.0f);
}

inline float VrWorldUnitsPerMeter(float fallback = 500.0f) {
    return std::clamp(Get().vrWorldUnitsPerMeter.value_or(fallback), 1.0f, 10000.0f);
}

inline float VrHudDistanceMeters(float fallback = 2.0f) {
    return std::clamp(Get().vrHudDistanceMeters.value_or(fallback), 0.25f, 10.0f);
}

inline float VrHudWidthMeters(float fallback = 2.4f) {
    return std::clamp(Get().vrHudWidthMeters.value_or(fallback), 0.25f, 20.0f);
}

inline bool VrHudVirtualScreen(bool fallback = true) {
    return Get().vrHudVirtualScreen.value_or(fallback);
}

// Races on the flat virtual screen the menus use, instead of immersive
// stereo. The launcher's Settings page shows the same default.
inline bool VrFlatScreen(bool fallback = false) {
    return Get().vrFlatScreen.value_or(fallback);
}

// The room, through the headset's cameras, around the menu screen and every
// other virtual screen, a Flat Screen race included (never an immersive
// race). Only the Quest offers it; the launcher's Settings page shows the same
// default.
inline bool VrPassthrough(bool fallback = true) {
    return Get().vrPassthrough.value_or(fallback);
}

inline bool VrStopAtDisplayCopy(bool fallback = true) {
    return Get().vrStopAtDisplayCopy.value_or(fallback);
}

inline bool VrSkipCopyClears(bool fallback = true) {
    return Get().vrSkipCopyClears.value_or(fallback);
}

inline bool VrSinglePassEyes(bool fallback = true) {
    return Get().vrSinglePassEyes.value_or(fallback);
}

inline bool VrFirstPerson(bool fallback = false) {
    return Get().vrFirstPerson.value_or(fallback);
}

inline bool VrFirstPersonToggleClick(bool fallback = true) {
    return Get().vrFirstPersonToggleClick.value_or(fallback);
}

inline float VrFirstPersonUnitsPerMeter(float fallback = kVrFirstPersonUnitsPerMeterDefault) {
    return std::clamp(Get().vrFirstPersonUnitsPerMeter.value_or(fallback), 1.0f, 10000.0f);
}

inline float VrFirstPersonHeadUpMeters(float fallback = kVrFirstPersonHeadUpDefault) {
    return std::clamp(Get().vrFirstPersonHeadUpMeters.value_or(fallback),
                      -kVrFirstPersonHeadOffsetLimit, kVrFirstPersonHeadOffsetLimit);
}

inline float VrFirstPersonHeadForwardMeters(float fallback = kVrFirstPersonHeadForwardDefault) {
    return std::clamp(Get().vrFirstPersonHeadForwardMeters.value_or(fallback),
                      -kVrFirstPersonHeadOffsetLimit, kVrFirstPersonHeadOffsetLimit);
}

inline float VrFirstPersonHeadRightMeters(float fallback = kVrFirstPersonHeadRightDefault) {
    return std::clamp(Get().vrFirstPersonHeadRightMeters.value_or(fallback),
                      -kVrFirstPersonHeadOffsetLimit, kVrFirstPersonHeadOffsetLimit);
}

inline std::string VrRecenterKey(std::string fallback = std::string(kVrRecenterKeyDefault)) {
    return Get().vrRecenterKey.value_or(std::move(fallback));
}

inline float VrLeanBackDegrees(float fallback = kVrLeanBackDegreesDefault) {
    return std::clamp(Get().vrLeanBackDegrees.value_or(fallback),
                      -kVrLeanBackDegreesLimit, kVrLeanBackDegreesLimit);
}

inline bool VrFirstPersonHideDriver(bool fallback = kVrFirstPersonHideDriverDefault) {
    return Get().vrFirstPersonHideDriver.value_or(fallback);
}

inline std::string VrMirrorView(std::string fallback = kVrMirrorViewDefault) {
    const auto& value = Get().vrMirrorView;
    return value && IsSupportedVrMirrorView(*value) ? *value : std::move(fallback);
}

inline std::string VrControllerMode(std::string fallback = kVrControllerModeDefault) {
    const auto& value = Get().vrControllerMode;
    return value && IsSupportedVrControllerMode(*value) ? *value : std::move(fallback);
}

inline uint32_t VrFrameInterpolationFps() {
    return mkw::vr::NormalizeFrameInterpolationFps(Get().vrFrameInterpolationFps.value_or(0));
}

inline bool DiagnosticsOpenXRLogging(bool fallback = false) {
    return Get().diagnosticsOpenXRLogging.value_or(fallback);
}

inline bool SetDiagnosticsOpenXRLogging(bool value) {
    Mutable().diagnosticsOpenXRLogging = value;
    return WriteSetting("diagnostics", "openxr_logging", value ? "true" : "false");
}

inline std::string VrFirstPersonRotation(std::string fallback = kVrFirstPersonRotationDefault) {
    const auto& value = Get().vrFirstPersonRotation;
    return value && IsSupportedVrFirstPersonRotation(*value) ? *value : std::move(fallback);
}

inline std::string VrPerformanceLevel(std::string fallback = kVrPerformanceLevelDefault) {
    const auto& value = Get().vrPerformanceLevel;
    return value && IsSupportedVrPerformanceLevel(*value) ? *value : std::move(fallback);
}

inline std::string VrFoveation(std::string fallback = kVrFoveationDefault) {
    const auto& value = Get().vrFoveation;
    return value && IsSupportedVrFoveation(*value) ? *value : std::move(fallback);
}

inline int32_t VrFirstPersonHiddenModel(int32_t fallback = kVrFirstPersonHiddenModelDefault) {
    return std::clamp(Get().vrFirstPersonHiddenModel.value_or(fallback), -1, 31);
}

inline std::string VrFirstPersonSeat(std::string fallback = kVrFirstPersonSeatDefault) {
    const auto& value = Get().vrFirstPersonSeat;
    return value && IsSupportedVrFirstPersonSeat(*value) ? *value : std::move(fallback);
}

inline float VrCockpitUnitsPerMeter(float fallback = kVrCockpitUnitsPerMeterDefault) {
    return std::clamp(Get().vrCockpitUnitsPerMeter.value_or(fallback), kVrCockpitUnitsPerMeterMin,
                      kVrCockpitUnitsPerMeterMax);
}

inline bool VrSteeringWheel(bool fallback = kVrSteeringWheelDefault) {
    return Get().vrSteeringWheel.value_or(fallback);
}

inline bool VrNativeSteeringWheel(bool fallback = kVrNativeSteeringWheelDefault) {
    return Get().vrNativeSteeringWheel.value_or(fallback);
}

inline bool VrHandSteering(bool fallback = kVrHandSteeringDefault) {
    return Get().vrHandSteering.value_or(fallback);
}

inline mkw::vr::WheelTuning VrWheelTuning() {
    const auto& config = Get();
    mkw::vr::WheelTuning tuning{};
    tuning.kartDegrees =
        std::clamp(config.vrWheelKartDegrees.value_or(tuning.kartDegrees), kVrWheelDegreesMin, kVrWheelDegreesMax);
    tuning.bikeDegrees =
        std::clamp(config.vrWheelBikeDegrees.value_or(tuning.bikeDegrees), kVrWheelDegreesMin, kVrWheelDegreesMax);
    tuning.grabDistance = std::clamp(config.vrWheelGrabDistance.value_or(tuning.grabDistance),
                                     kVrWheelGrabDistanceMin, kVrWheelGrabDistanceMax);
    tuning.grabAssist = std::clamp(config.vrWheelGrabAssist.value_or(tuning.grabAssist), kVrWheelGrabAssistMin,
                                   kVrWheelGrabAssistMax);
    tuning.response =
        std::clamp(config.vrWheelResponse.value_or(tuning.response), kVrWheelResponseMin, kVrWheelResponseMax);
    tuning.trackingGrace = std::clamp(config.vrWheelTrackingGrace.value_or(tuning.trackingGrace),
                                      kVrWheelTrackingGraceMin, kVrWheelTrackingGraceMax);
    tuning.haptics = config.vrWheelHaptics.value_or(tuning.haptics);
    return tuning;
}

inline std::string GraphicsApi(std::string fallback = "auto") {
    return Get().graphicsApi.value_or(std::move(fallback));
}

inline std::string DisplayMode(std::string fallback = "windowed") {
    return Get().displayMode.value_or(std::move(fallback));
}

inline bool NetworkEnabled(bool fallback = true) {
    return Get().networkEnabled.value_or(fallback);
}

inline std::string NandRoot(std::string fallback = "") {
    return Get().nandRoot.value_or(std::move(fallback));
}

inline std::string DvdRoot(std::string fallback = "") {
    return Get().dvdRoot.value_or(std::move(fallback));
}

// The one resolver for configured paths. A relative value means the same thing
// everywhere it can be configured: relative to the config file that named it,
// never to the process working directory (docs/WHEELWIZARD_CONTRACT.md).
inline std::filesystem::path ResolveRelativeTo(const std::filesystem::path& base,
                                               const std::string& value) {
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return path.lexically_normal();
}

inline std::filesystem::path ResolveRelativeToConfig(const std::string& value) {
    return ResolveRelativeTo(ResolveConfigPath().parent_path(), value);
}

// The extracted DATA directory. Empty when nothing is configured.
inline std::filesystem::path ResolvedDvdRoot() {
    const std::string configured = DvdRoot();
    return configured.empty() ? std::filesystem::path{} : ResolveRelativeToConfig(configured);
}

/// The canonical Retro Rewind installation the frontend owns, or "" when none is recorded.
inline std::string RetroRewindRoot(std::string fallback = "") {
    return Get().retroRewindRoot.value_or(std::move(fallback));
}

inline bool DiscordPresenceEnabled(bool fallback = true) {
    return Get().discordPresenceEnabled.value_or(fallback);
}

inline std::string DiscordClientId(std::string fallback = "1543984562369990706") {
    return Get().discordClientId.value_or(std::move(fallback));
}

inline const std::vector<std::string>& OverlayRoots() {
    return Get().overlayRoots;
}

inline void LogLoadedConfig() {
    static const bool logged = [] {
        const auto& config = Get();
        const auto configPath = ResolveConfigPath();
        std::cout << "[runtime-config] " << PathToUtf8(configPath);
        if (!std::filesystem::exists(configPath)) {
            std::cout << " not found; using built-in defaults";
        } else {
            std::cout << " loaded";
            if (config.widescreen) {
                std::cout << " widescreen=" << (*config.widescreen ? "true" : "false");
            }
            if (config.windowWidth || config.windowHeight) {
                std::cout << " window=" << config.windowWidth.value_or(0) << "x"
                          << config.windowHeight.value_or(0);
            }
            if (config.resolutionMultiplier) {
                std::cout << " resolution_multiplier=" << *config.resolutionMultiplier;
            }
            if (config.dvdRoot) {
                std::cout << " dvd_root=" << *config.dvdRoot;
            }
            if (config.graphicsApi) {
                std::cout << " graphics_api=" << *config.graphicsApi;
            }
            if (config.frameInterpolationFps) {
                std::cout << " frame_interpolation_fps=" << *config.frameInterpolationFps;
            }
            if (config.skipUnreadyPipelines) {
                std::cout << " skip_unready_pipelines=" << (*config.skipUnreadyPipelines ? "true" : "false");
            }
            if (config.disableCopyFilter) {
                std::cout << " disable_copy_filter=" << (*config.disableCopyFilter ? "true" : "false");
            }
            if (config.showFps) {
                std::cout << " show_fps=" << (*config.showFps ? "true" : "false");
            }
            if (config.textureReplacements) {
                std::cout << " texture_replacements=" << (*config.textureReplacements ? "true" : "false");
            }
            if (config.textureDumps) {
                std::cout << " texture_dumps=" << (*config.textureDumps ? "true" : "false");
            }
            if (config.vrEnabled) {
                std::cout << " vr_enabled=" << (*config.vrEnabled ? "true" : "false");
            }
            if (config.vrRequired) {
                std::cout << " vr_required=" << (*config.vrRequired ? "true" : "false");
            }
            if (config.audioVolume) {
                std::cout << " audio_volume=" << *config.audioVolume;
            }
            if (config.audioMuted) {
                std::cout << " audio_muted=" << (*config.audioMuted ? "true" : "false");
            }
            if (config.networkEnabled) {
                std::cout << " network_enabled=" << (*config.networkEnabled ? "true" : "false");
            }
            if (config.discordPresenceEnabled) {
                std::cout << " discord_enabled=" << (*config.discordPresenceEnabled ? "true" : "false");
            }
            if (config.nandRoot) {
                std::cout << " nand_root=" << *config.nandRoot;
            }
            if (config.retroRewindRoot) {
                std::cout << " retro_rewind_root=" << *config.retroRewindRoot;
            }
        }
        std::cout << std::endl;
        return true;
    }();
    (void)logged;
}

} // namespace RuntimeConfigFile
