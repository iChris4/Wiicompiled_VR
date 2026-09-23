# WiiCompiled VR for Meta Quest (Android)

Standalone Android/OpenXR build of the Mario Kart Wii recompilation for Quest 1,
Quest 2, Quest 3, Quest 3S and Quest Pro. The full design, build walkthrough and current
status live in [docs/quest-port.md](../docs/quest-port.md); this directory only
holds the Gradle project, its helper scripts, the game kit tooling
(`QuestGameKit.psm1`, `Build-QuestGame.ps1`), the on-headset build toolchain
(`Prepare-QuestToolchain.ps1`, `toolchain/`) and `nod-jni`.

```powershell
powershell -ExecutionPolicy Bypass -File android/Prepare-QuestDependencies.ps1          # stages the SDL3 3.4.4 AAR once
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Install               # the app, debug-signed, installs over adb
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Headset quest1 -Install # Quest 1 flavour
```

One app offers both games, with a toggle on Home: Mario Kart Wii and, when your
translation includes the mod, Retro Rewind. The translated game (the translator's
`generated/` tree for **your own** PAL `RMCP01` disc) is passed with
`-Generated <dir>`; it defaults to the installer workspace next to this checkout.
No game data is ever part of the APK, and neither is any translated game code:
the APK carries one game kit holding a recipe per game, and your `libmain.so` is
built from your own disc, either on the headset (**Build on this Quest**, about
half an hour) or on a PC and imported:

```powershell
powershell -ExecutionPolicy Bypass -File android/Build-QuestGame.ps1 -Product base -Install
powershell -ExecutionPolicy Bypass -File android/Build-QuestGame.ps1 -Headset quest1 -Product base -Install
powershell -ExecutionPolicy Bypass -File android/Build-QuestGame.ps1 -Product retro_rewind -Mod <RetroRewind6> -Install
```

`-Mod` puts the Retro Rewind pack in the package; **Import from computer** takes
the same `.wcgame` from the Import folder. The headset can also fetch that pack
itself with **Download Retro Rewind** on Home, from Retro Rewind's own
distribution server, the way the PC launcher does.
Copy your disc image to the headset (for example `adb push MarioKart.iso
/sdcard/Download/`) and press **Select disc image** in the launcher, which checks
and extracts it with nod, as the PC installer does. Or push an already extracted
disc to

```
/sdcard/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/DATA
```

`Config.toml`, saves and logs live in the same `WiiCompiledOpenXRVR` directory.

The app opens on a 2D launcher panel modelled on the PC launcher (WheelWizard VR):
**Home** picks the game with a toggle, starts it in the headset and says what is
still missing (the disc files, the game itself, or Retro Rewind's pack), and
**Settings** edits `Config.toml` (VR camera, render scale, virtual screen,
resolution, controllers, audio), with an **Other** tab for the files and the
games built from them, with a **Reset installation** action that removes the game files (and
on request the built games and the Retro Rewind pack) so they can be set up again, and an
**About** tab carrying the credits. The game itself is `QuestActivity`, in its own
`:game` process; `adb shell am start -n org.wiicompiled.quest/.QuestActivity`
still starts it directly.
