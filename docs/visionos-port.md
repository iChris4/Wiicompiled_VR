# WiiCompiled VR on Apple Vision Pro (visionOS)

This document is the design and build reference for the Apple Vision Pro build.
It complements `OPENXR.md`, which remains the specification for the
presentation policy, the virtual screen, the immersive window, the first-person
camera and frame interpolation: all of that is shared, unchanged, with the
Windows and Quest products. What differs is everything below the stereo replay:
the headset runtime (visionOS has no OpenXR), the graphics binding (Metal), the
input (hands, no controllers) and the app shell (SwiftUI, a static library).

Status: **runs on an Apple Vision Pro.** On 2026-09-26 the base game booted on
a headset running visionOS 26.6.1, reached the menus on the virtual screen with
the hand-driven pointer, and entered a race with the immersive stereo replay
submitted as a projection layer at 1888 x 1792 per eye; the same build runs in
the visionOS 27 simulator (head tracking, no hands). Comfort, frame timing and
input tuning on hardware are the open work. See
[Validation status](#validation-status).

## Sources of the design

- **KartPad** showed that
  the translated game runs on the iOS family: Aurora on Dawn/Metal, SDL3, a
  static library inside an app bundle. Its lessons carried over: the guest's 4
  GiB flat address space has to be placed with care because the iOS family's
  virtual address space is small (16 GiB there; 480 GiB here, below); `/tmp` is not writable (use `TMPDIR`);
  Dawn's `ios-arm64` prebuilt cannot be reused for another Apple platform, so
  Dawn is built from source for the SDK at hand (`scripts/build-dawn-ios-simulator.sh`
  there, `visionos/Build-VisionOSDawn.sh` here); the app exports its
  directories to the runtime through the environment.
- **Dolphin iOS**  informed the shape of the
  interface: a small native launcher, game data through the Files app, the
  emulator core on a thread of its own with UIKit never blocked by it.
- **Apple's CompositorServices and ARKit C APIs** supply what OpenXR runtimes
  supply elsewhere: frame pacing and prediction (`cp_frame_*`), the eye
  drawables and their view transforms and projections, the device pose
  (`ar_world_tracking_provider`) and the hand skeletons
  (`ar_hand_tracking_provider`).

## Architecture

### One runtime, a third graphics binding

`runtime/src/vr/openxr_integration.cpp` owns the pacing thread, policy
evaluation, the retained-layer protocol and the head-pose maths, written against
the backend-neutral vocabulary in `runtime/include/vr/openxr_backend.h`. It
selects one backend class at compile time:

| Platform | Backend | Binding |
| --- | --- | --- |
| Windows | `OpenXRD3D12Backend` (`openxr_d3d12.cpp`) | Dawn's D3D12 device; eyes copied on its queue into the XR images. |
| Android | `OpenXRVulkanBackend` (`openxr_vulkan.cpp`) | A second Vulkan device from the runtime; Dawn and it meet on `AHardwareBuffer`s. |
| visionOS | `OpenXRMetalBackend` (`openxr_metal.mm`) | Dawn's Metal device; the XR images are `IOSurface`s Dawn imports as shared texture memory. |

The Metal backend follows the D3D12 one step for step: Aurora renders straight
into the acquired swapchain images (no intermediate copy), a second pair of
swapchains holds the last submitted frame for retained resubmission, the
settings panel gets its own quad layer, and the virtual screen crops the eye
image to the drawn area so the room frames the picture.

### The OpenXR provider

visionOS ships no OpenXR runtime and no loader. Rather than rewrite the pacing
thread, the controller actions and the swapchain protocol against Apple's APIs,
`runtime/src/vr/visionos/` implements the OpenXR 1.0 entry points those call
(the ~45 `xr*` functions the runtime uses, plus `XR_KHR_convert_timespec_time`
and `XR_FB_display_refresh_rate`) on top of CompositorServices and ARKit. It
links in place of the Khronos loader as `mkw_openxr_visionos`
(`runtime/CMakeLists.txt`); the Khronos headers are fetched, nothing else of
the SDK is needed.

| OpenXR | visionOS |
| --- | --- |
| Instance, system, `xrEnumerateViewConfigurationViews` | The `cp_layer_renderer_t` the app hands over (`xr_visionos_set_layer_renderer`); eye sizes from the first drawable's view texture map, 1920 x 1824 until one is seen. |
| Session states | `cp_layer_renderer_get_state`: paused → `READY`/`SYNCHRONIZED` (should-render off), running → `VISIBLE`, `FOCUSED`, invalidated → `STOPPING`, `EXITING`. |
| `xrWaitFrame` | `cp_layer_renderer_query_next_frame` + `cp_frame_predict_timing`; the predicted display time is the frame's presentation time, the period its measured cadence (11.1 ms until measured). |
| `xrBeginFrame` | `cp_frame_end_update`, `cp_time_wait_until(optimal_input_time)`, `cp_frame_start_submission`, the drawable. |
| `xrLocateViews` | `cp_view_get_transform` (device-from-view) composed with the ARKit device anchor at the display time; the FOV from `cp_drawable_compute_projection`, which the runtime turns back into its own projection. |
| `xrEndFrame` | The provider *composites*: projection layers are drawn as full-view textured quads at a constant depth per eye, quad layers as world-placed quads; depth is written so the compositor can reproject; `cp_drawable_encode_present`, `cp_frame_end_submission`. |
| Swapchains | Triple-buffered `IOSurface`s with an `MTLTexture` each (`XrSwapchainImageMetalMKW` in `runtime/include/vr/visionos/xr_visionos.h`). |
| Reference spaces | `LOCAL` and `STAGE` are the ARKit world origin, `VIEW` the device anchor. |
| Actions | `xr_visionos_input.mm`: hand-tracking gestures. |
| Time | `XrTime` is nanoseconds on the `mach_absolute_time` clock, the clock behind `cp_time_t`. |

Synchronisation between Dawn's queue and the compositor's uses
`MTLSharedEvent`s in place of the Quest's sync fds: after `xrWaitSwapchainImage`
the backend asks for the event and value at which the compositor's last read of
the image completes (`xr_visionos_swapchain_image_acquire_fence`), and Aurora's
Dawn `BeginAccess` waits on it; Dawn's `EndAccess` exports the event and value
its copy reaches, which the backend hands back before `xrReleaseSwapchainImage`
(`xr_visionos_swapchain_image_set_release_fence`), and the compositor's render
pass waits on it. Aurora's side is `aurora-main/lib/webgpu/metal_interop.cpp`
(`aurora/metal_interop.h`), the Metal twin of `vulkan_interop.cpp`, using Dawn's
`SharedTextureMemoryIOSurface` and `SharedFenceMTLSharedEvent` features.

### Passthrough and immersion

The room is shown two ways at once. The SwiftUI `ImmersiveSpace` opens in
*mixed* or *full* immersion (the launcher's Settings > Virtual screen > "Show my room", remembered across launches); in mixed
immersion the compositor blends the drawable over the surroundings by alpha.
The game's live `[vr] passthrough` setting (the same one as on the Quest, on by
default) reaches the provider as `xr_visionos_set_frame_environment`: when set,
frames outside an immersive race clear the drawable transparent, so the room
frames the virtual screen and the immersive window; immersive races draw
opaque eyes and cover the room either way.

### Input

The Vision Pro has no controllers. `xr_visionos_input.mm` derives the OpenXR
action state from the ARKit hand skeletons, and the provider is a real
`XR_EXT_hand_tracking` runtime (`xr_visionos_hand_tracking.mm`, with
`XR_EXT_hand_tracking_data_source` and `XR_FB_hand_tracking_aim`), so the
runtime's bare-hand driving (`OPENXR.md`, "Steering wheel and hand steering")
runs here exactly as on a Quest with the controllers put down.

**Drive with your hands** (`[vr] hand_tracking`, on by default here). While the
runtime holds hand trackers the hands are bare hands and the provider answers
the `khr/simple_controller` bindings:

| Gesture | Effect |
| --- | --- |
| Close a hand on the wheel or handlebar | Holds it (grasp from the fingers' flexion) and holds the gas; turn to steer. |
| Pinch (thumb to index) with an open free hand | Uses an item (Z). |
| Flick both hands up on the wheel, or a free hand | A trick or wheelie (one shake of the remote, Wii Remote mode). |
| Little-finger pinch | Pause (+). visionOS keeps its palm-up system gesture to itself. |
| Index pinch, no hand on the wheel | A (select); the pointer is by gaze, below. |

The joints come from `ar_hand_tracking_provider_query_anchors_at_timestamp` at
the frame's predicted time, so every 90 Hz frame has a fresh sample (the latest
anchors repeat between ARKit's ~30 Hz updates, which the flick detector would
read as a hand standing still). ARKit has no palm joint; the provider
synthesizes one between the wrist and the middle knuckle, oriented from the
wrist and knuckle positions (OpenXR's −Z towards the fingers, +Y out of the
back of the hand) whether ARKit measures or estimates them, so the held item
faces the player. The other joints keep ARKit's own axes (+X along the bone);
only their positions are read. No hand of ours is
drawn: the app's `upperLimbVisibility(.visible)` composites the wearer's own
hands over the immersive race, and `BuildCockpit` sends each hand's pose
marked hidden, so Aurora draws no hand but still stands the held item
(`cockpit_item_hand`) over the real palm. Manual drift has
no gesture: choose Automatic drift. The headset panel's "Drive with your
hands" checkbox reads out each hand's grasp, hold and pinch for tuning.

**Off**, the hands play a Touch controller instead, as before:

| Gesture | Action |
| --- | --- |
| Index pinch | trigger / select |
| Middle-finger pinch | A (right) / X (left) |
| Ring-finger pinch | B (right) / Y (left) |
| Little-finger pinch | menu |
| Fist (fingers curled) | squeeze / grip (a fist near the wheel takes hold of it) |

In both modes: the grip pose is at the palm, from the hand skeleton; the left
aim pose runs from the wrist through the index knuckle; the right aim pose (the
Wii pointer) is look and pinch, below.

**Pointing is by gaze, adjusted by hand, pressed on release.** Aiming a
hand at a screen a few metres away proved far too coarse to land on a menu
button, and a press at the instant of the pinch landed where the eyes happened
to be. visionOS never exposes the gaze itself, but every pinch in an immersive
space arrives as a spatial event (`LayerRenderer.onSpatialEvent`, forwarded
through `mkw_visionos_spatial_event`) carrying the ray from the eyes to where
the user was looking when the fingers met, and, for as long as the pinch is
held, the pinching hand's pose. The provider aims the pointer hand's aim pose
along that ray at once, then moves it with the hand (the hand's displacement,
times a gain of 1.5, applied to the target at 2 m), so a wrong landing is
corrected by a small motion; when the fingers part it presses select where the
pointer is, for 120 ms, and the pointer stays parked there until the next
pinch. Either hand's pinch works, each with its own state, and the skeleton's
own index pinch never presses select, so nothing fires before the adjustment.
A pinch the system cancels presses nothing. Before the first pinch there is no
pointer. This holds on the virtual screen (menus, the flat-screen race); in an
immersive race the skeleton's pinch stands as it is and presses at once, since
there it is an item or a trick and the delay would only be lag. The provider
tells the two apart by the layers of the last `xrEndFrame`: a projection layer
means immersive.

That is enough for the menus, the settings panel and, with "Drive with your
hands", a race. A Bluetooth game controller (SDL3's GameController backend)
still works and is the way to race with the option off; `[controls]` bindings
apply as on the desktop. Haptics are no-ops.

### Platform glue

- **The games are frameworks loaded at Play.** The products are
  `WiiCompiledGame.framework` and `RetroRewindGame.framework`
  (`runtime/cmake/PublicProducts.cmake`), both embedded and signed with the app
  when the translation includes the mod. The app does not link them:
  `visionos/App/GameLibrary.swift` loads the chosen one with `dlopen` when the
  player presses Play and looks the C bridge up by name
  (`runtime/include/platform/visionos/visionos_host.h`), as the Quest launcher
  loads `libmain.so`. That order matters: dozens of the runtime's globals read
  `Config.toml` in their static initialisers, which run when the framework
  loads, so the launcher's Settings tab has to have written the file first.
  `main()` becomes `mkw_runtime_main`, called on a 64 MiB game thread by the
  bridge in `runtime/src/platform/visionos/visionos_host.mm`. One process
  holds one game (they define the same runtime); switching means relaunching.
- **Retro Rewind and its pack.** The Play tab's picker (`GameChoice`,
  `GameModel.swift`) chooses the game, remembered in `UserDefaults`. The pack
  the modded game reads (`[paths] retro_rewind_root = "RetroRewind6"`, next
  to `DATA`) is fetched by `visionos/App/RetroRewindPack.swift`, a port of the
  Quest's `RetroRewindPack.kt`: Retro Rewind's own feeds
  (`RetroRewindInstall.txt`, `RetroRewindVersion.txt`,
  `RetroRewindDelete.txt`), the base zip unpacked into a staging folder and
  swapped in, updates applied in order with their deletions, `version.txt`
  written last; only the zip's `RetroRewind6/` subtree is kept. Zips are read
  with `ZipArchive.swift` (central directory, ZIP64, raw deflate through the
  Compression framework), streamed to disk from a temporary download. One
  thing the Quest and PC launchers do that this one cannot: rebuild the game
  when the pack's `Code.pul` changes (the translator binds the mod, and the
  base translation's inlining, to that file). So the configure records the
  pack version and the `Code.pul` SHA-256 the translation used
  (`RetroRewindBuild.swift`, generated from `App/RetroRewindBuild.swift.in`),
  the launcher installs the pack only up to that version, and a pack whose
  `Code.pul` differs is shown as a mismatch and not played until the app is
  rebuilt from it.
