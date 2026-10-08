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
refresh_rate = 0
render_scale = 1.0
world_units_per_meter = 500.0
hud_distance_meters = 2.0
hud_width_meters = 2.4
hud_virtual_screen = true
flat_screen = false
immersive_window = false
first_person = false
first_person_toggle_click = true
bullet_bill_camera = "inside"
first_person_seat = "cockpit"
cockpit_units_per_meter = 100.0
first_person_units_per_meter = 50.0
first_person_head_up_meters = 1.5
first_person_head_forward_meters = 0.0
first_person_head_right_meters = 0.0
first_person_hide_driver = true
first_person_hidden_model = 0
body_ik = true
body_ik_hands_only = false
first_person_rotation = "yaw_pitch"
steering_wheel = true
native_steering_wheel = true
placeholder_steering_wheel = false
object_culling = false
hand_steering = true
cockpit_item_hand = "left"
cockpit_item_throw = true
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
While the window shows black or the eyes during an immersive race, the game's own flat render is
seen nowhere, so it stops after the last pass whose EFB copy the eyes sample, as it always does on
the Quest. This saves GPU time in proportion to the internal resolution. An eye choice shows that
render, in full, until the window has received the race's first immersive frame, and with VR
interpolation that can take a while. On a PC with a PS VR2 through SteamVR (Vulkan, interpolation
Auto, automatic internal resolution), the flat render of a race frame cost 0.50 ms of GPU time with
`"normal"`, 0.24 with `"left"` and 0.14 with `"none"`.

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

**F10 > VR > Display refresh rate** (`[vr] refresh_rate`, Hz; `0`, the default, keeps the rate
the headset chose) asks the runtime for that refresh through `XR_FB_display_refresh_rate`, so
only Meta runtimes (the Quest, Quest Link) take it; elsewhere the rate is set in the VR runtime.
A Quest 3 offers 72, 80, 90 and 120 Hz; the session logs the list and every change, and an
unoffered rate is logged and ignored. It applies immediately, and the Quest launcher's settings
page has it too. With VR frame interpolation every refresh gets a newly rendered image, so a
higher rate is smoother and costs a full eye replay per refresh (measured on the Quest in
`docs/quest-port.md`). Without it, 72 Hz shows one game frame in five for two refreshes, which
reads as a stutter, and 120 Hz shows nearly every 60 Hz game frame for two (96-98% on a Quest 3:
the game's clock and the display's are independent, so as they drift past the compositor's
deadline a few frames are shown for one or three). A repeated frame is re-warped for the head
only, so what moves across the view as the kart turns is seen twice at 120 Hz. The compositor
then runs twice as often, a little more power for the same app GPU time.

VR interpolation is independent of **Graphics > Race frame interpolation**. The simulation,
physics, audio and VI remain at 60 Hz. Scene motion is delayed by one game frame (about 16.7 ms)
to interpolate between known transforms. A monotonic scene playback clock is anchored when
continuous history starts and follows the VI cadence without following per-frame seal jitter.
Each rendered eye pair still uses a fresh predicted head pose. In particular, a runtime predicting
head poses 40-60 ms ahead must not advance scene playback beyond its known endpoints.
This needs enough GPU headroom to render both eyes at the target rate, and carries the
desktop interpolator's experimental artifacts, especially for unmatched or changing geometry.

Refresh detection uses `XR_FB_display_refresh_rate` when available and the OpenXR predicted
display period otherwise. Interpolation requires `XR_KHR_win32_convert_performance_counter_time`
for the packet's display-time metadata; the menu reports if it is unavailable.
The old Eager Frame Heartbeat option has been removed and existing `eager_frame_heartbeat`
settings are ignored. Completed rendering wakes the XR thread immediately. A 50 ms keep-alive
still protects pauses and window dragging without eager repeats during rendering.

