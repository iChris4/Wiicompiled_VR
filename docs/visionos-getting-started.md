# Mario Kart Wii on Apple Vision Pro: from your disc to the headset

This guide takes you from a Mario Kart Wii disc image you own to WiiCompiled
running on your Apple Vision Pro, in stereo, with your hands. One script does
the work; this page tells you what to install first, what to expect, and what
to do when something stops.

The app is not on the App Store and cannot be: visionOS only runs code Apple
or *you* signed, so the game has to be translated and built on your own Mac,
from your own disc, and signed with your own Apple ID. Nothing of the game is
downloaded or distributed by this project.

## What you need

| | Why |
| --- | --- |
| **An Apple Silicon Mac** (M1 or later), 15 GB free besides Xcode | The visionOS toolchain is arm64 only; the extracted disc (2.5 GB), the Retro Rewind pack (4 GB) and the build (3 GB) take room |
| **Xcode** (free, Mac App Store), with the **visionOS** platform | Only Xcode has the visionOS SDK and can sign apps for a headset. About 15 GB |
| **An Apple ID signed in to Xcode** | It signs the app. A free account works (see *Free or paid account* below) |
| **Homebrew** with `cmake`, `ninja` and the .NET SDK | The translator is a .NET program, the build uses CMake |
| **Your Mario Kart Wii PAL disc** as `.iso`, `.rvz`, `.wbfs`, `.ciso` or `.gcm` | The game. Only the European `RMCP01` release works; the script checks it |
| **Your Apple Vision Pro**, paired with the Mac | Pairing is done once in Xcode |
| An hour the first time | Most of it is compiling; later runs take a minute or two |

### Free or paid account

A free Apple ID signs apps that **stop opening after 7 days** and allows three
sideloaded apps on the headset. Re-running the script with `--reinstall`
(a minute or two) makes it good for another week. A paid Apple Developer
account ($99/year) signs for a year. Everything else is the same.

## Step by step

### 1. Install Xcode and the visionOS platform

1. Install **Xcode** from the Mac App Store and open it once; accept the
   licence and let it finish installing components.
2. **Xcode > Settings > Components**: install **visionOS** (a few GB).
3. **Xcode > Settings > Accounts**: press **+**, sign in with your Apple ID.

### 2. Install the build tools

Install [Homebrew](https://brew.sh) if you do not have it, then in Terminal:

```bash
brew install cmake ninja
brew install --cask dotnet-sdk
```

### 3. Pair the headset

1. On the Vision Pro: **Settings > General > Remote Devices**.
2. On the Mac, in Xcode: **Window > Devices and Simulators**; the headset
   appears, pair it and enter the code the headset shows.
3. When asked, turn on **Developer Mode** on the headset (**Settings >
   Privacy & Security > Developer Mode**) and restart it.

The dev strap is not required, but the disc copy (2.5 GB) is far faster over one.

### 4. Get the project

```bash
git clone --branch vision-pro https://github.com/iChris4/Wiicompiled_VR.git
cd Wiicompiled_VR
```

### 5. Run the script

Either double-click `visionos/Make-VisionOS-App.command` in Finder (it asks for
the disc image and whether you want Retro Rewind), or in Terminal:

```bash
visionos/Make-VisionOS-App.command --game ~/Downloads/RMCP01.rvz --retro-rewind download
```

Leave out `--retro-rewind download` for the plain game. Without `--game`, the
script asks for the disc at step 2: drag the image file from Finder onto the
Terminal window and press Return. If Xcode has several teams, it asks which
one signs the app; type its number. The app's bundle identifier is made from
that team's id (`org.wiicompiled.vision.` and the id), since the project's own,
`org.wiicompiled.vision`, belongs to its developer's team. `--bundle-id`
chooses another, which the build remembers; changing it later installs a
second, separate app. The script prints seven steps:

1. **Checking this Mac**: Xcode, the visionOS SDK, the tools, your team, the
   headset. Anything missing stops here with the fix to apply.
2. **Extracting the disc** (nodtool, downloaded once): checks it is a clean
   PAL image and leaves the extracted game in `Assets/DATA`.
3. **Retro Rewind**: downloads the pack from Retro Rewind's own server (about
   1.9 GB) and the Retro-WFC payload for online play.
4. **Translating the game**: the translator is built, then turns the game's
   PowerPC code into C++ (a few minutes).
5. **Building and signing the app**: the first time this compiles the graphics
   library (Dawn), the runtime and the translated code, **30 to 60 minutes**
   depending on the Mac. Later builds reuse all of it.
6. **Copying the disc into the app** on the headset (a few minutes over the
   cable, much longer over Wi-Fi).
7. **Launching** the app.

Keep the headset unlocked and awake during steps 5 to 7; a dozing headset
drops the connection (the script retries).

