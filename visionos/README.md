# WiiCompiled Vision

The Apple Vision Pro app around the game runtime. To make the app from your
own disc, follow [`../docs/visionos-getting-started.md`](../docs/visionos-getting-started.md)
(`Make-VisionOS-App.command` does every step). Design and status:
[`../docs/visionos-port.md`](../docs/visionos-port.md).

- `CMakeLists.txt`: the Xcode project (the runtime tree as a subdirectory, the
  SwiftUI app embedding the games as frameworks, WiiCompiled and, when
  translated, Retro Rewind, and loading the chosen one at Play).
- `App/`: the SwiftUI sources (launcher with Play, Profiles, Miis and Settings
  tabs, the game picker, the Retro Rewind pack downloader, the game loader,
  Retro Rewind's licences with their Retro WFC data and profile import and
  export, the Mii database editor
  and its picture renderer, the Config.toml editor), `Info.plist` and
  entitlements.
- `Build-VisionOSDawn.sh`: Dawn (Metal, static) for the `xros` or
  `xrsimulator` SDK, as the package Aurora's provider consumes.
- `Build-VisionOS.sh`: Dawn, configure, build, optionally install on the
  paired headset.
- `Make-VisionOS-App.command`: the whole path for players, from the disc image
  to the running app: prerequisite checks, extraction, translation
  (`Launcher/local-build-macos.command --translate-only`), Retro Rewind's pack,
  `Build-VisionOS.sh --install`, the disc copied to the headset with
  `devicectl`, launch. Double-clickable; every step is skipped once done.

```bash
visionos/Make-VisionOS-App.command --game /path/to/RMCP01.rvz --retro-rewind download
# or, with a translation already in generated/:
visionos/Build-VisionOS.sh --team <TEAMID> [--bundle-id <ID>] [--retro-rewind-dir /path/to/RetroRewind6] --install
```
