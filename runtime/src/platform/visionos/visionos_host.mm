// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/visionos/visionos_host.h"

#include "runtime_config.h"
#include "vr/visionos/xr_visionos.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <pthread.h>

// runtime/src/main.cpp on this platform.
extern "C" int mkw_runtime_main(int argc, char** argv);

namespace {

// The game thread's own stack. RuntimeMain runs the host side of the game
// (guest code runs on libco fibers with stacks of their own), but the desktop
// main thread it was written for has 8 MiB and a few deep host paths (shader
// compilation, the config parser) come close to it on the iOS family's default.
constexpr size_t kGameThreadStackSize = 64u << 20;

std::mutex g_mutex;
std::condition_variable g_exited;
std::atomic_bool g_running{false};
std::atomic_bool g_started{false};
int g_exit_code = 0;
std::string g_last_error;
std::string g_data_dir;
std::string g_resources_dir;
std::string g_game_dir_storage;
std::string g_config_path_storage;
std::string g_disc_dir_storage;

void SetError(std::string message) {
    std::lock_guard lock(g_mutex);
    g_last_error = std::move(message);
}

std::filesystem::path GameDirectory() {
    return RuntimeConfigFile::ApplicationDataDirectory();
}

void* GameThreadMain(void*) {
    pthread_setname_np("WiiCompiled game");
    static char program_name[] = "WiiCompiledVision";
    char* argv[] = {program_name, nullptr};
    const int code = mkw_runtime_main(1, argv);
    {
        std::lock_guard lock(g_mutex);
        g_exit_code = code;
        g_running.store(false, std::memory_order_release);
    }
    g_exited.notify_all();
    return nullptr;
}

} // namespace

extern "C" {

void mkw_visionos_set_directories(const char* data_dir, const char* resources_dir) {
    std::lock_guard lock(g_mutex);
    g_data_dir = data_dir != nullptr ? data_dir : "";
    g_resources_dir = resources_dir != nullptr ? resources_dir : "";
    if (!g_data_dir.empty()) {
        setenv("MKW_APPLE_DATA_DIR", g_data_dir.c_str(), 1);
    } else {
        unsetenv("MKW_APPLE_DATA_DIR");
    }
    if (!g_resources_dir.empty()) {
        setenv("MKW_APPLE_RESOURCES_DIR", g_resources_dir.c_str(), 1);
    } else {
        unsetenv("MKW_APPLE_RESOURCES_DIR");
    }
}

void mkw_visionos_set_layer_renderer(void* layer_renderer) {
    xr_visionos_set_layer_renderer(layer_renderer);
}

bool mkw_visionos_layer_invalidated(void) {
    return xr_visionos_layer_invalidated();
}

void mkw_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray, float origin_x,
                                float origin_y, float origin_z, float direction_x, float direction_y,
                                float direction_z, bool has_pose, float pose_x, float pose_y, float pose_z) {
    xr_visionos_spatial_event(event_id, phase, chirality, has_ray, origin_x, origin_y, origin_z, direction_x,
                              direction_y, direction_z, has_pose, pose_x, pose_y, pose_z);
}

bool mkw_visionos_start_game(void) {
    if (g_running.exchange(true, std::memory_order_acq_rel)) {
        SetError("the game is already running");
        return false;
    }
    if (g_started.exchange(true, std::memory_order_acq_rel)) {
        // The runtime keeps process-wide state (guest memory at a fixed address, HLE
        // singletons) that a second run in the same process would trip over.
        g_running.store(false, std::memory_order_release);
        SetError("the game already ran once in this process; relaunch the app to play again");
        return false;
    }
    // main() is SwiftUI's, so SDL is told the platform initialisation it would do
    // from SDL_main already happened. The offscreen video driver Aurora picks on
    // this platform (aurora-main/lib/window.cpp) needs nothing from UIKit.
    SDL_SetMainReady();
    {
        std::lock_guard lock(g_mutex);
        g_exit_code = 0;
    }
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kGameThreadStackSize);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int result = pthread_create(&thread, &attributes, &GameThreadMain, nullptr);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        g_running.store(false, std::memory_order_release);
        g_started.store(false, std::memory_order_release);
        SetError(std::string("could not create the game thread: ") + std::strerror(result));
        return false;
    }
    return true;
}

bool mkw_visionos_game_running(void) {
    return g_running.load(std::memory_order_acquire);
}

int mkw_visionos_exit_code(void) {
    std::lock_guard lock(g_mutex);
    return g_exit_code;
}

