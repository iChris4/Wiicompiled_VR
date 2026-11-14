# Voice chat

Room voice chat for Retro Rewind online play, on the PC and on the Quest. Everyone in the same
Retro Rewind room who runs voice chat is connected automatically: no codes, no separate app. It
speaks the protocol of [MKW-VoiceChat](https://github.com/zurasaaa/MKW-VoiceChat) (GPL-3.0, by
zurasaaa), so players of both clients share the same voice rooms; its voice engine is ported from
there. On top of it, VR gets a "who is talking" badge in races and positional voices.

Voice chat is **off by default** (`[voicechat] enabled = false`) and is offered in Retro Rewind
only, where its rooms live. Turn it on in F10 > Voice Chat, or in the headset panel's Voice tab.

## Privacy, read first

- **Voice goes peer to peer.** Each voice member connects directly to every other one (WebRTC,
  DTLS-encrypted), so they can see each other's IP addresses, as in the game's own online play.
- **Anyone can join a room's voice chat.** The signaling server resolves the room from the public
  RWFC room list and trusts the profile id a client claims; it cannot check that the client really
  plays in that room. A modified client could listen in and learn the voice members' addresses.
  This is MKW-VoiceChat's design, inherited as is: its authenticated path is not in service.
- The menu says both of these before voice chat can be enabled.
- What leaves the machine: your RWFC profile id (to the signaling server, which looks up your
  room), your voice and ICE candidates (to the room's voice members). The online session key the
  game receives is never read or kept, and nothing is sent outside an online room.

## How it works

```
 guest socket HLE ──GPCM login──▶ voice_api (identity: profile id)
 game thread ──RKNet room, karts──▶ voice_api (room snapshot ~2 Hz, positions 30 Hz in races)
                                     │
                          voice worker thread (runtime/voicechat/service)
                                     │
            signaling WebSocket ─────┼───── WebRTC data channels, one per voice member
     (MKW-VoiceChat's Worker:        │      (libdatachannel on mbed TLS; Opus packets)
      RR_DEV_ADMIT, peer intros,     │
      SIGNAL relay)                  │
                          miniaudio capture ─▶ processing ─▶ Opus ─▶ peers
                          peers ─▶ jitter buffers ─▶ Opus ─▶ stereo mix (gains) ─▶ miniaudio out
```

1. **Who you are.** The socket HLE (`runtime/src/hle/net/network_socket.cpp`) hands every TCP send
   and receive to `ObserveTcp*`, which only looks at the GameSpy presence stream on port 29900 and
   reads `\profileid\` from the `\lc\2\` login reply (`runtime/voicechat/proto/gpcm_identity.cpp`).
   The game opens that socket with a nonblocking connect, which never records a peer port in the
   socket table, so `ObserveTcpConnect` marks the descriptor at its first connect call instead.
   The bytes are copied, never changed: the game sees exactly what it would without voice chat.
2. **Where you are.** About twice a second the game thread reads the RKNet room from guest memory
   (`runtime/include/voicechat/voice_room_mkw.h`): room type, AIDs present, each AID's profile id
   from DWC's node list, the friend roster and race teams. Addresses are the PAL ones MKW-VoiceChat
   reads, mapped to NTSC through `MKW_GADDR`.
3. **Joining.** In a room, the worker opens a WebSocket to the signaling server and sends
   `RR_DEV_ADMIT <pid> PIDS=<room>`. The server answers with the voice room and introduces every
   voice member to every other one; each pair exchanges one complete ICE bundle (`MKWVC-ICE-1`) and
   opens an unordered data channel `mkw-voice`. The server finds the room in Retro WFC's public
   room list (`https://rwfc.net/api/wfc/groups`), which includes private (friend) rooms. A failed
   admission (a room the list has not caught up with) is retried every 10 s; MKW-VoiceChat itself
   does not retry.
4. **Talking.** 48 kHz mono, 20 ms Opus frames (VOIP, in-band FEC) in MKW-VoiceChat's 18-byte
   `MKWV` packet. The processing chain (level, RNNoise, gate, compressor) and the adaptive jitter
   buffer are MKW-VoiceChat's. Silence is still sent, so the mute and deafen flags stay current.
5. **Leaving.** Leaving the room, logging out or switching rooms closes the connection; the server
   tells the others.

The engine is its own static library (`runtime/voicechat`, C++23, `cmake/VoiceChat.cmake`) behind
the C++17 facade `runtime/include/voicechat/voice_api.h`; the runtime glue is
`runtime/src/voice_chat.cpp`. With `MKW_ENABLE_VOICECHAT=OFF` (the default outside Windows and
Android) the facade is a stub and the menu says the build has no voice chat.

### Positional voices

In a race, each voice comes from its player's kart: an equal-power pan on the horizontal angle and
a gentle roll-off (full volume to 8 m, never below 35%), plain stereo rather than HRTF
(`runtime/include/voicechat/voice_spatial.h`). The kart positions are read on the game thread
(online racer -> AID -> profile id), taken into the camera the scene is drawn with, into the
first-person seat when it is engaged, and in immersive VR rotated by the head pose the eyes were
built with (`vr/openxr_voice.h`). A voice straight ahead and close sounds exactly as without the
feature; outside races, or when no position is fresh, voices play centred.

### The headset badge

The desktop overlay shows who is talking, and the headset sees it on every menu screen. In an
immersive race only the eyes are drawn, so a small badge is laid onto the race's 2D screen at its
left edge (`aurora_imgui_set_stereo_badge`, a second slot beside the settings panel's). It exists
only while someone talks, while you transmit, or for 3 s after mute or deafen changed; otherwise
nothing is drawn or composited. It is drawn into the eyes even in the panel's layer mode, and hidden
while the panel is open (the Voice tab lists the same players).

## Settings (`[voicechat]` in Config.toml)

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | `false` | Voice chat on |
| `talk_mode` | `"voice_activation"` | `voice_activation`, `open_mic` or `push_to_talk` |
| `voice_activation_threshold` | `25` | 1-100 |
| `push_to_talk_key`, `mute_key`, `deafen_key` | `V`, none, none | SDL scancodes; in VR, push to talk is holding the left thumbstick in |
| `muted`, `deafened` | `false` | |
| `microphone_gain`, `playback_volume` | `1.0` | 0.25-5, 0-3 |
| `input_device`, `output_device` | system default | miniaudio device names |
| `normalization`, `noise_suppression`, `noise_strength` | on, on (PC) / off (Quest), 50 | RNNoise |
| `noise_gate`, `noise_gate_threshold`, `microphone_boost`, `compressor`, `compressor_strength` | off, 25, 1.0, off, 50 | |
| `platform_voice_processing` | `true` on the Quest | AAudio's voice-communication capture (system echo cancellation) |
| `mute_everyone`, `mute_only_friends`, `mute_everyone_but_friends`, `mute_teammates`, `mute_everyone_but_teammates`, `mute_new_players` | `false` | Mute rules |
| `peer_volume_<profile id>` | `1.0` | Per player, MKW-VoiceChat's key names |
| `positional` | `true` | Positional voices in races |
| `overlay_local_status`, `overlay_player_speakers` | `true`, `true` | Your voice state (bottom right) and who is talking (right); both also make up the headset badge |
| `signaling_url` | MKW-VoiceChat's Worker | Another signaling server |

## Platforms

- **Windows.** miniaudio on WASAPI. libdatachannel does not verify the signaling server's TLS
  certificate on Windows (an upstream limitation, MKW-VoiceChat included): the connection is
  encrypted but not authenticated, and an attacker on the network path could intercept signaling.
  The voice itself is end-to-end DTLS between peers.
- **Quest.** AAudio. The microphone is a runtime permission (`RECORD_AUDIO`): the launcher asks
  when voice chat is turned on in its Settings > Voice, or at Play if it is enabled without the
  permission, since the game's process runs in VR where no prompt can show. Refused, the game still hears the room. Capture uses the voice-communication preset so
  the headset's echo canceller keeps the strap speakers out of the microphone; playback uses the
  game usage, never call routing. TLS checks the signaling server against the APK's `cacert.pem`
  (mbed TLS has no access to Android's store). The new libraries change the game kit, so both games
  are rebuilt after updating the APK.

## Using another signaling server

The default is MKW-VoiceChat's public Worker, which runs on Cloudflare's free tier with a shared
daily request quota. Its author agreed to this project using it; heavy use eats into the same
quota MKW-VoiceChat's own users share. `signaling_url` points voice chat elsewhere, for instance a copy of MKW-VoiceChat's
`signaling_worker` deployed on your own Cloudflare account, or `wrangler dev` while developing.
Clients on different servers do not meet.

## Testing

- `ctest -R "voice|gpcm"`: the wire format byte for byte against MKW-VoiceChat's, the GPCM parser,
  the room and racer reads against a fake guest with faults injected, the positional gains, and a
  WebRTC loopback (`mkw_voice_transport_smoke`: two peers, real DTLS and SCTP, host candidates).
- A whole session without the game: `python -I runtime/tests/voice_signaling_mock.py 8788` stands
  in for the Worker, and two `mkw_voice_session_probe --pid A --peer B --server ws://127.0.0.1:8788/`
  processes play two consoles; each exits 0 once the other is connected and its packets arrive. They
  open this machine's default microphone and speakers.
- In the game: two consoles in the same Retro Rewind friend room (the PC and the Quest have
  different profile ids). The Voice tab shows the state, the players and their flags; the run log
  has `[voice]` lines for each step.