- **SDL without a window.** Aurora selects SDL's `offscreen` video driver and
  renders through a detached `CAMetalLayer` (`aurora-main/lib/dawn/MetalBinding.mm`,
  `aurora-main/lib/window.cpp`): SDL never touches UIKit, so the game thread
  need not be the main thread. Aurora's frame worker stays enabled here
  (`aurora.cpp`), unlike macOS; the headset owns the display.
- **Guest memory.** The flat guest space is reserved at 480 GiB
  (`runtime/include/guest_flat_memory.h`); `vm_allocate` replaces
  `mach_vm_allocate`, whose header the SDK refuses; the alias backing files go
  to `TMPDIR` (`guest_flat_memory_macos.cpp`). The app carries the
  `extended-virtual-addressing` and `increased-memory-limit` entitlements
  (`visionos/App/WiiCompiledVision.entitlements`). The translated code takes
  the flat path (plain loads and stores at the fixed base): the Darwin backend
  used to fall back to the checked path (`Memory::Read*`/`Write*`, a bounds
  check, a page-table lookup and a deferred-read check per access) because
  Apple Silicon's 16 KiB pages are larger than the Wii's 4 KiB, which cost the
  video-background menus half their frame rate (28 FPS on the cup select) and
  slowed every memory-heavy guest routine. The backend now protects deferred
  EFB-read destinations at 16 KiB granularity and, on the fault the runtime's
  POSIX handler routes to `HandleAccessViolation`, materializes every pending
  read sharing the page before reopening it; the MMIO window is `PROT_NONE`
  and a read there is reported fatally, as elsewhere. The executable-write
  guard is not implemented (code and data share 16 KiB pages).
  `MKW_CHECKED_GUEST_MEMORY=1` restores the checked path.
