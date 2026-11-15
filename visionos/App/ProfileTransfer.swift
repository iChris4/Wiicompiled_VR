// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// A player's profile moved between devices as one zip archive: what the Profiles tab's Export
/// writes and its Import reads. A profile is more than the save, which Retro WFC ties to a console:
///
/// - Retro Rewind's save folder (rksys.dat: the licences, each with its own login IDs, profile ID
///   and friend list) and the unmodded game's;
/// - the Mii database the licences point at by ID;
/// - the console's identity: setting.txt, whose serial Retro WFC checks at login (its error 22005
///   is a serial the profile does not know) and from which the runtime derives the console's MAC
///   address and the Miis' system ID (RuntimeConsoleIdentity::FromSerial); DWC_AUTHDATA, the
///   console's Nintendo WFC user ID; and keys.bin, the device certificate, when there is one;
/// - Pulsar's Retro Rewind folder: the VR and BR it plays with (RRRating.pul, by profile ID), its
///   settings and ghosts.
///
/// The archive lays them out as this app's folders do (and the Quest's, which match), so it can
/// also be unpacked by hand. Import finds them by their NAND or SD card paths wherever they sit in
/// an archive, so a zipped Dolphin `Wii` folder or Riivolution folder reads too.
enum ProfileTransfer {
    enum Part: CaseIterable {
        case retroRewindSave
        case gameSave
        case miis
        case consoleIdentity
        case wifiLogin
        case deviceKeys
        case retroRewindData

        var title: String {
            switch self {
            case .retroRewindSave: return "Retro Rewind save"
            case .gameSave: return "Mario Kart Wii save"
            case .miis: return "Miis"
            case .consoleIdentity: return "Console identity (setting.txt)"
            case .wifiLogin: return "Wi-Fi login (DWC_AUTHDATA)"
            case .deviceKeys: return "Device keys (keys.bin)"
            case .retroRewindData: return "Retro Rewind ratings, settings and ghosts"
            }
        }

        /// How a sentence names it.
        var name: String {
            switch self {
            case .retroRewindSave: return "the Retro Rewind save"
            case .gameSave: return "the Mario Kart Wii save"
            case .miis: return "the Miis"
            case .consoleIdentity: return "the console identity"
            case .wifiLogin: return "the Wi-Fi login"
            case .deviceKeys: return "the device keys"
            case .retroRewindData: return "Retro Rewind's ratings, settings and ghosts"
            }
        }

        /// Where it sits in an exported archive; a folder's path ends with a slash.
        var archivePath: String {
            switch self {
            case .retroRewindSave: return "riivolution/save/RetroWFC/RMCP/"
            case .gameSave: return "NAND/title/00010004/524d4350/data/"
            case .miis: return "NAND/shared2/menu/FaceLib/RFL_DB.dat"
            case .consoleIdentity: return "NAND/title/00000001/00000002/data/setting.txt"
            case .wifiLogin: return "NAND/shared2/DWC_AUTHDATA"
            case .deviceKeys: return "NAND/keys.bin"
            case .retroRewindData: return "NAND/shared2/Pulsar/RetroRewind6/"
            }
        }

        var isFolder: Bool { archivePath.hasSuffix("/") }

        /// Its path below the NAND or the virtual SD card, which finds it in any archive.
        fileprivate var marker: String {
            archivePath.hasPrefix("NAND/") ? String(archivePath.dropFirst(5)) : archivePath
        }
    }

    /// Where the parts live on this headset.
    struct Locations {
        let nand: URL
        /// Retro Rewind's save folder, the Riivolution save redirect's target.
        let retroRewindSave: URL

        static var current: Locations {
            Locations(nand: GameStorage.nandDirectory, retroRewindSave: GameStorage.retroRewindSave.deletingLastPathComponent())
        }

        func url(_ part: Part) -> URL {
            part == .retroRewindSave ? retroRewindSave : nand.appendingPathComponent(part.marker)
        }
    }

