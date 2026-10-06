# Installing WiiCompiled Vision on Apple Vision Pro

WiiCompiled Vision is the Apple Vision Pro app of this fork of [WiiCompiled](https://github.com/patchzyy/Wiicompiled), which turns Mario Kart Wii's PowerPC code into native code with a static recompiler: there's no emulator at runtime. On the headset the menus sit on a virtual screen in your room, and races play in stereo, from the chase camera or life-size in the cockpit, with bare hands or a Bluetooth game controller. The visionOS port is experimental and lives on the `vision-pro` branch. The full tutorial is [docs/visionos-getting-started.md](docs/visionos-getting-started.md).

## What you need

- An Apple Silicon Mac (M1 or later), with about 15 GB free besides Xcode
- Xcode 16 or later with the visionOS platform (**Xcode > Settings > Components**), and your Apple ID signed in under **Xcode > Settings > Accounts**. A free account works, but its apps stop opening after 7 days.
- Homebrew with CMake, Ninja and the .NET SDK 8 or newer:

  ```sh
  brew install cmake ninja
  brew install --cask dotnet-sdk
  ```

  Building Dawn, the graphics library, also uses Python 3 and git.
- Apple Vision Pro, paired with the Mac
- Your own Mario Kart Wii disc image: the European PAL release, `RMCP01`, unmodified, as `.iso`, `.rvz`, `.wbfs`, `.ciso` or `.gcm`
- About an hour the first time, mostly compiling

## Your game files

There's no Nintendo code, asset or game data in this repository or its releases. You make the app on your Mac from your own disc.

1. Give your disc image to `visionos/Make-VisionOS-App.command` (see *Build from source*): pass `--game /path/to/RMCP01.rvz`, drag the file onto the Terminal window when the script asks, or pick it in the file dialog when you double-click the script in Finder.
2. The script extracts the disc to `Assets/DATA` and checks that it's a clean PAL `RMCP01` image. Other regions and modified images are refused.
3. It translates the game's code, builds the app, and copies the extracted disc into the app on the headset (`Documents/WiiCompiled/DATA`).

If the copy can't finish, run the script again (it resumes), or copy everything inside `Assets/DATA` into the app yourself with Finder: the headset > **Files** > **WiiCompiled Vision** > **WiiCompiled** > **DATA**.

## Install

There's no prebuilt app or TestFlight build for Apple Vision Pro: visionOS only runs apps signed by Apple or by you, and the app carries code translated from your own disc. The releases on this repository are for Windows and Meta Quest. Build it with the steps below.

## Build from source

Start from a checkout of the `vision-pro` branch (the tutorial uses `git clone --branch vision-pro https://github.com/iChris4/Wiicompiled_VR.git`).

1. Pair the headset. On the Vision Pro, open **Settings > General > Remote Devices**. On the Mac, open **Xcode > Window > Devices and Simulators**, pair the headset and enter the code it shows. Turn on **Developer Mode** when asked (**Settings > Privacy & Security > Developer Mode**) and restart the headset.
2. Run the script: double-click `visionos/Make-VisionOS-App.command` in Finder, or in Terminal:

   ```sh
   visionos/Make-VisionOS-App.command --game /path/to/RMCP01.rvz
   ```

   Add `--retro-rewind download` to include [Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind); then, on the headset, pick it in the Play tab and press **Download Retro Rewind** (about 2 GB from Retro Rewind's server). If Xcode has several teams, the script asks which one signs the app (or pass `--team TEAMID`); `--device UDID` picks the headset. The app's bundle identifier is made from that team's ID; `--bundle-id com.yourname.wiicompiled` chooses another. The script checks the Mac, extracts the disc, translates the game, builds and signs the app, installs it, copies the disc and launches the app. The first build takes 30 to 60 minutes; later runs skip the finished steps. Keep the headset unlocked and awake while it installs and copies.
3. The first time, trust your developer profile on the headset: **Settings > General > VPN & Device Management**, tap your Apple ID, then **Trust**. Open **WiiCompiled Vision** from the Home View and press **Play**.

With a free Apple ID, run `visionos/Make-VisionOS-App.command --reinstall` every 7 days to sign the app again. To update, `git pull` and run the script again.

To run the steps by hand instead (translation, Dawn, then `visionos/Build-VisionOS.sh`), see *Building* in [docs/visionos-port.md](docs/visionos-port.md#building).

## Notes

- Menus appear on a virtual screen. Look at a button and pinch, move your hand to adjust the pointer while pinching, and let go to press. Press the Digital Crown to leave the game.
- Races start in first person, seated in the cockpit. In the launcher, **Settings > Camera** switches to the chase camera, and **Race view** chooses **Immersive**, **Immersive window** (the stereo race through a window, with your room around it) or **Flat screen**.
- To race without a controller, use **Settings > Drive with your hands** (on by default, in the cockpit): close a hand on the wheel to steer and hold the gas, pinch with an open free hand to use an item, and flick your hands up for a trick. Choose automatic drift; manual drift has no gesture.
- Each run of the game needs a fresh launch of the app.
- If Retro Rewind online play stops with error 61070, turn off iCloud Private Relay, or **Limit IP Address Tracking** for your Wi-Fi network. The tutorial explains why.
- Comfort, frame timing and the bare-hand gestures are still being tuned on hardware; [docs/visionos-port.md](docs/visionos-port.md) lists the known gaps.