- **Directories.** The app's `Documents/WiiCompiled` folder holds `Config.toml`,
  `DATA` (the extracted disc), `NAND` and `Logs`; it is visible in the Files
  app and through Finder file sharing (`UIFileSharingEnabled`). The Mii parts
  the Miis tab downloads live apart from it, in `Application Support/MiiRendering`. Bundled
  resources (`wii_bootstrap/`, `dsp_coef.bin`, `initial_pipeline_cache.db`)
  live at the bundle root, where `RuntimePlatform::ExecutableDirectory()` finds
  them (`host_platform.cpp`). The runtime's transcript is mirrored to the
  unified log (subsystem `org.wiicompiled.vision`) since stdout goes nowhere on
  a device.
- **Blob assembly.** A translation made on Windows or Linux carries
  PE/COFF or ELF section directives in `data_sections_init_blobs.S`;
  `PublicProducts.cmake` rewrites them for Mach-O and adds the `_`-prefixed
  label aliases (`generate-data-init --target-os macos` emits that syntax
  directly).

### The app

`visionos/App/` is a SwiftUI app: a launcher `WindowGroup` with a Play tab
(the game picker, disc status, the folder paths, Retro Rewind's pack, Play), a
Profiles tab when the app carries Retro Rewind, a Miis tab and a Settings tab,
and an `ImmersiveSpace` whose content
is a `CompositorLayer` (dedicated layout, `bgra8Unorm_srgb`, `depth32Float`,
foveation off). The layer's `LayerRenderer` is passed to the provider and the
game thread starts; the launcher window then closes itself, and the game's
audio is anchored to the listener rather than to that window so it plays on.

The Settings tab mirrors the Quest launcher's Settings page
(`android/.../launcher/SettingsPage.kt`) for what the Vision Pro supports:
camera, headset (render scale, frame interpolation), virtual screen, rendering,
controls, volume, files and diagnostics. It edits `Config.toml` with
`visionos/App/TomlConfig.swift`, a port of the Quest's `TomlConfig.kt` that
follows `RuntimeConfigFile::WriteSetting` line for line, so comments and keys it
does not know survive. Every change is saved at once and applies at Play. A watchdog polls the bridge: when the runtime returns, the
launcher says so; when the immersive space is dismissed (the layer is
invalidated) the app asks the runtime to quit, since it would otherwise carry on
rendering to an invisible mirror. **A second run needs a relaunch of the app**:
the runtime keeps process-wide state (the fixed guest reservation, HLE
singletons) a second start would trip over.

The Miis tab is the Quest launcher's My Miis (`docs/quest-port.md`, "My Miis"),
ported to Swift, so a player can go online under their own Mii instead of a
guest. `MiisView` lists the Miis of the Wii's Mii database in the game's NAND,
`shared2/menu/FaceLib/RFL_DB.dat`, favourites first, and makes, edits,
duplicates, imports, exports and deletes them; `MiiEditorView` is the editor,
with the same ten pages, ranges and value displays as the Quest's and the PC's.
The game's own Mii library reads that file, so a Mii made here is offered in
the game when a licence is created and in License Settings > Change Mii, in
both games: Retro Rewind's Riivolution save redirect covers only the title's
data folder. The NAND is found as the runtime finds it: `[paths] nand_root`
resolved against `Config.toml`'s folder, else `Documents/WiiCompiled/NAND`
(`GameStorage.nandDirectory`, `RuntimeNandPath` in `nand_path.h`).
`MiiDatabase.swift` is `MiiDatabase.kt`: a new NAND has no database, so the tab
creates the Wii's empty one the first time it opens (the hidden database's
10,000 entries linked to nothing, `0x7FFF`); every change checks the file's
CRC-16/XMODEM first and refuses a corrupt file, and writes through a temporary
file. A Mii is the 74-byte block of the PC's `MiiSerializer` (`Mii.swift`),
which, like the PC's and the Quest's, writes zeros in the bits the format leaves
unused, where a Wii may have stored something.