    /// What an archive holds, read and checked.
    struct Contents {
        /// A file part's bytes under "", a folder part's files under their paths inside it.
        fileprivate(set) var files: [Part: [String: Data]] = [:]
        /// The Retro Rewind save's licences, or else the game's, to say whose profile it is.
        fileprivate(set) var licenses: [RksysProfiles.License] = []
        /// The console the profile comes from, as Retro WFC knows it (CODE and SERNO).
        fileprivate(set) var console: String?
        fileprivate(set) var miiCount = 0

        var parts: [Part] { Part.allCases.filter { files[$0] != nil } }
    }

    struct Report {
        let parts: [Part]
        let backup: URL?
        let miis: (added: Int, replaced: Int, skipped: Int)?
    }

    struct Failure: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    /// A Mario Kart Wii save's size, and where its CRC-32 of everything before it sits.
    static let saveSize = 0x2BC000
    private static let saveCrcOffset = 0x27FFC
    /// The most an archive may unpack to: a profile is a few megabytes, ghosts and all.
    private static let sizeLimit: UInt64 = 256 << 20

    // MARK: Export

    /// Every part of the profile this headset has, as a zip archive, or nil when there is none.
    static func archive(_ locations: Locations = .current, date: Date = Date()) throws -> Data? {
        var writer = ZipWriter(date: date)
        var found = false
        for part in Part.allCases {
            for (path, data) in try collect(part, locations).sorted(by: { $0.key < $1.key }) {
                try writer.add(part.archivePath + path, data)
                found = true
            }
        }
        guard found else { return nil }
        try writer.add("README.txt", Data(readme(date).utf8))
        return writer.finish()
    }

    /// The export's file name: the licence's, or Retro Rewind's, and the day.
    static func fileName(for license: RksysProfiles.License?, date: Date = Date()) -> String {
        let format = DateFormatter()
        format.locale = Locale(identifier: "en_US_POSIX")
        format.dateFormat = "yyyy-MM-dd"
        let owner = license.map(ProfileStore.displayName).map(safe) ?? ""
        return "\(owner.isEmpty ? "Retro Rewind" : owner) profile \(format.string(from: date)).zip"
    }

