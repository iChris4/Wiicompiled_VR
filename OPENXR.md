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

OpenXR remains disabled until requested in `Config.toml`. The file is next to the installed game
configuration and is created with the following defaults:

```toml
[vr]
enabled = false
required = false
render_scale = 1.0
world_units_per_meter = 500.0
hud_distance_meters = 2.0
hud_width_meters = 2.4
hud_virtual_screen = true
stop_at_display_copy = true
skip_copy_clears = true
```

Set `enabled = true`, close the game completely, and start it again. These settings are read only
at launch. The in-game F10 settings bar also exposes the enable switch, but a restart is still
required.

`required = false` is the safe default: an absent runtime, disconnected headset, unsupported GPU,
or graphics-binding failure is logged and the game continues in ordinary desktop mode. Set it to
`true` only when a failed VR startup should stop the game with an error.

`render_scale` scales the per-eye size recommended by the OpenXR runtime.
`world_units_per_meter` controls the scale of headset translation in the game world.
`hud_distance_meters` and `hud_width_meters` place and size the virtual screen. They are read at
launch and govern both the menu screen and the in-race 2D screen, so 2D content keeps its place
across the transition. `hud_virtual_screen` decides whether the race's 2D layer uses that screen;
it is live and can be flipped from the F10 settings bar.
`stop_at_display_copy` ends eye replay at the final `GXCopyDisp`, matching the frame shown on the
desktop. `skip_copy_clears` independently suppresses the EFB reset performed after a copy. Both
default on and can be changed live from the F10 settings bar for diagnostics.

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
asynchronous menu/race transition cannot replay race transforms over unsafe content. A pause or
minimized window can withdraw an unencoded packet and end that compositor frame without layers; an
encoded-work stall requests teardown at Aurora's next safe producer boundary. All OpenXR session
and swapchain calls remain on their owning thread.

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