A Mii made or duplicated here gets a new ID and this console's system ID, from
the MAC address the runtime derives from `setting.txt`'s serial
(`RuntimeConsoleIdentity::FromSerial`). The Quest asks the player to start the
game once so that the runtime writes that file; here, where each game run needs
the app relaunched, the launcher writes it the first time a Mii is made, as
`RuntimeNandSettings::Ensure` would (the serial from the clock, Dolphin's PAL
fields, the same encryption, `MiiIds.ensureConsoleMac`), and never replaces an
existing file, even a damaged one. The runtime then finds a valid file and
keeps it. An imported `.mii` gets the PC's import address and, like any Mii from
another console, a globe. A tap selects one Mii; after **Select** in the title bar,
taps add and remove Miis, where the Quest uses a long press. Each Mii also has
a context menu with its actions. Export writes the `.mii` files to a temporary
folder and moves them where the player chooses (SwiftUI's `fileMover`); Import
takes any number of files. Changes are refused while the game runs.

The pictures come from the Quest's Kotlin renderer ported to Swift
(`MiiRenderer`, `FflResource`, `MiiBodies`), framed as the PC's `face`
pictures, with the Quest's one deliberate fix (a beard in the facial hair
colour). `MiiRenderResource` downloads FFL's Mii parts once from the Internet
Archive's copy of Miitomo's `AFLResHigh_2_3.dat` (a 4.4 MB zip, unpacked with
`ZipArchive`) and the 3DS body models (29 KB) from WheelWizard's repository,
checks each against its SHA-256, and keeps them in the app's Application
Support folder, out of the Files app and of backups: they are Nintendo's and
never in the app. Until then Miis show as silhouettes and the editor's choices
as numbers. `MiiImages` draws on two threads and caches by look; the editor's
face is drawn one request at a time, so a burst of changes costs one render.