`render_scale` scales the per-eye size recommended by the OpenXR runtime (0.25 to 2, never above the
runtime's maximum). It defaults to 1.0 on PC and 0.8 on the Quest, whose mobile GPU needs the
headroom. It is live: **F10 → VR → Render resolution** (also on the headset panel's VR tab) sets it
in percent, applies it when the slider is let go, and saves it. Below the slider, *Each eye* gives
the left eye's size now and, while they differ, the size the slider's value gives.

A new scale never interrupts the picture. Each backend keeps two swapchain pairs, one on display and
one Aurora writes next, and rebuilds only the second, at the start of the frame that writes it; the
other follows a frame later, once it is the one written. The new swapchains are created before the
old ones go, and a replaced pair is not destroyed at once: the compositor may still be consuming the
layer that last showed it (the spec lets a runtime use the images after `xrDestroySwapchain`, and
destroying a pair straight after its last frame lost the Vulkan device on a PC runtime), so it lives
on for `kOpenXRRetiredSwapchainCycles` (8) more pacing cycles, each of which ends another compositor
frame, and goes with the session if it is still waiting then. Before a pair is destroyed Aurora
forgets its images: the D3D12 bridge waits for any copy still writing them, and the Windows Vulkan
bridge drains Dawn's queue and drops its wraps of the old `VkImage`s (the runtime may hand the same
handles to the new swapchains); on the Quest the backend's own copy device is idled. The Windows
Vulkan binding also takes Dawn's device guard around `xrCreateSwapchain` and `xrDestroySwapchain`,
since mid-session Dawn's worker is submitting on the shared queue. The Quest also replaces its
shared eye buffers, whose Dawn imports are released (`aurora_vulkan_forget_stereo_buffers`). Those
buffers are kept as large as both pairs, growing with the first pair rebuilt larger and shrinking
once the second has followed it down, and each copy moves only what fits the image it writes. If the
runtime cannot allocate a size, the log says so, the eyes keep the size they had (a pair already
rebuilt goes back to it), and that size is not tried again until the scale changes; the saved value
is still what the next launch asks for. `mkw_openxr_replay_tests` and
`mkw_openxr_vulkan_replay_tests` cover the rebuild, the display pair left alone, the replaced pairs
outliving their last frame by exactly that many cycles and going with the session otherwise, the
images forgotten before their swapchains are destroyed, and both kinds of refusal.
`world_units_per_meter` controls the scale of headset translation in the game world.
`hud_distance_meters` and `hud_width_meters` place and size the virtual screen. They are read at
launch and govern both the menu screen and the in-race 2D screen, so 2D content keeps its place
across the transition. `hud_virtual_screen` decides whether the race's 2D layer uses that screen;
it is live and can be flipped from the F10 settings bar.
`flat_screen` (default off) keeps races on that same flat screen, as the menus are, instead of
immersive stereo: the whole race, 3D world and HUD alike, is the game's own picture on the quad, as
in DolphinXR's Flat Screen mode. The first-person camera, hand steering, the lean-back angle, VR
frame interpolation and `hud_virtual_screen` shape only the immersive race view, so none of them
apply while it is on; the right-thumbstick camera switch is ignored. It is live, as
**F10 → VR → Race view → Flat screen** (the headset panel's VR tab) and the Quest launcher's
Settings page, and turning it on or off mid-race switches on the next frame through
the presentation policy's safety generation.
`immersive_window` (default off) is the third race view, between the two: the race keeps its
immersive stereo view, head tracking and all, but is seen only through a window, with the room
around it on the Quest; see [The immersive window](#the-immersive-window). `flat_screen` wins when
both are set. The settings present the three as one choice, **Race view**: Immersive, Immersive
window or Flat screen.
`passthrough` (Quest only, default on) shows the room through the headset's cameras around the
menu screen and every other virtual screen, instead of black: an `XR_FB_passthrough`
reconstruction layer submitted under the screen's quad, as PPSSPP VR does, with the blend mode
left `OPAQUE`. A fully immersive race never shows it, and the cameras are paused for the race; a
race in `flat_screen` is a virtual screen like the menus, so the room shows around it too, and so it
does around the immersive window. It is
live, from the headset panel's VR tab or the launcher's Settings page. The app declares
`com.oculus.feature.PASSTHROUGH`, without which Horizon OS composites nothing for that layer.
So that the room frames the picture rather than black bands, the Quest's menu quad shows only the
part of its eye-sized image Aurora draws into (the desktop snapshot, and the in-eye settings
panel's rectangle), at the same size per pixel, so nothing moves.
`hand_tracking` (Quest only, default off) makes the first-person cockpit's hands follow the
headset's hand tracking; see "Tracked hands" under
[Steering wheel and hand steering](#steering-wheel-and-hand-steering).
How each eye is replayed is fixed; the former `stop_at_display_copy`, `skip_copy_clears` and
`single_pass_eyes` settings are ignored. An eye ends at the frame's final `GXCopyDisp`, so it holds
the frame shown on the desktop. It keeps the EFB reset that follows a display copy: Aurora marks
only a display copy's reset, the final one lies past the replay's end, and an earlier one erases
exactly what the final copy did not show. And it is drawn in one render pass. The desktop image ends
a render pass at every GX copy, because the copy reads what was drawn before it; an eye samples the
copies the desktop image made and never performs them, so it keeps drawing in the pass it has open,
and it leaves out whatever a later clear of the whole color and depth erases. The picture is the
same with less GPU memory traffic, which a tiled mobile GPU pays for at every split (the "Eye replay
plan" log line reports each new pass structure).
`first_person` (the camera each race starts on), `bullet_bill_camera` and the `first_person_*` values
are the first-person camera described below, and `body_ik` and `body_ik_hands_only` the
[character body](#character-body) drawn in its cockpit. All are live and are also exposed in the F10
settings bar.
`performance_level` is the level asked of the runtime through `XR_EXT_performance_settings` for
its CPU and GPU domains: `boost`, `sustained_high`, `sustained_low`, `power_savings`, or
`default` to leave the runtime's own choice. Standalone headsets clock their cores by this
request (see `docs/quest-port.md`); desktop runtimes rarely offer the extension, and the setting
then does nothing. It is read at launch, and the session log records whether the runtime accepted
it and any later performance notification (a thermal or rendering warning).
`foveation` (Quest only, default `medium`) shades the edges of the immersive race view more coarsely:
`off`, `low`, `medium` or `high`, see [Foveated rendering](#foveated-rendering). A session launched
with it off runs without fragment density maps, so going from `off` to a level takes a restart;
between levels, and back to `off`, it is live from the headset panel's VR tab. The launcher's
Settings page has it too.

## Controllers

The headset's tracked controllers reach the game through an OpenXR action set synced on the pacing
thread (`runtime/src/vr/openxr_input.cpp`), which feeds a virtual SDL gamepad that Aurora assigns
to a port like any other. `controller_mode` decides what the game finds on that port, and is live
from **F10 > Controller settings > VR controllers**; the game sees a change as a controller reconnection.

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
| Left X or left menu | + |
| Left stick | Nunchuk stick |
| Left trigger | Z |
| Left Y | Settings panel (not a Wii button) |
| Either grip | Takes hold of the wheel (not a Wii button) |
| Right stick click | This race's camera: game / first person (not a Wii button) |
| Right controller motion and aim | Wii Remote accelerometer and pointer |
| Left controller motion | Nunchuk accelerometer |

Analog inputs count as pressed past half travel. The grips, right stick left / right and the left
stick click press no Wii button, and nothing presses − or HOME: Mario Kart Wii never reads −. Left
X presses + as well as the left menu because the PlayStation VR2's controllers give no usable left
menu, so pause would otherwise be out of reach there. C sits on right B rather than a grip
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

**Held item.** `cockpit_item_hand` accepts `"left"` (default), `"right"`, or `"off"` and is also
available in F10 > VR. In cockpit view, the selected tracked hand holds one item model from the
game's `Race/Common.szs` after the roulette settles. The item stands upright just above the palm
with its front toward the player. It turns only with the hand's heading, so rolling or tilting the
hand never tips it over. Triple items show their remaining inventory count beside the model, facing
the player. The display follows player 1's inventory: using, losing, or deploying the
item removes it from the hand even if a deployed object remains near the kart. Stick steering and
the existing item buttons still work. The imported models use their static bind pose; item effects
and animations are not reproduced in the hand. Each material is drawn from its own data: texture
layers with their wrap modes, SRT and environment mapping, vertex colours, culling, blending and up
to four TEV stages. Only the lighting is approximated, by a fixed cockpit light in place of the
course's light set.

**Throwing the held item.** With `cockpit_item_throw` on (the default; F10 > VR and the Quest
launcher), a quick swing of the item hand forward throws the item ahead of the kart, and a swing back
throws it behind. The swing plays what the game reads for an aimed throw: the stick pushed fully
forward or back, the item button (the left trigger: Z, or the GameCube's L) pressed and released
while it stays pushed, then the stick handed back. Items used on the press and items thrown on the
release (a trailed shell or banana) both see the aim. Swinging while holding the item button throws
the trailed item, and that held button is then ignored until it is let go, so it does not use the
next item of a triple. Only the stick's Y axis moves, so a held wheel keeps steering. A swing counts
when the hand covers enough ground along the seat's forward axis within a short window, mostly
along that axis, and is still moving that way: with a controller 18 cm forward or 15 cm back within
0.12 s. A bare hand gets an easier profile, 15 cm forward or 13 cm back within 0.15 s. The headset's
cameras lag and smooth a bare hand, and often lose it for a few frames in the middle of a fast swing.
A loss of up to 0.2 s (0.1 s for a controller) is bridged by the positions either side of it. The
window counts only tracked time, and across a gap the swing must still average 70 % of the window's
speed, so a slow drift the cameras briefly lost never throws. Reaching for the wheel, bringing a
hand back to rest, a sideways sweep and the upward flick of a trick never throw either. The hand
must not be holding the wheel and must have been free for a quarter of a second, and after a throw
the next one waits 0.6 s. It works with controllers and tracked hands, in both controller modes, and
a short pulse confirms it. Each throw's line in the run log gives what the swing measured (for
example `Held item thrown forward (bare hand, 21 cm in 150 ms, across a tracking gap)`).
Mario Kart Wii reads tricks and wheelies from the Wii Remote's accelerometer only
(`MotionController::UpdateForNunchuck` never reads the Nunchuk's), so a swing of the left hand cannot
trick. With the item in the right hand, a hard swing can also read as a shake of the remote.

**Bare hands.** On the Quest, with `hand_tracking` on and the controllers put down, the hands drive
`khr/simple_controller`: a right pinch is A with the pointer on the hand's aim ray, the left
palm-up pinch is + (pause), and in the cockpit, while a hand holds the wheel, that hand holds A and
a free hand's pinch is Z; see "Tracked hands" under [Steering wheel and hand
steering](#steering-wheel-and-hand-steering). A bare hand feeds no motion; flicking the hands up
plays one shake instead. With `hand_tracking` off a bare hand presses nothing but +.

`"gamepad"` keeps the controllers one ordinary gamepad read through PAD as a GameCube controller:
A/B → South/East, X/Y → West/North, index triggers → trigger axes, grips → shoulders, thumbsticks
→ sticks (clicks → stick buttons), left menu → Start. Every binding in the F10 controller menu
applies. Left Y is GameCube Y here, so clicking both thumbsticks together opens the settings panel
instead. The right thumbstick click on its own still switches the race's camera.

`"none"` makes the controllers nothing to the game, for playing with another controller (a desktop
gamepad, a USB wheel or a Bluetooth Wii Remote). The virtual gamepad is unplugged, so it takes no
port and the other controllers keep theirs; the game sees a switch to or from `"none"` as that
controller disconnecting or connecting. The plugging follows the mode on the game thread, where the
gamepad is already written, so the pacing thread never waits on SDL's joystick lock for it. The
controllers still open the settings panel with left Y, point at it and switch the race's camera
with a right thumbstick click; they press no game button, drive no Wii Remote, cannot take hold of
the cockpit's wheel, and the game's rumble does not reach them.

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

The panel has the F10 bar's menus as tabs (VR, Camera, Graphics, Controllers, Audio, Diagnostics) and a
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
that `openxr_input.cpp` publishes through `vr/openxr_settings_panel.h`. That context shares the
desktop context's renderer backend data: ImGui's current context is one process-wide pointer, and
Aurora's frame worker renders draw data while the game thread may have switched to the panel
context, so the WebGPU backend has to find its device objects through either (without this, a
worker render during the switch crashed on a null backend, seen while dragging the panel's sliders). Aurora renders that draw data
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
rectangle on a virtual-screen eye image.

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

With the game's camera the headset sits where Mario Kart's own chase camera sits, and
`world_units_per_meter` of 500 presents the race as a small diorama on a table. First person moves
the camera to the local driver's head instead, at one of two seats:

- `first_person_seat = "cockpit"`, the default, sits you at the driver's own eyes, behind the
  steering wheel, at a life-size scale, so the wheel or handlebar is within reach of your hands.
  The eye is measured once per race from the character's head bone, while the kart drives straight,
  undamaged and at normal size, and then frozen. Custom models without a separate eye mesh use
  the animated head with a small upward/forward offset; until calibration completes the bind pose,
  or the vehicle's authored seat height, stands in. Rejected measurements never alter that fallback.
  Eight consecutive samples must stay within two units of the first sample, so a slowly moving
  starting animation cannot qualify just by moving little each frame. The eye is kept at least
  0.45 m behind the wheel and at most 0.40 m above the neutral hand targets, so long faces and necks
  do not leave the controls below comfortable reach. The world scale is
  `cockpit_units_per_meter` (default 100) multiplied by the character's eye height over 100 units,
  so tall characters sit at a comparable height, and by the player's current size, so a lightning
  strike or a mega mushroom resizes the view, the wheel and the grab reach together. The seat
  follows the simulation's position and driving direction, never the animated chassis, so damage
  spins and tricks do not throw it around.
  **Recenter view** (including its key binding) and headset reference-space recentering request a
  fresh cockpit calibration as well as resetting the headset position. The previous seat stays
  in place until a new stable, neutral measurement is ready; then height and wheel clearance are
  recalculated together. Recenter while driving straight at normal size to replace a bad initial seat.
- `first_person_seat = "custom"` places the head at `first_person_head_up_meters` and its two
  companions in the kart's own frame, at `first_person_units_per_meter`.

Both are a matter of taste rather than properties of the game, so the F10 bar exposes them.

**Camera modes.** After heurazy's
[Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) fork, a race shows one of
two cameras, the game's or first person. Each race shows the game's own camera through its intro
(the course preview, `RaceManager`'s stage 0), so the flyover plays as the game made it, then
starts on the default camera once the countdown begins: `first_person`, where `true` is first
person (**F10 → Camera → Default camera**, also the Quest launcher's *Default camera*). During the
race the camera can be switched for that race only, with the right thumbstick click below or
**F10 → Camera → This race** (shown during a race, greyed out until the intro is over). That choice
is kept in memory, never saved, and the next race starts on the default again; outside races and
during the intro a switch is refused, so it cannot leak into the next race. Each switch logs
`[mkw-vr] this race's camera: first person` (or `game camera`). `mkw_vr_camera_mode_tests` covers
these rules.

`bullet_bill_camera` decides what first person shows while you are a Bullet Bill: `"inside"`, the
default, rides inside the bullet as described below, and `"game"` shows the game's own camera until
the Bullet Bill ends, then first person again (**F10 → Camera → Bullet Bill**: *Ride inside* or
*Game camera*; also in the Quest launcher). That override leaves the race's own choice alone, and F10
notes it under *This race*; a click during it switches that choice, which shows once the Bullet Bill
ends.

**Switching from a controller.** Clicking the right thumbstick switches the race in progress between
the game's camera and first person, as *This race* does, and is never saved. It works on the VR
controllers in either presentation (the right controller gives a short tick), and on any other
gamepad while VR is running. A click counts on release, and only if the left thumbstick stayed up
and the settings panel stayed closed throughout, so clicking both thumbsticks to open the panel in
gamepad mode never switches the camera. A gamepad whose right thumbstick click is bound to a
GameCube control on its port, as a button or in an input expression, keeps it for the game instead.
A click in a menu or during the intro changes nothing and logs one
`[mkw-vr] right thumbstick click ignored` line. `first_person_toggle_click = false`, or the F10
checkbox under the camera choices, turns the click off. `mkw_vr_camera_toggle_tests` covers the
click rule.

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
stops or the race ends. This is the one place the first-person camera modifies the game's state
rather than only reading it (the [character body](#character-body) rewrites only matrices it puts
back before gameplay sees them). In the cockpit seat with `body_ik` on, the character body takes
over the driver instead, and these two settings apply only to drivers it does not draw (Miis and
unknown models); the F10 toggles and the launcher's row are greyed out then.

With `bullet_bill_camera = "inside"`, in cockpit view, your Bullet Bill keeps its animated arms but
hides the body, eyes and rear exhaust cone, which otherwise fills the view from inside. The
runtime resolves the local racer's Killer model and registers its rigid body position arrays
with the renderer for that frame.
Only draws at that Bullet Bill's own model-view transform are skipped; opponents sharing the
model, arm joints, and the separate ground shadow remain visible. The requests are cleared after
the frame and when cockpit view stops. This changes rendering only, without modifying game assets
or the item's behaviour. `mkw_vr_bullet_bill_tests` and Aurora's `HiddenModelTest` cover body/arm
selection, malformed models, instance matching, and both FIFO and raw draw paths.

As with the rest of the race instrumentation, the object offsets this reads are specific to the
project's supported PAL `RMCP01` translation.

### Object culling

Mario Kart hides what its own chase camera cannot see, and that camera does not know where the
headset is looking. Turn your head far enough in an immersive race, or look over your shoulder
in first person, and karts, characters and course objects are simply missing until the game
camera catches up; the race intro's pan shows it too, since the other racers are culled from the
intro camera's narrow view. `object_culling = false` (F10 > Camera > Object culling, also on the
headset settings panel's Camera tab) draws them anyway, and takes effect immediately. That is
the PC's default. The Quest defaults to `true`, the game's own culling, because every model
drawn costs its GPU twice, once per eye.

Measured on a Quest 3 (base game, the first Grand Prix race at Luigi Circuit after the intro, player
idle, first-person cockpit, `render_scale = 0.8`, foveation medium, six interleaved rounds per
state, GPU clock fixed at 492 MHz), turning culling off kept 60 FPS in the game and the headset:

| Culling | Draw calls a frame | Both eyes | App GPU per frame | GPU load | CPU load |
| --- | --- | --- | --- | --- | --- |
| On | 352 | 7.4 ms | 9.7 ms | 67% | 54% |
| Off | 482 | 8.9 ms | 11.0 ms | 74% | 64% |

A track that already keeps the Quest's GPU near its limit, such as Retro Rewind's SNES Ghost Valley
2, would lose headset frames to the extra eye time, hence the Quest default.

The game culls in two places, and the switch covers both. NW4R's scene gather tests each model's
bounding box against the camera frustum (`nw4r::math::FRUSTUM::IntersectAABB_Ex`); with culling
off that test reports every box as partially inside. Mario Kart's own `ClipInfoMgr` then tests
each kart, item and object against per-screen side planes derived from the camera
(`ClipInfoMgr::UpdateScreenInfo`); with culling off those planes carry zero normals, which no
model can be beyond. Both functions are replaced by faithful native reimplementations in
`runtime/src/vr/mkw_vr_culling.cpp`, so with culling on they compute exactly what the translated
originals did. `mkw_vr_culling_tests` covers the frustum test. What stays as the game decides
it: the draw distance, the course's area-based clipping groups, and every gameplay rule, since
none of this changes physics or object updates.

The setting only takes effect while VR is enabled (a session that fell back to the desktop for
want of a headset included), and not in the Flat screen race view, which shows the game camera's
own view. It costs GPU time: every model the camera would have dropped is drawn for
both eyes. On the Quest, where the eye passes are geometry-bound, leave
it on unless the missing racers bother you more than the frame time.

## Steering wheel and hand steering

Ported from [heurazy's mario-kart-wii-VR-port](https://github.com/heurazy/mario-kart-wii-VR-port)
(GPL-3.0-or-later). It applies to the cockpit seat.

**The wheel turns.** With `steering_wheel = true` (the default) the kart's steering wheel or the
bike's handlebar turns with your steering: the left stick's deflection at the full-lock angle
(`wheel_kart_degrees` 90, `wheel_bike_degrees` 45), eased so a flicked stick does not snap it round,
or the hands' own angle while they hold it. `native_steering_wheel = true` turns the vehicle's own
model. Karts bake the wheel into the body, so at the race draw boundary the runtime decodes the
body's MDL0 position arrays and shape connectivity. Hand grips locate the wheel, but its complete
rim determines the rotation centre, radius and tilt: grip height/spacing varies by character.
Whole rim and spoke components turn together on a copy; a column or chassis component crossing
the selection stays intact. The authored transform of the bone that draws the body's node 0 is
included when locating and turning the wheel (the Baby Booster authors its body with rotated
axes). That bone is found by its node id, not its place in the bone dictionary: the Flame Flyer and
Cheep Charger list an `nw4r_root` bone first, and taking that one left both on the separate VR
wheel. The copy goes to the GX thread; Aurora substitutes it into draws that bind that array with
the player's model-view matrix including that bone's transform (`aurora_set_native_wheel_vertices`),
checking each changed vertex's matrix slot, so an opponent sharing the asset and other joints of
the same draw are untouched. The
guest's own vertices are never written, and the copies are dropped after the frame's draws. Bikes
turn their handle part in the game already; its copy is only re-seated on the cockpit frame so the
bars stay with your hands while the bike banks. The wheel rides in the same frame as the view: the
level seat for `"yaw"`, the kart's own orientation for `"yaw_pitch"` and `"full"`. While no draw takes
the copy (for 30 frames running; the race's opening pan does this) the vehicle's own is drawn as
the game poses it. The copy keeps being published, so the vehicle's own wheel turns again as soon as
draws take it again, and the log notes both switches.

**The placeholder wheel.** `placeholder_steering_wheel = true` (off by default; F10 and the Quest
launcher's Settings > VR) draws a separate VR wheel for karts, or handlebar for bikes, whenever the
vehicle's own is not the one turning: with `native_steering_wheel = false`, and during the stretches
above where no draw takes the copy. Off, only the vehicle's own is seen, and the hands reach for it wherever its geometry is known,
turning or not; where it is not (a kart whose grips were not found), they reach for where the
placeholder would stand, in front of the seat.

Validated on the extracted PAL disc's 216 single-player kart/character and Mii combinations
(all 18 kart types): each resolves its node-0 bone as the runtime does and selects the complete
21-position rim and 15-position spoke assembly, with the remaining positions unchanged and
connected-piece distances preserved. Regression tests also cover raised/narrow grips, domed hubs,
rotated roots, a node-0 bone listed behind another, child joints, chassis triangles crossing the
wheel volume, degenerate strip connectors and malformed MDL0 data. This asset check does not by
itself verify every combination's live draw matching or modded vehicle models.

The substitution is decided per draw, and a draw that folds into a neighbour renders through that
neighbour's array binding, so only draws that reached the same decision may merge. Deciding this
per array instead, and so refusing to merge every primitive that binds the vehicle's array, cost 6 ms
of GPU time a frame on a Quest 3 (a race frame has 228 such primitives, recorded once and replayed in
the mono pass and both eyes) and took a 56 FPS race down to 42. `debug.wiicompiled.fpslog 1` reports
the draw calls a frame and the primitives merged away, which is where that shows up first, and a line
counting why the other draws did not join the one before them (a GX state change, a particle quad, a
2D draw while stereo records, the first draw of a pass, lines or points, a vertex-motion or instanced
predecessor, or the wheel's arrays), and with the geometry cache on (`[video] geometry_cache_mb`, the
Quest's default) a line counting the display lists it served and resolved.

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

**Tracked hands.** `hand_tracking` (Quest only for now, default off; the Quest launcher's
Settings > VR and the headset panel's Camera tab, under hand steering, which it needs) poses the
cockpit hands from the headset's hand tracking instead of curling them with the grip. Two hand
trackers (`XR_EXT_hand_tracking`) are located every XR frame at the display time. While the
controllers are held the Quest builds the joints from their touch sensors
(`XR_EXT_hand_tracking_data_source`'s controller source: the trigger finger, the thumb on its rest or
a button, the grip); once they are put down, from its cameras. The runtime's hand mesh is then
skinned with the joints themselves, each joint's tracked pose times its inverse bind pose (the bind
poses are in the mesh's space, as `xrLocateHandJointsEXT` reports poses), with no curl and no grip;
a runtime with joints but no mesh (SteamVR, Virtual Desktop) gets a skeleton along the joints. A
hand whose joints are not located, or not finite, falls back to the curl at its grip. The trackers
exist only while the option and hand steering are both on, and serve the mesh too; the extensions
(with `XR_FB_hand_tracking_aim`) are asked for when either is on at launch, otherwise turning the
option on applies after a restart. The log says `OpenXR tracked hands ready (controller-driven
hands: yes|no)` and, at each change, `OpenXR tracked hands: left camera, right controller`; the
headset panel shows each hand's source under the checkbox.

The Quest app declares `horizonos.permission.HAND_TRACKING` (and the older
`com.oculus.permission.HAND_TRACKING`), normal permissions granted at install with no prompt, and
`oculus.software.handtracking` as optional: without that flag Horizon OS keeps the app
controllers-only. With it, putting the controllers down drives `khr/simple_controller` from the
hands (an index pinch is select, the left palm-up pinch the menu), and every change of interaction
profile is logged (`OpenXR interaction profiles: left ..., right ...`). A hand driving that profile
instead of the Touch one (its squeeze action inactive, its select active) is a bare hand; with
tracked hands off it presses nothing but the menu gesture, which pauses, is not drawn, and feeds
no Wii Remote motion, so the permission changes nothing for players who leave the option off. On
PC none of this applies: a PC runtime can drive real controllers through `khr/simple_controller`
and synthesize joints for them, so a hand-edited `hand_tracking = true` only changes the drawing.

Left to itself, Horizon OS switches all input between the controllers and the hands, and it
switches back to the controllers as soon as one lying on a table moves, so a race started with the
controllers connected tended to stay on them (a Quest 3 log went controllers, hands, controllers
within seconds). An app cannot disconnect them. Instead, with tracked hands on, the input resumes
simultaneous hands and controllers (`XR_META_simultaneous_hands_and_controllers`, Meta's
"multimodal"), which overrides that switching: a controller that is not in a hand no longer owns
it, so the cameras track that hand at once, while a held controller keeps working (its fingers
from its touch sensors). The log says `OpenXR simultaneous hands and controllers on` (and `off`
when the option goes off). Meta documents that it cannot run together with passthrough and wide
motion mode both on, and not while body tracking is; this app uses neither of the last two.

With tracked hands on, bare hands drive. A hand is bare while it has no controller in it (its
squeeze action inactive) and either drives `khr/simple_controller` or has camera-tracked joints (a
free hand under simultaneous tracking may get no profile our actions are bound in),
latched through `wheel_tracking_grace` (Meta also drops the select
action while a hand is lost) and cleared as soon as a controller's squeeze is back. Its palm joint
stands in for the grip and a grasp for the squeeze: the middle, ring and little fingers' flexion,
summed over each finger's three joints, read as 0 below 1.2 radians (a relaxed hand) and 1 from
3.0 (a hand closed on a rim), so the wheel's own 55 % press and 15 % release apply, and closing a
hand on the rim takes hold. The grasp never reaches the game's buttons (as a squeeze it would press
the gamepad's shoulders). In the cockpit, while a bare hand holds the wheel it holds the gas (A,
the gamepad's South) until both hands let go, and a pinch from a free bare hand, either one, uses
an item (Z, the gamepad's L) if the hand has been off the wheel for 0.15 s and is still mostly open
(a grasp under 0.5), so neither opening a hand off the rim nor closing one on it fires one;
holding the pinch holds the button. The left palm-up pinch pauses (a
pinch made in that gesture is not an item: `XR_FB_hand_tracking_aim` reports the system gesture).
With no bare hand on the wheel, in the cockpit or anywhere else, a right pinch is A and the
pointer follows the hand's aim ray, which is what the pause menu and the results need; in a race
it also gives gas without steering. A left pinch then does nothing, so the headset panel cannot be
opened with bare hands (an open one takes a right pinch, and the palm-up pinch closes it). The
game's own pointer switch cannot tell driving from those menus: MKW keeps it on in a race (checked
on a Quest 3, 2026-09-25). Manual drift has no gesture: choose Automatic drift. The headset panel
reads out each hand's source, grasp, hold and pinch under the checkbox, for tuning.

A bare hand feeds no Wii Remote motion: camera-tracked poses are too noisy to differentiate twice,
and turning the wheel would trick and wheelie. Tricks come from a flick instead. Both hands on
the wheel rising together (at least 1.2 m/s on average, 0.6 m/s each, within 0.6 m/s of each
other) or a free bare hand rising at 1.5 m/s makes one; a turn, where one hand rises as the other
drops, never does, nor does a lone hand on the wheel. A rise has to last three samples and cover
5 cm within 150 ms, a pose jumping faster than 5 m/s (tracking coming back) resets it, and
flicks are 0.5 s apart. A flick plays one shake on the remote's accelerometer: 150 ms, so the
guest sees it on at least three of its frames, one cycle up to +2 g and down to the -3.6 g limit,
as Dolphin's emulated shake does. Only the Wii Remote presentation has it (the gamepad has no
shake); `debug.wiicompiled.inject <n>:flick` plays the same shake with or without hands.

**Hands and the separate wheel.** Hands are drawn while hand steering is on: the runtime's own hand
mesh where it offers one (`XR_EXT_hand_tracking` and `XR_FB_hand_tracking_mesh`, requested when
hand steering or tracked hands are on at launch), otherwise procedural gloves that curl with the
squeeze. A Quest 3 offers that mesh even without the app declaring hand tracking, and the log says
which is drawn (`[mkw-vr] cockpit hands:`); a hand the [character body](#character-body) draws
gets neither. Both close their fingers towards the palm: the mesh's joints point
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
geometry and the hand-off to the game, and `mkw_vr_hand_tracking_tests` the tracked hands' rules
(grasp, bare latch, pinch gate, bare-hand buttons, flick and its shake); `gx_fifo_tests` covers the
per-draw substitution and the overlay geometry, joint skinning included, and `cockpit_gpu_smoke`
its depth test on a real GPU.

## Character body

Ported from heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS)
(GPL-3.0-or-later), whose `docs/BODY-IK.md` describes the original. It applies to the cockpit seat.

With `body_ik = true` (the default; **F10 → Camera → Character body**, the headset panel's Camera
tab and the Quest launcher's Settings > VR) your own character is drawn around you in the cockpit,
from the game's own skeleton, mesh and textures: the torso sits in the seat, the arms reach for the
tracked controllers, and the head is hidden so it never blocks the view. `body_ik_hands_only = true`
(*Character hands only*) hides the body and arms and keeps only the character's hands, following
the controllers. Both are live. Nothing is added to the game's data.

It is drawn only while first person is what the race shows, with the cockpit seat, and not while
you are a Bullet Bill. Standard rigs (both arms' `arm_*1`, `arm_*2` and `wrist_*1` bones, and a head
bone) are supported. Miis, whose face is a separate model, and skeletons it does not recognise keep
the previous behaviour: the driver is hidden as `first_person_hide_driver` says, and the VR gloves
are drawn. King Boo, whose head and body cannot be separated, is always hands only.

**How it is drawn.** One native function wraps `nw4r::g3d::ScnRoot::CalcView` (0x8006FA50,
registered with `REGISTER_NATIVE_FUNCTION_AS`, which keeps the translated body for the wrapper to
call). Before the view calculation it rewrites the local driver's world-matrix palette, and only
that:

- the seated torso is latched once per driver and model, in the kart's own frame, so it stays with
  the kart and the game's drift, jump and trick animations cannot lift it into the view;
- the legs keep their native driving and drift articulation relative to that seat, only while both
  of the driver's animation layers play a driving pose (driving, drift, steering, reverse, wheelie
  or waiting); tricks, mini-turbo bonuses and celebrations keep the seated legs;
- each arm is solved with two-bone IK to its controller grip, its length fitted to the reach, its
  sleeve at 65% of the model's thickness and its hand at half size, placed by one of 24 built-in
  palm profiles, one per standard character, recorded in heurazy's calibration workshop (a
  standard rig without a profile, such as a custom character, gets a generic wrist offset);
- the head, and Toadette's separate hair, collapse at their pivot.

It then re-runs the skinning for those matrices, calls the original `CalcView`, and restores the
original palette and its attributes, so gameplay, physics, the other racers and network data never
see them. Being a new native registration, it needs both games retranslated, as
`Launcher/LocalBuild.ps1` does.

**Hands.** The cockpit draws no VR glove or hand mesh for a hand the character draws, but still
draws what that hand carries, the held item and its count. Which hands the character drew travels
with the sealed guest frame (`AuroraCockpitItem::characterHands`, applied to
`AuroraCockpitHand::characterHand` for the replayed frame), so a glove is hidden on exactly the
frames whose character draws that hand. The arm follows the controller pose sampled for the game
frame, while the held item stays on the controller pose of each displayed frame, so in fast motion
(a throw swing) the item can lead the character's hand by about a frame. A bare tracked hand keeps
the tracked-hand mesh, and its character arm stays seated.

The seated skeleton is latched once per driver and model, at the first frame the body is drawn
(normally the end of the race intro, in the waiting pose). A recenter or a settings change keeps it;
a new race, a driver change or a new kart takes it again. If CalcView runs more than once in a guest
frame, the pose is solved once and only the palette is set again around each call.

`mkw_vr_body_ik_tests` covers the leg-animation gate, the seated limbs, the profile keys and all 24
palm profiles, the reach fit, the IK solve, the wrist placement, segment thinning, alignment and
the rig classification.

**Cost on a Quest 3** (2026-10-08, base game, Birdo at the start of the first Grand Prix race with
the CPUs racing, controllers idle, GPU clock fixed at 492 MHz; `body_ik` switched live in one session,
8 rounds of 20 s with the order swapped each round): the compositor's App GPU time went from 6.80 to
7.11 ms a frame (GPU 56.7% to 59.3%), the driver being drawn in both eyes again; CPU 48.2% against
46.1%, so no measurable CPU cost. The game held 60 FPS and the headset 72 both ways. simpleperf
puts the body's own work at about 0.17% of the game thread.

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
- `immersive_window` is not a policy state: the race is `ImmersiveRace` either way, and only the
  packet and the layer that shows it carry the window.

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
frames. It interpolates at the local scene playback time, then applies that packet's predicted head pose and
the scene anchor to both eyes. Native offscreen effects and the 2D HUD retain their game-frame
updates. A mid-frame EFB readback invalidates retained GPU data; a policy-tag mismatch rejects
the replay. Missing matches use current transforms, and stalls clamp at the last known pose
instead of extrapolating. The ordinary desktop interpolation settings remain independent.

Matched rigid draws retain transform history through rotations over 90 degrees per game frame
(such as spinning kart tires); large spins use spherical interpolation to preserve angular speed
and wheel shape. A one-matrix draw is split into a rotation and an upper-triangular stretch
(scale and shear), so a sheared matrix interpolates too. Lakitu sways by tilting his whole
body's Y axis (`Lakitu::Movement::UpdateScale`): his goggles and eyes ride his face bone as
one-matrix draws, and his head is skinned to that same bone. When the shear was rejected, the
goggles stayed on the game frame while the head moved on, and sank into it. Camera/seat
anchors keep their conservative cut and rigidity guards, and draw translation, matrix validity
and indexed-palette topology checks still apply. Exact mesh/pipeline identity can
also retain motion across texture-pattern swaps, as used by the Waluigi Stadium crowd. The crowd's
image animation itself still updates at the game's cadence; its transform can move at the headset
rate without snapping whenever the image changes.

For single-player races the guest thread also publishes the recorded scene camera with the
frame. VR matching and previous transform endpoints are expressed in the current camera's
coordinates, while replay samples the world-space camera/seat pose separately. Camera turns
therefore do not trip the 1500-unit object-motion guard on distant scenery. Held particles,
new objects and rejected billboard animations also follow that sampled camera instead of
mixing a current-frame view with an interpolated seat. Direct vertices already in view space
with an identity position matrix retain that matrix, avoiding a second camera transform.
Particle simulation and texture/vertex animation still run at the game rate. Actual teleports,
camera cuts and malformed view matrices retain their guards. Multi-camera frames and hosts
that do not publish a view keep the existing interpolation path.

CPU-built particle quads are paired one quad at a time, and only where the pairing is
unambiguous. An emitter draws many look-alike quads: a boost speed line (`rk_koukasen`) is two
crossed 12 × 300 quads, two new lines start on the same ring every frame, and each moves
further per frame than the gap to its neighbours. Pairing nearest centres swapped about half
of them and swept every new line in from one that had just died. Each quad therefore also
records its two edges. A pair must change shape by less than 30%, each side must prefer the
other, and its cost (squared centre distance plus squared edge change) must beat the runner-up
among other particles by 1.5×. The two quads of one cross share a centre and do not count as
rivals. A particle already tracked must land within a quarter of its last step of its
predicted position. A particle without a path is read two ways, fixed in the world or carried
with the camera, and the group follows the reading that fits its tracked particles: carried
ones step less in camera space. A first step is drawn moving only when both readings choose
it; otherwise it just seeds the path. An unpaired quad is held where the game drew it: in
camera space when its group follows the camera (speed lines, kart sparks), in the world
otherwise (smoke left behind). Groups with more than 4096 candidate pairs are held without
matching.

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

Two classes of draw are deliberately left on their recorded transforms: native framebuffer effects,
which belong to the rendered image rather than to the game's 2D layer, and any draw whose matrix is
not actually affine. A native framebuffer effect is recognised three ways: it samples a freshly
produced EFB copy that is reduced or blended back (bloom and the rest of the post-processing chain);
it samples a fresh copy inside a viewport that does not cover the frame (an offscreen bake such as
the 440x440 corner in which Mario Kart Wii builds its object shadow map, copying each stage back
out); or it samples no texture and blends with destination alpha. The last is how Mario Kart Wii
draws its dynamic shadows:
the shadow volumes are perspective draws that count their coverage into the EFB's alpha plane, and one
full-screen orthographic quad then darkens the image by destination alpha. The eyes replay the
volumes, so that alpha exists in each eye, and the quad has to cover the whole eye: on the virtual
screen it shaded only the screen's rectangle, cutting every shadow off at its edge. Retained one-shot
EFB bakes such as Mario Kart Wii's minimap are treated as game art and remain eligible for the
screen.

One native effect is replayed differently: a composite that samples a freshly produced,
frame-sized depth copy. Mario Kart Wii draws a ghost kart by rendering it alone into the cleared
EFB, copying the frame's colour and depth out, drawing the race, and blending the copies back
with one orthographic quad whose depth comes from the depth copy. Left on its recorded transforms,
that quad stamps the desktop's flat image of the ghost over each eye, following the head and cut
off by the eye's own ground. An eye therefore skips the perspective draws of the pass that
produced the depth copy where they were recorded and re-issues them in the composite's place,
with this eye's transforms, a constant-alpha blend (`kCompositeSourceAlpha`, about the strength of
the game's own composite) and a depth test against the world the eye has drawn by then: the ghost
in stereo, translucent, where the game put it. The desktop image is unchanged. The link is made as
the composite is recorded, so frames without a composite do no work for it, and a draw only
costs one comparison. The constant-alpha pipeline variants exist only for pipelines seen among
a composite's source draws: such a pipeline gets them from its next draw, and until then the
eye leaves that draw out, so a ghost first drawn in a session is missing from the eyes for that
one frame. The link is logged
once (`Immersive replay: pass N ... is re-issued by each eye`).

A reprojected 2D draw
uses the full eye viewport and scissor because its recorded rectangle no longer describes where it
ended up; its original viewport is folded into the projection instead.

## The immersive window

`immersive_window` shows the immersive race through a window rather than all around you. The window
is the race's 2D-layer screen: `hud_width_meters` across and `hud_distance_meters` ahead of the
race origin latched at the race start, turned by the lean-back angle, its height following the
picture's aspect. That is where the HUD, the Wii Remote pointer and the settings panel already sit
in an immersive race, so the HUD lies on the window's plane and the pointer aims at it. The window
always carries the 2D layer, whatever `hud_virtual_screen` says: stretched across the eye, the HUD
would be cut by the window's edges. It sits straight ahead of the race's forward direction, as the
HUD does, which is the reference space's forward rather than the heading the menu screen was
anchored at, so after an in-game recenter while facing sideways the two can differ.

Inside the window nothing changes: the eyes are the immersive race's, with their per-eye frusta,
head tracking, first person, hand steering and VR frame interpolation. Moving your head therefore
shifts the view through the window like a real window's, and geometry nearer than the window's
plane is still cut by its edges, as a stereo picture's frame cuts it.

How it is drawn: the pacing thread marks the stereo packet (`AuroraStereoFrame::window`), and after an
eye's last draw Aurora covers the eye with one full-screen triangle (`aurora-main/lib/gfx/window_mask.hpp`)
that keeps the colour inside the window with alpha 1 and leaves transparent black outside it, with a
one-pixel ramp at the edge. The triangle carries, at each corner, where that pixel's ray meets the
window's plane in homogeneous window coordinates (`stereo_replay::window_mask`), which interpolate
exactly across the image. It is drawn in the eye's own last render pass, so it adds no pass and no
tile load; an eye that is still split into several render passes gives it a pass of its own, as
the cockpit overlay has. On
the Quest the backend submits the passthrough layer, then the projection layer with
`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` (premultiplied alpha), then the settings panel.
The flag travels with the packet, so the eyes Aurora masked and the layer that blends them always
belong to the same frame, and switching the race view mid-race needs no safety generation:
presentation stays `ImmersiveRace`. The PC backends keep their projection layer opaque, so the window
is surrounded by black there.

**Only the window is rendered on the Quest.** Rather than render the whole eye and mask most of it,
the pacing thread aims each eye through the window itself (`AimEyesThroughWindow` in
`openxr_integration.cpp`): the eye keeps its position but looks square-on at the window's plane, through
an off-axis frustum just around the window, so the image is the window. It keeps the display's pixel
density (the swapchain's pixels per unit of tangent as the eye is located) at the window's size seen
from the race origin, which is fixed while the window's geometry is: about 680 x 380 per eye with the
default window at `render_scale` 0.8, against 1344 x 1408 for a whole eye. A two-pixel border around
the window is left transparent by the mask. The frame's views carry that pose and field of view to the
projection layer, whose `imageRect` is the rendered part of the swapchain image, and the compositor
reprojects it like any other. Aurora copies the smaller eye into the corner of the shared buffer
(`vulkan_interop.cpp`), and does not foveate these eyes: their field of view follows the head, which
would rebuild the density map every frame, and they are small already. The PC backends copy whole eyes
into the swapchain, so there the window's eyes stay full size and masked.

Measured on a Quest 3 with a Retro Rewind race paused (the same 439 draw calls every frame,
`render_scale` 1.0, 60 FPS throughout), switching the race view from the headset panel:

| Race view | GPU level and clock | App GPU per frame | Both eyes | GPU load | Compositor |
| --- | --- | --- | --- | --- | --- |
| Immersive window | 1, 456 MHz | 11.0 ms | 8.4 ms | 82% | 1.6 ms |
| Immersive | 3, 599 to 640 MHz | 13.4 ms | 10.3 ms | 88% | 0.7 ms |

The headset raised the GPU's level for the fully immersive race and it still took longer: in clock
cycles the window's frame is about 42% cheaper (5.0 against 8.6 million), which lets the Quest keep the
GPU at its lowest level. The compositor's extra time is the passthrough. During a race at `render_scale`
0.8, switching the two with a temporary debug property, both eyes took 6.3 to 7.5 ms through the window against
9.1 to 9.5 ms whole and masked at similar draw counts (the compositor's `SF` field read 0.31 against
0.80), with the clock wandering between 350 and 600 MHz. The saving is smaller than the eye's pixels
(about 13% of a whole eye's) would suggest because much of an eye's cost is the geometry of every draw,
which each eye still processes; it grows with `render_scale`. Read the VrApi line's
`CPU4/GPU=<levels>,<clocks>MHz` before comparing two timings.

`gx_fifo_tests` covers the window's geometry (its corners through an asymmetric eye frustum, its
agreement with the HUD's placement, an eye turned away or beyond the window, a sideways step), and
`mkw_vr_config_tests` how the two keys read as one race view.

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
when it is drawn in a single render pass, as every eye is by default; an eye that a partial clear still
splits is drawn at full rate. The session log reports what happened: "Fragment density maps:
enabled" at startup, one "eye foveation" line per eye and level with the map's size, and the "Eye
replay plan" lines. `debug.wiicompiled.fdm 0` launches without density maps at all
(`docs/quest-port.md`).

What it saves depends on how much of an eye's cost is shading pixels. The numbers below are from a
Quest 3 at Luigi Circuit's Grand Prix start: GPU time of both eyes per frame, all settings
interleaved within one session (`docs/quest-port.md` has the method).

| `render_scale` (eye size) | One pass per recorded pass | One pass per eye | `low` | `medium` | `high` |
| --- | --- | --- | --- | --- | --- |
| 0.8 (1344x1408) | 5.82 ms | 5.11 ms | 5.09 ms | 5.36 ms | 5.11 ms |
| 1.3 (2184x2288) | 6.32 ms | 5.81 ms | 5.33 ms | 5.00 ms | 4.56 ms |

At the Quest's default 0.8 an eye's time goes mostly to geometry and to storing its tiles at full
resolution. The Wii's shading is cheap, so foveation saves nothing measurable there, although the
density map verifiably applies (4x4 blocks at the view's edges on High). At higher render scales it
takes 8 to 22% off the eyes, which is where it earns its keep, bought with a softer periphery. The
Quest defaults to `medium` all the same: it costs nothing measurable at 0.8, it is already on when
the render scale is raised, and a session launched with a level can change it live, where one
launched with `off` needs a restart. The Quest's GPU applies the density per screen tile, and inside a
reduced-rate tile it also samples textures one level blurrier per halving. Most surfaces hide it,
but fine animated detail does not: on Retro Rewind's swamp goo the tiles show as squares where the
ripples give way to a smoother look. It is no fix for a heavy track: on Retro Rewind's SNES Ghost Valley
2 at 1.0, GPU-bound at about 40 FPS, no level raised the frame rate, while merging the eye passes
did (39 to 41.5 FPS).

The geometry side has since been cut by the eye's last pass discarding its depth and stencil and by
the resident geometry cache with native vertex input (`[video] deindex_vertices`, `geometry_cache_mb`,
`native_vertex_input`, on by default on the Quest, opt-in on the PC): the course's and karts' display
lists are resolved once into flat records in a GPU buffer and drawn through the vertex-input stage
instead of decoded per vertex in the shader. At Luigi Circuit both eyes went from 6.7 to 4.6 ms and
the frame's GPU time from 9.5 to 7.2 ms; `docs/quest-port.md` has the method and the per-step numbers.
Since then the Quest also binds Dawn's own device to the session (`[vr] direct_presentation`, a
further 3 ms of the frame's GPU time) and draws both eyes in one Vulkan multiview pass
(`[vr] multiview`: both eyes 4.3 to 3.9 ms, the frame worker's encode 3.3 to 2.5 ms); the `fpslog`
GPU line then shows one `eyes` figure instead of `eyeL` and `eyeR`. In that pass every GX draw binds
its pair of eye uniforms, indexed by the view; a draw the eyes keep on its recorded transforms, such as
the dynamic shadows' tint quad, binds two copies of the uniform it recorded with.

## Diagnostics

**F10 > Diagnostics** holds three bug-report aids.

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
- **Scene motion (`[vr-motion]`).** With VR interpolation active, Aurora logs a separate
  one-second summary. `blended`, `previous`, and `current` count samples inside the two
  known scene endpoints or clamped to either endpoint. `discontinuous` counts samples
  without continuous history/timing. `same-time` and `backwards` compare the sampled
  game-scene timeline, not head poses, pixels, or compositor submission FPS. Persistent
  `same-time` with fresh XR layers can explain smooth head tracking but juddering steering.
  The latest `sample-boundary` and `weight` expose exhausted scene history; `display-boundary`
  and `prediction-lead` describe the independent runtime head-prediction horizon.
  Latest draw counts distinguish identity `matched` from transform `prepared`; `rejected`
  means a paired draw failed the transform guards. Unmatched draws can also snap, and
  these counts do not measure GPU time. `max-rejected` is the highest rejection count
  observed during the window. `scene-step` gives minimum/maximum forward scene-time
  advancement per sample in milliseconds; steady 90 Hz scene motion should advance about
  11.11 ms each time, even when every submission is fresh. Enable the same renderer log with
  `debug.wiicompiled.fpslog=1` on Android, or `AURORA_VR_MOTION_LOG=1` in the environment
  before launching a host that does not use OpenXR.

  `camera-separated=true` confirms the frame used the published scene camera and the separate
  camera/object interpolation path. If false during a single-player race, include that in the
  capture so a missing/invalid camera publication can be distinguished from object rejection.
  For steering judder, capture a single-player race with **VR frame interpolation = Auto**
  (the Graphics interpolation setting is separate). Keep the headset refresh, render scale,
  camera mode, and opponent count fixed. Compare a stationary head while alternating steering
  left/right with a stationary kart while turning the head. On PC, keep SSW disabled for this
  capture so generated compositor frames do not mask the application's scene cadence.
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

**First-person camera logging** is off by default as well. When it is on, a race in first person
writes a set of `[mkw-vr] first-person` and `[mkw-vr] cockpit` lines to `console.log` once per
second: the anchor's head offset from the race camera, the scene's view matrix and the kart's
physics pose (with the raw bits of its translation), where each candidate camera sits relative to
the kart, and the cockpit's wheel placement and race-camera check. They are the first thing to
read when the first-person view is misplaced. It applies immediately and is remembered as:

```toml
[diagnostics]
first_person_logging = false
```

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
build also fetches Vulkan headers (`vulkan_headers` dependency). The Quest's statically linked Dawn
(`android/Build-QuestDawn.ps1`) carries the same patches, and its direct presentation
(`docs/quest-port.md`) calls the same ABI directly.

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
| Android Vulkan (Meta Quest) | Implemented and running on a Quest 3. Since 2026-10-07 it binds Dawn's own Vulkan device to the session through the same custom-Dawn ABI as the Windows binding (`openxr_vulkan_direct.cpp`, `[vr] direct_presentation`) and draws the eyes straight into the swapchain images, which took the compositor's App GPU time from 8.2 to 5.1 ms on a Luigi Circuit race; with the setting off, or a Dawn without the hooks, the OpenXR side owns its own Vulkan device (`XR_KHR_vulkan_enable2`, `XR_KHR_vulkan_enable` fallback) and shares eyes with Dawn through `AHardwareBuffer`s ordered by sync-fd fences. Controllers arrive through OpenXR actions as a virtual SDL gamepad. See `docs/quest-port.md`. |
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
renderer a 60 Hz GX stream and an independent 90 Hz stereo provider, with a default 40 ms
head-prediction lead to exercise the multi-frame horizon seen in VDXR. The development check
produced 359 new stereo submissions in 4 seconds (89.7 FPS). This verifies submission cadence,
not full-race performance or visual quality on a headset. Pass a draw count, for example
`stereo_frame_worker_smoke 2000`, to stress uniform preparation and renderer/producer overlap;
`stereo_frame_worker_smoke 2000 0` checks native stereo with interpolation Off.
The fourth and fifth arguments select headset Hz and prediction lead in milliseconds:
`stereo_frame_worker_smoke 1 1 0 90 65` exercises a 90 Hz headset predicting 65 ms ahead.
Add a sixth argument of `1` to rotate the published camera around geometry 100000 units away;
`stereo_frame_worker_smoke 1000 1 1 90 65 1` combines that with indexed-palette stress. Check
`camera-separated=true`, transform rejection counts and scene cadence together.
Add a seventh argument of `1` for CPU-authored particle quads, for example
`stereo_frame_worker_smoke 1000 1 0 90 65 1 1`. Their matrices stay at identity
while their vertex centers move. VR interpolation records each four-vertex,
direct-F32 quad separately, pairs it across frames by centre and shape, and
shifts the current shape to the sampled center. Current texture, colour and
shape changes still occur at the game's rate; this is position interpolation,
not a particle simulation or a blend between texture animation frames. The
smoke's thousand overlapping quads exceed the pairing bound, so they measure
decoder and replay cost, not pairing. The GX tests cover pairing:
`VrSpeedLineEmitterKeepsEachStreakOnItsOwnPath` replays a speed-line emitter
for 90 frames and prints how many quads moved, were held or were misattributed.

Rigid meshes with a stable basis also reject a large reverse jump relative to
their measured, camera-independent velocity. This holds the new phase when an
animation resets, instead of interpolating backward through the loop. Coconut
Mall's escalator uses a repeating 20-unit phase. Normal direction changes remain
interpolated. `[vr-motion]` reports `vertex-motion` (usable particle pairs in
the latest seal), `vertex-held` (particle quads drawn where the game put them
because no unambiguous partner exists yet) and cumulative `wrap-cuts`; check
those during the affected effects, then confirm the visual result in the headset.
The `[vr-motion]` scene-step range measures actual playback cadence separately from submission FPS.
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
- Scene-specific comfort options and replay/spectator classification are future work.
- Hand steering works on a Quest 3 (2026-09-22): the kart's own wheel animated (228 draws a frame,
  the race camera's view matching the scene's exactly) and the wheel can be grabbed and turned. In
  that race the driver's eye was never calibrated, so the fallback placed the wheel centre about
  13 cm above eye level. Bikes and Quacker, and the PC, are still unvalidated. Hand steering needs
  analog grips (Touch); the simple controller profile cannot grab.
- The character body and the camera modes are not yet validated in a headset in this port, on the
  PC or the Quest, and the body's CPU cost on the Quest is not yet measured.
- The headset settings panel has no laser beam, only the cursor on the panel itself, and text fields
  cannot be typed into without a keyboard.
- On the PC the immersive window renders the whole eye and only masks it, so it costs what a fully
  immersive race costs. Hands and a separate VR wheel are seen only through the window.
- The desktop window remains available as a mirror/fallback.

OpenXR diagnostics are written to the normal run log under
`%LOCALAPPDATA%\WiiCompiled\Logs`. Search for `OpenXR` when reporting a startup or submission
failure.
