# Experimental OpenXR VR

WiiCompiled has an opt-in OpenXR rendering path. The first functional backend is Windows D3D12.
It asks the OpenXR runtime for the required GPU before Aurora creates Dawn, then copies each eye
on that same D3D12 device and queue into the acquired OpenXR swapchain images. Eye submission
stays on the GPU; there is no CPU texture readback and no second graphics device.

This is an experimental renderer, not yet a release-ready VR mode.

## Requirements

- A Windows OpenXR runtime selected as the system's active runtime.
- A connected headset supported by that runtime.
- A D3D12-capable GPU and driver accepted by both OpenXR and Dawn.
- A build made with `MKW_ENABLE_OPENXR=ON`, which defaults on for Windows and off elsewhere while
  the Vulkan bridge remains capability-gated.

For managed installation, use [WheelWizard VR](https://github.com/iChris4/WheelWizard_VR/releases/latest)
and enable **Settings → Other → WiiCompiled (beta) → Enable WiiCompiled OpenXR VR (beta)**.
The launcher sets `vr.enabled=true`, `vr.required=false`, and `video.graphics_api="d3d12"` before
each VR launch, preserving other preferences. Its portable configuration lives at
`RecompVR/UserData/Config.toml` beneath WheelWizard's data folder. Normal graphics settings remain
in `Recomp/UserData/Config.toml`. Both backends use the normal installation's effective NAND.

Standalone launches remain opt-in. `Config.toml` is created with the following defaults:

```toml
[vr]
enabled = false
required = false
mirror_view = "normal"
frame_interpolation_fps = 0
render_scale = 1.0
world_units_per_meter = 500.0
hud_distance_meters = 2.0
hud_width_meters = 2.4
hud_virtual_screen = true
stop_at_display_copy = true
skip_copy_clears = true
first_person = false
first_person_units_per_meter = 30.0
first_person_head_up_meters = 3.0
first_person_head_forward_meters = 0.0
first_person_head_right_meters = 0.0
first_person_hide_driver = true
first_person_hidden_model = 0
first_person_rotation = "yaw"
```

Set `enabled = true`, close the game completely, and start it again. These settings are read only
at launch. The in-game F10 settings bar also exposes the enable switch, but a restart is still
required.

`required = false` is the safe default: an absent runtime, disconnected headset, unsupported GPU,
or graphics-binding failure is logged and the game continues in ordinary desktop mode. A temporary
notification explains the failure; the message remains available under **F10 → VR**. Set it to
`true` only when a failed VR startup should stop the game with an error.

`mirror_view` chooses what the desktop window shows while the headset is running: `"normal"`
keeps the ordinary desktop view, `"both"`, `"left"` and `"right"` mirror the headset's eyes, and
`"none"` blacks the window out. It is live and can be changed from the F10 settings bar, where it
sits directly under the enable switch as *Desktop view*. Menus reach the headset as a virtual
screen carrying the desktop image itself, so there is no separate eye view to mirror there and the
three eye choices show that same image; only `"none"` differs. The F10 bar is drawn over whichever
image is chosen, so the setting can always be changed back.
Eye mirror modes retain the last eye image when a desktop frame has no new XR packet, so they
do not alternate with the normal camera. `"none"` also stays black between XR packets.

**F10 > VR > VR frame interpolation (experimental)** offers **Off, Auto, 72, 90, 120** and
applies immediately. `frame_interpolation_fps` stores `0` for Off (the default), `1` for Auto,
or the selected rate. The earlier `frame_interpolation = true` checkbox migrates to Auto.
Auto renders at the headset's display deadlines; the numbered choices cap the rate of new
stereo frames. They do not change the headset's physical refresh setting. For VDXR with Virtual
Desktop set to 90 Hz, select Auto or 90. The menu shows both the detected headset rate and the
rate of newly rendered VR frames, excluding repeated images. Menus and other virtual-screen
scenes continue at the game's rate; assess interpolation during an immersive race.

VR interpolation is independent of **Graphics > Race frame interpolation**. The simulation,
physics, audio and VI remain at 60 Hz. Scene motion is delayed by one game frame (about 16.7 ms)
to interpolate between known transforms; each rendered eye pair uses a fresh predicted head
pose. This needs enough GPU headroom to render both eyes at the target rate, and carries the
desktop interpolator's experimental artifacts, especially for unmatched or changing geometry.

Refresh detection uses `XR_FB_display_refresh_rate` when available and the OpenXR predicted
display period otherwise. Interpolation requires `XR_KHR_win32_convert_performance_counter_time`
to relate those display deadlines to the game's clock; the menu reports if it is unavailable.
The old Eager Frame Heartbeat option has been removed and existing `eager_frame_heartbeat`
settings are ignored. Completed rendering wakes the XR thread immediately. A 50 ms keep-alive
still protects pauses and window dragging without eager repeats during rendering.

`render_scale` scales the per-eye size recommended by the OpenXR runtime.
`world_units_per_meter` controls the scale of headset translation in the game world.
`hud_distance_meters` and `hud_width_meters` place and size the virtual screen. They are read at
launch and govern both the menu screen and the in-race 2D screen, so 2D content keeps its place
across the transition. `hud_virtual_screen` decides whether the race's 2D layer uses that screen;
it is live and can be flipped from the F10 settings bar.
`stop_at_display_copy` ends eye replay at the final `GXCopyDisp`, matching the frame shown on the
desktop. `skip_copy_clears` independently suppresses the EFB reset performed after a copy. Both
default on and can be changed live from the F10 settings bar for diagnostics.
`first_person` and the `first_person_*` values are the first-person camera described below. All
four are live and are also exposed in the F10 settings bar.

## The first-person camera

By default the headset sits where Mario Kart's own chase camera sits, and `world_units_per_meter`
of 500 presents the race as a small diorama on a table. Turning on `first_person` moves the camera
to the local driver's head instead, and switches the world scale to
`first_person_units_per_meter`, whose default of 30 is what makes the race read life-size from the
seat. It is a matter of taste rather than a property of the game, so the F10 bar exposes it.

The kart is selected through the game's local-screen-to-racer mapping, including online races
where your racer is not slot zero. First person requires a locally controlled racer; spectating
another racer keeps the game's own camera.

The game's own transforms are never modified. Each guest frame the runtime reads the race camera's
view matrix and the player kart's physics pose and derives one affine transform from the recorded
view space into the space to render from. That transform is published with the sealed frame, and
the renderer composes it onto every perspective draw's model-view matrix, alongside the headset's
own per-eye delta. The kart's *physics* pose is used deliberately, not the animated model: an
animated frame would bob and lurch the camera.

`first_person_rotation` decides where the view's orientation comes from, mirroring DolphinXR's
camera-anchor modes. `"yaw"`, the default, keeps the horizon level through a chase-camera tilt or a
banked corner. `"yaw_pitch"` adds the kart's climb, so a slope or a wheelie tips the view while a
banked corner still never rolls it. `"full"` takes the kart's whole orientation, banking included.
All three are the same construction from a forward and an up axis, differing only in which pair
they take: pairing a forward with world up is what removes roll. The headset always adds free look
on top of whichever is chosen, and only the translation onto the head is common to all three.

The head's place in the kart is `first_person_head_up_meters` and its two companions, measured in
the kart's own frame; the F10 sliders exist because the comfortable value is a matter of taste and
is best judged from inside the headset.

The mode engages only in a single-screen race, the same content that already qualifies for
immersive stereo. Menus, split-screen, and the virtual-screen fallback are unaffected, and so is
the desktop mirror, which keeps showing the game's ordinary third-person view. If the kart or
camera cannot be read the camera stays where the game put it rather than guessing.

Your own driver sits exactly where your eyes are, so their head would fill the view.
`first_person_hide_driver` removes it. The game applies one draw byte across every model of a kart
and to its body, so clearing it outright takes the vehicle along with the driver;
`first_person_hidden_model` names a single model to hide instead. On PAL `RMCP01` a kart carries two
models and index `0` is the driver, which is the default: the character goes and the vehicle stays.
`-1` restores the blunt behaviour and hides everything. An index the kart does not have hides
nothing, and the log reports how many it has when the mode engages. The F10 bar presents this as
two toggles, "Hide driver" and "Hide driver and kart", alongside a button that restores every
first-person default. Both settings touch your own kart
only, so the other racers are untouched, and the original values are restored when first person
stops or the race ends. This is the one place the first-person camera modifies the game rather than
only reading it.

One limitation is worth knowing: Mario Kart still culls the scene from its own chase camera, so a
wide head turn in first person can reveal the edge of what the game decided to draw. As with the
rest of the race instrumentation, the object offsets this reads are specific to the project's
supported PAL `RMCP01` translation.

## Presentation policy

The runtime deliberately fails safe instead of guessing which Mario Kart camera is active:

- Menus, loading screens, unclassified scenes, and multiplayer render on a head-locked virtual
  screen.
- A PAL `RMCP01` race scene switches to immersive stereo only after translated-code observers
  confirm exactly one distinct race camera for the current GX frame.
- Leaving the race or observing zero or multiple cameras immediately returns presentation to the
  virtual screen. Session/runtime loss safely tears down XR and continues on the desktop mirror.

Aurora records the original GX frame once and replays it for both OpenXR eyes. Perspective GX draws
receive asymmetric headset projections, while the game's 2D layer goes on a fixed virtual screen
(see below). Menus and unsafe whole scenes use the virtual-screen path.
Head pose is sampled by the OpenXR pacing thread, while Aurora's frame worker consumes a
short-lived immutable stereo packet. Each sealed GX frame and immersive packet carry the same
policy-generation tag; a mismatch is rendered in mono and the acquired XR frame is canceled, so an
asynchronous menu/race transition cannot replay race transforms over unsafe content.

With VR interpolation enabled, Aurora retains each sealed race's command stream and matched
previous/current transform uniforms. New OpenXR packets wake the frame worker between game
frames. It interpolates at the requested display time, then applies that packet's head pose and
the scene anchor to both eyes. Native offscreen effects and the 2D HUD retain their game-frame
updates. A mid-frame EFB readback invalidates retained GPU data; a policy-tag mismatch rejects
the replay. Missing matches use current transforms, and stalls clamp at the last known pose
instead of extrapolating. The ordinary desktop interpolation settings remain independent.

The D3D12 pacing thread retains the last completed projection or virtual-screen layer and
resubmits it during stalls, including while moving the desktop window,
pausing, or minimizing. The scene freezes until rendering resumes; the compositor can still
reproject the retained image for head movement. Repeated layers keep their original render poses
and field of view, paired with the new compositor display time. Two pairs of eye swapchains keep
the retained image separate from pending or canceled rendering (at the cost of additional GPU
memory). Unencoded packets can be withdrawn after 50 ms; encoded work retains its images while
the pacing thread continues submitting the last completed layer. A stall alone no longer requests
desktop fallback after 250 ms.

Before the first valid image, when OpenXR requests no rendering, or after a session/reference-space
change invalidates the retained content, frames can still have no layers. Actual runtime or GPU
submission failures retain the safe teardown path. This does not detect black images rendered by
the game itself, and cannot keep submitting if the entire process or XR runtime is suspended.
All OpenXR session and swapchain calls remain on their owning thread.

## The race's 2D layer

The minimap, race position, item roulette, lap times and the rest of the game's orthographic layer
would otherwise be stretched across each eye's entire field of view. With `hud_virtual_screen` on
they are instead placed on a rectangle fixed in the recorded camera's own frame, `hud_distance_meters`
ahead of it and `hud_width_meters` across, its height following the aspect ratio the game is
presenting at. The screen stays where the camera puts it, so looking around moves the view across it
rather than dragging it along.

An orthographic GX projection is affine, so the draw's clip position is already its position on the
flat frame. Replay folds three further steps into that same projection matrix, one per eye: the
draw viewport into full-frame coordinates, the frame position onto the screen rectangle, and the
screen through that eye's view and OpenXR frustum. The draw's own position matrices are left alone.

Depth uses the equivalent of DolphinXR's Exact Screen Depth path. A replay-only shader variant
carries the draw's original GX depth through a flat-interpolated value and explicitly writes it at
the fragment, including the draw's recorded viewport depth range. The reprojected geometry itself
is parked at mid-depth for clipping. This avoids the view-dependent perspective-divide rounding
that otherwise breaks equal-depth `LEQUAL` ordering and causes overlapping menu/HUD elements to
z-fight.

Two classes of draw are deliberately left on their recorded transforms: native framebuffer effects
(bloom and the rest of the post-processing chain, recognised by sampling a freshly produced,
reduced or blended-back EFB copy), which belong to the rendered image rather than to the game's 2D
layer, and any draw whose matrix is not actually affine. Retained one-shot EFB bakes such as Mario
Kart Wii's minimap are treated as game art and remain eligible for the screen. A reprojected 2D draw
uses the full eye viewport and scissor because its recorded rectangle no longer describes where it
ended up; its original viewport is folded into the projection instead.

## Backend status

| Backend | Status |
| --- | --- |
| Windows D3D12 | Implemented: same-adapter, same-device asynchronous OpenXR submission. |
| Linux Vulkan | Capability-gated scaffold. The pinned Dawn package does not expose the complete native Vulkan instance/device/queue context needed for safe same-device OpenXR interop, so the runtime logs the limitation and falls back to desktop rendering. |
| Other platforms | Not wired yet. |

The Vulkan path intentionally does not create an unrelated Vulkan device or use a CPU readback as
a workaround. It accepts a future explicit Dawn native context, including external queue locking,
so it can be enabled once Aurora exposes those handles safely.

### Interpolation validation

The GX tests cover retained transform endpoints with desktop interpolation off and continuous
sampling at 72/90/120 Hz. `mkw_frame_interpolation_pacing_tests` covers fixed-rate scheduling,
live changes, stalls and configuration migration; `mkw_openxr_replay_tests` exercises swapchain
ownership and retained-layer submission without a headset.

For a Windows GPU check, configure Aurora with its tests enabled and
`AURORA_GPU_SMOKE_TESTS=ON`, then build/run `stereo_frame_worker_smoke`. This feeds the actual
renderer a 60 Hz GX stream and an independent 90 Hz stereo provider. The development check
produced 359 new stereo submissions in 4 seconds (89.7 FPS). This verifies submission cadence,
not full-race performance or visual quality on a headset. Pass a draw count, for example
`stereo_frame_worker_smoke 2000`, to stress uniform preparation and renderer/producer overlap;
`stereo_frame_worker_smoke 2000 0` checks native stereo with interpolation Off.
The test checks that the producer stays above 55 FPS as well as checking headset submissions;
replaying an old scene more often must not hide a slowed simulation. Validate actual races in VDXR at 90 Hz with
Auto/90 selected, including race entry/exit, first person, recentering and pauses.

Stereo uniform calculations use cached CPU memory, followed by a single write into the upload
buffer. Reading or modifying matrices directly in D3D12 upload memory can be extremely slow,
especially with many character draws; see Microsoft's [Map guidance](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map).
Retained interpolation reserves eye ranges at seal time and fills them once at the headset sample
time. VR interpolation also releases the producer after sealing so eye encoding can overlap the
next game frame, as it does with desktop interpolation.

## Current limitations

- Only the project's supported PAL `RMCP01` translation has race instrumentation addresses.
- Motion-controller/Wii Remote emulation and OpenXR action bindings are not implemented yet; use
  the existing game-controller input path.
- Dedicated Quest, Android, and Apple visionOS packaging is not implemented. The static recompilation
  architecture avoids a runtime JIT, but each platform still needs an Aurora graphics bridge,
  windowing/lifecycle work, and packaging.
- Scene-specific comfort options, culling fixes, replay/spectator classification, and a broader VR
  settings UI beyond the current enable/replay controls are future work.
- The desktop window remains available as a mirror/fallback.

OpenXR diagnostics are written to the normal run log under
`%LOCALAPPDATA%\WiiCompiled\Logs`. Search for `OpenXR` when reporting a startup or submission
failure.
