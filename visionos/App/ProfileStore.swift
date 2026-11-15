// SPDX-License-Identifier: GPL-3.0-or-later

import CoreGraphics
import Foundation
import ImageIO

/// What the Profiles tab shows, as the Quest launcher's ProfileStore.kt gathers it: the licences of
/// Retro Rewind's save and their Miis, which one is primary, and what Retro WFC knows of them (the
/// Miis they last played with, who is in a room now), plus WheelWizard's badges. Files are read
/// and the network asked off the main actor. What Retro WFC saw of a Mii is also kept on disk, so
/// it shows at once, even offline.
@MainActor
final class ProfileStore: ObservableObject {
    /// The four licence slots of the save, an empty one nil, and the licences' Miis the headset's
    /// Mii database holds, by ID.
    struct Snapshot: Equatable {
        let licenses: [RksysProfiles.License?]
        let miis: [UInt32: Mii]
        var any: Bool { licenses.contains { $0 != nil } }
    }

    /// What Retro WFC last saw of a profile's Mii: the Mii, to draw it, and its own 64-pixel picture
    /// of it. Immutable, so it is read off the main actor as freely as on it.
    final class Remote: @unchecked Sendable {
        let mii: Mii?
        let picture: CGImage?
        let data: Data?
        let image: Data?

        init?(data: Data?, image: Data?) {
            let mii = data.flatMap { try? MiiData.parse($0) }
            let picture = image.flatMap(Remote.decode)
            guard mii != nil || picture != nil else { return nil }
            self.mii = mii
            self.picture = picture
            self.data = data
            self.image = image
        }

        private static func decode(_ bytes: Data) -> CGImage? {
            guard let source = CGImageSourceCreateWithData(bytes as CFData, nil) else { return nil }
            return CGImageSourceCreateImageAtIndex(source, 0, nil)
        }
    }

    /// What stands for a licence's Mii.
    enum Picture {
        /// Drawn from the Mii parts, with Retro WFC's own picture should the drawing fail.
        case mii(Mii, fallback: CGImage?)
        /// Retro WFC's picture, until the Mii parts are downloaded.
        case image(CGImage)
        /// A silhouette.
        case none
    }

    @Published private(set) var snapshot: Snapshot?
    @Published private(set) var loaded = false
    /// The licence shown.
    @Published var slot = -1
    @Published private(set) var primarySlot: Int
    @Published private(set) var badges: [String: [RetroWfc.Badge]] = [:]
    /// The friend codes of every player in a Retro WFC room, while the tab watches the rooms.
    @Published private(set) var onlineFriendCodes: Set<String> = []
    @Published private(set) var remotes: [String: Remote] = [:]
    @Published private(set) var partsInstalled = MiiRenderResource.installed
    /// Changes each time the save is read again, which reloads the VR history: races since count.
    @Published private(set) var generation = 0

    /// An archive read for Import, waiting for the player to confirm it.
    struct PendingImport: Identifiable {
        let id = UUID()
        let fileName: String
        let contents: ProfileTransfer.Contents
        /// This headset's console as Retro WFC knows it (CODE and SERNO), when it has one.
        let currentConsole: String?
    }

    struct Message: Identifiable {
        let id = UUID()
        let title: String
        let text: String
    }

    @Published var pendingImport: PendingImport?
    @Published var message: Message?
    /// What Import or Export is doing, while it does.
    @Published private(set) var transferring: String?
    /// The archive Export wrote, waiting for the player to choose where it goes.
    @Published private(set) var exportFiles: [URL] = []
    @Published var exporting = false
    /// A passing word on what Import or Export did.
    @Published private(set) var notice = ""
    private var noticeTask: Task<Void, Never>?

    private var remotesAsked: Set<String> = []
    private var badgesLoaded = false
    private var roomsTask: Task<Void, Never>?

    /// GameLicenseService's FOCUSED_USER, remembered as the Quest remembers it.
    private static let primaryKey = "primaryProfile"
    nonisolated private static let imageDirectory = "profile-miis"
    /// Retro WFC's rooms are asked for again this often while the tab is on screen, as the Quest's LiveRooms does.
    private static let roomsInterval: Duration = .seconds(40)
    /// SettingValues.NoName: the name the game's guest Miis carry.
    nonisolated private static let guestName = "no name"

    init() {
        primarySlot = min(max(UserDefaults.standard.integer(forKey: ProfileStore.primaryKey), 0), RksysProfiles.slots - 1)
    }

    var current: RksysProfiles.License? {
        guard let licenses = snapshot?.licenses, licenses.indices.contains(slot) else { return nil }
        return licenses[slot]
    }