The Profiles tab is the Quest launcher's My profiles (`docs/quest-port.md`), the
PC's UserProfilePage. It shows only when the app carries Retro Rewind, whose save
it reads: online play is Retro Rewind's, on Retro WFC, and the unmodded game has
no server to reach. `ProfilesView` shows the four licences of the save the pack's
Riivolution XML redirects to, `riivolution/save/RetroWFC/RMCP/rksys.dat` in the
virtual SD card around the pack (`RiivoFindXmls`, `GameStorage.retroRewindSave`),
one at a time. Each has its Mii turned three-quarters, its name, its friend code
(derived from the profile ID as `FriendCodeGenerator` does) with Copy, Make
Primary (the licence the tab opens on) and WheelWizard's badges, beside a VR
History page and a Stats page (VR, BR, games won and played). The VR and BR come
from Pulsar's `RRRating.pul` in the NAND when it knows the profile
(`RksysProfiles.swift`, the PC's `RRratingReader`). Retro WFC's public API gives
the rest (`RetroWfc.swift`, `ProfileStore`):
- A licence is Online, with a glow, while its friend code is in one of
  `/api/roomstatus`'s rooms, asked every 40 s while the tab is on screen.
- `VrHistoryView` draws `/api/leaderboard/player/<fc>/history?days=N` with Swift
  Charts, by time or by match, over the PC's periods, and reloads whenever the
  tab reads the save again.
- `/api/leaderboard/player/<fc>` gives the Mii the licence last played with and
  Retro WFC's 64-pixel picture of it, kept in the app's caches.

The Mii shown is the one of the Mii database with the licence's ID, as the PC
looks it up, so a Mii made in the Miis tab shows once a licence takes it; else
Retro WFC's, while its ID is still the licence's; and Retro WFC's picture until
the Mii parts are downloaded. Renaming a licence and changing its Mii are the
game's License Settings. The Quest's and the PC's sidebar card has no
counterpart, since the launcher has no sidebar.

The tab's **Import** and **Export** move a whole profile between devices, which
neither the PC nor the Quest does (`ProfileTransfer`). Retro WFC ties a profile
to its console. Each licence keeps its own login IDs in `rksys.dat`, but the
login also sends the console's serial (`csnum`, `setting.txt`'s CODE and SERNO;
Retro WFC answers error 22005 to a serial the profile does not know), and this
runtime derives the console's MAC address from that serial too. An export is
therefore one zip of:
- Retro Rewind's save folder, and the unmodded game's;
- the Mii database;
- `setting.txt`, `DWC_AUTHDATA` (the console's Nintendo WFC user ID) and
  `keys.bin` when the NAND has one (the device certificate; without it the
  runtime uses Dolphin's default);
- Pulsar's `RetroRewind6` folder: `RRRating.pul`'s VR and BR by profile ID, the
  settings and the ghosts.

The zip mirrors the app's folders (`riivolution/…`, `NAND/…`, which the Quest's
match) and carries a README; `ZipWriter` writes it with the Compression
framework's DEFLATE. Import finds each part by its NAND or SD card path wherever
it sits in an archive, so a zipped Dolphin `Wii` folder and Riivolution folder
read too, and checks each part as the game would: a save's size and CRC-32, the
Mii database's CRC-16, the fields `RuntimeNandSettings::HasIdentity` requires of
`setting.txt`, and `DWC_AUTHDATA`'s 32 bytes. A sheet shows the licences, what
each part does, and the console the headset becomes. It warns that a profile
goes online from one device at a time, or, without a `setting.txt`, that Retro
WFC may refuse it. The current profile is first exported into
`Documents/WiiCompiled/Backups`, and importing that file goes back. Import writes
files over the headset's and never deletes any, so files the archive lacks stay.
The Miis are merged block for block, a Mii with the same ID taking its slot, so
no bit a Wii wrote is lost. Both are refused while the game runs. A profile from
Dolphin keeps its serial but not its MAC address, which Dolphin takes from
Dolphin.ini.

## Building

Players: follow [`visionos-getting-started.md`](visionos-getting-started.md);
`visionos/Make-VisionOS-App.command` runs every step below from the disc image
to the app on the headset, skipping what is already done. There is no
distributable build and there cannot be one: visionOS runs only code signed
by Apple or by the device's own developer, an app may not load code it did not
ship with (so the Quest's on-device compilation has no equivalent), and a
TestFlight build would have to carry the translated game. Each player
therefore makes the app from their own disc, as the Quest and PC launchers
build from the user's disc.

The steps by hand, for development:

Prerequisites: an Apple Silicon Mac, Xcode 16 or later with the visionOS
platform installed, CMake 3.28+, Ninja, Python 3, git, and a .NET SDK, 8 or later,
for the translator (built for .NET 8, it rolls forward to a newer runtime when 8
is absent). An Apple ID (a free personal team suffices for a headset paired
with the Mac) for signing.

1. **Translate the game** exactly as for the desktop:
   `docs/building-macos.md`, steps 1 to 5 (extract the disc, build the
   translator, translate, `generate-data-init`, `emit-build-shards`). The
   translation is platform-neutral; `generated/build_shards/shards.cmake` must
   exist. For Retro Rewind, include step C: download the pack
   (`RetroRewindInstall.txt` on `update.rwfc.net` names the current full zip;
   only its `RetroRewind6/` folder matters), stage its `Code.pul`, translate
   the base *with it staged* (the translator refuses to reuse a base
   translation made without the mod's `Code.pul`), then `translate-mod` and
   `emit-build-shards` with `--resolved-profile`/`--retro-cpp-dir`.
2. **Build Dawn for the visionOS SDK** (once, cached under
   `.scratch/visionos-dawn/`; compiles all of Dawn and Tint, Metal only):

   ```bash
   visionos/Build-VisionOSDawn.sh            # device
   visionos/Build-VisionOSDawn.sh --simulator
   ```

3. **Build the app**:

   ```bash
   visionos/Build-VisionOS.sh --team <TEAMID> [--retro-rewind-dir <RetroRewind6>] [--simulator] [--install]
   ```

   This configures `visionos/CMakeLists.txt` with the Xcode generator (the
   runtime tree is a subdirectory of that project), builds
   `WiiCompiledVision.app` under `build-visionos/Release-xros/`, and with
   `--install` puts it on the paired headset with `devicectl`. `--open` opens
   the generated Xcode project instead, for signing setup or debugging.
   Retro Rewind is embedded whenever the translation has it;
   `--retro-rewind-dir` names the pack folder the mod was translated from, so
   the app knows which pack version to install on the headset
   (`--without-retro-rewind` leaves the mod out).

   By hand:

   ```bash
   cmake -S visionos -B build-visionos -G Xcode \
       -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros \
       -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0 \
       -DAURORA_DAWN_PACKAGE_URL=file://$PWD/.scratch/visionos-dawn/dawn-visionos-arm64.tar.gz \
       -DMKW_VISIONOS_TEAM=<TEAMID> \
       -DMKW_VISIONOS_RETRO_REWIND_ROOT=/path/to/RetroRewind6
   cmake --build build-visionos --config Release --target WiiCompiledVision -- -allowProvisioningUpdates
   ```

4. **Game files.** Launch the app once so it creates `Documents/WiiCompiled`
   with a first `Config.toml` (VR on, `dvd_root = "DATA"`), then copy the
   extracted PAL disc (the folder holding `sys/` and `files/`) into `DATA` with
   the Files app or Finder file sharing. For Retro Rewind, pick it in the Play
   tab and press **Download Retro Rewind**: the launcher fetches the pack
   (about 2 GB) from Retro Rewind's server into `RetroRewind6` beside `DATA`,
   at the version the app was built for, and offers updates up to that version
   later. The pack is needed on top of the disc, as on the other platforms.

Sideloading with a free Apple ID re-signs weekly and allows three apps on the
device; the capabilities used here (Extended Virtual Addressing, Increased
Memory Limit) are ordinary Xcode capabilities.

## Validation status

Done on an Apple Silicon Mac with Xcode 27 (visionOS 27 SDK):

- `visionos/Build-VisionOSDawn.sh` builds the pinned Dawn revision for
  `CMAKE_SYSTEM_NAME=visionOS` unpatched (Metal only, 19 MB static archive,
  Mach-O platform `xros`, `SharedTextureMemoryIOSurface` present).
- `cmake -S runtime -G Xcode -DCMAKE_SYSTEM_NAME=visionOS ...` with that
  package configures (`MKW_BUILD_PRODUCTS=OFF`, no translation at hand), and
  `aurora_core` (including `metal_interop.cpp`, `gpu.cpp`, `MetalBinding.mm`,
  `window.cpp`, `aurora.cpp`), `mkw_openxr_visionos` (all 49 exported `xr*`
  symbols) and `mkw_platform` build without warnings from the new sources.
- Syntax-checked against the same SDK (`clang++ -fsyntax-only`, target
  `arm64-apple-xros2.0`): `openxr_metal.mm`, `openxr_integration.cpp`,
  `openxr_input.cpp`, `visionos_host.mm`, `host_platform.cpp`,
  `guest_flat_memory_macos.cpp`, `main.cpp`, `settings_overlay.cpp`.
- The Swift sources type-check with `swiftc -typecheck` against the SDK.
- The translator, built for .NET 8 with `RollForward` set to `Major`, builds
  with the .NET 10 SDK and runs where the .NET 10 runtime is the only one
  installed, as with Homebrew's current `dotnet-sdk` cask alone. There, the
  base game's `translate-recursive` output (source bundle, metadata,
  mod-awareness file) is byte-identical to the one made on .NET 8.
- With a base-game translation (29,637 functions, 72 shards),
  `visionos/Build-VisionOS.sh --team <id>` produces a 119 MB
  `WiiCompiledVision.app` that passes `codesign --verify --deep --strict`, with
  the two kernel entitlements and the bundled resources in place. Two Xcode
  facts shaped `visionos/CMakeLists.txt`: every Mach-O in the project would be
  signed (so signing is disabled for the library targets and enabled for the
  app alone), and a post-build copy lands after the seal (so the resources are
  bundle resources, the bootstrap folder as a folder reference).

- **Simulator** (visionOS 27, Apple M1 Max): the app installs with `simctl`,
  the disc copied into its container, and with `simctl launch ... --autoplay`
  the game boots into the immersive space and renders the title sequence on
  the virtual screen (3840 x 2160 per simulated eye). ARKit reports hand
  tracking unsupported there, so the provider runs head tracking only.
- **Device** (Apple Vision Pro, visionOS 26.6.1, wired): installed and
  launched with `devicectl`, the disc pushed into the app's data container
  with `devicectl device copy to`. The OpenXR session reached FOCUSED, eyes are
  1888 x 1792, hand tracking was granted, the Wii Remote pointer (hand aim pose)
  reached the menu screen, the settings panel got its quad layer, and a race
  started with the stereo GX replay prepared for both eyes and submitted as a
  projection layer.

Two runtime facts surfaced on the way and are now handled: globals in the
settings overlay load the config during static initialisation, before any Swift
runs, so the bridge fills `dvd_root` into the cached config as well as the file
(`visionos_host.mm`); and SDL ties its virtual-joystick driver, which relays
OpenXR input to the game, to HIDAPI, which it turns off on visionOS, so
`aurora-main/cmake/AuroraSDL3Patches.cmake` loosens that dependency.

- Frame timing (`--fpslog` launch argument, Aurora's five-second frame log with
  GPU pass timings and the GX thread's costliest records): with flat guest
  memory the cup-select menu went from 28.5 FPS (game thread bound, GPU idle at
  4.4 ms) to a locked 60, and a Ghost Valley 2 race holds 60 FPS in stereo with
  a 5 to 6.5 ms GPU span per frame.

- The Miis tab: the Swift renderer draws what the Quest's Kotlin one draws,
  pixel for pixel. On the real Mii parts, 555 of 555 pictures were identical to
  the Kotlin sources' own output: 164 Miis covering every part and colour, with
  and without bodies, turned three-quarters and whole, and every flat part's
  choice icons. The Quest's `MiiDataTest`, `MiiDatabaseTest`, `MiiIdsTest`,
  `MiiRendererTest` and `MiiBodiesTest` pass on the Swift port.
  `MiiIds.encodeSettings` writes the same bytes as `RuntimeNandSettings::EncodeNew`
  on 3,005 serials, and the runtime's `Read`, `HasIdentity` and `Ensure` accept
  the launcher's `setting.txt` and leave it as it is. In the visionOS simulator
  the tab created the database, drew the list and the editor's pages, and made,
  renamed and saved Miis; the new Mii carried the system ID the runtime derives.
  On an Apple M1 Max a list picture takes 4.5 ms to draw and the editor's
  600-pixel face 14 ms. On the device, Miis made in the tab were offered in the
  game.
- The Profiles tab: the Quest's `RksysProfilesTest` and `RetroWfcTest` pass on
  the Swift port. On a Quest's Retro Rewind save it read the licence, its
  friend code, `RRRating.pul`'s VR and its Mii from the Mii database. In the
  simulator it showed them with Retro WFC's live VR history, and with faked
  data the online glow and every badge.
- Profile Import and Export, on copies of a Quest's data and of the headset's:
  - The Quest's export (14 files, 262 KB) imported onto the headset moved its
    save, identity, Wi-Fi login and ratings. The headset's own ghost and Mii
    stayed, and the Quest's Mii replaced the one with its ID block for block.
  - The backup restored the headset's identity and save.
  - A zipped Dolphin layout read; a path leaving the archive was ignored, and
    damaged parts were refused.
  - In the simulator the confirmation sheet, the import and Export's
    destination picker ran.
  - Neither the Profiles tab nor its Import and Export has run on a headset yet.

Not done: comfort tuning of the projection quad depth, the gesture thresholds,
`render_scale`. Things to expect to tune first on hardware: the constant depth the
projection quads are drawn at (3 m in `xr_visionos_compositor.mm`, which sets
how the compositor reprojects late frames), the gesture thresholds in
`xr_visionos_input.mm`, and the default `render_scale` (1.0; the M2 is far
stronger than the Quest's XR2 but drives two 1920 x 1824 eyes at 90 Hz).

## Known gaps and next steps

- Bare-hand driving is untuned on this headset: the grasp thresholds
  (`kGraspOpenRadians`, `kGraspClosedRadians` in `openxr_hand_tracking.h`) and
  the flick speeds were set on a Quest 3; ARKit's skeleton may want different
  ones. Manual drift has no gesture. With the option off, racing needs a
  Bluetooth controller.
- Foveated rendering is off; CompositorServices' rasterization rate maps would
  have to shape Dawn's eye passes, the same problem the Quest solved with a
  Dawn patch (`docs/quest-port.md`, "Foveated rendering").
- The provider composites projection layers as quads at a fixed depth, so the
  compositor's late reprojection is planar. Writing the game's real depth
  would need Aurora to export its depth buffers through the same IOSurface
  bridge.
- One game run per app launch.
- No launcher-side disc extraction: the disc is extracted on a computer and
  copied over.