    /// A file part's bytes under "", a folder part's files under their paths inside it.
    private static func collect(_ part: Part, _ locations: Locations) throws -> [String: Data] {
        let url = locations.url(part)
        let manager = FileManager.default
        guard part.isFolder else {
            guard manager.fileExists(atPath: url.path) else { return [:] }
            return ["": try read(url)]
        }
        guard let enumerator = manager.enumerator(at: url, includingPropertiesForKeys: [.isRegularFileKey]) else { return [:] }
        var files: [String: Data] = [:]
        let base = url.standardizedFileURL.pathComponents
        for case let file as URL in enumerator {
            guard (try? file.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true,
                  file.lastPathComponent != ".DS_Store" else { continue }
            let relative = file.standardizedFileURL.pathComponents.dropFirst(base.count).joined(separator: "/")
            files[relative] = try read(url.appendingPathComponent(relative))
        }
        return files
    }

    private static func readme(_ date: Date) -> String {
        let format = ISO8601DateFormatter()
        return """
        A Mario Kart Wii and Retro Rewind profile, exported by WiiCompiled Vision on \(format.string(from: date)).
        Import it in the Profiles tab of WiiCompiled Vision.

        riivolution/  Retro Rewind's save, which its Riivolution XML keeps beside the pack.
        NAND/         The game's NAND: Mario Kart Wii's save, the Miis, the console's identity
                      (setting.txt, DWC_AUTHDATA, keys.bin) and Retro Rewind's ratings, settings
                      and ghosts.

        The Meta Quest keeps the same two folders in Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR.
        Retro WFC ties the profile to this console identity: play online with it on one device at a time.

        """
    }

    // MARK: Import

    /// Reads a profile archive, checking each part as the game and the runtime would: the saves'
    /// size and CRC, the Mii database's CRC, the console identity's fields, the login's size.
    static func read(_ archiveURL: URL) throws -> Contents {
        let archive: ZipArchive
        do {
            archive = try ZipArchive(url: archiveURL)
        } catch {
            throw Failure(message: "\(archiveURL.lastPathComponent) is not a zip archive.")
        }
        var contents = Contents()
        var total: UInt64 = 0
        let scratch = FileManager.default.temporaryDirectory.appendingPathComponent("ProfileImport-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: scratch) }
        for entry in archive.entries where !entry.isDirectory {
            guard let (part, path) = locate(entry.name), contents.files[part]?[path] == nil else { continue }
            total += entry.uncompressedSize
            guard total <= sizeLimit else { throw Failure(message: "\(archiveURL.lastPathComponent) holds more than a profile.") }
            let file = scratch.appendingPathComponent(UUID().uuidString)
            try archive.extract(entry, to: file)
            contents.files[part, default: [:]][path] = try read(file)
        }

        for part in [Part.retroRewindSave, .gameSave] {
            guard let files = contents.files[part] else { continue }
            guard let save = files["rksys.dat"] else {
                contents.files[part] = nil
                continue
            }
            guard isValidSave(save) else { throw Failure(message: "The \(part.title) in this archive is damaged: its size or checksum is wrong.") }
        }
        if let save = contents.files[.retroRewindSave]?["rksys.dat"] ?? contents.files[.gameSave]?["rksys.dat"] {
            contents.licenses = (RksysProfiles.parse([UInt8](save.prefix(RksysProfiles.licensesEnd))) ?? []).compactMap { $0 }
        }
        if let settings = contents.files[.consoleIdentity]?[""] {
            guard let fields = MiiIds.settings(settings), MiiIds.hasIdentity(fields) else {
                throw Failure(message: "The console identity in this archive (setting.txt) is damaged; the game would refuse it.")
            }
            contents.console = (fields["CODE"] ?? "") + (fields["SERNO"] ?? "")
        }
        if let login = contents.files[.wifiLogin]?[""], login.count != 32 {
            throw Failure(message: "The Wi-Fi login in this archive (DWC_AUTHDATA) is not the 32 bytes the game writes.")
        }
        if let miis = contents.files[.miis]?[""] {
            let db = [UInt8](miis)
            guard MiiDatabase.isValid(db) else { throw Failure(message: "The Mii database in this archive is damaged: its checksum is wrong.") }
            contents.miiCount = MiiDatabase.slots(db).compactMap { $0 }.count
        }
        if contents.files[.deviceKeys]?[""]?.isEmpty == true { contents.files[.deviceKeys] = nil }
        guard !contents.parts.isEmpty else {
            throw Failure(message: "\(archiveURL.lastPathComponent) holds no profile: no save, Miis or console identity was found in it.")
        }
        return contents
    }

    /// A Mario Kart Wii save the game accepts: its size, magic and the CRC-32 it keeps of itself.
    static func isValidSave(_ save: Data) -> Bool {
        guard save.count == saveSize, save.prefix(4) == Data("RKSD".utf8) else { return false }
        let stored = save[saveCrcOffset..<(saveCrcOffset + 4)].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
        return ZipWriter.crc32(save.prefix(saveCrcOffset)) == stored
    }

    /// Which part an archive entry is, and its path inside a folder part ("" for a file part).
    private static func locate(_ name: String) -> (Part, String)? {
        let components = name.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        // What macOS and Windows leave in archives.
        if components.contains("__MACOSX") || components.last == ".DS_Store" || components.last?.hasPrefix("._") == true { return nil }
        let lower = name.lowercased()
        for part in Part.allCases {
            let marker = part.marker.lowercased()
            if part == .deviceKeys {
                // keys.bin sits at the NAND's root: "keys.bin", "NAND/keys.bin" or "Wii/keys.bin".
                if components.count <= 2, lower.hasSuffix("keys.bin"), components.last?.lowercased() == "keys.bin" { return (part, "") }
                continue
            }
            guard let range = lower.range(of: marker), range.lowerBound == lower.startIndex || lower[lower.index(before: range.lowerBound)] == "/" else { continue }
            if part.isFolder {
                // The same characters in the original case; lowercasing these ASCII markers keeps their length.
                let start = name.index(name.startIndex, offsetBy: lower.distance(from: lower.startIndex, to: range.upperBound))
                let path = String(name[start...])
                let pieces = path.split(separator: "/", omittingEmptySubsequences: false)
                guard !path.isEmpty, !pieces.contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }) else { return nil }
                return (part, path)
            }
            if range.upperBound == lower.endIndex { return (part, "") }
        }
        return nil
    }

