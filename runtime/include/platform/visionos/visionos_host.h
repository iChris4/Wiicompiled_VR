// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The bridge between the visionOS SwiftUI app (visionos/) and the game runtime.
//
// The runtime is a static library on this platform (WiiCompiledGame.a or
// RetroRewindGame.a, cmake/PublicProducts.cmake); the app links it with
// -force_load and drives it through these C functions. main() belongs to
// SwiftUI: the game runs on a thread of its own, started once the immersive
// space's CompositorLayer exists, since the OpenXR provider (vr/visionos) needs
// the layer renderer before the runtime creates its session.
//
// Every function is safe to call from the main thread.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where the runtime keeps and looks for things. `data_dir` is the parent of the
// WiiCompiled folder (Config.toml, DATA, NAND, Logs): the app's Documents
// directory, which file sharing exposes. `resources_dir` holds the bundled
// wii_bootstrap/, dsp_coef.bin and initial_pipeline_cache.db: the bundle's
// resource path. Call before mkw_visionos_start_game.
void mkw_visionos_set_directories(const char* data_dir, const char* resources_dir);

// The cp_layer_renderer_t of the immersive space's CompositorLayer, retained by
// the provider. Set it before starting the game; setting NULL detaches it.
void mkw_visionos_set_layer_renderer(void* layer_renderer);

// A spatial event of the immersive space (the CompositorLayer's onSpatialEvent):
// visionOS's look-and-pinch selection, which the provider turns into the
// game's pointer and select. `phase`: 0 active, 1 ended, 2 cancelled;
// `chirality`: 0 unknown, 1 left, 2 right; the ray (when `has_ray`) is the
// event's selectionRay and the pose (when `has_pose`) its inputDevicePose's
// position, both in the space's coordinates.
void mkw_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray, float origin_x,
                                float origin_y, float origin_z, float direction_x, float direction_y,
                                float direction_z, bool has_pose, float pose_x, float pose_y, float pose_z);

// True once the layer renderer was invalidated: the immersive space was
// dismissed. The runtime then carries on without a headset, invisibly, so the
// app asks it to quit (mkw_visionos_request_quit).
bool mkw_visionos_layer_invalidated(void);

// Starts the runtime on the game thread. False when it is already running or
// the thread could not be created (see mkw_visionos_last_error).
bool mkw_visionos_start_game(void);

bool mkw_visionos_game_running(void);

// The runtime's exit code once it returned; 0 before.
int mkw_visionos_exit_code(void);

// Asks the runtime to quit the way closing its window would. Returns once the
// request is posted, not once the game stopped.
void mkw_visionos_request_quit(void);

// Blocks until the game thread returned, or `timeout_ms` elapsed (false).
bool mkw_visionos_wait_for_exit(uint32_t timeout_ms);

// The game folder under data_dir (mkw_visionos_set_directories), Config.toml
// inside it and the extracted disc folder (DATA) the first config names.
// Pointers into static storage, valid until the next call of the same function.
const char* mkw_visionos_game_directory(void);
const char* mkw_visionos_config_path(void);
const char* mkw_visionos_disc_directory(void);

// Creates the game folder and, when there is none, a first Config.toml with VR
// on and dvd_root pointing at the disc folder. False with the error recorded.
bool mkw_visionos_prepare_game_directory(void);

// Whether DATA holds an extracted disc the runtime's DVD layer accepts: a
// files/ folder and sys/fst.bin (see android/.../GameStorage.kt).
bool mkw_visionos_disc_present(void);

// The last error any of these functions recorded, or "".
const char* mkw_visionos_last_error(void);

#ifdef __cplusplus
}
#endif
