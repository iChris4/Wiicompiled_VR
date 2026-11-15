// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// Where the game keeps its files, as the runtime resolves them on visionOS
/// (RuntimePlatform::ApplicationDataDirectory in runtime/src/platform/host_platform.cpp):
/// the WiiCompiled folder in the app's Documents, which the Files app and Finder file sharing
/// show. The launcher prepares it before the game is loaded, so the runtime finds a
/// Config.toml that already names the disc.
enum GameStorage {
    /// The runtime's kApplicationDirectoryName off Windows and Android.
    static let appDirectoryName = "WiiCompiled"

    static var documents: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
    }
    static var resources: URL {
        Bundle.main.resourceURL ?? Bundle.main.bundleURL
    }
    static var gameDirectory: URL { documents.appendingPathComponent(appDirectoryName, isDirectory: true) }
    static var configFile: URL { gameDirectory.appendingPathComponent("Config.toml") }
    static var discDirectory: URL { gameDirectory.appendingPathComponent("DATA", isDirectory: true) }
    static var logsDirectory: URL { gameDirectory.appendingPathComponent("Logs", isDirectory: true) }
    /// Where the Profiles tab's Import keeps the profile it replaced, in view of the Files app.
    static var backupsDirectory: URL { gameDirectory.appendingPathComponent("Backups", isDirectory: true) }

    /// The game's NAND as the runtime resolves it (RuntimeNandPath in runtime/include/nand_path.h):
    /// Config.toml's `[paths] nand_root`, relative to the file's folder, or else the managed NAND
    /// folder beside it. The Miis tab finds the game's Mii database there.
    static var nandDirectory: URL {
        guard let configured = loadConfig()?.string("paths", "nand_root"), !configured.isEmpty else {
            return gameDirectory.appendingPathComponent("NAND", isDirectory: true)
        }
        let path = configured.hasPrefix("/") ? URL(fileURLWithPath: configured, isDirectory: true)
            : gameDirectory.appendingPathComponent(configured, isDirectory: true)
        return path.standardizedFileURL
    }

    /// Retro Rewind's save: the pack's Riivolution XML sends the game's saves to
    /// riivolution/save/RetroWFC/<game ID><region> in the virtual SD card, the folder around the
    /// pack (RiivoFindXmls in runtime/src/hle/storage/riivolution.cpp), and this build runs PAL only.
    static var retroRewindSave: URL {
        RetroRewindPack.directory.deletingLastPathComponent().appendingPathComponent("riivolution/save/RetroWFC/RMCP/rksys.dat")
    }

    /// Pulsar's record of the VR and BR Retro Rewind plays with, per profile.
    static var retroRewindRatings: URL { nandDirectory.appendingPathComponent("shared2/Pulsar/RetroRewind6/RRRating.pul") }

    enum DiscStatus { case missing, incomplete, ready }

    /// Whether DATA holds what the runtime's DVD layer accepts: the whole extracted partition,
    /// not just its files/ folder (as android/.../GameStorage.kt checks it).
    static var discStatus: DiscStatus {
        let manager = FileManager.default
        var isDirectory: ObjCBool = false
        guard manager.fileExists(atPath: discDirectory.path, isDirectory: &isDirectory), isDirectory.boolValue else {
            return .missing
        }
        let files = discDirectory.appendingPathComponent("files").path
        let fst = discDirectory.appendingPathComponent("sys/fst.bin").path
        guard manager.fileExists(atPath: files, isDirectory: &isDirectory), isDirectory.boolValue,
              manager.fileExists(atPath: fst) else {
            return .incomplete
        }
        return .ready
    }

    /// Tells the runtime where things are. Read by its static initialisers, so this must run
    /// before the game framework is loaded (visionos_host.mm sets the same variables again).
    static func exportDirectories() {
        setenv("MKW_APPLE_DATA_DIR", documents.path, 1)
        setenv("MKW_APPLE_RESOURCES_DIR", resources.path, 1)
        // `--fpslog` (a launch argument, from devicectl or an Xcode scheme) has Aurora log the game's
        // frame rate and where each frame's time went every five seconds, as the Quest's
        // debug.wiicompiled.fpslog property does.
        if CommandLine.arguments.contains("--fpslog") {
            setenv("MKW_FPSLOG", "1", 1)
        }
    }

    /// Creates the game folder and, when there is none, a first Config.toml with VR on and the
    /// disc path filled in, as the Quest launcher does. An existing file keeps every setting it
    /// has and only gains the paths it lacks. Returns an error message, or nil.
    @discardableResult
    static func prepare() -> String? {
        let manager = FileManager.default
        do {
            try manager.createDirectory(at: discDirectory, withIntermediateDirectories: true)
        } catch {
            return "Could not create \(gameDirectory.path): \(error.localizedDescription)"
        }
        guard manager.fileExists(atPath: configFile.path) else {
            let lines = [
                "# WiiCompiled Apple Vision Pro configuration. Edit it in the launcher's Settings, in the",
                "# in-game panel, or here through the Files app (On My Apple Vision Pro > WiiCompiled Vision).",
                "[paths]",
                "dvd_root = \"DATA\"",
                "retro_rewind_root = \"RetroRewind6\"",
                "",
                "[video]",
                "widescreen = true",
                "resolution_multiplier = 1.0",
                "",
                "[vr]",
                "enabled = true",
                "render_scale = 1.0",
                "# New players start in the cockpit: first person, following turns and climbs.",
                "first_person = true",
                "first_person_rotation = \"yaw_pitch\"",
                "first_person_seat = \"cockpit\"",
            ]
            do {
                try (lines.joined(separator: "\n") + "\n").write(to: configFile, atomically: true, encoding: .utf8)
            } catch {
                return "Could not write \(configFile.path): \(error.localizedDescription)"
            }
            return nil
        }
        // A config the runtime wrote itself (its desktop template) names no disc.
        guard var config = loadConfig() else { return "Could not read \(configFile.path)" }
        var changed = false
        if (config.string("paths", "dvd_root") ?? "").isEmpty {
            config.setString("paths", "dvd_root", "DATA")
            changed = true
        }
        if (config.string("paths", "retro_rewind_root") ?? "").isEmpty {
            config.setString("paths", "retro_rewind_root", "RetroRewind6")
            changed = true
        }
        return changed ? save(config) : nil
    }

    static func loadConfig() -> TomlConfig? {
        guard let text = try? String(contentsOf: configFile, encoding: .utf8) else { return nil }
        return TomlConfig(text: text)
    }

    /// Returns an error message, or nil.
    static func save(_ config: TomlConfig) -> String? {
        do {
            try config.text.write(to: configFile, atomically: true, encoding: .utf8)
            return nil
        } catch {
            return "Could not save \(configFile.lastPathComponent): \(error.localizedDescription)"
        }
    }
}
