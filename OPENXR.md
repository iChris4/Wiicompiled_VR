# Experimental OpenXR VR

WiiCompiled has an opt-in OpenXR rendering path. The first functional backend is Windows D3D12.
It asks the OpenXR runtime for the required GPU before Aurora creates Dawn, then copies each eye
on that same D3D12 device and queue into the acquired OpenXR swapchain images. Eye submission
stays on the GPU; there is no CPU texture readback and no second graphics device. Windows Vulkan is
an opt-in second binding built on the same design: the OpenXR runtime creates Dawn's Vulkan instance
and device (`XR_KHR_vulkan_enable2`) and eyes are copied on that same queue. It needs a custom Dawn
build; see [Windows Vulkan](#windows-vulkan).

This is an experimental renderer, not yet a release-ready VR mode.

## Requirements

- A Windows OpenXR runtime selected as the system's active runtime.
- A connected headset supported by that runtime.
- A D3D12-capable GPU and driver accepted by both OpenXR and Dawn, or for the opt-in Vulkan
  binding a Vulkan 1.1+ driver plus the custom Dawn described under [Windows Vulkan](#windows-vulkan).
- A build made with `MKW_ENABLE_OPENXR=ON`, which defaults on for Windows and off elsewhere while
  the Vulkan bridge remains capability-gated.

For managed installation, use [WheelWizard VR](https://github.com/iChris4/WheelWizard_VR/releases/latest)
and enable **Settings → Other → WiiCompiled (beta) → Enable WiiCompiled OpenXR VR (beta)**.
The launcher sets `vr.enabled=true` and `vr.required=false` before each VR launch, preserving other
preferences. Its **Graphics API** row picks the binding, DirectX 12 or Vulkan, and keeps that choice;
any other value is repaired to `d3d12` at launch, because OpenXR refuses the rest. Its portable
configuration lives at
`RecompVR/UserData/Config.toml` beneath WheelWizard's data folder. Normal graphics settings remain
in `Recomp/UserData/Config.toml`. Both backends use the normal installation's effective NAND.

Standalone launches start in VR too: this is the VR build, and `required = false` makes a failed
headset startup fall back to the desktop renderer rather than stop the game. `Config.toml` is
created with the following defaults, and a configuration that never mentions `enabled` reads the
same way:

```toml
[vr]
enabled = true
required = false
mirror_view = "normal"
controller_mode = "wii_remote"
frame_interpolation_fps = 0
render_scale = 1.0
world_units_per_meter = 500.0
hud_distance_meters = 2.0
hud_width_meters = 2.4
hud_virtual_screen = true
flat_screen = false
stop_at_display_copy = true
skip_copy_clears = true
single_pass_eyes = true
first_person = false
first_person_toggle_click = true
first_person_seat = "cockpit"
cockpit_units_per_meter = 100.0
first_person_units_per_meter = 50.0
first_person_head_up_meters = 1.5
first_person_head_forward_meters = 0.0
first_person_head_right_meters = 0.0
first_person_hide_driver = true
first_person_hidden_model = 0
first_person_rotation = "yaw_pitch"
steering_wheel = true
native_steering_wheel = true
hand_steering = true
performance_level = "boost"
```

The seven `wheel_*` hand-steering tuning keys are described in
[Steering wheel and hand steering](#steering-wheel-and-hand-steering).

To play this installation on the desktop instead, set `enabled = false`, close the game completely,
and start it again. These settings are read only at launch. The in-game F10 settings bar also
exposes the enable switch, but a restart is still required.

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

## Local multiplayer

During 2-, 3-, and 4-player races, the headset replays Player 1's world in immersive stereo
with head tracking. Keep **F10 > VR > Desktop view** set to **Normal** (`mirror_view = "normal"`)
for the original desktop split-screen layout. No extra multiplayer switch is required.
Menus continue to use the virtual screen.

Only headset replay filters the other players' viewports and expands Player 1 to each eye.
The desktop split-screen partition (the game's `partition_line` layout, one-pixel textured
picture panes on the split boundaries) and full masks of the other panes are omitted from
the eyes; the desktop image keeps them.
Player-local HUD viewports follow Player 1; shared orthographic overlays keep their full-screen
layout on the virtual screen. Framebuffer effects that sample the desktop split-screen image
are omitted from multiplayer eyes, since those textures contain the other cameras too.
The local-screen count is sealed with each frame, including retained VR interpolation frames,
and a layout change invalidates older XR packets.

Multiplayer uses Player 1's game camera. The optional first-person relocation and model hiding
remain single-player-only: guest model visibility changes would also affect the desktop players.

The opt-in `stereo_multiplayer_smoke` D3D12 test reads back both eye images and the desktop EFB
for 1/2/3/4/1-screen transitions with VR interpolation on and off. Actual headset racing still
needs visual validation for course effects, HUD layout, pause/resume, and scene transitions.

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

`render_scale` scales the per-eye size recommended by the OpenXR runtime. It defaults to 1.0 on PC
and 0.8 on the Quest, whose mobile GPU needs the headroom.
`world_units_per_meter` controls the scale of headset translation in the game world.
`hud_distance_meters` and `hud_width_meters` place and size the virtual screen. They are read at
launch and govern both the menu screen and the in-race 2D screen, so 2D content keeps its place
across the transition. `hud_virtual_screen` decides whether the race's 2D layer uses that screen;
it is live and can be flipped from the F10 settings bar.
`flat_screen` (default off) keeps races on that same flat screen, as the menus are, instead of
immersive stereo: the whole race, 3D world and HUD alike, is the game's own picture on the quad, as
in DolphinXR's Flat Screen mode. The first-person camera, hand steering, the lean-back angle, VR
frame interpolation and `hud_virtual_screen` shape only the immersive race view, so none of them
apply while it is on; the right-thumbstick first-person toggle is ignored rather than changing the saved
setting. It is live, as **F10 → VR → Flat Screen mode** (the headset panel's VR tab) and the Quest
launcher's Settings page, and turning it on or off mid-race switches on the next frame through the
presentation policy's safety generation.
`passthrough` (Quest only, default on) shows the room through the headset's cameras around the
menu screen and every other virtual screen, instead of black: an `XR_FB_passthrough`
reconstruction layer submitted under the screen's quad, as PPSSPP VR does, with the blend mode
left `OPAQUE`. An immersive race never shows it, and the cameras are paused for the race; a race in
`flat_screen` is a virtual screen like the menus, so the room shows around it too. It is
live, from the headset panel's VR tab or the launcher's Settings page. The app declares
`com.oculus.feature.PASSTHROUGH`, without which Horizon OS composites nothing for that layer.
So that the room frames the picture rather than black bands, the Quest's menu quad shows only the
part of its eye-sized image Aurora draws into (the desktop snapshot, and the in-eye settings
panel's rectangle), at the same size per pixel, so nothing moves.
`stop_at_display_copy` ends eye replay at the final `GXCopyDisp`, matching the frame shown on the
desktop. `skip_copy_clears` independently suppresses the EFB reset performed after a copy. Both
default on and can be changed live from the F10 settings bar for diagnostics.
`single_pass_eyes` draws each eye in one render pass. The desktop image ends a render pass at every
GX copy, because the copy reads what was drawn before it; an eye samples the copies the desktop
image made and never performs them, so it keeps drawing in the pass it has open, and it leaves out
whatever a later clear of the whole color and depth erases. The picture is the same with less GPU
memory traffic, which a tiled mobile GPU pays for at every split (the "Eye replay plan" log line
reports each new pass structure). It defaults on and is live; turning it off replays one render pass
per recorded pass.
`first_person` and the `first_person_*` values are the first-person camera described below. All
four are live and are also exposed in the F10 settings bar.
`performance_level` is the level asked of the runtime through `XR_EXT_performance_settings` for
its CPU and GPU domains: `boost`, `sustained_high`, `sustained_low`, `power_savings`, or
`default` to leave the runtime's own choice. Standalone headsets clock their cores by this
request (see `docs/quest-port.md`); desktop runtimes rarely offer the extension, and the setting
then does nothing. It is read at launch, and the session log records whether the runtime accepted
it and any later performance notification (a thermal or rendering warning).
`foveation` (Quest only, default `off`) shades the edges of the immersive race view more coarsely:
`off`, `low`, `medium` or `high`, see [Foveated rendering](#foveated-rendering). A session launched
with it off runs without fragment density maps, so going from `off` to a level takes a restart;
between levels, and back to `off`, it is live from the headset panel's VR tab. The launcher's
Settings page has it too.

## Controllers

The headset's tracked controllers reach the game through an OpenXR action set synced on the pacing
thread (`runtime/src/vr/openxr_input.cpp`), which feeds a virtual SDL gamepad that Aurora assigns
to a port like any other. `controller_mode` decides what the game finds on that port, and is live
from **F10 > VR > VR controllers**; the game sees a change as a controller reconnection.

The pacing thread only publishes that gamepad; the game thread writes it to SDL where it already
polls controllers (`OpenXRApplyControllerState`, called from `PAD__Read_HLE` and the overlay's
per-frame work). SDL holds its joystick lock for the length of a device enumeration, and the
Bluetooth Wii Remote rescan (**F10 > Controller settings > Keep scanning**, `wii_continuous_scan`,
off by default) makes SDL close and reopen every HID device twice per scan. Measured at 15 ms on a
plain desk and over 200 ms with a Lighthouse setup's dongles on the bus, which is why the pacing
thread must not wait on it: a frame it holds open that long costs the compositor every display slot
that passes, and `[xr-diag]` reports it as a stalled, late frame with skipped display slots. That
rescan still pauses the *game* thread for as long, so leave it off unless a real Wii Remote is in
use.

`"wii_remote"`, the default, presents them as a Wii Remote with a Nunchuk, the way DolphinXR's
OpenXR Wii Remote does, with buttons adapted from its default `OpenXR Wii Remote` profile for the
Touch controllers. The port is served through KPAD like a Bluetooth remote
(`wii_remote_input.cpp`), so `WPADProbe` reports a Nunchuk and the game runs its own Wii Remote + Nunchuk control scheme:

| Controller | Wii |
| --- | --- |
| Right A | A |
| Right trigger | B |
| Right B | C (look behind) |
| Right stick up / down | 1 / 2 |
| Left X | − |
| Left menu | + |
| Left stick | Nunchuk stick |
| Left trigger | Z |
| Left Y | Settings panel (not a Wii button) |
| Either grip | Takes hold of the wheel (not a Wii button) |
| Right stick click | First-person camera on / off (not a Wii button) |
| Right controller motion and aim | Wii Remote accelerometer and pointer |
| Left controller motion | Nunchuk accelerometer |

Analog inputs count as pressed past half travel. The grips, right stick left / right and the left
stick click press no Wii button, and nothing presses HOME. C sits on right B rather than a grip
because hand steering holds a grip down for a whole corner, and C is the game's look-behind. The
game's Wii Remote rumble vibrates both controllers, subject to the ordinary controller-vibration
switch.

**Motion.** Each XR frame the aim and grip poses are located at the measured current time
(`XR_KHR_win32_convert_performance_counter_time`, `XR_KHR_convert_timespec_time` on Android), not
the predicted display time, whose extrapolation sprays fast wrist motion. The grip's linear
velocity, averaged with one derived from its position, is differentiated over XrTime into
acceleration; gravity is added and the result is expressed in the aim pose's frame. KPAD's
accelerometer axes are the aim pose's `(x, -y, z)` in g: a level controller reads `(0, -1, 0)`,
pointing at the floor `(0, 0, 1)`. Readings saturate at ±3.6 g like the remote's sensor, and a
controller that loses tracking repeats its last reading. The game's own motion detection (tricks,
wheelies) then works on these readings as it would on a remote's.

**Pointer.** The pointer is absolute, as in DolphinXR: the right controller's aim ray is intersected
with the screen the renderer is showing, and the point it meets is where the cursor goes, so there
is nothing to recenter. On the menu screen that is the quad layer, `hud_width_meters` across with
the eye texture's aspect, and the pointer spans the game picture inside it (Aurora letterboxes the
desktop image into the quad and the picture into the desktop image, so a 4:3 picture keeps its
pillarboxes; the Quest crops the quad to the desktop image without changing where it is). During a race it is the 2D layer's screen, `hud_distance_meters` ahead of the latched
race origin and turned by the lean-back angle, with the picture's aspect. With
`hud_virtual_screen = false` the race's 2D layer has no fixed place and the pointer is off. The
game's own pointer switch (`KPADEnableDpd` / `KPADDisableDpd`) is honoured as well.

The hit becomes KPAD's `pos` (−1..1 across the picture, +y down), `horizon` (the controller's roll
on the screen) and `dist` (perpendicular distance in metres, so rotating the controller does not
change it). Like a real remote's camera, the pointer keeps tracking up to 1.9 half-widths and 1.5
half-heights past the picture's centre; a lost hit or an excursion beyond that holds or pins the
cursor for 100 ms before it disappears, so tracking spikes during fast motion do not drop it.
Raw IR camera dots in `KPADGetUnifiedWpadStatus` stay invalid; the game reads the pointer from
`KPADStatus`.

**Settings in the headset.** Left Y opens the settings panel described below; while it is open the
controllers operate the panel and the game sees them idle.

**Hand steering.** With `hand_steering` on, in the first-person cockpit, a grip squeezed near the
steering wheel takes hold of it, and while held the wheel steers through the Nunchuk stick's X axis;
see [Steering wheel and hand
steering](#steering-wheel-and-hand-steering). Turning the wheel moves the controllers, and the game's
own motion detection still reads them, so a sharp enough turn can read as a shake.

`"gamepad"` keeps the controllers one ordinary gamepad read through PAD as a GameCube controller:
A/B → South/East, X/Y → West/North, index triggers → trigger axes, grips → shoulders, thumbsticks
→ sticks (clicks → stick buttons), left menu → Start. Every binding in the F10 controller menu
applies. Left Y is GameCube Y here, so clicking both thumbsticks together opens the settings panel
instead. The right thumbstick click on its own still toggles the first-person camera.

Bindings are suggested for `oculus/touch_controller` (Quest 2, 3 and Pro) and
`khr/simple_controller`. `mkw_vr_wii_remote_tests` checks the accelerometer frame, the pointer
raycast and debounce, the picture placement and the button profile without a headset.

## Settings in the headset

The F10 settings bar is only visible on the desktop window, so the same settings are also offered on
a panel inside the headset, in menus and during an immersive race alike, including on the Quest.
**Press left Y** to open it, and again to close it (with `controller_mode = "gamepad"`, **click both
thumbsticks together** instead); the left controller's menu button and the panel's *Close* button
also close it. It can be opened from the desktop as well, with
**F10 → VR → Show these settings in the headset**.

The panel has the F10 bar's menus as tabs (VR, Graphics, Controllers, Audio, Diagnostics) and a
*Recenter view* button. Aim a controller at it: the cursor goes where you aim, a trigger (or A / X)
selects and drags sliders, and a thumbstick scrolls. Whichever hand last pulled its trigger does the
pointing. Changes apply exactly as they do from the F10 bar, and the two stay in step.

While the panel is open, and until every button has been released after it closes, the game sees
the VR controllers idle: no buttons, no pointer and a remote at rest. Nothing reaches the game from
the panel button, the trigger that clicked *Close*, or the menu press that closed the panel. The
game is not paused, so a race carries on while you change settings. Other controllers (keyboard, desktop
gamepads, Bluetooth remotes) are not affected.

The panel sits centred on the virtual screen, three quarters of its width across (1.8 m with the
default `hud_width_meters`). On a menu that is the anchored menu quad; in a race it is the 2D layer's
screen, `hud_distance_meters` ahead of the latched race origin and turned by the lean-back angle,
whether or not `hud_virtual_screen` places the HUD there. **Recenter view** brings both back in front
of you.

How it is drawn: `settings_overlay.cpp` builds the panel with a second Dear ImGui context of its own,
a 1440 × 1080 canvas at twice the desktop menu's scale with its own font atlas, fed by the pointer
that `openxr_input.cpp` publishes through `vr/openxr_settings_panel.h`. Aurora renders that draw data
into a panel texture once per sealed frame (`aurora-main/lib/stereo_overlay.cpp`). The ImGui backend
keeps a single projection uniform, so the panel's pass is submitted on its own command buffer before
the desktop's ImGui pass of the same frame is recorded.

The panel is shown as a compositor quad layer of its own, submitted over the scene's projection or
menu quad layer. The compositor samples the 1440 × 1080 canvas directly, so its text stays sharp
whatever `render_scale` gives the eyes. Every backend (D3D12, Windows Vulkan, Quest) makes the
panel's swapchain pair the first time the panel opens (two 1440 × 1080 swapchains, plus two shared
buffers on the Quest) and keeps it for the session. Until then nothing is allocated, and while the
panel is closed nothing is copied or submitted. While it is open, each frame hands Aurora one more
target after the eyes: the stereo bridge copies the panel texture into it with the eyes (or a
transparent image on a frame where the panel is not drawn). The layer follows the eyes' swapchain
pairing: the image a frame wrote is shown only once that frame is submitted, so a cancelled frame
never shows an unwritten panel. The quad hangs exactly where the pointer's hits are tested
(`SettingsPanelScreen` in `openxr_integration.cpp`). ImGui's premultiplied output is blended with
`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`, and Aurora leaves the panel out of the eyes
(`aurora_set_stereo_panel_layer`).

If a backend cannot make the panel's swapchains, it logs that once and the panel is drawn into the
eye images instead: through the eye's frustum and `viewFromCenter` onto the screen rectangle for an
immersive eye (including headset-rate interpolated eyes, which reuse the texture), and as a centred
rectangle on a virtual-screen eye image. On the Quest, `adb shell setprop
debug.wiicompiled.panel_layer 0` switches to that path at run time, to compare the two.

Measured on a Quest 3 (base game, a Grand Prix start with the player idle, `render_scale = 0.8`,
60 FPS, eight interleaved rounds per state), the layer costs nothing while the panel is closed. While
it is open, the app's GPU time is 10.5 ms per frame with the layer, against 9.7 ms drawn into the eyes
(9.4 ms closed). GPU load is 74% against 67%, and the compositor's time 1.05 ms against 0.75 ms. Game
and headset frame rates did not change. The compositor redraws the layer at display rate, so the
panel stays steady even when the game drops frames.

`mkw_vr_settings_panel_tests` covers the panel button in both controller modes, the release latch,
selection, scrolling and the canvas mapping; `gx_fifo_tests` covers where the panel lands in each eye
on the fallback path. `mkw_openxr_replay_tests` and `mkw_openxr_vulkan_replay_tests` cover the layer:
nothing made before the panel opens, the panel image of a cancelled frame never shown, no layer while
the panel is closed or has no place yet, and render-first pacing.

## The first-person camera

By default the headset sits where Mario Kart's own chase camera sits, and `world_units_per_meter`
of 500 presents the race as a small diorama on a table. Turning on `first_person` moves the camera
to the local driver's head instead, at one of two seats:

- `first_person_seat = "cockpit"`, the default, sits you at the driver's own eyes, behind the
  steering wheel, at a life-size scale, so the wheel or handlebar is within reach of your hands.
  The eye is measured once per race from the character's head bone, while the kart drives straight,
  undamaged and at normal size, and then frozen; until then the bind pose, or the vehicle's authored
  seat height, stands in. It is kept at least 0.45 m behind the wheel so a long face or a
  leaned-forward riding pose cannot put it over the controls. The world scale is
  `cockpit_units_per_meter` (default 100) multiplied by the character's eye height over 100 units,
  so tall characters sit at a comparable height, and by the player's current size, so a lightning
  strike or a mega mushroom resizes the view, the wheel and the grab reach together. The seat
  follows the simulation's position and driving direction, never the animated chassis, so damage
  spins and tricks do not throw it around.
- `first_person_seat = "custom"` places the head at `first_person_head_up_meters` and its two
  companions in the kart's own frame, at `first_person_units_per_meter`.

Both are a matter of taste rather than properties of the game, so the F10 bar exposes them.

**Toggling it from a controller.** Clicking the right thumbstick turns first person on or off
exactly as the F10 checkbox does, and the choice is saved the same way. It works on the VR
controllers in either presentation (the right controller gives a short tick), and on any other
gamepad while VR is running. A click counts on release, and only if the left thumbstick stayed up
and the settings panel stayed closed throughout, so clicking both thumbsticks to open the panel in
gamepad mode never toggles the camera. A gamepad whose right thumbstick click is bound to a
GameCube control on its port, as a button or in an input expression, keeps it for the game instead.
Toggled in a menu, the change applies from the next race. `first_person_toggle_click = false`, or
the F10 checkbox under the camera toggle, turns the click off. `mkw_vr_camera_toggle_tests` covers
the click rule.

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
camera-anchor modes. `"yaw"` keeps the horizon level through a chase-camera tilt or a banked corner.
`"yaw_pitch"`, the default, adds the kart's climb, so a slope or a wheelie tips the view while a
banked corner still never rolls it: sitting in the cockpit, the vehicle's own climb reads as the
ground rising rather than as the view tipping. `"full"` takes the kart's whole orientation, banking included.
All three are the same construction from a forward and an up axis, differing only in which pair
they take: pairing a forward with world up is what removes roll. The headset always adds free look
on top of whichever is chosen, and only the translation onto the head is common to all three.

In the cockpit, `"yaw"` takes the kart's own driving direction rather than the chase camera's
lagging heading, from the level seat frame, which also damps a damage spin. `"yaw_pitch"`, the
default, and `"full"` take the kart's live orientation about that same seat, keeping only its
stabilised position, so a wheelie, a slope or a spin moves the view with the vehicle. With the custom seat, the head's place in the kart is
`first_person_head_up_meters` and its two companions, measured in the kart's own frame; the F10
sliders exist because the comfortable value is a matter of taste and is best judged from inside the
headset.

The mode engages only in a single-screen race, the same content that already qualifies for
immersive stereo. Menus, split-screen, `flat_screen`, and the virtual-screen fallback are
unaffected, and so is the desktop mirror, which keeps showing the game's ordinary third-person
view. If the kart or
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

## Steering wheel and hand steering

Ported from [heurazy's mario-kart-wii-VR-port](https://github.com/heurazy/mario-kart-wii-VR-port)
(GPL-3.0-or-later). It applies to the cockpit seat.

**The wheel turns.** With `steering_wheel = true` (the default) the kart's steering wheel or the
bike's handlebar turns with your steering: the left stick's deflection at the full-lock angle
(`wheel_kart_degrees` 90, `wheel_bike_degrees` 45), eased so a flicked stick does not snap it round,
or the hands' own angle while they hold it. `native_steering_wheel = true` turns the vehicle's own
model. Karts bake the wheel into the body, so at the race draw boundary the runtime decodes the
body's MDL0 position arrays, turns only the disc around the authored hand grips on a copy, and hands
the copy to the GX thread; Aurora substitutes it into the draws that bind that array with the
player's own model-view matrix (`aurora_set_native_wheel_vertices`), checking each changed vertex's
matrix slot, so an opponent sharing the asset and other joints of the same draw are untouched. The
guest's own vertices are never written, and the copies are dropped after the frame's draws. Bikes
turn their handle part in the game already; its copy is only re-seated on the cockpit frame so the
bars stay with your hands while the bike banks. The wheel rides in the same frame as the view: the
level seat for `"yaw"`, the kart's own orientation for `"yaw_pitch"` and `"full"`. While no draw takes
the copy (for 30 frames running; the race's opening pan does this) a separate VR wheel stands in,
which is also what `native_steering_wheel = false` draws. The copy keeps being published, so the
vehicle's own wheel returns as soon as draws take it again, and the log notes both switches.

The substitution is decided per draw, and a draw that folds into a neighbour renders through that
neighbour's array binding, so only draws that reached the same decision may merge. Deciding this
per array instead, and so refusing to merge every primitive that binds the vehicle's array, cost 6 ms
of GPU time a frame on a Quest 3 (a race frame has 228 such primitives, recorded once and replayed in
the mono pass and both eyes) and took a 56 FPS race down to 42. `debug.wiicompiled.fpslog 1` reports
the draw calls a frame and the primitives merged away, which is where that shows up first.

The copy is matched against the race camera's view (`RaceCamera::GetViewMtx` with no dolly offset),
because the scene camera is only set once the draws run. The log reports, once a second, how far
that view is from the scene camera at the seal (`[mkw-vr] cockpit: race camera view vs scene view`)
and how many draws took the copy; the F10 bar shows the same under the steering-wheel settings.
Aurora adds a line after about half a second, five seconds and a minute of copies
(`Native steering wheel: N sets; draws binding a replaced array ...`) counting the draws that bound a
copied array, those that bound one outside the window it was set for, the matches, and how far the
closest position matrix was from the expected one; the first such line with a bound draw also prints
both matrices.

**Hand steering.** `hand_steering` (on by default, and in WheelWizard's OpenXR VR settings and the
Quest launcher's Settings > VR, beside the seat)
lets you take hold of the wheel or handlebar with the tracked controllers. It costs nothing until a
grip actually takes hold: until then the stick steers as it always has. Squeeze a grip near it:
past 55 % squeeze, within `wheel_grab_distance` metres of its plane (default 0.35) and near the rim,
or near a bar end, scaled by `wheel_grab_assist`. Once taken, only letting go of the grip releases
it. One hand steers by its angle around the hub; two hands steer by the line between them, so leaning
or moving both arms together does not steer, and a hand joining, leaving or crossing the hub keeps
the steering where it was. Turning past full lock is kept, so retracing the gesture returns to the
same centre, while the game's steering saturates at full lock. `wheel_response` scales how quickly
the wheel follows, `wheel_tracking_grace` (seconds) how long a hand that loses tracking keeps hold,
and `wheel_haptics` gives a short pulse on grab and release.

While the wheel is held it replaces the left stick's X axis, in both the Wii Remote and the gamepad
presentation, and the game keeps its own steering curve. The stick's Y axis still aims items, and a
holding grip no longer reaches the game (a shoulder on the gamepad; the Wii Remote presentation
leaves the grips unbound for this reason); the triggers, A and the right stick are unchanged. Releasing both grips gives steering back
to the stick. The settings panel withholds the wheel like any other input.

**A USB wheel.** With a USB wheel and pedals set up (see the README), the wheel drives the race as
player 1's GameCube controller. The cockpit's wheel follows its calibrated steering, at the same
full-lock angle as the stick (`wheel_kart_degrees`, `wheel_bike_degrees`), and hand steering steps
aside while it drives.

**Hands and the separate wheel.** Hands are drawn while hand steering is on: the runtime's own hand
mesh where it offers one (`XR_EXT_hand_tracking` and `XR_FB_hand_tracking_mesh`, requested only when
hand steering is on at launch), otherwise procedural gloves that curl with the squeeze. A Quest 3
offers that mesh without the app declaring hand tracking, and the log says which is drawn
(`[mkw-vr] cockpit hands:`). Both close their fingers towards the palm: the mesh's joints point
-Z towards the fingertip and +Y out of the back of the hand, so flexion is negative about the
joint's own X, on both hands. They and the
separate VR wheel or handlebar travel with the stereo packet in metres in the seated frame, and each
eye draws them inside the scene's pass just before the first 2D-layer draw, depth-tested with the
world's own depth mapping, so the kart and the track hide them. Visible cockpit samples
also mark one stencil bit; virtual-screen draws test that bit for zero, so even depth-disabled
HUD elements and black screen effects cannot paint over the hands. Only stereo eye targets
use `Depth24PlusStencil8`; desktop/EFB depth stays unchanged. The mask is cleared once per
eye replay and retained across its passes. This adds no draw, full-screen copy, or render pass;
eye-format pipeline siblings share shader modules and are cached when recording the game
frame (`aurora-main/lib/gfx/cockpit.hpp`). The anchor also carries the frame's exact world scale
(`aurora_set_stereo_scene_anchor_scaled`), and Aurora rescales each eye's head translation to it, so
a scale change between the XR packet and the frame cannot misplace the hands.

The guest offsets involved (driver, movement, damage, grip frames, bike handle, driver bones and
their world matrices) are PAL `RMCP01` constants listed with the leaf getter or constructor that
proves each in `runtime/src/vr/mkw_vr_first_person.cpp`. `mkw_steering_wheel_tests`,
`mkw_vr_cockpit_tests` and `mkw_vr_hand_steering_tests` cover the grab model, the seat and wheel
geometry and the hand-off to the game; `gx_fifo_tests` covers the per-draw substitution and the
overlay geometry, and `cockpit_gpu_smoke` its depth test on a real GPU.

## Presentation policy

The runtime deliberately fails safe instead of guessing which Mario Kart camera is active:

- Menus, loading screens, unclassified scenes, and multiplayer render on a head-locked virtual
  screen.
- A PAL `RMCP01` race scene switches to immersive stereo only after translated-code observers
  confirm exactly one distinct race camera for the current GX frame.
- Leaving the race or observing zero or multiple cameras immediately returns presentation to the
  virtual screen. Session/runtime loss safely tears down XR and continues on the desktop mirror.
- `flat_screen` clears the policy's `immersive_races`, so a race stays on the virtual screen
  however complete the observations are. Changing it advances the safety generation like any
  other change of presentation, and the pacing thread still treats that race as a race: pipeline
  caches are not stored mid-race on the virtual screen either.

Aurora records the original GX frame once and replays it for both OpenXR eyes. Perspective GX draws
receive asymmetric headset projections, while the game's 2D layer goes on a fixed virtual screen
(see below). Menus and unsafe whole scenes use the virtual-screen path.
Head pose is sampled by the OpenXR pacing thread, while Aurora's frame worker consumes a
short-lived immutable stereo packet. Each sealed GX frame and immersive packet carry the same
policy-generation tag; a mismatch is rendered in mono and the acquired XR frame is canceled, so an
asynchronous menu/race transition cannot replay race transforms over unsafe content.

With interpolation off, PC (D3D12 and Windows Vulkan) and standalone (Android Vulkan) pace render-first:
the pacing thread locates views for an estimated display time (two periods past the last
prediction), hands Aurora a packet without leaving a compositor frame open, and waits for
rendering. A 50 ms stall repeats the retained layer; cancellation also advances a keep-alive
cycle to refresh timing. Once rendering is submitted, the thread calls xrWaitFrame and
xrBeginFrame, completes backend-specific copy/release work, and ends the frame using the
packet's original render poses with the current compositor display time.

Android Vulkan renders into shared buffers and copies them into newly acquired XR images afterward.
Both PC bindings acquire images from their non-retained swapchain pair before rendering; Aurora
queues the copy on the session's queue before reporting completion. PC therefore needs no additional
copy in the short compositor cycle. Pending images remain acquired and separate from the
retained pair until completion or confirmed cancellation before encoding. GPU failure still
requires the existing queue-drain teardown. Rendered poses keep the session/reference-space
serials recorded when the packet was prepared, so changes during rendering invalidate them.

VR interpolation keeps the frame-first order on both backends because it renders for the
frame's own predicted display time. The log announces `OpenXR D3D12 pacing: render-first` (or
`OpenXR Vulkan pacing: …` on the Vulkan binding) or
`frame-first (VR interpolation)` on each transition. For PC testing, disable **VR** frame
interpolation for a race capture; changing desktop interpolation alone does not select this
path. Menus use render-first even when VR interpolation is configured for races. Compare the
new diagnostic `open`, `end-gap`, `late`, and stage timings against a frame-first capture on
the same course and settings. Shorter `open` alone does not prove fewer black frames: rendering
and xrWaitFrame still take time outside that interval. Hardware testing is needed to measure
latency, runtime throttling and visible blackouts.

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
change invalidates the retained content, frames can still have no layers (on the Quest outside a
race, only the passthrough layer while `passthrough` is on, so a recenter does not flash black). Actual runtime or GPU
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

## Foveated rendering

On the Quest, `foveation` shades the edges of the immersive race view in 2x2, then 4x4 pixel
blocks, where the headset's lenses blur the picture anyway, and gives the GPU time back for a
higher `render_scale` or a steadier frame rate. Each eye's render pass runs under a fragment
density map (`VK_EXT_fragment_density_map`, attached through dynamic rendering). The map is centred
on that eye's forward direction, which the asymmetric frustum places off the image centre, towards
the nose. Its rings are angles from that direction (`aurora-main/lib/gfx/foveation.hpp`):

| Level | Full rate | Half (2x2) | Quarter (4x4) |
| --- | --- | --- | --- |
| `low` | within 30° | beyond | never |
| `medium` | within 25° | 25° to 40° | beyond 40° |
| `high` | within 18° | 18° to 34° | beyond 34° |

The HUD is drawn in the same render pass as the world and is foveated with it. `low` and `medium`
keep the default HUD screen (2.4 m wide at 2 m) at half rate or better while you look straight
ahead; `high` coarsens its corners. Menus and every other virtual screen, the settings panel, and
anything drawn outside an immersive race are never foveated.

`XR_FB_foveation`, the extension DolphinXR uses by default, cannot help here. The runtime's density
maps only shape render passes that draw into its swapchain images, and on the Quest Dawn draws each
eye on its own device and hands it to the OpenXR device, which copies it into the swapchain. So the
map has to go into Dawn's own eye passes. The stock Dawn package has no such feature, so the Quest
build links a Dawn built with Aurora's patches (`aurora-main/patches/dawn`, built by
`android/Build-QuestDawn.ps1`, see `docs/quest-port.md`). The patch enables the extension only when
Aurora asks for it at device creation, and every render pipeline then carries the density-map
pipeline flag. That is why the launch decides.

A density map forces Adreno into binned rendering, where every extra render pass in an eye stores
and reloads the whole eye. DolphinXR measured foveation as a net loss on Mario Kart Wii for exactly
that reason (its bloom chain splits the frame about 20 times). An eye is therefore foveated only
when it is drawn in a single render pass (`single_pass_eyes`); an eye that a partial clear still
splits is drawn at full rate. The session log reports what happened: "Fragment density maps:
enabled" at startup, one "eye foveation" line per eye and level with the map's size, and the "Eye
replay plan" lines. `debug.wiicompiled.foveation <0-3>` overrides the level for A/B timing, and
`debug.wiicompiled.fdm 0` launches without density maps at all (`docs/quest-port.md`).

What it saves depends on how much of an eye's cost is shading pixels. The numbers below are from a
Quest 3 at Luigi Circuit's Grand Prix start: GPU time of both eyes per frame, all settings
interleaved within one session (`docs/quest-port.md` has the method).

| `render_scale` (eye size) | One pass per recorded pass | `single_pass_eyes` | `low` | `medium` | `high` |
| --- | --- | --- | --- | --- | --- |
| 0.8 (1344x1408) | 5.82 ms | 5.11 ms | 5.09 ms | 5.36 ms | 5.11 ms |
| 1.3 (2184x2288) | 6.32 ms | 5.81 ms | 5.33 ms | 5.00 ms | 4.56 ms |

At the Quest's default 0.8 an eye's time goes mostly to geometry and to storing its tiles at full
resolution. The Wii's shading is cheap, so foveation saves nothing measurable there, although the
density map verifiably applies (4x4 blocks at the view's edges on High). That is why it defaults to
`off`. At higher render scales it takes 8 to 22% off the eyes, which is where it earns its keep,
bought with a softer periphery.

## Diagnostics

**F10 > Diagnostics** holds two bug-report aids.

**OpenXR diagnostic logging** is off by default. When it is off, each hook on the pacing thread is
one atomic test. It applies immediately and is remembered as:

```toml
[diagnostics]
openxr_logging = false
```

When it is on, `console.log` receives lines tagged `[runtime] [xr-diag]`
(`runtime/src/vr/openxr_diagnostics.cpp`). They cover both the D3D12 and the Vulkan backend.

- **Session description.** Written when logging starts and again for every new OpenXR session. It
  gives the runtime and system names and versions, vendor id, tracking support, backend, reference
  space, blend mode, enabled extensions, recommended and maximum eye sizes, `render_scale`, swapchain
  sizes, display period, and the VR frame interpolation setting.
- **View geometry.** Written on the first located views and again whenever they change by more
  than 0.5° or 0.5 mm. It gives per-eye FOV half-angles, the eye cant (the angle between the two
  eyes' forward axes: 0 for parallel displays, non-zero for canted ones such as Pimax without
  parallel projections), and the IPD.
- **A one-second summary.** Timings are `median/worst` in milliseconds; for `end-margin`, worst is
  the minimum.

| Field | Meaning |
| --- | --- |
| `predicted-rate`, `cycles` | Reciprocal of the runtime's predicted display period, **not necessarily physical headset refresh rate**; compositor cycles (xrWaitFrame/xrEndFrame pairs, repeats included). |
| `skipped-slots` | Display slots the predicted display time jumped over: the runtime throttled or dropped frames. |
| `late` | Frames whose xrEndFrame came after their predicted display time (needs `XR_KHR_win32_convert_performance_counter_time` or `XR_KHR_convert_timespec_time`). |
| `layers new/repeat/empty` | Cycles ending with a newly rendered layer, the retained layer again, or no layer at all (black). |
| `discarded`, `layer-rejected` | Retained layers dropped by a session or reference-space change; rendered layers not submitted (invalid pose or views, failed release). |
| `wait-frame`, `open`, `end-call` | Time blocked in xrWaitFrame, from xrBeginFrame to xrEndFrame, and inside xrEndFrame. |
| `end-margin`, `end-gap` | Predicted display time minus the xrEndFrame time; interval between xrEndFrame calls. |
| `pickup`, `render` | Stereo packet published until Aurora's frame worker takes it (without interpolation this includes waiting for the next 60 Hz game frame); taken until the pacing thread observes the submission result. These are CPU wall times for completed submissions, **not GPU timestamps**; canceled packets are measured separately below. |
| `acquire`, `release` | Swapchain image acquire+wait and release. |
| `keepalive` | Retained-layer repeats while Aurora was still encoding past the 50 ms keep-alive. |
| `packet-unused`, `packet-rejected`, `submit-failed` | Packets not picked up before cancellation; packets picked up but not encoded by the bridge before cancellation (the precise rejection cause is not known); failed stereo copies. |
| `interp-skip` | Cycles the VR interpolation rate cap chose not to render. |
| `frames immersive/screen` | Cycles per presentation mode; `not-rendered` counts cycles without views to render. |
| `no-orientation`, `no-position` | Cycles whose head orientation or position was not valid. |
| `suppressed` | Event lines dropped by the rate limit. |

- **Stage timings.** A separate `[xr-diag] stages ms` line accompanies each nonempty
  window, independently of the event rate limit. Each field is `median/worst` in ms;
  `@cycle=N,t=Ts` identifies the worst call's diagnostic cycle and completion time
  since logging/session reset. The main summary also includes the last `cycle` and `t`.
  Cycle 0 is before the first wait; work between cycles belongs to the previous cycle.
  These are wall times, including time the OS did not schedule the thread. Nested
  measurements (notably `sync-actions` within `input-sync`) must not be added together.

| Stage | What it isolates |
| --- | --- |
| `poll-events`, `begin-call`, `locate-views` | Event polling, the xrBeginFrame call itself, and xrLocateViews. |
| `input-sync`, `sync-actions` | Complete input update and its xrSyncActions call. |
| `publish`, `withdraw` | Packet construction/publication and withdrawal, including mutex waits. |
| `set-targets` | D3D12/Vulkan bridge target registration, including its mutex wait. |
| `submission-wait` | Actual time waiting for a render result, including timeout paths; compare against the requested 50 ms. |
| `cancel` | Bridge cancellation attempt, whether it succeeds or fails. |
| `cancel-age` | Publication to cancellation, including packets never picked up. |
| `cancel-pickup`, `cancel-after-pickup` | Publication to pickup and pickup to cancellation for consumed, canceled packets. |

For a blackout report, enable logging before entering a race, reproduce the blackout,
and export the logs immediately afterward. Include the approximate time and whether
both eyes and the desktop mirror went black. Check `frames immersive` is nonzero for
an immersive-race capture. A large stage maximum identifies where the pacing thread
spent time, but cannot distinguish API blocking from OS scheduling without a system
trace. No empty layers does not rule out black image contents or compositor/display
problems. This instrumentation does not change frame pacing or inspect image pixels.

- **Event lines.** At most 8 per second; the rest are counted in `suppressed`. They report late
  frames, skipped display slots, stalls (more than 2.5 display periods, and at least 25 ms, between
  xrEndFrame calls), empty frames and their reason, discarded retained layers, rejected layers,
  withdrawn or rejected packets, failed submissions, and head-tracking loss and recovery.
  Reference-space change events are never rate-limited.
- **Presentation changes.** While logging is on, every `[mkw-vr] presentation=` transition is
  logged, not just the first 16.

**Export Logs** opens the system folder picker. It then creates a
`WiiCompiled-logs-YYYYMMDD-HHMMSS` folder at the chosen location, containing:

- `Logs/`: every retained run folder, the current session included. The runtime prunes run
  folders after four days.
- `Config.toml`.
- `export-info.txt`: the export time, the exporting process id (whose run folder ends in `_pid<id>`),
  and the OpenXR state.

The current `console.log` is copied through a shared-read stream while it is still being written.
The copy runs on SDL's dialog thread (`runtime/src/log_export.cpp`), and the outcome is shown under
the button. `mkw_openxr_diagnostics_tests` and `mkw_log_export_tests` cover both without a headset.

## Windows Vulkan

`video.graphics_api = "vulkan"` selects a second Windows binding, `runtime/src/vr/openxr_vulkan_win32.cpp`,
with the same pacing thread, retained-layer protocol and policy as D3D12
(`runtime/include/vr/openxr_windows.h` picks the backend at startup). It is opt-in. It has raced on
a headset (SteamVR/OpenXR with a PlayStation VR2): immersive projection held the headset's full
90 Hz with no skipped display slots, and a 646-second session recorded no rejected or discarded
layers and no failed submissions. Other runtimes are still unexercised.

**Why a custom Dawn.** The pinned prebuilt Dawn DLL exposes no native Vulkan device, so
`aurora-main/patches/dawn` adds a small versioned C ABI to the pinned Dawn source
(`aurora_dawn_vulkan_abi.h`, `AURORA_DAWN_VULKAN_ABI = 1`): hooks that let the OpenXR runtime
create Dawn's `VkInstance`, choose the physical device and create the `VkDevice`; wrapping of a
borrowed `VkImage` as a Dawn texture; the release barrier back to `COLOR_ATTACHMENT_OPTIMAL`; a
device-guard lock; and a queue drain. `Launcher/Build-DawnVulkan.ps1` builds that DLL from the pinned
revision on a machine with Visual Studio 2022, Python 3.12+ and CMake, and writes `aurora-vulkan.json`
(revision, ABI, DLL hash). `Launcher/Prepare-Dependencies.ps1 -DawnVulkanPackage <dir>` installs it as
`dawn_prebuilt` in a fresh dependency destination after checking that provenance; re-harvest
`native_prebuilt` afterwards because the archives are pinned to the Dawn DLL hash. The runtime
build also fetches Vulkan headers (`vulkan_headers` dependency).

**Startup.** `QueryGraphicsRequirements` loads `xrGetVulkanGraphicsRequirements2KHR`,
`xrCreateVulkanInstanceKHR`, `xrCreateVulkanDeviceKHR` and `xrGetVulkanGraphicsDevice2KHR`, then
installs the hooks in Dawn. Without the custom DLL it fails with *"requires the custom Dawn library
with Aurora Vulkan ABI 1"* and the game continues on the desktop renderer. During
`aurora_initialize` the runtime creates Dawn's instance (Vulkan 1.2 is requested when the loader
and runtime allow it, so that timeline semaphores, which PC runtimes create on the application's
device, are a core feature the device hook can enable) and device on the runtime's physical GPU.
`BindAurora` confirms that Dawn's physical device is the one the runtime selected, creates the
session on Dawn's graphics queue, picks the sRGB sibling of Aurora's UNORM colour format (with
`XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT`) so the compositor decodes the gamma-encoded bytes, and
enables the bridge.

**Frames.** Each acquired XR image is wrapped once as a Dawn texture and reused. Aurora's frame
worker records the eye copies into its own command buffer; immediately after `queue.Submit`, still
under Aurora's submit mutex, the bridge appends the release barrier through Dawn's queue and
publishes the token that the pacing thread's `WaitForSubmission` consumes. The runtime may use
the VkQueue only inside `xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage` and
`xrReleaseSwapchainImage`, so `OpenXRRuntime::LockGraphicsQueue` holds Dawn's device guard around
exactly those four calls and never across `xrWaitFrame` or `xrWaitSwapchainImage`.

**Tests.** `mkw_openxr_vulkan_replay_tests` compiles the real backend against the deterministic
compositor of the D3D12 replay tests, including the queue-guard requirement on acquire and release.
`vulkan_native_bridge_smoke` (aurora, `AURORA_GPU_SMOKE_TESTS=ON`, real GPU, no headset) drives the
custom DLL's ABI through three borrowed-image copy/readback cycles; run it with that DLL beside it.

## Backend status

| Backend | Status |
| --- | --- |
| Windows D3D12 | Implemented: same-adapter, same-device asynchronous OpenXR submission. |
| Windows Vulkan | Implemented, opt-in (`video.graphics_api = "vulkan"`): the runtime creates Dawn's Vulkan instance and device through `XR_KHR_vulkan_enable2`, eyes are copied on the same queue, and Dawn's device guard is held around the four queue-touching OpenXR calls. Needs the custom Dawn from `Launcher/Build-DawnVulkan.ps1`. Raced on SteamVR/PSVR2 at the headset's full rate; other runtimes unexercised. See [Windows Vulkan](#windows-vulkan). |
| Android Vulkan (Meta Quest) | Implemented and running on a Quest 3: the OpenXR side owns its own Vulkan device (`XR_KHR_vulkan_enable2`, `XR_KHR_vulkan_enable` fallback) and shares eyes with Dawn through `AHardwareBuffer`s ordered by sync-fd fences. Controllers arrive through OpenXR actions as a virtual SDL gamepad. See `docs/quest-port.md`. |
| Linux Vulkan | Not wired. The pinned Dawn package does not expose a native Vulkan device, and the AHardwareBuffer bridge is Android-only; a dma-buf/opaque-fd variant of the same design would cover desktop Linux. |
| Other platforms | Not wired yet. |

Both bindings share `openxr_integration.cpp`: the pacing thread, policy evaluation, the
retained-layer protocol and the head-pose maths are compiled once against the neutral types in
`vr/openxr_backend.h`, and only the backend class differs per platform.

### Interpolation validation

Probe-sized EFB readbacks retain completed pixels in host memory and publish them only inside
the next compatible `GXCopyTex` call. GPU completion callbacks must not write to guest RAM:
a race restart can reuse a freed probe buffer for `RaceCamera`, and a late 4x4 Z24X8 tile then
turns its rotation fields into NaNs and triggers `triangular.h` / `PPCHalt`.
The optional Windows GPU test `efb_ram_lifetime_smoke` exercises that allocation reuse and
format/size changes. It fails with the former callback write and passes with deferred publication.

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
Use `stereo_frame_worker_smoke 1000 1 1` to exercise ten-matrix palettes and their
larger uniform history, or `stereo_frame_worker_smoke 1000 2 1` to switch interpolation
On/Off during recording. The test compositor discards obsolete ticks and uses
high-resolution waits on Windows, keeping missed ticks from accumulating into bursts.
It pre-warms the next game frame like the runtime and excludes the first 60 frames
from timing so shader compilation and initial resource allocation do not skew steady-state results.
The test checks that the producer stays above 55 FPS as well as checking headset submissions;
replaying an old scene more often must not hide a slowed simulation. Validate actual races in VDXR at 90 Hz with
Auto/90 selected, including race entry/exit, first person, recentering and pauses.

Stereo uniform calculations use cached CPU memory, followed by a single write into the upload
buffer. Reading or modifying matrices directly in D3D12 upload memory can be extremely slow,
especially with many character draws; see Microsoft's [Map guidance](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map).
Retained interpolation reserves eye ranges at seal time and fills them once at the headset sample
time. The frame worker always releases the producer after sealing, so eye encoding overlaps the
next game frame whether or not interpolation is on. It used to publish that phase only after the
encode unless interpolation was enabled, and the producer's first GX drain of every frame then
waited for the previous frame's whole encode and submit (3 to 4.5 ms per frame on a Quest 3).

When VR interpolation is enabled at batch start, uniform recording also uses cached CPU
memory. Matching and history capture read that buffer, then the used prefix is copied to
the mapped upload buffer before unmapping. The backing choice stays fixed until the batch
ends, including mid-frame flushes, so live setting changes cannot invalidate pending tasks.

## Current limitations

- Only the project's supported PAL `RMCP01` translation has race instrumentation addresses.
- The tracked controllers are always Player 1's Wii Remote; there is no left-handed swap, and only
  the Touch and simple controller profiles have suggested bindings. The Wii Remote presentation
  still needs headset validation: cursor direction and roll, trick/wheelie motion, rumble strength
  and the HOME Menu.
- The Quest build (`android/`, `docs/quest-port.md`) runs on a Quest 3 through menus and races.
  Lifecycle events and performance (about 43 game FPS) are still open. Apple visionOS packaging
  is not implemented.
- Scene-specific comfort options, culling fixes and replay/spectator classification are future work.
- Hand steering works on a Quest 3 (2026-09-22): the kart's own wheel animated (228 draws a frame,
  the race camera's view matching the scene's exactly) and the wheel can be grabbed and turned. In
  that race the driver's eye was never calibrated, so the fallback placed the wheel centre about
  13 cm above eye level. Bikes and Quacker, and the PC, are still unvalidated. Hand steering needs
  analog grips (Touch); the simple controller profile cannot grab.
- The headset settings panel has no laser beam, only the cursor on the panel itself, and text fields
  cannot be typed into without a keyboard.
- The desktop window remains available as a mirror/fallback.

OpenXR diagnostics are written to the normal run log under
`%LOCALAPPDATA%\WiiCompiled\Logs`. Search for `OpenXR` when reporting a startup or submission
failure.