### 6. On the headset

- The first time, visionOS refuses the app as coming from an untrusted
  developer: **Settings > General > VPN & Device Management**, tap your Apple
  ID and **Trust**. Then open **WiiCompiled Vision** from the Home View.
- The launcher shows the disc as found. Press **Play**: the menu appears on a
  virtual screen in your room; look at a button and pinch, move your hand to
  adjust the pointer while pinching, let go to press.
- For **Retro Rewind**: pick it in the Play tab, press **Download Retro
  Rewind** (about 2 GB from Retro Rewind's server to the headset), then Play.
- Online, other players see your Mii: its face and its name. Make yours in the
  **Miis** tab before you press Play (**Download** there once to see the Miis'
  faces), then pick it in the game when you create your licence, or later in
  **License Settings > Change Mii**. Both games share these Miis.
- Races start in first person, seated in the cockpit; the launcher's
  **Settings > Camera** switches to the chase camera.
- Race with a Bluetooth game controller, or with bare hands (**Settings >
  Drive with your hands**). Press the Digital Crown to leave the game.

## Later

| You want to | Do |
| --- | --- |
| Open the app after the 7-day expiry (free account) | `visionos/Make-VisionOS-App.command --reinstall` |
| Update to a newer version of this project | `git pull`, then run the script again; it rebuilds what changed |
| Add or remove Retro Rewind | Run the script with or without `--retro-rewind download` (the game is retranslated) |
| Follow a Retro Rewind update that changed `Code.pul` | The app tells you it does not match the pack. Run `--retro-rewind download --retranslate` |
| Change graphics or controls | The launcher's **Settings** tab, or `Config.toml` in the Files app (On My Apple Vision Pro > WiiCompiled Vision > WiiCompiled) |
| Make, change or share a Mii | The launcher's **Miis** tab (it imports and exports `.mii` files); then in the game, **License Settings > Change Mii** |
| See your friend code, VR and VR history | The launcher's **Profiles** tab, once you have played Retro Rewind |
| Move your profile to or from another device | **Profiles > Export…** and **Import…**: one zip with the saves, the Miis and the console identity. Play online with it on one device at a time |
| Use another headset or team | `--device UDID`, `--team TEAMID` (another team signs a separate app, with its own data) |

## When it stops

The script names what is wrong and what to do. The usual ones:

- **"Xcode is not installed" / "visionOS platform is required"**: step 1.
  If Xcode is installed but not selected, `sudo xcode-select -s /Applications/Xcode.app`.
- **"No Apple developer team found"**: sign in to Xcode (step 1.3). If you
  are signed in, the script also takes your Team ID pasted in, or `--team`.
  With several teams it asks which one to use.
- **"No paired Apple Vision Pro"**: step 3. `xcrun devicectl list devices`
  must list it as `available (paired)`.
- **"disc is not the supported clean PAL RMCP01 image"**: only the European
  release is supported, unmodified. NTSC and modified dumps are refused.
- **Signing errors during the build**: open Xcode > Settings > Accounts and
  check the Apple ID is still signed in (its session can expire). A free
  account that already has three sideloaded apps on the headset must remove
  one.
- **Xcode says the app identifier is not available** (it cannot be registered
  to your team): another team owns that bundle identifier, as with a
  `build-visionos/` made before the identifier came from your team. Run again
  with `--bundle-id` and one of your own, such as `com.yourname.wiicompiled`.
- **"Failed to install the app on the device"**: unlock the headset and keep
  it awake; run again with `--reinstall`.
- **The launcher says "No extracted disc yet"**: the disc copy did not
  finish. Run the script again; it checks the headset and copies again if
  needed. If the game itself stops, its transcript is in the Logs folder next
  to `Config.toml` in the Files app.
- **Retro Rewind online stops with error code 61070** ("You have been
  disconnected from Retro WFC"), but works right after another device on your
  network went online: iCloud Private Relay is on. Retro WFC only lets the game
  in from the internet address that logged in, and Private Relay sends the
  headset's login through Apple's servers instead. Turn it off on the headset,
  for your home network only (**Settings > Wi-Fi**, ⓘ next to the network,
  **Limit IP Address Tracking** off) or everywhere (**Settings > *your name* >
  iCloud > Private Relay**).
- **Everything else**: the full output is in Terminal; open an issue with it.

## What it does not do

- Run on the visionOS **simulator** with your hands (build for it with
  `visionos/Build-VisionOS.sh --simulator`; a controller works there).
- Support **other regions** than PAL, or other games.
- Distribute the game: there is no build to download. Everyone makes their
  own, from their own disc.

For how the port works, see [`visionos-port.md`](visionos-port.md); for the
scripts underneath, `visionos/README.md`.
