# WiiCompiled VR on Meta Quest (standalone Android)

This document is the design and build reference for the native Quest build. It
complements `OPENXR.md`, which remains the specification for the presentation
policy, the virtual screen, the first-person camera and frame interpolation:
all of that is shared, unchanged, between the Windows D3D12 product and the
Quest Vulkan product. What differs is everything below the stereo replay: the
graphics binding, the app shell and the platform glue.

## Sources of the design

- **KartPad** (`kartpad-main/`, the `kartpad-android` runtime branch of the
  WiiCompiled fork) proved that the translated game runs on Android arm64 with
  Aurora on Dawn/Vulkan under SDL3's `SDLActivity`. Its lessons carried over:
  the products are shared libraries SDL loads, the activity exports its
  directories through the environment before native code runs, the
  Windows-generated blob assembly needs its section syntax rewritten for ELF,
  and Dawn's android-aarch64 prebuilt package needs one path rewritten.
  KartPad's Android fiber/JNI split (never calling Java-backed SDL APIs from a
  guest fiber stack) is respected here by keeping every OpenXR call on the
  dedicated pacing thread, which is a real `std::thread`.
- **DolphinXR** (`Dolphin-OpenXR-2/`, quest flavour) supplied the Quest-side
  specifics: the loader must be initialised with the *activity* as its
  context or the session never leaves `IDLE`; `XrInstanceCreateInfoAndroidKHR`
  must be chained on instance creation; the manifest needs the Khronos runtime
  broker queries, the `OPENXR_SYSTEM` permission, the `com.oculus.intent.category.VR`
  intent category and the `XR_ACTIVITY_START_MODE_FULL_SPACE_UNMANAGED`
  property; `XR_KHR_android_thread_settings` may reject the renderer-worker
  type on some runtime builds.

## Architecture

### One runtime, two graphics bindings

`runtime/src/vr/openxr_integration.cpp` owns the pacing thread, policy
evaluation, the retained-layer protocol and the head-pose maths. It is written
against the backend-neutral vocabulary in `runtime/include/vr/openxr_backend.h`
(`OpenXRPresentation`, `OpenXRBackendFrame`, `OpenXRBeginStatus`,
`OpenXRSubmissionStatus`) and selects one backend class at compile time:

| Platform | Backend | Binding |
| --- | --- | --- |
| Windows | `OpenXRD3D12Backend` (`openxr_d3d12.cpp`) | Dawn's own D3D12 device and queue are bound to the session; eyes are copied on that queue. |
| Android | `OpenXRQuestVulkanBackend` (`openxr_vulkan_direct.cpp`): direct presentation through `OpenXRVulkanDirectBackend` by default, the two-device bridge `OpenXRVulkanBackend` (`openxr_vulkan.cpp`) when `[vr] direct_presentation` is off or Dawn lacks the hooks | Direct: Dawn's own `VkDevice` and queue, created through the OpenXR runtime, are bound to the session and the eyes are drawn straight into the swapchain images. Bridge: the backend creates its **own** `VkInstance`/`VkDevice` through the runtime; Dawn and that device meet on `AHardwareBuffer`s. |

The D3D12 types keep their old names through aliases, so `openxr_d3d12_replay_tests`
and the desktop code did not change.

### Direct presentation