void mkw_visionos_request_quit(void) {
    if (!g_running.load(std::memory_order_acquire)) {
        return;
    }
    // Aurora turns SDL's quit into AURORA_EXIT (aurora-main/lib/window.cpp), the
    // same path a closed desktop window takes; the runtime then saves and shuts
    // down in order.
    if (SDL_WasInit(SDL_INIT_EVENTS) != 0) {
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
}

bool mkw_visionos_wait_for_exit(uint32_t timeout_ms) {
    std::unique_lock lock(g_mutex);
    return g_exited.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [] { return !g_running.load(std::memory_order_acquire); });
}

const char* mkw_visionos_game_directory(void) {
    std::lock_guard lock(g_mutex);
    g_game_dir_storage = RuntimeConfigFile::PathToUtf8(GameDirectory());
    return g_game_dir_storage.c_str();
}

const char* mkw_visionos_config_path(void) {
    std::lock_guard lock(g_mutex);
    g_config_path_storage = RuntimeConfigFile::PathToUtf8(GameDirectory() / RuntimeConfigFile::kConfigFileName);
    return g_config_path_storage.c_str();
}

const char* mkw_visionos_disc_directory(void) {
    std::lock_guard lock(g_mutex);
    g_disc_dir_storage = RuntimeConfigFile::PathToUtf8(GameDirectory() / "DATA");
    return g_disc_dir_storage.c_str();
}

bool mkw_visionos_prepare_game_directory(void) {
    const std::filesystem::path game = GameDirectory();
    std::error_code ec;
    std::filesystem::create_directories(game, ec);
    std::filesystem::create_directories(game / "DATA", ec);
    if (ec) {
        SetError("could not create " + RuntimeConfigFile::PathToUtf8(game) + ": " + ec.message());
        return false;
    }
    const std::filesystem::path config = game / RuntimeConfigFile::kConfigFileName;
    if (!std::filesystem::is_regular_file(config, ec)) {
        // The same first config the Quest launcher writes (android/.../GameStorage.kt),
        // with the paths relative to the config so the folder can move with the app.
        std::ofstream file(config, std::ios::binary | std::ios::trunc);
        if (!file) {
            SetError("could not write " + RuntimeConfigFile::PathToUtf8(config));
            return false;
        }
        file << "# WiiCompiled Apple Vision Pro configuration. Edit with the in-game panel, or here\n"
                "# through the Files app (On My Apple Vision Pro > WiiCompiled Vision > WiiCompiled).\n"
                "[paths]\n"
                "dvd_root = \"DATA\"\n"
                "retro_rewind_root = \"RetroRewind6\"\n"
                "\n"
                "[video]\n"
                "widescreen = true\n"
                "resolution_multiplier = 1.0\n"
                "\n"
                "[vr]\n"
                "enabled = true\n"
                "render_scale = " MKW_VR_RENDER_SCALE_DEFAULT_TEXT "\n"
                "# New players start in the cockpit: first person, following turns and climbs.\n"
                "first_person = true\n"
                "first_person_rotation = \"yaw_pitch\"\n"
                "first_person_seat = \"cockpit\"\n";
        if (!file) {
            SetError("could not write " + RuntimeConfigFile::PathToUtf8(config));
            return false;
        }
    }
    // In practice the runtime gets here first: globals of the settings overlay read the
    // config during static initialisation, before any Swift runs, so EnsureConfigFile has
    // already written the desktop template (dvd_root commented out) and cached it. The
    // paths this app decides for the player are therefore filled in after the fact, in the
    // file and in the cached config alike, the way the in-game panel's setters do.
    bool ok = true;
    if (RuntimeConfigFile::DvdRoot().empty()) {
        RuntimeConfigFile::Mutable().dvdRoot = "DATA";
        ok = RuntimeConfigFile::WriteSetting("paths", "dvd_root", "\"DATA\"") && ok;
    }
    if (RuntimeConfigFile::RetroRewindRoot().empty()) {
        RuntimeConfigFile::Mutable().retroRewindRoot = "RetroRewind6";
        ok = RuntimeConfigFile::WriteSetting("paths", "retro_rewind_root", "\"RetroRewind6\"") && ok;
    }
    if (!ok) {
        SetError("could not record the game paths in " + RuntimeConfigFile::PathToUtf8(config));
    }
    return ok;
}

bool mkw_visionos_disc_present(void) {
    const std::filesystem::path disc = GameDirectory() / "DATA";
    std::error_code ec;
    return std::filesystem::is_directory(disc / "files", ec) && std::filesystem::is_regular_file(disc / "sys" / "fst.bin", ec);
}

const char* mkw_visionos_last_error(void) {
    std::lock_guard lock(g_mutex);
    return g_last_error.c_str();
}

} // extern "C"