    /// Puts `contents` in place of this headset's profile, after exporting the current one into
    /// `backups`. Saves and Retro Rewind's files are written over the ones here, which keeps files the
    /// archive does not carry; the Miis join the headset's, a Mii with the same ID taking its place.
    static func apply(_ contents: Contents, _ locations: Locations = .current, backups: URL, date: Date = Date()) throws -> Report {
        let manager = FileManager.default
        var backup: URL?
        if let current = try archive(locations, date: date) {
            let format = DateFormatter()
            format.locale = Locale(identifier: "en_US_POSIX")
            format.dateFormat = "yyyy-MM-dd HH.mm.ss"
            let url = backups.appendingPathComponent("Profile before import \(format.string(from: date)).zip")
            do {
                try manager.createDirectory(at: backups, withIntermediateDirectories: true)
                try current.write(to: url, options: .atomic)
            } catch {
                throw Failure(message: "The current profile could not be backed up, so nothing was imported: \(error.localizedDescription)")
            }
            backup = url
        }
        var miis: (added: Int, replaced: Int, skipped: Int)?
        for part in contents.parts {
            let files = contents.files[part] ?? [:]
            let target = locations.url(part)
            do {
                // The Miis join a database the game can read; a damaged one is replaced (the backup keeps it).
                if part == .miis, let db = files[""], let here = try? Data(contentsOf: target), MiiDatabase.isValid([UInt8](here)) {
                    miis = try MiiDatabase.merge(target, from: [UInt8](db))
                    continue
                }
                for (path, data) in files {
                    let file = path.isEmpty ? target : target.appendingPathComponent(path)
                    try manager.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try data.write(to: file, options: .atomic)
                }
                if part == .miis { miis = (contents.miiCount, 0, 0) }
            } catch {
                let restore = backup.map { " The profile from before is in \($0.lastPathComponent)." } ?? ""
                throw Failure(message: "\(sentence(part.name)) could not be imported (\(error.localizedDescription)).\(restore)")
            }
        }
        return Report(parts: contents.parts, backup: backup, miis: miis)
    }

    /// Names in a sentence: "a, b and c".
    static func list(_ parts: [Part]) -> String {
        let names = parts.map(\.name)
        guard names.count > 1 else { return names.first ?? "" }
        return names.dropLast().joined(separator: ", ") + " and " + names[names.count - 1]
    }

    /// A sentence starting with `text`.
    static func sentence(_ text: String) -> String {
        text.prefix(1).uppercased() + text.dropFirst()
    }

    private static func read(_ url: URL) throws -> Data {
        do {
            return try Data(contentsOf: url)
        } catch {
            throw Failure(message: "Cannot read \(url.lastPathComponent): \(error.localizedDescription)")
        }
    }

    /// A licence name fit for a file name: its printable ASCII, the characters files cannot hold replaced.
    private static func safe(_ name: String) -> String {
        let ascii = String(String.UnicodeScalarView(name.decomposedStringWithCanonicalMapping.unicodeScalars.filter { (0x20...0x7E).contains($0.value) }))
        return ascii.split(whereSeparator: { "/\\:*?\"<>|".contains($0) }).joined(separator: "_").trimmingCharacters(in: .whitespaces)
    }
}