    /// Reads the save again, which the game may have changed since, and fetches the badges once.
    func refresh() {
        partsInstalled = MiiRenderResource.installed
        Task {
            let read = await Task.detached(priority: .userInitiated) { ProfileStore.read() }.value
            snapshot = read
            loaded = true
            generation += 1
            let licenses = read?.licenses ?? []
            if !licenses.indices.contains(slot) || licenses[slot] == nil {
                // UserProfilePage opens on the focused user, here the primary licence.
                slot = licenses.indices.contains(primarySlot) && licenses[primarySlot] != nil
                    ? primarySlot : (licenses.firstIndex { $0 != nil } ?? -1)
            }
            for case let license? in licenses { fetchRemote(license.friendCode) }
        }
        if !badgesLoaded {
            Task {
                if let fresh = try? await RetroWfc.badges() {
                    badges = fresh
                    badgesLoaded = true
                }
            }
        }
    }

    nonisolated private static func read() -> Snapshot? {
        guard let save = RksysProfiles.readLicenses(GameStorage.retroRewindSave) else { return nil }
        let ratings = (try? Data(contentsOf: GameStorage.retroRewindRatings)).map { RksysProfiles.parseRatings([UInt8]($0)) } ?? [:]
        guard let licenses = RksysProfiles.parse(save, ratings: ratings) else { return nil }
        // GameLicenseService.ParseMiiData: a licence's Mii is looked up in the Mii database by ID.
        let ids = Set(licenses.compactMap { $0?.miiId })
        let file = MiiDatabase.file(nand: GameStorage.nandDirectory)
        let miis = FileManager.default.fileExists(atPath: file.path) ? ((try? MiiDatabase.byId(file)) ?? [:]) : [:]
        return Snapshot(licenses: licenses, miis: miis.filter { ids.contains($0.key) })
    }

    /// UserProfilePage.SetUserAsPrimary.
    func makePrimary(_ slot: Int) {
        primarySlot = slot
        UserDefaults.standard.set(slot, forKey: ProfileStore.primaryKey)
    }

    /// A licence's name as the PC shows it: a guest Mii's "no name" reads No name.
    nonisolated static func displayName(_ license: RksysProfiles.License) -> String {
        license.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || license.name == guestName ? "No name" : license.name
    }

    // MARK: Import and export

    /// Writes this headset's profile into a zip named after `license`, then asks where it goes.
    func export(_ license: RksysProfiles.License?) {
        guard transferring == nil else { return }
        transferring = "Exporting…"
        let name = ProfileTransfer.fileName(for: license)
        Task {
            let result = await Task.detached(priority: .userInitiated) { () -> Result<URL?, Error> in
                Result {
                    guard let archive = try ProfileTransfer.archive() else { return nil }
                    let folder = FileManager.default.temporaryDirectory.appendingPathComponent("ProfileExport-\(UUID().uuidString)", isDirectory: true)
                    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                    let file = folder.appendingPathComponent(name)
                    try archive.write(to: file)
                    return file
                }
            }.value
            transferring = nil
            switch result {
            case .success(let file?):
                exportFiles = [file]
                exporting = true
            case .success(nil):
                message = Message(title: "Nothing to export yet", text: "This headset has no save, Miis or console identity yet. Play once, then export.")
            case .failure(let error):
                message = Message(title: "The profile could not be exported", text: error.localizedDescription)
            }
        }
    }

    /// Where Export's archive went, or why it did not.
    func exported(_ result: Result<[URL], Error>) {
        if let folder = exportFiles.first?.deletingLastPathComponent() { try? FileManager.default.removeItem(at: folder) }
        exportFiles = []
        switch result {
        case .success(let moved):
            if let file = moved.first { say("Saved \(file.lastPathComponent)") }
        case .failure(let error):
            if (error as? CocoaError)?.code == .userCancelled { return }
            message = Message(title: "The profile could not be exported", text: error.localizedDescription)
        }
    }

    /// Reads the archive the player picked and asks them to confirm what it replaces.
    func inspect(_ url: URL) {
        guard transferring == nil else { return }
        transferring = "Reading \(url.lastPathComponent)…"
        Task {
            let result = await Task.detached(priority: .userInitiated) { () -> Result<PendingImport, Error> in
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                return Result {
                    let contents = try ProfileTransfer.read(url)
                    let here = (try? Data(contentsOf: ProfileTransfer.Locations.current.url(.consoleIdentity))).flatMap(MiiIds.settings)
                    let console = here.flatMap { MiiIds.hasIdentity($0) ? ($0["CODE"] ?? "") + ($0["SERNO"] ?? "") : nil }
                    return PendingImport(fileName: url.lastPathComponent, contents: contents, currentConsole: console)
                }
            }.value
            transferring = nil
            switch result {
            case .success(let pending): pendingImport = pending
            case .failure(let error): message = Message(title: "This profile cannot be imported", text: error.localizedDescription)
            }
        }
    }