Since 2026-10-07 the Quest binds Dawn's own Vulkan device to the session, as
the Windows Vulkan binding does (`OPENXR.md`, "Windows Vulkan"), and the Metroid
Prime port did after it. `OpenXRVulkanDirectBackend::QueryGraphicsRequirements`
installs the hooks of the patched Dawn's Vulkan ABI
(`aurora-main/patches/dawn/aurora_vulkan_interop.inc`, `AuroraDawnVulkan*`,
which the Quest's Dawn has carried since the foveation work) before Aurora
creates its device, so Dawn's `VkInstance` and `VkDevice` come from
`xrCreateVulkanInstanceKHR` and `xrCreateVulkanDeviceKHR` (Vulkan 1.2 and the
timeline-semaphore feature are asked for, as on Windows). `BindAurora` then
hands Dawn's instance, physical device, device and graphics queue to
`xrCreateSession`, creates the eye swapchains in the sRGB sibling of Aurora's
UNORM format, and installs Aurora's direct stereo sink
(`aurora-main/lib/webgpu/vulkan_direct_interop.cpp`): each acquired swapchain
`VkImage` is wrapped once as a Dawn texture (`AuroraDawnVulkanWrap`) and the
eye is drawn into it with the same full-screen triangle as the bridge below,
decoding to the sRGB attachment, inside the frame worker's command buffer right
after the eye passes; after the submit, `AuroraDawnVulkanRelease` returns the
images to the colour-attachment layout on the same queue and the XR thread
releases them. Nothing crosses devices, so there are no fences: ordering is the
queue's. The OpenXR rule that the runtime may use the queue only in
`xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage` and
`xrReleaseSwapchainImage` is honoured by taking Dawn's device lock around
exactly those calls (and swapchain creation and destruction), never across
`xrWaitFrame` or `xrWaitSwapchainImage`.

Measured on the same automated Luigi Circuit race (player idle, `render_scale`
0.8, GPU at 492 MHz, 60 FPS; the binding is chosen at start, so three rounds
of alternating launches, each captured for a minute): the compositor's
`App=` GPU time went from 8.2 ms with the bridge to 5.1 ms, `GPU%` from 0.57
to 0.38, `CPU&GPU=` latency from 9.0 to 5.3 ms and `CPU%` from 0.52 to 0.45,
while Aurora's own eye timings stayed within their launch-to-launch spread:
the saving is the bridge's `vkCmdCopyImage` on the second device and its
fences, which Prime had measured at about 2.6 ms for both eyes. Screenshots of
the two bindings are identical.
`[vr] direct_presentation` (the settings page's "Direct to headset") turns it
off, and a Dawn without the hooks (`Build-Quest.ps1 -StockDawn`) or a runtime
without `XR_KHR_vulkan_enable2` falls back on its own, both logged as
`direct presentation unavailable (...)`; the bridge below then takes over.

### The shared-buffer bridge, the fallback

Before that, and still when direct presentation is off: the stock pinned Dawn
package (`v20260603.191052`) exposes only `VkInstance` from its Vulkan backend,
no `VkDevice`, `VkQueue` or queue family, and it will not enable the device
extensions the OpenXR runtime demands. Binding Dawn's device to the session is
impossible without the patched Dawn. Instead:

1. `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` (`XR_KHR_vulkan_enable2`,
   with an `XR_KHR_vulkan_enable` fallback that queries the extension lists)
   create a small Vulkan device the runtime is happy with.
2. Per eye, two `AHardwareBuffer`s (R8G8B8A8_UNORM, or RGBA16F when Aurora
   renders float) are allocated and imported on that device
   (`VK_ANDROID_external_memory_android_hardware_buffer`).
3. Aurora imports the same buffers as Dawn shared texture memory
   (`SharedTextureMemoryAHardwareBuffer`) and, inside the frame worker's
   command buffer, draws each replayed eye into the buffer with one
   full-screen triangle that loads the eye's texels
   (`aurora-main/lib/webgpu/vulkan_interop.cpp`, the twin of
   `d3d12_interop.cpp` and registered through the same stereo sink).
   `CopyTextureToTexture` remains only for a buffer Dawn cannot render into:
   on Adreno a transfer copy is the slow path (below).
4. Ordering across the two devices uses Android sync file descriptors:
   Dawn's `EndAccess` exports a `SharedFenceSyncFD` the OpenXR device waits on
   before its `vkCmdCopyImage` into the acquired swapchain image, and the copy
   signals an exportable semaphore whose sync fd Dawn waits on before it
   writes that buffer again. Image layouts follow Vulkan's rule that a queue
   family ownership release and acquire must repeat the same old/new layout
   pair: Dawn reports its release layout, the OpenXR side acquires with it,
   transitions for the copy, and hands the buffer back in `GENERAL` with a
   transition-free release so Dawn's acquire can mirror it.
5. The copy runs on the queue bound to the session before
   `xrReleaseSwapchainImage`, so the compositor sees ordinary same-queue work.

Dawn's release fences are imported into semaphores owned by the copy's
submission slot, not by the shared buffer: importing into a semaphore whose
previous wait is still pending is invalid, and only the slot's fence proves
that wait completed before the semaphore is reused. Each frame hands Dawn a
duplicate of the buffer's copy-out fence, so a frame cancelled before encoding
keeps the ordering against the last real reader. A copy that never reached the
queue (a submit refused for memory, Aurora failing before it recorded anything)
ends its frame on the retained layer with the `VkResult` in the log, and the
next frame is tried; only work that may have been queued with no completion
marker, a lost device above all, ends the session. Three hundred skipped copies
in a row end it too.

The cost is one extra pass per eye per frame on Dawn's side and the
`vkCmdCopyImage` on the runtime's (the 3.1 ms above); the benefit is that the
bridge needs nothing from Dawn beyond its public API, and the OpenXR device
outlives Aurora's, which is exactly the failure DolphinXR hit on Vulkan when a
game's device was destroyed under the compositor. Dawn's side was a transfer
copy too until 2026-10-07: the Metroid Prime port, on this renderer, measured a
transfer copy of both eyes into the runtime's images at about 2.6 ms a frame
against 0.4 ms for a draw, and here the draw took 0.2 to 0.6 ms of App GPU
time off a Luigi Circuit race at `render_scale` 0.8 (measured below). The same
blitter (`aurora-main/lib/webgpu/eye_blitter.hpp`) serves both bindings.

`gpu.cpp` steers Aurora to an RGBA8 surface format under `xrInterop` on Android
(there is no BGRA `AHardwareBuffer` format) and requests the two Dawn features
the bridge needs.

### Foveated rendering

`[vr] foveation` (`OPENXR.md`, Foveated rendering) shades the edges of the
immersive eyes more coarsely under a `VK_EXT_fragment_density_map`. The Quest 3
(Adreno 740, Vulkan 1.3) exposes it with non-subsampled attachments, as well as
dynamic rendering, `VK_EXT_fragment_density_map2`,
`VK_QCOM_fragment_density_map_offset` and `VK_KHR_fragment_shading_rate`
(`adb shell cmd gpu vkjson`).

`XR_FB_foveation` cannot be used: its maps only shape render passes that draw
into the runtime's swapchain images on the runtime's device, and the eyes reach
that device through the copy above. The map has to be attached to Dawn's own eye
passes, which the stock package cannot do. So `Build-Quest.ps1` first runs
`android/Build-QuestDawn.ps1`, which builds the pinned Dawn revision
(`13abc3bc`, the stock package's) with `aurora-main/patches/dawn`, then links it
instead of the stock package:

- The patch (`aurora_fdm.inc`, applied by `apply.py`) adds the extension to
  Dawn's Vulkan backend and enables it only when Aurora asks before creating
  the device, and only for dynamic rendering. It then flags every render
  pipeline for density-mapped passes, and chains a density map into any render
  pass whose first color attachment is a texture view bound to one. Maps are raw
  `VkImage`s uploaded once through Dawn's queue. They are read on the CPU when a
  render pass is recorded, so a map is used only after its upload has completed.
  Its C ABI is `aurora-main/include/aurora/dawn_fdm_abi.h`.
- The build mirrors the dawn-build CI's Android configuration (NDK
  `29.0.14206865`, `android-28`, static monolithic library, samples, tests and
  tools off, `llvm-strip --strip-debug`). It also sets `DAWN_BUILD_PROTOBUF=OFF`:
  the stock CI hands the build a host `protoc`, and without one the build tried
  to run the `protoc` it had cross-compiled for Android.
- The package lands in `.scratch/quest-dawn/package` with an `aurora-dawn.json`
  recording the revision, the patch files' hash, the NDK and the flags. A later
  run with the same inputs reuses it and takes no time. That matters: the game
  kit fingerprints the Dawn archive, so a rebuilt one would make every player
  rebuild their game. The first build compiles all of Dawn and Tint and takes a
  while; a patch change re-extracts `src/` and rebuilds Dawn's own sources
  only, keeping the fetched `third_party/`.
- `AuroraDawnProvider.cmake` reads that manifest. Only when `AuroraFdmAbi` is
  present does `aurora_core` compile against the ABI (`AURORA_DAWN_FDM`), so
  `Build-Quest.ps1 -StockDawn` still builds, with foveation unavailable.

Aurora (`lib/webgpu/fdm.cpp`, `lib/gfx/foveation.hpp`) builds one map per eye,
32 pixels per texel (42x44 for 1344x1408 eyes). The map is centred on that eye's
forward direction and rebuilt when the eye's size, field of view or level
changes. It is bound to a second view of the eye texture that only a
single-render-pass immersive eye renders through (every eye, by default). Density
bytes are 255, 127 and 63: a fragment covers 1/density pixels rounded down to a
supported size, so a half written as 128 could round back to one pixel.
Changing the level is live. The launch decides whether the device has density
maps at all, because every pipeline carries the flag and Dawn's pipeline cache
keys on it: the first launch with foveation on recompiles every pipeline once.

Measured on the Quest 3 on 2026-09-24. The game was launched with `foveation = "medium"` and
`debug.wiicompiled.fpslog 1`, then taken by injected presses to Luigi Circuit's Grand Prix
start, first-person cockpit, player idle. Settings were switched through temporary debug
properties (removed since the choice was made), interleaved in 11 s windows
over three or four rounds. Each 5 s `GPU ms/frame` line was assigned to the setting active for
all of it, using the runtime's own switch log lines. The GPU stayed at clock level 3 (492 MHz)
at 0.8, and 492 to 525 MHz at 1.3, the unfoveated windows running at the higher clocks.

- `render_scale` 0.8 (1344x1408): eyes 5.82 ms with one render pass per recorded pass
  (the race's 4 EFB passes), 5.11 ms merged into one (-12%). Low, Medium and High gave 5.09,
  5.36 and 5.11 ms: no change beyond the spread between windows (up to ±0.4 ms). Compositor app GPU
  time went from 8.31 to 7.66 ms with the merge, and the headset held 60 FPS throughout.
- `render_scale` 1.3 (2184x2288): 6.32 ms split, 5.81 ms merged (-8%); Low, Medium and High
  gave 5.33, 5.00 and 4.56 ms (-8, -14 and -22%), each window within ±0.2 ms of its
  setting's mean. The compositor showed about 35 FPS at every setting: that scale is too heavy
  for other reasons, the copies of such large eyes among them.
- A compositor screenshot (`TAKE_SCREENSHOT`, headset still on a desk) at High shows 4x4 pixel
  blocks on the kart body at the bottom of the view, and smooth shading with foveation off.

- Retro Rewind's SNES Ghost Valley 2 at `render_scale` 1.0 (1680x1760), paused at the
  countdown, GPU-bound at clock 640 MHz. That session ran without `fpslog`, so these are the
  compositor's figures per setting (headset FPS, app GPU time, GPU busy). One render pass per
  recorded pass: 38.9 FPS, 23.3 ms, 97%. Merged: 41.5 FPS, 21.7 ms, 96%. Low, Medium and High:
  40.3, 39.4 and 39.6 FPS at 22.2, 22.4 and 22.0 ms, with the GPU busy falling to 95, 94 and 94%.
  The merge helps here too, and no foveation level raised the frame rate.

So an eye's time on these tracks is mostly geometry and full-resolution tile stores, which a
density map does not reduce (the stores stay full size for non-subsampled images). Foveation
pays only when `render_scale` makes the eyes pixel-bound, as at 1.3 on Luigi Circuit; it
defaults to `medium` anyway, so it is already on when the render scale is raised and can be
changed without a restart.

### Quest 1 renderer compatibility

Quest 1 identifies itself as Android device `monterey` and uses an Adreno 540
driver that misrenders Aurora's general filtered EFB-copy shader. On that
device, `GXCopyDisp` keeps the destination equal to the resolved EFB region so
WebGPU can use `CopyTextureToTexture`; the later presentation pass still scales
the result. Live RGB5A3 menu copies use a minimal RGBA passthrough shader for
the same reason. Other Android devices retain the normal filtered and
format-converting paths.

Quest 1 must be built with the `quest1` flavour (`Build-Quest.ps1 -Headset
quest1`). It targets the Snapdragon 835's Kryo CPU; the modern flavour targets
`cortex-a77`, whose instructions can terminate the game with `SIGILL` on Quest
1. Each variant's exported game kit records this CPU target, and the APK build,
PC game build, on-headset build and package import all verify it. This prevents
a kit left by another flavour from producing a library that can crash with
`SIGILL`.

The final Quest 1 firmware also cannot reliably promote this app's 2D setup
panel into an immersive activity. Its APK therefore exposes two library
entries: **WiiCompiled VR** starts the game directly in VR, while
**WiiCompiled Settings** opens the setup/settings panel. After changing a
setting, close the panel and start WiiCompiled VR from the library. The Play
button in the settings panel explains this limitation instead of opening a
black panel. Modern Quest builds keep the normal single launcher, whose Play
button starts VR directly.

The workaround was isolated on a physical Quest 1: it changed the boot/menu
output from flickering black or white frames to a complete menu with character
and vehicle previews. It does intentionally skip Wii-era RGB5A3 quantization on
that device.

Pipeline compilation is also scheduled differently on Quest 1. Its Adreno
serializes much of `vkCreateGraphicsPipelines`, so a large equal-priority worker
pool starved the translated game and OpenXR pacing threads without shortening
the compile materially. Quest 1 uses two nice-level 5 workers and does not
prewarm cached recipes in the background; a cached recipe is promoted and its
workers are awakened when the game first requests it. Newer Android headsets
and desktop retain the normal worker pool and prewarm behavior.

### Controllers

Quest Touch controllers are not HID gamepads, so `openxr_input.cpp` syncs an
OpenXR action set on the pacing thread and feeds a virtual SDL joystick
(`SDL_AttachVirtualJoystick`, type gamepad) that Aurora assigns to player 1.
By default (`[vr] controller_mode = "wii_remote"`) that port is then served
through KPAD as a Wii Remote with a Nunchuk, with motion and an IR pointer
aimed at the virtual screen; see "Controllers" in `OPENXR.md` for the mapping
and the geometry. With `controller_mode = "gamepad"` it stays an ordinary pad:
A/B → South/East, X/Y → West/North, index triggers → trigger axes, grips →
shoulders, thumbsticks → sticks (clicks → stick buttons), left menu → Start,
and every existing binding, dead zone and overlay setting applies. With
`controller_mode = "none"` the virtual joystick is unplugged, leaving the ports
to a Bluetooth gamepad; left Y still opens the settings panel. Bindings are
suggested for `oculus/touch_controller` and `khr/simple_controller`.

The manifest declares hand tracking (`horizonos.permission.HAND_TRACKING`, the
deprecated `com.oculus.permission.HAND_TRACKING` for older builds, and
`oculus.software.handtracking` as optional). Both permissions are `normal` on a
Quest 3 (`adb shell pm list permissions -g -f`), so there is no prompt. Without
the feature flag Horizon OS keeps the app controllers-only: the game process logs
`setting hand mode control settings to ControllersOnly` / `sethandmanifest 0`,
and putting the controllers down logs `going to controller mode because hands are
disabled by manifest flag`; with it, `Is hands or controller` /
`sethandmanifest 2`. Bare hands then drive `khr/simple_controller` (a pinch is
select, the left palm-up pinch the menu), which the runtime ignores apart from
the menu gesture unless `[vr] hand_tracking` is on; see "Tracked hands" in
`OPENXR.md`. The Quest 3 runtime (`libvrapiimpl.so` in the `com.meta.xr` APEX's
VrDriver.apk) implements `XR_EXT_hand_tracking_data_source`,
`XR_FB_hand_tracking_aim`, `XR_EXT_hand_interaction`, microgestures and the
wide-motion modes; Meta's manifest filter skips the wide-motion modes unless the
app also declares `com.oculus.software.body_tracking`.

### Android platform glue

- `runtime/src/vr/openxr_android.cpp`: `xrInitializeLoaderKHR` with the
  JavaVM and activity SDL already holds, the `XrInstanceCreateInfoAndroidKHR`
  chain (`OpenXRConfig::instance_create_next`), and the `XR_KHR_android_thread_settings`
  hints: the game thread (SDL's main thread, which carries the guest fibers) as
  application main, Aurora's frame worker as renderer main and the pacing
  thread as renderer worker, so the runtime keeps the two busy threads on the
  fast cores. The log says which hints the runtime accepted.
- `runtime/src/platform/host_platform.cpp` / `runtime_config.h`: the activity
  exports `MKW_ANDROID_DATA_DIR` (external files dir, user reachable) and
  `MKW_ANDROID_RESOURCES_DIR` (unpacked `wii_bootstrap/`, `dsp_coef.bin`,
  `initial_pipeline_cache.db`); the latter stands in for the executable
  directory so the existing adjacent-file lookups work unchanged.
- `main.cpp` includes `SDL_main.h` on Android so `SDLActivity` finds
  `SDL_main` in `libmain.so`, and passes the resources path to Aurora.
- Fibers use the vendored libco AArch64 backend (Bionic is Linux), guest memory
  uses the Linux `mmap` path, the MPRIS media monitor is compiled out.
- **Surface readiness.** Aurora presents only while `g_surfaceReady` is set, and
  on Android that flag starts false. Stock SDL3 exports neither its activity
  mutex (`Android_LockActivityMutex`) nor a readiness hook, so the app's
  `QuestSurface` subclass brackets SDL's `surfaceChanged`/`surfaceDestroyed`
  with `aurora_android_begin/end_surface_mutation` (`aurora/android.h`), and
  Aurora's `SurfaceLock` owns its own recursive mutex. This is KartPad's design.
- **No full-display mirror.** SDL sizes the app's Android surface to the whole
  display (4128x2208 on a Quest 3), which nobody sees while OpenXR drives the
  headset. `QuestSurface` pins the surface buffer to 1280x720. That size also
  sets Aurora's presentation snapshot, which is the image the menu virtual
  screen shows in each eye, where it spans about 900 pixels. While a stereo
  provider is registered on Android, Aurora skips the surface present and the
  desktop mirror copy (`headset_owns_display` in `lib/aurora.cpp`). The game's
  own render size is unaffected: at `resolution_multiplier = 1` it is 640x528.
  Since 2026-09-19 an immersive race also stops that native render after the
  last pass whose EFB copy the eye replays sample (`last_pass_feeding_replay`):
  the main scene and display copy of a 1280x720 image nobody sees were 4 to
  6 ms of a 12 ms GPU frame on a Quest 3. A frame capture still renders the
  whole image, and menus (the virtual screen) keep it because their eyes are
  built from that snapshot. The PC takes the same cut whenever its desktop
  window is not showing that image (Desktop view None or an eye).
- **JNI only on the real thread stack.** Guest threads run on libco stacks
  inside the SDL thread, and SDL's Android event pump can reach Java (joystick
  polling, HIDAPI). ART binds JNI transitions to the thread's real stack, so
  Aurora's `pump_events` is a no-op on Android and `UpdateAuroraAndProcessEvents`
  defers a poll made from a guest fiber until control is back on the scheduler
  context (`GuestFiberManager::IsOnSchedulerFiber`). The default guest thread
  shares the scheduler context, so most polls run immediately. Also KartPad's
  finding, from device crashes.
- Time conversion for frame interpolation uses `XR_KHR_convert_timespec_time`
  (CLOCK_MONOTONIC, the clock behind `steady_clock` on Bionic).
- **Passthrough around the menus** (`[vr] passthrough`, default on, live):
  `runtime/src/vr/openxr_passthrough.cpp` owns one `XR_FB_passthrough`
  reconstruction layer, the PPSSPP VR design. The Vulkan backend starts it
  (created on first use) or pauses it as each presentation arrives, and submits
  it first, under the virtual screen's quad, or alone while there is no image
  yet (startup, a recenter). An immersive race never submits it and pauses the
  cameras; a `flat_screen` race is a virtual screen, so it keeps the room (as
  does a race's intro under `flat_race_intro`, until the countdown), and
  so does an `immersive_window` race, whose projection layer is submitted over
  it with `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` (Aurora leaves
  each eye transparent outside the window; see OPENXR.md, "The immersive window").
  The quad is cropped to the snapshot Aurora letterboxes into the
  nearly square eye image (`OpenXRVirtualScreenContentRect`), or its black
  bands would frame the picture against the room. The manifest's `com.oculus.feature.PASSTHROUGH` is what lets Horizon
  OS composite it; DolphinXR found that without it every call succeeds and the
  layer stays empty. The log says `OpenXR passthrough started`, `paused` and
  `resumed`; when it runs, logcat also shows `Starting camera streams for
  purpose: passthrough` and `is_displaying_passthrough_content` going to true.
  ClientMgrFocus logs `[App Enabled for PT: 0]` at every launch, flag or not
  (it is about the launch transition), so it proves nothing. Checked on a
  Quest 3 on 2026-09-22: the room shows around the title screen.

### Launcher and game process

The app opens on `LauncherActivity` (`android/app/src/main/java/org/wiicompiled/quest/launcher/`),
a 2D Horizon OS panel modelled on the PC launcher, WheelWizard VR, and using its
palette. **Home** has the Play button and reports a missing or incomplete `DATA`
(the check is the runtime's own `IsDvdDataRoot`: `files/` and `sys/fst.bin`).
**Settings** edits `Config.toml` in tabs: VR (render scale, performance level, foveation, VR
interpolation, refresh rate, virtual screen size and distance, passthrough), Camera (race view:
immersive, immersive window or flat screen, race intro, lean back, default camera, Bullet Bill
camera, rotation, driver hiding, seat, character body and hands only, hand steering, held items,
tracked hands), as the in-headset panel splits them,
Graphics (resolution, widescreen, bloom, shader stutter), Controls (controller
mode, vibration, the Wii Remote mapping), Audio, Other (game data, builds, paths, OpenXR
logging) and About. The launch-time geometry (`hud_distance_meters`, `hud_width_meters`)
is only reachable here, not from the in-headset panel; `render_scale` is also the
panel's live **VR → Render resolution** slider (OPENXR.md).

**Patches**, between the two, is the PC launcher's mods page (`PatchesPage`,
`ModLibrary`). Import takes one or more picked files, asks for a name and makes
them one mod under `WiiCompiledOpenXRVR/Mods/<name>/` with the PC's
`<name>.ini` (Name, Author, ModID, IsEnabled, Priority), so a `Mods` folder
copied from WheelWizard reads the same. Unlike the PC's Import, a picked
`.zip`, `.7z` or `.rar` is unpacked into the mod rather than stored in it.
Browse opens the mod browser in place of the list (`ModBrowser`, `GameBanana`,
`ModInstaller`), WheelWizard's `ModBrowserWindow` and `ModContent`: GameBanana's
Mario Kart Wii mods (game 5896) through the same two public API calls, a name
search a page at a time as the list scrolls and a mod's profile page. An empty
search lists "Mod" ("Patches" with Patches only on), mods tagged `Patches` come
first or alone, and rated mods are left out, all as on the PC. Download and
Install asks for the mod's name first, then downloads into the app's cache,
checks the file against the MD5 (or size) GameBanana lists, and installs it
like an Import with the mod's author and GameBanana id, which is how the
browser shows it as Installed with Uninstall; View Mod in a row's menu opens it
again. A mod with several files asks which one (the PC takes the first, often
an older version). Only one install runs at a time, sharing Import's slot; the
download has its own thread, so Play never waits behind it. Each mod can be
switched off, moved up or down, renamed or deleted. As on the PC, mods only
change Retro Rewind: its Play first
flattens the enabled mods into `RetroRewind6/Patches` exactly like
`ModsLaunchService.PrepareModsForLaunch` (the top of the list wins a file both
carry, `<name>.<tag>.szs` archives take their mod's priority as a prefix, and
loose files no mod provides are removed). With no mod enabled, a Patches folder
that still holds files is only cleared if the player says so. The pack's
Riivolution XML maps that folder onto `/patches` and `/sound`. `ModLibraryTest`
covers the rules.

**My profiles**, between Home and Patches, is the PC's UserProfilePage
(`ProfilesPage`, `ProfileStore`, `RksysProfiles`, `RetroWfc`), with the PC's sidebar
profile card above General (`SidebarProfileCard`). Like the PC it reads Retro Rewind's
save only (`riivolution/save/RetroWFC/RMCP/rksys.dat`): the four licences with their
name, friend code (derived from the profile ID as `FriendCodeGenerator` does), VR, BR
and race counts, the VR and BR replaced by Pulsar's `RRRating.pul` in the NAND when it
knows the profile, as the PC's `RRratingReader` does. Retro WFC's public API gives the
rest: a licence is Online (card glow) while its friend code is in a room of the Rooms
page's live rooms (below), its VR history comes from `/api/leaderboard/player/<fc>/history?days=N`, and
`/api/leaderboard/player/<fc>` gives the Mii it last played with, as the 74-byte
`miiData` and a 64-pixel PNG, kept on disk. The Mii is drawn as the PC draws it,
turned three-quarters (`CurrentUserSideProfile`) by My Miis' renderer (below): the
Mii of the headset's Mii database with the licence's avatar ID (RKPD `+0x28`), as the
PC looks it up, so a Mii made in My Miis shows once a licence takes it; else Retro
WFC's `miiData` while its ID is still the licence's. It is 320 dp on the page, and
80 dp in the sidebar, where, as on the PC, it stands on the card's bottom edge and
rises out of it. Until the Mii parts are downloaded Retro WFC's PNG stands in, and
a licence with neither shows a silhouette. Badges come from WheelWizard's
`badges.json`. It only reads, without the PC's Rename and change Mii, and the PC's
region picker is gone since the Quest runs PAL only. The card and the
carousel sit side by side on the wide panel. `RksysProfilesTest` and `RetroWfcTest`
cover the parsing.

**My Miis**, after Patches, is the PC's MiiListPage with its Mii editor
(`MiisPage`, `MiiEditor`). It lists and changes the Wii's Mii database in the
game's own NAND, `shared2/menu/FaceLib/RFL_DB.dat` (`MiiDatabase`, the PC's
`MiiRepositoryService` and `MiiDbService`), which the game reads, so a Mii made here
can be picked for a new licence. The Quest's NAND starts without one, so the page
creates it empty the first time, as the PC does, with one difference: a database
the Wii formatted links its 10,000 hidden-Mii entries to nothing (`0x7FFF`), and
the PC leaves those links zero, so the headset writes the Wii's (checked byte for
byte against Dolphin's database). A Mii is the 74-byte block of the PC's
`MiiSerializer` (`MiiData`). A Mii made or duplicated here gets a new ID and this
console's system ID, from the MAC address the runtime derives from `setting.txt`'s
serial (`RuntimeConsoleIdentity::FromSerial`), so making one needs the game to have
started once. An imported `.mii` gets the PC's import address and, like any Mii
from another console, the PC's globe. The PC selects more Miis with Shift or Ctrl;
here a long press adds or removes one. Export saves one `.mii` through the save
dialog, several into a chosen folder. Changes are refused while the game runs.

The pictures come from a Kotlin port of the PC's software renderer (`MiiRenderer`,
`FflResource`, `MiiBodies`), framed as the PC's `face` pictures: the head, and below
it the upper body in the Mii's favourite colour, sized by its height and build. It
draws from FFL's Mii parts (`FFLResHigh.dat`) and the 3DS body models the PC carries
in its assembly (`mii_static_body_3ds_{male,female}_LE.rmdl`), all Nintendo's and
never in the APK: the page downloads the parts once from the Internet Archive's copy
of Miitomo's `AFLResHigh_2_3.dat` (a 4.4 MB zip), as the PC does, and the bodies
(29 KB) from WheelWizard's repository at the commit that added them, and checks each
against its SHA-256 (`MiiRenderResource`). A headset with the parts but not the
bodies draws heads alone, and the page offers the bodies. Compared with the PC's C#
renderer on 257 Miis covering every part and colour, 283 of 299 head pictures were
identical and the rest differed by one colour level in at most four pixels; with
bodies, 62 of 66 front pictures were identical and the rest one pixel apart, and the
three-quarter ones differed in at most 32 edge pixels of 160,000. One deliberate
fix: the PC colours a beard with the hair colour, here with the facial hair colour,
as the Wii does. The editor's choices are drawn from the same parts, the flat parts
from their textures in the Mii's colours and hairstyles, head shapes and beards as
the Mii's head without its body, where the PC shows icons of its own. On a Quest 3
the 424-pixel face in the editor takes about 150 ms, drawn in four bands of rows in
parallel, and a choice about 40 ms. `MiiDataTest`, `MiiDatabaseTest`, `MiiIdsTest`,
`MiiBodiesTest` and `MiiRendererTest` (on made-up parts and body files) cover it.

**Rooms**, under Online after Settings, is the PC's RoomsPage and RoomDetailsPage
(`RoomsPage`), fed by `LiveRooms`, the PC's RRLiveRooms: while the launcher is on
screen it asks Retro WFC every 40 s for `/api/roomstatus` and the top 50 of
`/api/leaderboard/top/50` (`Leaderboard`, the PC's RrLeaderboardSingletonService:
kept 90 s for this page and the Leaderboard alike, the last ones reused when it
fails), splits
the rooms Retro WFC merged by mistake where their players' connection maps are not
linked both ways (the PC's SplitMergedRooms, checked against it on a real answer),
and marks top-50 players with their place. The same answers drive the sidebar's
player count and the profiles' Online glow. The room status is read as Retro WFC
sends it now (`isSuspended`, a Mii object of `data` and `name`) as well as the PC's
older fields. The page lists rooms with their game mode (the PC's `rk` names), ID,
time online and player count, or with a search the players whose name or friend
code holds it; a room opens its details (ID, time online, mode, average VR, and its
players with their Mii, VR, badges, place and open host), and a player there Copy
Friend Code, View Mii, Add Friend and View Profile (`PlayerActions`, shared with the
Leaderboard and Friends). View Mii is the PC's MiiCarouselWindow: the
whole Mii in the renderer's `all_body` view, turned by dragging and zoomed with the
thumbstick, small while it moves and sharp once it rests. View Profile is the PC's
PlayerProfileWindow, with the VR history of My profiles (`VrHistoryPanel`, one
`view_vr_history` layout for both). Add Friend is the Friends page's (below). Rows
are `TapRow`s, which take every
tap so a tooltip or the badge strip inside cannot swallow one, while the pointer's
hover still shows their tips; only a button inside a row keeps the taps that begin
on it. `LiveRoomsTest` and `RetroWfcTest` cover it.

**Leaderboard**, after Rooms, is the PC's LeaderboardPage (`LeaderboardPage`): the
top 50 from that shared cache, asked again each time the page is shown as the PC
makes the page anew, ordered by rank (else the active rank, else the place in the
answer, as ResolveRank), a blank name shown as Unknown Player. The first three stand
on the PC's podium (`PodiumCard`, LeaderboardPodiumCard): second, first and third,
each card in its medal's colours with the rank, the place, the Mii, the first badge,
the name, the VR and a mark on suspicious players, and the PC's animations, which
move only view properties: the cards rise in one after another, then float, their
glow breathes, a glint sweeps across and three sparkles rise, and a card grows a
little under the pointer; they stop while the page is hidden. Unlike the PC, the card
clips its glow and glint to its rounded box, and the glint is stretched to the taller
cards' height and swept from beyond one side to beyond the other so it crosses all of
the wider cards; only the sparkles sit outside. The rest are
PlayerListItem rows with their place in grey. A tap on a player opens the same
actions as in a room; a row whose player is in a room now also has View Room, which
opens that room on the Rooms page (the PC's JoinRoom, matching friend codes by their
digits) and follows the rooms as they change. Loading, the error with Retry and the
empty board with Refresh are the PC's too. `LeaderboardTest` and `RetroWfcTest`
cover it.

**Friends**, after Leaderboard, is the PC's FriendsPage (`FriendsPage`), and the
only page besides My Miis that changes the game's files: the friend list of the
licence the sidebar shows (the primary one, else the first) in Retro Rewind's save.
`RksysFriends` reads and changes it as the PC's GameLicenseService does: 30 slots of
0x1C0 bytes at 0x56D0 in each RKPD block, and 12 bytes each at 0x8B50 for the Wii's
friend registration; a friend is pending while the slot's state is 1 and that
registration's control byte 0x10 (or 0). Add Friend writes the PC's one-sided request
(the friend code's upper half and the profile ID as its key, state 1, the Mii and its
CRC-16/XMODEM, VR clamped to 65535, BR 5000, country and region 0xFF, the ten records
0xFFFFFFFF, control 0x10) into the first empty slot, and Remove Friend empties the
slot; both write the save's CRC-32 over its first 0x27FFC bytes again. `FriendList`
reads the whole save, changes it in memory and replaces the file with a finished,
synced copy, then reads the list again; the save is also read each time the
launcher comes back, as the game may have changed it, and nothing is changed while
the game runs (`FriendActions`). The cards are the PC's FriendsListItem at its sizes:
the Mii turned sideways (FriendsSideProfile; FriendsSideProfilePending's angry face,
FFL expression 2, for a pending friend) in a grey strip fading over Online, Offline
or Pending, name and friend code, VR and BR (9999 shown as 9999+), badges, wins and
losses, and View Room, enabled while the friend is in a room, which opens it on the
Rooms page; an online friend's card has the green outline and glow, a pending one's
the yellow. A NUL inside a Mii's name is left out when shown, as Avalonia draws it.
The list sorts as the PC's (Is Online, VR, BR, Name, Wins, Races played, for the
session), except that names go from A to Z rather than Z to A. Add Friend on the page
asks for a friend code with the PC's checks (12 digits, a valid checksum, not your
own, a warning for a friend already listed), looks it up on Retro WFC for the name
and Mii, and confirms with the PC's AddFriendConfirmationWindow; Add Friend from a
room or the leaderboard uses the Mii and VR shown there. Unlike the PC, Remove Friend
asks first. The sidebar shows friends online out of all of them. `RksysFriendsTest`
covers the save, on a made-up save; on the headset an add then a remove left the
save byte for byte as it was.

The sidebar ends as the PC's does (`SidebarFooter`): the Wheel Wizard team's status,
the info menu, Settings and the version. The status is WheelWizard-Data's
`status.json` (`WheelWizardStatus`, the PC's WhWzStatusManager), asked every 90 s
while the launcher is on screen and shown as an icon whose tip is its message: the
PC's preset icon and colour for each variant, or an icon of its own as SVG path data
in its colour (`SvgPath`, every path command, arcs as cubic curves), nothing for
None, and the PC's red error when the status cannot be read. The info menu names who
made the Quest port and Wheel Wizard, opens Settings on About, and links to Wheel
Wizard's Discord, this repository on GitHub and Wheel Wizard's Ko-fi.
`SidebarStatusTest` covers the status and the path data.

The launcher reads values by the runtime's rules. `TomlConfig` edits one line
the way `RuntimeConfigFile::WriteSetting` did before upstream v0.2.34 made it
re-emit the whole document through toml11 (keys, order and attached comments
kept, layout possibly not), and every edit re-reads the file, so values the
in-headset panel wrote are kept. Each row reads its key with the
runtime's default and accepted range. `TomlConfigTest` (`gradlew
:app:testBaseDebugUnitTest`) covers the editor.

`QuestActivity`, the immersive game, is started from Play, like DolphinXR's
`EmulationActivity`: `com.oculus.intent.category.VR` without `LAUNCHER`. It runs
in its own `:game` process, and its `onDestroy` ends that process. This is
required, not tidiness:

- SDLActivity calls `System.exit(0)` when an activity is created again after
  `SDL_main` has returned. In one shared process, the second Play would kill
  the launcher along with the game.
- Guest memory, fibers, static configuration and the OpenXR Vulkan device all
  belong to the process. DolphinXR crashed when it opened a second Vulkan VR
  session in the same process.

Every session therefore starts in a fresh process, and the launcher loads no
game code. While the `:game` process is alive, Home shows *Resume* and Settings
warns that changes wait for a restart. `adb shell am start -n
org.wiicompiled.quest/.QuestActivity` still starts the game directly, which is
what `Run-Quest.ps1` does.

### Extracting the player's disc image

Without `DATA`, Home's main button is **Select disc image** (About has the same
action for replacing `DATA`). The player picks their own image with Android's
document picker, and the launcher extracts it the way the PC installer does
(`Launcher/WiiCompiled.Setup.Windows/InstallerEngine.cs`). Both use nod
v2.0.0-alpha.10, so both write the same layout:

1. The file name must be a format nod reads: ISO, GCM, GCZ, CISO, WBFS, WIA or RVZ.
2. The header must be the pinned game ID, and `main.dol` and `StaticR.rel`,
   read straight from the image, must match the pinned SHA-256s. Gradle
   reads all three pins from `projects/mkwii/recomp.yml` into `BuildConfig`, so
   a wrong disc fails in seconds, before anything is written.
3. The data partition is extracted into `DATA.extracting` next to `DATA`, after a
   free-space check.
4. The extracted files are checked against the same pins, as
   `ValidateExtractedGame` does. Only then is an existing `DATA` renamed away,
   the new one moved in, and the old one deleted. A failed, cancelled or killed
   run never costs a working `DATA`.

nod is the one piece of native code the launcher loads: `android/nod-jni` is
a Rust `cdylib` with three JNI calls for discs (header, read one file,
extract). The extraction is nodtool's `extract` command, plus progress
reporting and cancellation. It reads the picker's file descriptor with
`pread`, so nod's preloader threads each hold their own clone. Wii partition
decryption needs the common key, and that key lives in the nod crate fetched at
build time, not in this repository. The PC installer is the same way: it
downloads nodtool.

A fourth call (`ModArchive.extract`, `src/archive.rs`) unpacks the `.7z` and
`.rar` mods GameBanana serves, which WheelWizard opens with SharpCompress and
Android cannot open at all: sevenz-rust2 (LZMA, LZMA2, PPMd, BZip2, Deflate,
BCJ/BCJ2; no AES, since there is no password to give) and rars (RAR 1.5 to 4.x
and RAR5), both pure Rust and both checking every file's CRC or hash. It
writes regular files only, refuses a name that climbs out with `..` as the
zip reader does, and tells the formats apart by signature, not by name. On the
PC it was checked against 7-Zip on nine real mods (three 7z, three RAR4, three
RAR5, byte-identical) and on flipped and truncated copies, which all fail. Its
path and signature rules have unit tests in `archive.rs`.

`GameSetupService` runs the job as a `dataSync` foreground service with a
partial wake lock. A multi-minute extraction then survives the panel being
closed and the headset being taken off. `DiscChecksTest` covers the acceptance
rules and their messages.

### No game code in the APK: the game kit

Like the PC installer, which compiles the translated game on the player's machine, the base
APK contains no translated Mario Kart code. It carries a **game kit** (`assets/game_kit`, about
105 MB before compression). The kit is everything `libmain.so` links except the game: the
runtime, aurora, Dawn and the other dependencies, prebuilt and stripped, plus `kit.json`, the
recipe that compiles and links a player's own translation against them. The player's
`libmain.so` is built from their disc and loaded from internal private storage (`GameLibrary`),
the only place Android lets an app load native code it did not install.

The kit is exported from CMake's own build graph, so its flags cannot drift from a normal build:

- `runtime/cmake/PublicProducts.cmake` defines `mkw_quest_kit_probe` on Android: WiiCompiled
  `WITHOUT_GAME` (no base shards, no registration objects, and the `mkw_game_data` objects
  `$<FILTER>`ed of the disc-generated sources: `data_sections_init` with its blob assembly and
  `guest_symbol_table`; the game-independent `mkw_runtime_common` objects and the per-build glue in
  `runtime/game/` stay), linked with the game's symbols unresolved. The app builds this probe
  instead of the product.
- `android/QuestGameKit.psm1` (`Export-QuestGameKit`, run by the `export*QuestGameKit`
  Gradle tasks) turns the probe's ninja link edge into `kit.json`'s ordered link inputs. It adds
  `{game:runtime}`, `{game:product}` and `{game:translated}` markers where WiiCompiled had those
  objects, and translated shards link inside `--start-lib/--end-lib` with archive semantics as
  `libmkw_base_shared.a` did. It takes the compile flags CMake recorded for one source of each
  generated kind. The fingerprint hashes the recipe and every file.
- `Invoke-QuestGameBuild` replays the recipe with ninja. On the development PC, a library built
  this way had the same 61,975 defined and 785 undefined dynamic symbols, 29,995 translated
  functions, `NEEDED` list and soname as the CMake-built one, in 2.4 minutes.

`Build-Quest.ps1` refuses an APK that contains any `libmain*.so` or lacks the kit, and packaging
excludes `**/libmain*.so` and `**/libmkw_quest_kit_probe*.so`, since AGP packages every library
left in the CMake output directory. The `func_8…` symbols the kit's runtime objects define are
hand-written HLE overrides (`PPC_NATIVE_OVERRIDE_*` in `hle_stubs.h`), not translated code.

**Retro Rewind rides in the same app, on its own kit** (`mkw_quest_kit_probe_retro`, RetroRewind
without any translated code), so the APK ships neither game. The mod needs one more link slot than
the base game, because the modded product links more kinds of translated code: `{game:runtime}`
(the disc-generated sources), `{game:product}` (the mod's registration and dispatch shards),
`{game:mod}` (the mod's own shards, the profile-sensitive base shards it replaces, and the mod's
data patches) and `{game:translated}` (the shared base shards, with the archive semantics
`libmkw_base_shared.a` has). Rather than teach each builder which game it is building, `kit.json`
carries a `sources` map naming the `shards.cmake` list behind each slot, and the builders just
follow it.

One APK, one kit: `kit.json` (schema 3) holds a `products` map with a `base` entry and, when the
translation includes the mod, a `retro_rewind` entry, each with its own compile flags, link line,
slots and `fingerprint`; `runtimeIncludeFingerprint` and the kit's own `fingerprint` stay at the
top level. Everything that builds a game names the product it wants: `Build-QuestGame.ps1
-Product base|retro_rewind`, `Setup --quest-product`, and on the headset `GameProfile`. Each game
lives in its own directory under `files/game/<profile>`, so both can be installed at once and the
launcher's toggle switches between them; the selected one is remembered in `filesDir/selected-game`
(a plain file, because the launcher and the `:game` process do not share preferences).

Retro Rewind also needs its 2 GB pack on the headset. The app writes `[paths] retro_rewind_root`
into `Config.toml`, and the pack arrives either way a PC player gets it:

- **Download Retro Rewind** (`RetroRewindPack`, a `GameSetup` task) fetches it from Retro Rewind's
  own distribution server, exactly as WheelWizard does on a PC. `RetroRewindInstall.txt` names the
  full install zip, `RetroRewindVersion.txt` lists `<version> <url> <path> <description>` per
  published update and `RetroRewindDelete.txt` lists `<version> <path>` deletions; an installation
  is the base zip plus every update newer than the `version.txt` it holds. Only entries under
  `RetroRewind6/` are kept (the Riivolution XML beside them belongs to a Wii setup), a base install
  is staged and swapped like every other task, and the version is written after the last update, so
  an interrupted update simply runs again. Nothing of the pack ships in the APK.
- Or a `.wcgame` carries it (below), for a headset with no Wi‑Fi to spare.

Home's main button becomes **Download Retro Rewind** whenever that game is selected and its pack is
missing, and Settings → About shows the installed version with an Update button. Building the mod on
the headset needs the mod's `Code.pul`, which is part of the pack, so the same rule covers it.

A pack update needs no new app: the runtime and the kit do not depend on the pack. But Retro
Rewind's code is translated from the pack's `Code.pul`, so an update that changes it needs the game
built again. `RetroRewindUpdates` keeps track of both:

- Each time the launcher comes to the front with the pack installed, it reads
  `RetroRewindVersion.txt` in the background, at most every 4 hours (30 minutes after a failed
  read). A newer version is offered in a dialog once per published version. Until it is installed,
  Home shows an info banner with an **Update Retro Rewind** second button, and Settings → About
  shows `Version x (y available)`.
- `game.json` records `modCodePulSha256`, the Code.pul the game was translated from. The headset
  build writes it. On a PC, `translate-mod` writes `mod_source.json` next to its output, which
  `Build-QuestGame.ps1` reads through `Get-QuestModCodePulSha256`. When it differs from the
  installed pack's Code.pul, `GameLibrary` reports **PackChanged**. Home then leads with **Build on
  this Quest**, adds **Play anyway** (the old game still loads, but may not match the new pack and
  online play may refuse it) and shows a warning banner. A dialog offers the build once per
  Code.pul. Right after an update, a game whose `game.json` predates the key gets the same offer,
  because it cannot be checked.

Online play (Retro Rewind WFC) needs the Retro-WFC payload translated into the mod, as on a PC:
`translate-mod --retro-wfc-payload`, with the payload Setup downloads and verifies from
`https://rwfc.net/api/wfc/payload?g=RMCPD00`. Without it the mod downloads `WWFC/Payload` while
connecting and jumps into code that was never translated, and the game stops with a missing
translated function (seen: `0x81895BF4`, called from `rr_kamek_*` on the `NHTTPi_CommThreadProc`
thread, with `r3` pointing at `"WWFC/Payload"`). So the headset
build downloads the payload before translating and checks it with `validate-retro-wfc-payload`, and
`Invoke-QuestGameBuild` refuses a Retro Rewind translation whose `mod_data_patches.cpp` has no
`kRetroWfcInitializerAddress`. The payload is fixed at build time: when rwfc.net publishes a new
one, rebuild the game.

### Game packages (.wcgame) and Import from computer

A `.wcgame` is a zip holding `game.json`, `libmain.so` and optionally `DATA/…` (the extracted
disc) and `MOD/…` (the RetroRewind6 pack), both written without compression. `game.json` records
the profile, game ID, `main.dol` and `StaticR.rel` pins, the kit fingerprint, the library's
SHA-256, whether the package carries game files and mod content, who built it and, for Retro
Rewind, `modCodePulSha256`.
`android/Build-QuestGame.ps1 -Product base|retro_rewind` builds one on a PC from the translator's
output and the kit the last APK build exported; `-Data` includes the game files and `-Mod
<RetroRewind6>` the pack. `-Install` pushes it into the app's `Import` folder. The launcher
creates that folder itself so it owns it, and imports the newest package the next time it opens,
once per package. An import selects the game it just installed.

Players get the same build from WheelWizard VR: Settings → WiiCompiled → Meta Quest → **Build**.
WheelWizard asks which Quest (Quest 2, 3, 3S and Pro, or the original Quest, whose app it fetches
from the installed release under its `Quest1` name), whether to include the game files, whether to
build Retro Rewind with its pack (off builds the base game; the pack only travels with the mod, so
one switch decides both) and where to save the package. The APK is downloaded from the GitHub
release the installation came from, or chosen by hand for an unpublished build; it then runs the
installed setup:

```
WiiCompiled-Setup.exe --build-quest --install-dir <install> --quest-apk <app.apk> --output <file.wcgame>
                      --quest-product base|retro_rewind [--include-game-files]
                      [--retro-dir <RetroRewind6> --include-mod-content] --progress-json
```

Setup extracts `assets/game_kit` from the APK into `<install>\QuestBuild\kit`, so the game always
matches the app it goes to. It then runs the installation's staged copy of `Build-QuestGame.ps1`
over its own `BuildWorkspace\generated`, `recomp.yml`, the toolkit's ninja and, with
`--include-game-files`, `GameAssets\DATA`. The Android compiler is not part of the toolkit: the
first build downloads Google's `android-ndk-r29-windows.zip` (834 MB, SHA-1 pinned in
`QuestBuildService.Ndk`, under the Android SDK License, so it is never redistributed). It keeps
only the ~230 MB that a build needs (clang, lld, clang's headers, the aarch64 runtime libraries and
sysroot) in `<install>\QuestBuild\android-ndk-29.0.14206865`, and deletes the archive.
`WIICOMPILED_QUEST_NDK_TOOLCHAIN` points it at an existing NDK LLVM directory instead. Besides
`progress` and the terminal `result`, the stream carries one
`{"type":"quest-package","path","kitFingerprint","includesGameFiles","sizeBytes"}` line. A setup
that supports all this says `"questBuild": true` in `--info-json`, and WheelWizard asks older ones
to update. The package is written as `<file>.partial` and renamed at the end; a failed or
cancelled build removes it. The kit's `runtimeIncludeFingerprint` must match the installation's
`runtime/include`, so an installation only builds for the Quest app from the same release.

`GamePackageImport` (Home's **Import from computer**, or the Import folder) stages the library
and any `DATA` next to their destinations. It accepts them only if `game.json` names this
profile, the pinned disc, this APK's kit fingerprint, and a library hash matching the bytes, and
if the game files pass the same checks as an extraction. A package built for another app version
is refused, and an installed game whose kit fingerprint no longer matches the APK shows as stale
on Home.

Home's main button is always the next step: **Select disc image** while there are no game files,
**Build on this Quest** once they are there and no game is installed (or the installed one is
stale), and **Play** once both are present. **Import from computer** sits beside the first two.
**Reset installation** leads instead when the game files are there but unusable, or when an
attempt to set them up failed and left none that work; a failed attempt over working files offers
it as the second button, and Settings → Other always has it. It removes the game files with any
unfinished extraction or import, and on request the built games with the on-device build
workspace and the Retro Rewind pack; Config.toml, the saves and the logs stay (`InstallReset`, a
`GameSetup` task like the others, with progress and cancel). Nothing that replaces files the game
reads, a reset included, starts while the game process is alive.

### Building the game on the headset

**Build on this Quest** (`GameBuild`, a `GameSetup` task in `GameSetupService`) does on the
headset what `Build-QuestGame.ps1` does on a PC, from `DATA`, in about 28 minutes on a Quest 3:

1. It unpacks `assets/quest_toolchain` and `assets/game_kit` into `files/build/`.
2. It downloads the NDK files a build needs (below) into `files/build/ndk`.
3. It translates the disc: `translate-recursive`, `generate-data-init --target-os android` and
   `emit-build-shards`, in a workspace made of the kit's `translation/` copy of `recomp.yml`,
   `MAP.txt` and `runtime/src`, plus `main.dol` and `StaticR.rel` from `DATA`. A translation of the
   same kit, toolchain and disc is reused.
4. It compiles the generated sources with the kit's flags, up to four at a time (fewer when the
   available memory allows less than 700 MB each). The blob assembly is compiled as plain
   `-x assembler`: preprocessing a `.S` makes clang start itself, which the linker trick below
   cannot do. Finished objects survive a cancelled or failed run.
5. It links with `kit.json`'s `link.lld` and installs `libmain.so` and `game.json` through the same
   staging swap as an import. `builtBy` says the headset built it. A successful build deletes
   `files/build`.

The log goes to `Logs/build_<stamp>.log` next to `DATA`.

The toolchain (`android/Prepare-QuestToolchain.ps1`, run by the `prepareBase*QuestToolchain`
Gradle tasks) is 249 MB unpacked. It travels as one deflated 80 MB `files.zip`, because asset
packaging never compresses `.so` files, next to `toolchain.json`, which lists every file. It holds:

- the translator published for `linux-bionic-arm64` from a copy of the sources retargeted to
  net10.0, which is the first .NET with those runtime packs. That runtime is Mono.
- `translator_host` (`android/toolchain/translator_host.c`).
- Termux's clang/lld 21.1.8 and the ten shared libraries they and the translator load, pinned by
  package SHA-256 and stored under the names their users load. OpenSSL is stored as `libssl.so`,
  the name .NET's shim opens on Android; Android's own BoringSSL lacks symbols it needs.
- `ndk.json`: the pin of the NDK files.

Android facts this design rests on, all measured on a Quest 3:

- An app cannot `exec` files in its private storage, but `/system/bin/linker64 <absolute path>`
  runs them (`PrivateCodeExecutionTest`). Every tool starts that way (`ToolProcess`), with
  `LD_LIBRARY_PATH` at the toolchain's `llvm/lib`.
- Under the linker, the .NET apphost reads `/proc/self/exe`, gets the linker, and cannot find the
  app. `translator_host` hands hostfxr the app directory instead. It also turns off bionic's heap
  pointer tagging, which crashes the runtime at startup.
- Termux's clang driver compiles with the same `cc1` arguments as the NDK's. Its link line is
  patched for Termux (`-rpath`, `-L/system/lib64`), and clang could not start lld anyway. So the kit
  export expands the link with the NDK's own driver (`clang++ -###`) into `link.lld`, a raw lld
  command with `{kit}`, `{ndk}`, `{output}` and `{game:*}` placeholders, and the headset runs
  `ld.lld` directly. The compile uses Termux's clang resource headers, which match that compiler.
- The NDK files come from Google's `android-ndk-r29-linux.zip`, not from the APK. `RemoteZip`
  reads the zip's central directory and then only the 3,499 wanted entries, with HTTP range
  requests: the aarch64 sysroot, `libc++_shared.so`, the API 29 stubs and CRT objects,
  compiler-rt builtins, `libunwind.a` and `libatomic.a`. That is about 8 MB compressed of 784 MB.
  Each file must match the SHA-256 in `ndk.json`, and the list must match the digest pinned in the
  script. The requests say `Accept-Encoding: identity`: Android's HTTP stack asks for gzip by
  default, and Google's server then serves a gzip-encoded zip whose byte ranges are not the file's
  (HTTP 416). The Linux zip is used because its sysroot holds headers whose names differ only in case
  (`xt_TCPMSS.h`, `xt_tcpmss.h`), which a Windows copy of the NDK loses.
- Translation peaks at 3.1 GB with Mono's default heap and 2.1 GB with
  `MONO_GC_PARAMS=soft-heap-limit=1200m` and four threads, for byte-identical output. The builder
  uses the latter.

`BuildRecipeTest` and `RemoteZipTest` cover the command lines and the zip reader.

### Build system

- `runtime/CMakeLists.txt` recognises `CMAKE_SYSTEM_NAME=Android` on arm64 as
  `MKW_PLATFORM_ANDROID`: OpenXR on by default, Dawn from the pinned
  android-aarch64 package (digest pinned in `AuroraDawnProvider.cmake`, which
  also rewrites the package's absolute `liblog.so` path and looks the package
  up with `NO_CMAKE_FIND_ROOT_PATH` so the NDK sysroot rule does not hide it),
  or, as `Build-Quest.ps1` passes it (`-PmkwQuestDawnDir`, turned into
  `FETCHCONTENT_SOURCE_DIR_DAWN_PREBUILT` by `android/app/src/main/cpp/CMakeLists.txt`),
  the same revision built with Aurora's patches (Foveated rendering, above),
  SDL3 built shared (or `-DAURORA_SDL3_PROVIDER=system` for the AAR prefab),
  tests off, products built as `libmain.so` / `libmain_retro_rewind.so`,
  `-mcpu=cortex-a77` (Quest 2's XR2 Gen 1; Quest 3/Pro are supersets).
  The products link with `-Wl,-Bsymbolic` and the translated shards compile
  with `-fno-stack-protector`: without them every one of the 29,000 translated
  functions called its neighbours and the runtime through a PLT stub (26,973
  of them, 702 after), which was 4% of the game thread on a Quest 3, and the
  NDK's default canaries cost cycles in code whose state lives in guest memory.
  With those and a larger indirect-dispatch memo (`kIndirectDispatchCacheEntries`),
  a twelve-kart race start went from a 30 fps retrace lock to a steady 60.
  The initial-exec TLS model is not an option: Bionic refuses it in a library
  loaded with `dlopen`, which is how SDL loads the game
  (`dlopen failed: TLS symbol ... using IE access model`).
- `runtime/cmake/PublicProducts.cmake` gains `MKW_GENERATED_DIR` so a build
  configured from a checkout can name the translator output tree, and on
  Android rewrites the PE/COFF `.section .rdata,"dr"` of a Windows-generated
  blob `.S` into ELF `.rodata` plus a GNU-stack note. The translator itself
  also learned `--target-os windows|macos|linux|android` for
  `generate-data-init` and `translate-mod`, for pipelines that generate on
  another host.
- `android/`: the Gradle project, with `modernQuest` (the default script
  target) and `quest1` headset flavours. They share the application ID and
  storage, but select the appropriate CPU baseline, supported-device manifest,
  and launcher behavior.
  `app/src/main/cpp/CMakeLists.txt` adds the repository's `runtime/` as a
  subdirectory with those Android choices and builds both game kit probes
  (the Retro Rewind one only when the translation includes the mod), which
  each variant's `export<Variant>QuestGameKit` task (for example
  `exportModernQuestDebugQuestGameKit`) turns into the single kit that
  variant's APK carries, read from the CMake tree whose `MKW_ANDROID_CPU`
  matches the flavour.
- `android/nod-jni`: Gradle's `buildNodJni` task runs `cargo build --release
  --locked --target aarch64-linux-android` with the NDK's clang as linker and C
  compiler. `stageNodJni` puts `libnod_jni.so` into the APK's `arm64-v8a`
  libraries. `-Pcargo=<path>` overrides the cargo binary.

## Building

Prerequisites on the Windows host (all already present on the machine this
was developed on): JDK 17, Android SDK with platform 34+, NDK `29.0.14206865`,
SDK CMake `3.22.1`, `adb`, Rust 1.93+ with `rustup target add aarch64-linux-android`,
the .NET 10 SDK (for the headset's translator; the APK build downloads ~70 MB of Termux
packages into `android/.dependencies` the first time); a translated graph for your own disc (the
installer's `BuildWorkspace/generated`, produced by the normal Windows pipeline).

```powershell
powershell -ExecutionPolicy Bypass -File android/Prepare-QuestDependencies.ps1        # SDL3 3.4.4 AAR into android/app/libs
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Install             # the app, its game kit and toolchain, debug-signed (the first run also builds Dawn from source)
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Headset quest1 -Install  # Quest 1: Kryo CPU and direct-VR library entry
powershell -ExecutionPolicy Bypass -File android/Build-QuestGame.ps1 -Install         # your game, against that kit, into Import (or WheelWizard VR's Build for Quest)
powershell -ExecutionPolicy Bypass -File android/Build-QuestGame.ps1 -Product retro_rewind -Mod <RetroRewind6> -Install  # the mod and its pack (needs translate-mod output with --retro-wfc-payload)
powershell -ExecutionPolicy Bypass -File android/Build-QuestGame.ps1 -Headset quest1 -Install  # game package from the Quest 1 kit
adb push MarioKart.iso /sdcard/Download/                                               # then Select disc image in the launcher
```

Instead of the disc image, an already extracted partition can be pushed to
`/sdcard/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/DATA`, or included in the
game package with `Build-QuestGame.ps1 -Data <dir>`. A game package only fits the APK whose kit
it was built against. After a native or runtime change, run both scripts again; after a
Kotlin-only change the kit fingerprint stays the same and the installed game keeps working.

`Config.toml`, saves and per-run logs live next to `DATA` under
`WiiCompiledOpenXRVR`; the launcher (or the game activity, when started
directly) writes a first `Config.toml` with `[vr] enabled = true` and
`paths.dvd_root` set. Logs: `adb logcat -s SDL WiiCompiledQuest WiiCompiledLauncher`
plus the `Logs/<product>_<stamp>_pid<pid>/console.log` folder the runtime writes.
An abort leaves `crash_sigabrt.txt` there with the guest state and the
aborting thread's native stack. System libraries' frames are named; the
game library's carry `module+0x` offsets, which the PC copy of that build's
`libmain.so` names (it is the file the headset runs, symbol table included):

```bash
.scratch/vr-build-workspace/Toolkit/llvm-mingw/bin/llvm-symbolizer.exe \
  --obj=android/app/build/questGame/modernQuestDebug/base/libmain.so 0x139A34C
```

A CMake-only cross-compile of the native runtime (no game) is the quick
compile check and needs no Gradle:

```powershell
cmake -S runtime -B .scratch/android-audit-build -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=$env:LOCALAPPDATA/Android/Sdk/ndk/29.0.14206865/build/cmake/android.toolchain.cmake `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared `
  -DCMAKE_BUILD_TYPE=Release -DMKW_BUILD_PRODUCTS=OFF
cmake --build .scratch/android-audit-build --target mkw_android_native_compile aurora_core aurora_gx
```

## Validation status

What has been verified on the development machine (September 2026):

- Translator: `dotnet test` passes with the new `--target-os` tests (17/17 in
  the touched suites).
- Windows: `mkw_openxr_replay_tests` and `mkw_vr_policy_tests` pass; the
  `mkw_runtime_common` and `aurora_core` targets compile with the refactored
  integration; the workspace product rebuild links `WiiCompiled.exe`.
- Android: the cross-compile audit passes. `mkw_android_native_compile`,
  `aurora_core` and `aurora_gx` all build for `aarch64-none-linux-android29`
  with NDK 29.0.14206865, which covers the whole native runtime including
  `openxr_vulkan.cpp`, `openxr_android.cpp`, `openxr_input.cpp` and the Aurora
  AHardwareBuffer bridge. Three Bionic portability fixes came out of it: the
  `std::min` call in `hle/audio/audio.cpp` needed an explicit type (`int64_t`
  is `long` on LP64 Android while the clock rep is `long long`),
  `guest_flat_memory.cpp` needs a `__NR_memfd_create` shim below API 30, and
  Crypto++'s `cpu.cpp` needs the NDK's `cpu-features` source compiled in.
- **Device, 2026-09-16: running on a Quest 3** (HorizonOS 14, API 34). The
  runtime negotiates `XR_KHR_vulkan_enable2` (Vulkan 1.0 to 1.2), creates
  1680x1760 `R8G8B8A8_SRGB` (VkFormat 43) swapchains, attaches the controller
  actions, and the session reaches `FOCUSED`. The game boots through the title
  movies into the attract race, the policy switches to `immersive-race` with
  all 189 perspective draws replayed per eye, the first projection layer is
  submitted, and the menus return to the virtual screen. No WebGPU or OpenXR
  errors over a 90 second session.
- **Device, 2026-09-17: the headset built its own game.** Build on this Quest ran inside the app
  in 27.8 minutes on a Quest 3: unpacking the toolchain, 43 MB of NDK files downloaded from Google
  and checked in about 6 seconds, translation in 646 s (2.1 GB peak), 92 sources compiled four at a
  time in 16 minutes (about 250 MB each, 3 GB still available), `ld.lld` in under a second, then
  the install and the cleanup of everything it unpacked. The game ran from the result: session
  `FOCUSED`, past 1,000 frames, the intro movie on the virtual screen. That `libmain.so` is
  byte-identical to one the same toolchain built from a shell, and against the PC-built library it
  has the same soname, `NEEDED` list, 785 undefined symbols and 29,995 translated functions
  (61,974 defined against 61,975: the PC's older clang keeps one inline helper out of line).
- **Device, 2026-09-17: Retro Rewind from its own kit.** The Retro Rewind APK carries a kit and no
  game (66 MB). Its game built from that kit on the PC in 2.9 minutes (166 generated sources:
  72 base shards, 24 profile-sensitive, 48 mod shards, the mod's data patches, 17 registration
  shards and the 3 disc-generated sources), linked to a 157.6 MB `libmain.so` with the same import
  list as the base one, 67,872 defined symbols against the base library's 61,975, and 5,905 symbols
  the base library does not have. Packaged with the disc files (2.55 GB), imported on the Quest 3 in
  under two minutes, and the mod runs: its own title screen, "Press the A Button", and the licence
  menu, with the pack read from `retro_rewind_root`.
- **Device, 2026-09-17: one app, both games.** The merged APK (121 MB) carries one kit with a `base`
  and a `retro_rewind` recipe, no `libmain*.so` and no probe. Both games were built on the PC from
  that one kit (base in 0.1 min from cached objects, Retro Rewind in 3.3 min) and imported on a
  Quest 3: the base package in 3 s, the Retro Rewind one — 2.0 GB, carrying the pack — in 93 s,
  which installed `RetroRewind6` and selected the game it had just installed. Both live side by
  side in `files/game/<profile>`. Retro Rewind ran first (its title screen), then Home's toggle
  switched to Mario Kart Wii, which ran from the same app. Two bugs this found: the on-device
  builder read a per-product `fingerprint` that schema 3 keeps at the kit's top level, and a
  `Config.toml` written before this app offered Retro Rewind named no pack, so the mod would have
  found none — `GameStorage.prepare` now adds that one line to an existing config.

Bring-up fixes that only a device could reveal:

| Symptom | Cause | Fix |
| --- | --- | --- |
| Activity crashed with `EACCES` on `Config.toml` | `adb shell mkdir` had created the app's data directory, so the shell user owned it | Let the app create its own directory; `Run-Quest.ps1` launches once before placing DATA |
| DATA unreadable by the game | adb-placed files stay owned by the shell user and directories are `2770` | `chmod -R a+rX DATA` as the owning shell user; `run-as` cannot reach shared storage (SELinux) |
| "Cannot persist NAND setting.txt" | FUSE storage has no hard links; `link()` fails with `EACCES`, not `EPERM` | Android falls back to exists-check plus `rename` in `nand_settings.h` |
| `SharedFence ... signaled value (0) was not 1` | A sync fd is binary; Dawn expects value 1 | `vulkan_interop.cpp` passes 1 |
| Link error on `Android_LockActivityMutex` | SDL's activity mutex is not exported | Aurora-owned mutex plus the `QuestSurface` bracket (above) |
| Crypto++ `cpu-features.h` not found | The NDK ships cpu-features as source | Compiled into `mkw_cryptopp` on Android |
| Exploded racers and menu characters; smeared movie panels in the menus; then, once those were fixed, damaged eyes and slightly misplaced detail on characters | The Adreno 740 driver reads the wrong bytes when the shader multiplies an index by a stride that is not a multiple of 4. That covers the vertex fetch (`ubuf.vtx_start + vidx * stride + offset`) and indexed array reads (`array_start + index * stride`, e.g. 6-byte S16 normals). GX packs both byte-tight, so skinned models (a 1-byte `PNMTXIDX` first, stride 7) broke everywhere | Android pads every uploaded vertex and every indexed-array element to a 4-byte stride (`padded_upload_stride` in `lib/gx/gx.cpp`). Offsets inside a vertex or element are unchanged, and desktop is unchanged. **Fixed, headset-verified 2026-09-16** at character select and a Grand Prix start |
| Every launch recompiled every shader: a 14 to 34 s prewarm, and the Dawn blob cache reporting exactly one miss and no stores | Dawn's monolithic Vulkan pipeline cache is only written by `PerformIdleTasks`, which `gpu.cpp` resolved through the Windows DLL alone, and the quit path ends the process without `aurora_shutdown`, so nothing compiled after prewarm was kept either | Static Dawn calls it directly; `aurora_store_pipeline_caches` runs at a race exit, when the session loses focus and on the quit path, and the compiler stores idle bursts itself while the headset shows the virtual screen. The unpacked `initial_pipeline_cache.db` is also refreshed per APK install now |
| The game and its music froze for 0.9 to 3.8 s at the end of a race, longer as a session went on | Dawn serializes its monolithic Vulkan pipeline cache (58 MiB on the Quest, and it barely compresses) under its device lock and hands it to Aurora's blob-cache callback, which compressed it and committed it to SQLite still inside that lock; the frame worker waited on the device and the game thread on the frame worker. The store also ran on the XR pacing thread | `gpu_cache.cpp` copies the blob and queues it; a writer thread compresses and commits, and bytes identical to what the database holds are not rewritten. The race exit uses `aurora_request_pipeline_cache_store`, off the pacing thread. The log's `Stored the pipeline caches` line reports the part still spent holding the device |

How the explosion was isolated, so the next Adreno rendering bug starts further
ahead:

- The CPU side was identical to Windows: a per-draw audit of palette indices
  and matrices matched byte for byte. Menus reach the headset as the mono
  desktop image, so stereo replay was not involved either.
- Shader-side rewrites did **not** help and were removed: constant-index palette
  matrix selection, shift-free sign extension, replacing `extractBits`, and
  byte helpers rewritten with constant shifts or integer division. The last two
  made menus worse, which is what pointed away from any one helper.
- Moving every attribute to a 4-byte boundary on the CPU fixed the explosion.
  Padding only the stride, with offsets still packed, fixed it just as well,
  which narrows the fault to the `vidx * stride` term. Mario's eyes stayed
  wrong under both, until indexed arrays got the same element padding. That
  combined padding is the shipped fix. It costs one copy per vertex and per
  array element. Peak uploads at a 12-racer race start were about 570 KB of
  the then 3 MB vertex buffer and 710 KB of the 8 MB storage buffer (the
  vertex staging has since grown to 8 MB for the de-indexed records, see
  the geometry cache below).
- KartPad's Android reports of corrupted drivers on Adreno 750 match this
  symptom. That is plausible but not tested.

Diagnostics that stay in the build, all read once at launch from system
properties. Set them with `adb shell setprop <name> <value>` before starting
the app:

| Property | Effect |
| --- | --- |
| `debug.wiicompiled.vtxpad 0` | Turns the stride padding off, to re-check a driver update |
| `debug.wiicompiled.validation 1` | Keeps WebGPU validation and robustness on in release builds |
| `debug.wiicompiled.fdm 0` | Launches without fragment density maps at all, whatever `foveation` says, which also drops their flag from every pipeline; `1` asks for them even with `foveation = "off"` |
| `debug.wiicompiled.inject <n>:<button>` | Presses `a`, `b`, `x`, `y`, `start`, `up`, `down`, `left` or `right` for 12 XR frames each time `<n>` changes. As a Wii Remote, `x`/`y`/`start` are 1/2/+, the directions push the Nunchuk stick, and `home`, `c` and `z` also exist. `panel` presses the settings panel's button (left Y, or both thumbsticks as a gamepad), opening or closing it (see `OPENXR.md`). `flick` plays the bare hands' flick, one 150 ms shake of the remote (a trick off a ramp, a wheelie on a bike), with the controllers or none. `throw_forward` and `throw_backward` play a throw of the held item: the stick pushed that way while the item button is pressed and released (see "Throwing the held item" in `OPENXR.md`) |
| `debug.wiicompiled.fpslog 1` | Logs the game's rendered frame rate every 5 s, with per-frame averages of the producer's waits for the frame worker's DONE and SEALED phases and of the worker's seal, permit wait, prepare and encode stretches, and of the draw calls the recorded frame holds and the primitives that merged into them (an overlay that stops draws merging shows up there first). A following line counts, per frame, why the other draws did not join the draw before them: a GX state change, a particle quad, a 2D draw while stereo records, the first draw of a pass, lines or points, a previous vertex-motion or instanced draw, or the wheel's animated arrays (a race start at Luigi Circuit: about 320 by state, 20 2D, 3 first). A third line reports the GX thread's command ring (records, waits, busy share). With the geometry cache on, a fourth counts, per frame, the display lists drawn from it (how many through native vertex input, how many after a re-digest), those resolved into it, those left in place and why (unsupported, untracked, no room, rewritten by the game), and the entries and MiB resident. A fifth gives the frames' staging high water (vertex and index KiB of their regions), the frames on which de-indexing stopped early for want of room, and the draws skipped. A second line gives the GPU time per frame from timestamp queries on every pass (`mono` native render, `eyeL`/`eyeR` replays, or `eyes` for the one multiview pass, `screen`, `panel`, `efbcopy`, `palette`, `peek`, plus `passes-span` from the first pass begin to the last pass end and `between-passes` for copies and idle gaps). The compositor's `VrApi` log line gives headset FPS, `GPU%`, `CPU%`, clock levels and app GPU time (`App=`) |

A `Config.toml` written with `adb push` (or `sed -i` in `adb shell`) belongs
to the shell user afterwards, and the app then fails every save with EACCES
(the launcher logs `GameStorage.prepare ... open failed`). `chmod 664` on the
pushed file gives the app's group write access back; a file the app created
itself never has the problem.

The injector makes headset tests possible with nobody wearing the headset.
Keep the display awake, drive the menus, then take a compositor screenshot:

```powershell
adb shell am broadcast -a com.oculus.vrpowermanager.prox_close
adb shell setprop debug.wiicompiled.inject 1:a    # title -> licence; bump the number per press
adb shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT
adb pull /sdcard/Oculus/Screenshots/<newest>.jpg
```

From a cold start, five `a` presses about 5 s apart, starting once the title
screen is up, reach Grand Prix character select. A value left over from an
earlier run is ignored on the first read. Presses only land while the XR
session is `FOCUSED`.

A debug APK also builds the game on the headset without a press, once `DATA` is there; the
build log is the newest `Logs/build_*.log`:

```powershell
adb shell am start -n org.wiicompiled.quest/.launcher.LauncherActivity --ez org.wiicompiled.quest.debug.BUILD_GAME true
```

It opens the mod browser the same way, on a GameBanana mod when one is named, and with
`INSTALL_MOD` installs that mod as Download and Install would (its first file, the suggested
name); `adb logcat -s WiiCompiledLauncher` shows the searches and the install:

```powershell
adb shell am start -n org.wiicompiled.quest/.launcher.LauncherActivity --ei org.wiicompiled.quest.debug.MOD_BROWSER 699980 --ez org.wiicompiled.quest.debug.INSTALL_MOD true
```

`MIIS` opens My Miis, and `edit:N` or `edit:N:<Page>` the Mii editor on its Nth Mii
and one of its pages. The launcher panel takes `adb shell input tap x y` in its
1280x800 pixels, and `adb shell uiautomator dump` gives its views' bounds, so a
test can go on from there with nobody wearing the headset:

```powershell
adb shell am start -n org.wiicompiled.quest/.launcher.LauncherActivity --es org.wiicompiled.quest.debug.MIIS edit:0:Hair
```

Performance, measured 2026-09-16 on a 50cc Luigi Circuit start with the player
idle, over 40 s, with an optimized build (`-O3`, translated code `-O2`,
`-mcpu=cortex-a77`):

| Build | Game FPS | Headset FPS | App GPU time | GPU% |
| --- | --- | --- | --- | --- |
| Full-display mirror (4128x2208) | about 48.5 | 48.7 | 15.6 ms | 83 |
| 1280x720 surface, no present | about 48.6 | 49.3 | 14.9 ms | 81 |

Removing the mirror saved about 0.7 ms of GPU time per frame but did not raise
the game rate. Two leads remain. The game runs below 60 FPS with the GPU at
about 80%, and CPU and GPU clock levels sit at 4/3. The headset FPS also
follows the game rate instead of holding 72 Hz, so the pacing thread is not
repeating the last layer as it does on desktop. Both need profiling on the
XR2 Gen 2.

Profiled 2026-09-19 on a twelve-kart 50cc Grand Prix start (Luigi Circuit,
`render_scale` 0.8, intro skipped, driven unattended by the button injector:
ten `a` presses 5 s apart from the title screen reach the race, one more skips
the course intro). The game thread is the limit, not the GPU: it is one
libco-hosted thread (about 60% translated game code, 20% GX HLE, 12% Aurora's
FIFO decode), and Aurora's frame worker, which encodes and submits the Dawn
work, runs at about half a core with most of its own time inside the Adreno
driver's ioctls. Trimming the GX HLE (a 4 KiB write-tracking granule, one
pointer probe for the texture-object shadow, inline padded vertex copies, a
throttled clock poll) changed nothing measurable against the same automated
start, and asking for `XR_EXT_performance_settings` BOOST is accepted but the
runtime keeps its own dynamic clocks (CPU level 4 at 1.9 to 2.2 GHz, GPU level
3 at 490 to 640 MHz). What did matter was a scheduler trace of the game
thread: it slept 3 to 4.5 ms of every frame, in 1 ms slices, on Aurora's
SEALED phase. Without interpolation the worker published SEALED only after the
whole encode and submit, so the producer's first GX drain of each frame waited
for the previous frame's encode (5 ms in menus, 9 to 11 ms in a race). The
worker now always releases the producer right after sealing; the same start
went from 47 to 53 fps to 55 to 58 fps, the game thread from 82% to 98% busy,
and menus lost the same 4.5 ms of idle wait per frame. With `fpslog` on, the
`Game frame rate` line now carries that breakdown (producer waits for DONE and
SEALED; the worker's seal, permit wait, prepare and encode) so the next
regression of this kind shows up in the session log. The remaining gap to 60
at the start is about 1 ms of game-thread CPU per frame, with the GPU at 85 to
89%, so the next steps are on both sides: the guest-code share (translator
output quality) and the eye replay's GPU cost.

The GPU side, measured the same day with per-pass timestamp queries (the second
`fpslog` line): on SNES Ghost Valley 2 at `render_scale` 0.5 (840x880 eyes) a
stereo frame cost 13.2 ms, of which the native render was 5.7 ms, the eyes 3.5
and 3.8, copies and gaps 0.4. That native render is a 1280x720 image nobody
sees during an immersive race, so it now stops after the last pass whose EFB
copy the eyes sample: `mono` fell to 0.15 ms and a Luigi Circuit start at 0.5
renders in 5.5 to 10 ms of GPU per frame. The last limiter was the headset
pacing: with the display at 72 or 90 Hz, each headset frame stayed open for
the next 60 Hz game frame plus the whole encode (`open` 16 ms in the pacing
summary), so cycles spanned one to two display slots and the headset got 40 to
60 frames per second while the game rendered 60. The Vulkan backend now paces
render-first (`PreparePacket`, `BeginFrameForPacket`, `CopyRenderedEyes` in
`openxr_vulkan.cpp`; see `OPENXR.md`): the packet is located and handed to
Aurora with no compositor frame open, and the frame is begun only once the
eyes exist, for the copy alone. On the same automated start at 0.75 the
summary reads `cycles=60 skipped-slots=12 late=0 layers new=60 repeat=0
open=5.5 end-gap=16.7`, the compositor shows 60 to 61 of 72 with the
inherent 12 stale slots, app-to-compositor latency fell from 51 to 9 to 13 ms,
and the frame worker's encode fell from 8 to 2.7 ms because the eye copy and
its fence wait moved off the worker onto the pacing thread.

Retro Rewind tracks then showed a game-thread limit of their own: on Athens
Dash (a Mario Kart Tour port) the display-list index scan
(`WalkDisplayList<DlIndexScanVisitor>`) was 11.5% of the thread while the base
game's tracks spend 0.3% there. The scan cache in `gx_dl.cpp` refused lists
above 64 KiB, so that track's large shape lists were scanned again on every
call; the cap is now 4 MiB. With it the scan is 0.2%, the game rate on Athens
Dash went from 47 to 51 fps to 50 to 58, and the thread splits into 62% game
plus mod code, 9% GX HLE, 6% FIFO decode, 4% memory copies, 3.5% dispatch and
the rest. What remains on such tracks is the game's own code plus the mod's,
which no host change shrinks; a GX thread could move about 20% of it.

That GX thread exists now (`runtime/include/gx_thread.h`, `[video] gx_thread`,
on by default on Android and opt-in elsewhere). Every GX HLE override is split
into a game-thread front, which keeps the guest-visible side effects (GXData
shadow registers, the getters, display-list recording, the texture meta table),
and a `_gx` back holding the aurora work and the parser state, posted through
one ordered 16 MiB command ring; immediate-mode gather-pipe bytes travel as
8 KiB chunks in call order. The hazard rule follows the hardware: whatever the
SDK copied into the FIFO at call time (matrices, projection, colours, light
objects, copy filters, layout quads, texture object registers) is snapshotted
when posted, and whatever the GP read from memory when it reached the command
(display lists, vertex arrays, indexed matrices, texture data) is read when the
GX thread executes it, so `GXDrawDone` drains the ring and the frame's
schedule, first-person anchor and policy tag are latched into the present
record on the game thread. The desktop overlay became a game-thread-owned
ImGui frame whose draw data Aurora copies per sealed frame, which also removed
the frame-worker join `GXCopyDisp` used to make. With `fpslog` on, a third
line reports the ring: records and bytes per frame, the game thread's waits
for ring space and in drains, the GX thread's busy share and any exceptions
it caught. A texture or matrix that is wrong only with the thread on is a
hazard-rule violation (a front reading guest memory the game rewrites before
the GX thread runs, or a back writing guest memory). Measured on the same
automated Grand Prix start at `render_scale` 0.75, same build, switched by the
config key: with the thread off the crowded first half minute ran at 52 to
56 fps before settling at 60; with it on the same stretch ran at 56.5 in the
window that includes the countdown and 60.0 in every window after, while the
ring carried 4.5k to 6.2k records (250 to 380 KiB) per frame, the game thread
waited under 0.1 ms per frame in its two `GXDrawDone` drains and never for
ring space, and the GX thread was 25 to 40% busy. Retro Rewind's menus were
unaffected (prewarm 5.2 s, 60 fps).

Two things the first day on it taught. The Retro Rewind menu with the blurred
background fell to 14 to 18 fps, with the GX thread on or off, and the
per-record profile that the `fpslog` line now carries (`costliest:`) put it
all in the FIFO records: the game re-initialises its capture texture objects
every frame, and the split had kept one aurora object per guest object alive
across those re-initialisations, so `GXInitTexObjData` kept incrementing
`texDataVersion`, which is part of aurora's static upload key, and every
frame converted every such texture again (`convert_texture` 18% of the
thread). A guest `GXInitTexObj` now rebuilds the aurora object, as it always
had, so the version restarts and the upload cache hits. Second, that menu
calls `GXDrawDone` 22 to 24 times per frame (the base main menu 9 times),
and each drain cost about 1.5 ms while the game thread slept on a condition
variable: both the drain and the idle consumer now spin for a few hundred
microseconds before blocking, with a sequentially consistent sleep handshake,
and the 22 drains cost 2.6 ms per frame in total; that screen runs at 60 with
the thread on.

Three renderer changes measured on 2026-10-07 came from comparing this port
with the Metroid Prime port (`MetroidPrimePortVR`, the same Aurora lineage,
whose Quest 3 campaign is in its `docs/CHOZO_PERFORMANCE.md`). Method: the
base game's first Grand Prix race at Luigi Circuit, player idle while the
CPUs race (so the draw count cycles between about 280 and 450 a frame as the
pack passes; eye times are compared at a matched draw count through a line
fit per state), `render_scale` 0.8, foveation medium, `fpslog` on, each
change alternated on and off in 11 s windows for 6 to 10 rounds inside one
session, the GPU at 492 MHz throughout, the game at 60 FPS throughout, and
`VrApi App=` from the game's process.

| Change | Both eyes, matched draws | App GPU per frame |
| --- | --- | --- |
| Constant position-matrix index baked into compacted-layout shaders instead of a uniform load and a dynamic index (`gx/shader.cpp`; Prime `cd00553e`) | 7.59 -> 6.39 ms at 356 draws (-16%) | 10.06 -> 8.82 ms |
| The eye's last render pass discards depth and stencil instead of storing them (`gfx/common.cpp` `render_eye_planned`) | 7.04 -> 6.51 ms at 328 draws (-7.5%, 10 rounds) | 9.51 -> 9.08 ms |
| Eyes drawn into the AHardwareBuffers with a full-screen triangle instead of `CopyTextureToTexture` (`webgpu/vulkan_interop.cpp`) | 6.89 -> 6.47 ms at 337 draws, then 6.72 -> 6.51 at 333 (10 rounds) | 9.41 -> 8.76, then 9.14 -> 8.91 ms |
| Indexed vertex attributes resolved on the CPU into flat, aligned records as the FIFO is decoded, so the vertex shader reads one record instead of an index and a dependent array fetch (`gx/command_processor.cpp` `deindex_plan`; Prime `229bc15f`) | 6.72 -> 6.16 ms at 338 draws (-8%) | 9.14 -> 8.73 ms |
| Display lists that only draw kept resolved on the GPU across frames, a hit staging only its indices (`gx/geometry_cache.cpp`, `gfx/geometry_buffer.cpp`; Prime `1885428c`) | 6.06 -> 6.03 ms at 332 draws | 8.70 -> 8.54 ms |
| The cached lists drawn through the GPU's vertex-input stage instead of decoded in the vertex shader (`gx/native_vertex.hpp`, `gx/shader.cpp`; Prime `3aa490a4`) | 6.13 -> 4.61 ms at 333 draws (-25%) | 8.88 -> 7.16 ms |
| Both eyes drawn in one Vulkan multiview render pass over two-layer eye targets, each draw recorded and bound once with its pair of eye uniforms indexed by the view (`gfx/stereo_multiview.hpp`, `patches/dawn`; after Prime `cddf7d16`), measured with direct presentation on | 4.29 -> 3.88 ms at 343 draws (-10%) | 4.68 -> 4.41 ms; the frame worker's encode 3.33 -> 2.50 ms |

The first three together take the race from about 10.1 to 8.8 ms of App GPU
time at the same clock, with the image unchanged (compositor screenshots with
the blit on and off are identical in content). The last three are Prime's
largest finding, native vertex input over a resident de-indexed geometry
cache (App GPU 9.6 -> 5.1 ms on its scene), measured here in a second
session the same way. All three against the shipping path: both eyes
6.69 -> 4.57 ms at 338 draws (-32%), App GPU 9.52 -> 7.21 ms (-24%), the
image identical. A race frame at Luigi Circuit calls about 370 display lists
that only draw (the course's and the karts' shapes, about 300 distinct). On
the old path every vertex reached the GPU as one index per attribute, and
the vertex shader fetched the array element through it and decoded the
bytes. Now (`[video] deindex_vertices`, `geometry_cache_mb`,
`native_vertex_input`, all on by default on the Quest and opt-in on the PC;
the settings page's Resident geometry toggle turns the three off together)
such a list is resolved once into flat, 4-byte-aligned records in a 32 MiB
storage and vertex buffer (`gx/geometry_cache.cpp`,
`gfx/geometry_buffer.cpp`), keyed by its address, size and the layout of
its records (vertex format, arrays and offsets), valid while the guest-RAM
write tracking says neither the list nor the arrays it indexes were written;
a list whose granule moved is re-digested before its entry is dropped. A hit
stages only 32-bit indices, and because every block sits at a multiple of
its stride the surfaces of one material still merge into one draw. The
records are converted once to the GPU's byte order and drawn through real
vertex attributes (`gx/native_vertex.hpp`), a pipeline variant whose bit
came out of the recipe's padding so the stored recipes keep their bytes; the
first race after an install compiles the new variants at first use and the
player's pipeline cache keeps them. Lists the game rewrites every frame
(four a frame here) are left to the plain path after their second change.
Everything is read where the GX thread executes the call, as the hazard rule
wants, and the entries go at the race exit. Luigi Circuit holds 2.7 MiB of
records; a full buffer leaves the lists beyond it on the plain path and logs
once.

The frame's staging regions are mapped buffers that cannot grow, and a push
past the end of one aborted the process without a line of log: that is what
a Retro Rewind race start did once the cache existed, because a frame's
de-indexed records are four to eight times its FIFO bytes and the first
frame of a race, with the cache cold, resolves every list in place. The
vertex staging grew from 3 to 8 MB and the index staging from 2 to 4 MB (a
cached list's 32-bit indices take twice the plain path's), and the FIFO
processor now keeps to them: past three quarters of the vertex staging the
frame's remaining draws keep their indexed FIFO records and the arrays'
storage path (de-indexing is latched again at the next frame), a cached list
whose indices do not fit is drawn in place, and a draw not even its FIFO
bytes or indices fit is skipped and reported, never merged into. With
`fpslog` on, a line reports the frames' staging high water, the frames on
which de-indexing stopped early and the draws skipped.

Last, Vulkan multiview (`[vr] multiview`, on by default on the Quest, the
settings page's "Both eyes in one pass"): the eye targets become the two
layers of one colour and one depth texture, the planned eye replay runs once
with view mask 0b11, and every GX draw, clear, cockpit and window-mask draw is
recorded and bound once for both views. The GX shaders' multiview variant (a
`ShaderConfig` bit taken from the padding, so the stored recipes keep their
bytes) declares `@builtin(view_index)` and binds the draw's two eye uniforms,
which the per-eye replay already stages back to back, as one range indexed by
the view: the per-eye mathematics is untouched and the images are identical.
That pair is the only way the multiview shader reads a uniform, so a draw the
eyes keep on its recorded transforms (a native EFB effect such as the
dynamic shadows' tint quad, 2D content the virtual screen does not claim,
another player's draw) gets two copies of the uniform it recorded with;
before it did, the right view read the bytes after that uniform and the
dynamic shadows were missing from the right eye.
The cockpit and the window mask hand both eyes' vertices in as two vertex
buffer slots, the held item both eyes' frames in one uniform. The feature
needs the Dawn built with Aurora's patches, which gained Tint's
`chromium_experimental_multiview` extension, the `DawnMultiview` feature and
view masks on dynamic rendering (`aurora-main/patches/dawn/apply.py`, after
Prime's), plus layered density maps (`dawn_fdm_abi.h` version 2) so foveation
covers both layers; a stock Dawn keeps the per-eye passes. Measured as above,
switched live in 11 s windows for 6 rounds: both eyes 4.29 -> 3.88 ms at 343
draws, App GPU 4.68 -> 4.41 ms, and the frame worker's encode fell from 3.33
to 2.50 ms a frame, the eyes being encoded once. With `fpslog` on, the GPU
line then reports `eyes=` for the single pass in place of `eyeL`/`eyeR`. Over
the day, the Luigi Circuit race went from about 10.1 to 4.4 ms of App GPU
time at the same clock.

Frame pacing without interpolation: at 72 Hz the compositor shows one 60 Hz
game frame in five for two refreshes, and in a player's race trace the
repeats landed 5 or 7 refreshes apart instead of 6 in 10-25% of cases, which
reads as a stutter. `[vr] refresh_rate = 120` (the settings page's "Display
refresh rate") shows 95.9-97.8% of frames for two refreshes; the rest, shown
for one or three, come in bursts while the game's and the display's
independent clocks drift past the compositor's deadline. 120 Hz cost no app
GPU time or CPU and about 4 points of GPU load. Locking the game's VI
timeline to the display took that to 99.8-100%, but it was dropped once
interpolation (below) became affordable.

Showing a frame twice still doubles what moves across the view when the
kart turns: the compositor re-warps the repeat for the head, not for the
game's camera. Only a new image per refresh removes that, which is what VR
frame interpolation renders. Its first Quest measurement (Retro Rewind
race, player idle, one launch per setting) found the native mono render
drawn in full, about 2 ms a game frame for an image nobody sees: with
interpolation most frames are sealed without a stereo packet and their eyes
come from the retained replay, which the cut after the last pass feeding
the replay did not cover. Covering it took interpolation at 72 Hz from 55%
to 48% GPU busy (App GPU 6.60 -> 5.74 ms a displayed frame, missed frames
1.0 -> 0 a second, the frame worker 38 -> 32% of a core, its encode
2.25 -> 1.07 ms), and at 120 Hz from 83% to 80% (4.3 -> 1.1 missed frames a
second, the game's lowest 5 s window 53.9 -> 57.0 FPS). Interpolation off
at 120 Hz runs at 45%. At 120 Hz every displayed frame is a full eye replay
and the runtime keeps the GPU at level 3 (492 MHz), so 72 Hz is the
comfortable interpolation rate on this headset.

Verified on device since: the menus on the virtual screen, controller input
(the user has driven races), and an immersive Grand Prix start with all 12
racers rendering correctly. Not yet verified: stereo comfort and scale,
lifecycle (Quest menu, guardian, sleep), and a full race to the finish.
When diagnosing a new device, the session log should show, in order: the loader log line
(`OpenXR Android loader initialized`), the requirements line (which binding
extension was negotiated), `OpenXR Vulkan swapchains ready`, the session
state reaching `FOCUSED`, `presentation=virtual-screen` for the menus, and
`first immersive packet consumed` on race entry. A black headset with a working
Android mirror points at the AHardwareBuffer copy (check for `vkImportSemaphoreFdKHR`
or `EndAccess` errors); a black mirror too points at Aurora itself.

## Known gaps and next steps

- **Device bring-up.** Run on a Quest 3, capture logcat, fix what the runtime
  rejects. Likely first candidates: the exact `XR_KHR_vulkan_enable2` device
  extension negotiation, Dawn's begin/end layout reporting for AHardwareBuffer
  imports, and swapchain format choice (`R8G8B8A8_SRGB` is expected).
- **Performance.** The desktop product targets x86-64-v3; nothing has been
  profiled on the XR2. The first run compiles every bundled pipeline recipe
  (about half a minute); later runs load Dawn's pipeline cache from `Cache/`
  next to `DATA`. `render_scale` defaults to 0.8 here (1.0 on
  PC); lower it further if the compositor reports missed frames. It can be
  changed during a race from the headset panel (VR → Render resolution).
  Foveated rendering (above) is off by default: at `render_scale` 0.8 it saves
  nothing measurable, above that 8 to 22% of the eyes' GPU time.
- **Lifecycle.** Backgrounding (the Quest menu, guardian) pauses the session
  through the ordinary `STOPPING`/`READY` events; SDL's Android surface loss is
  handled by Aurora's existing Android paths. Neither has been exercised.
- **Input.** D-pad (trick inputs) is not bound; remap in `Config.toml` or bind
  the thumbstick directions in a follow-up. Haptics are wired but nothing calls
  them yet.
- **Retro Rewind on the headset** runs from a kit-built library (below), but its game must be built
  on a PC and its pack copied next to `DATA` by hand. `adb push` cannot create directories inside
  an app's external files directory (`secure_mkdirs failed`), so push the pack to `Download` and
  copy it over on the device, then `chmod -R a+rX` it. The launcher does not fetch or update the
  pack, and cannot build the mod on the headset.
- **Release signing and store packaging** are out of scope; `Build-Quest.ps1`
  produces debug-signed APKs for sideloading.