    /// Imports the confirmed archive, the current profile being backed up first.
    func confirmImport(_ pending: PendingImport) {
        pendingImport = nil
        guard transferring == nil else { return }
        transferring = "Importing…"
        let backups = GameStorage.backupsDirectory
        Task {
            let result = await Task.detached(priority: .userInitiated) {
                Result { try ProfileTransfer.apply(pending.contents, backups: backups) }
            }.value
            transferring = nil
            switch result {
            case .success(let report):
                // Show the imported licences, from the one the tab opens on.
                slot = -1
                refresh()
                var text = "Imported \(ProfileTransfer.list(report.parts))."
                if let miis = report.miis, miis.skipped > 0 {
                    text += " \(miis.skipped) Mii(s) did not fit in the 100 slots."
                }
                if let backup = report.backup {
                    text += " What this headset had before is in Backups, as \(backup.lastPathComponent); import that file to go back."
                }
                message = Message(title: "Profile imported", text: text)
            case .failure(let error):
                refresh()
                message = Message(title: "The profile could not be imported", text: error.localizedDescription)
            }
        }
    }

    private func say(_ text: String) {
        notice = text
        noticeTask?.cancel()
        noticeTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(4))
            if !Task.isCancelled { self?.notice = "" }
        }
    }

    // MARK: Online

    /// While the tab is on screen, who is online follows Retro WFC's rooms, as the PC's page does.
    func watchRooms() {
        guard roomsTask == nil else { return }
        roomsTask = Task { [weak self] in
            while !Task.isCancelled {
                if let rooms = try? await RetroWfc.roomStatus() {
                    self?.onlineFriendCodes = Set(rooms.flatMap { room in room.players.map(\.friendCode) })
                }
                try? await Task.sleep(for: ProfileStore.roomsInterval)
            }
        }
    }

    func stopWatchingRooms() {
        roomsTask?.cancel()
        roomsTask = nil
    }

    func isOnline(_ license: RksysProfiles.License) -> Bool {
        !license.friendCode.isEmpty && onlineFriendCodes.contains(license.friendCode)
    }

    // MARK: Miis

    /// `license`'s Mii, as the PC's profile page shows it. Like the PC, the Mii is the one of the
    /// headset's Mii database (the game's own, which the Miis tab edits) with the licence's ID. A
    /// licence whose Mii is not there shows the Mii Retro WFC last saw it play with, while that is
    /// still the licence's Mii. Both are drawn from the Mii parts once the Miis tab downloaded them;
    /// until then Retro WFC's 64-pixel picture stands in.
    func picture(for license: RksysProfiles.License) -> Picture {
        if let local = snapshot?.miis[license.miiId], partsInstalled {
            return .mii(local, fallback: nil)
        }
        guard let remote = remotes[license.friendCode] else { return .none }
        // What Retro WFC saw is of another Mii once the licence took a new one.
        if let seen = remote.mii, seen.miiId != license.miiId { return .none }
        if let seen = remote.mii, partsInstalled { return .mii(seen, fallback: remote.picture) }
        return remote.picture.map(Picture.image) ?? .none
    }

    /// Asks Retro WFC once a session what it last saw of `friendCode`'s Mii, showing what the disk
    /// kept from last time meanwhile. A licence never taken online has no friend code, and nothing here.
    private func fetchRemote(_ friendCode: String) {
        guard !friendCode.isEmpty, remotesAsked.insert(friendCode).inserted else { return }
        Task {
            let cached = await Task.detached(priority: .utility) { ProfileStore.cachedRemote(friendCode) }.value
            if remotes[friendCode] == nil, let cached { remotes[friendCode] = cached }
            guard let fresh = try? await RetroWfc.playerMii(friendCode) else { return }
            guard fresh.data != cached?.data || fresh.image != cached?.image, let remote = Remote(data: fresh.data, image: fresh.image) else { return }
            remotes[friendCode] = remote
            await Task.detached(priority: .utility) { ProfileStore.store(friendCode, fresh) }.value
        }
    }

    nonisolated private static func cacheFiles(_ friendCode: String) -> (data: URL, image: URL) {
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
        let directory = caches.appendingPathComponent(imageDirectory, isDirectory: true)
        return (directory.appendingPathComponent("\(friendCode).mii"), directory.appendingPathComponent("\(friendCode).png"))
    }

    nonisolated private static func cachedRemote(_ friendCode: String) -> Remote? {
        let files = cacheFiles(friendCode)
        return Remote(data: try? Data(contentsOf: files.data), image: try? Data(contentsOf: files.image))
    }

    nonisolated private static func store(_ friendCode: String, _ mii: RetroWfc.PlayerMii) {
        let files = cacheFiles(friendCode)
        try? FileManager.default.createDirectory(at: files.data.deletingLastPathComponent(), withIntermediateDirectories: true)
        for (file, bytes) in [(files.data, mii.data), (files.image, mii.image)] {
            if let bytes { try? bytes.write(to: file, options: .atomic) } else { try? FileManager.default.removeItem(at: file) }
        }
    }
}
