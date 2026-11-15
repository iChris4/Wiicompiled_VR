// SPDX-License-Identifier: GPL-3.0-or-later

import CryptoKit
import Foundation

/// Fetches the Retro Rewind pack the modded game reads, from Retro Rewind's own distribution
/// server, the way the Quest launcher (RetroRewindPack.kt) and WheelWizard on a PC do: the
/// server publishes the URL of a full install zip, a list of update zips with their versions,
/// and a list of files each update deletes. An installation is the base zip plus every update
/// newer than the version it holds.
///
/// None of that content is part of this app; this only downloads what Retro Rewind publishes.
/// Only the pack's own `RetroRewind6` tree is kept, which is what `[paths] retro_rewind_root`
/// names; the Riivolution XML and Wii channels beside it mean nothing here.
///
/// Unlike the Quest and PC launchers, this one cannot rebuild the game when the pack's
/// `Code.pul` changes, so updates stop at the version the app was translated from
/// (RetroRewindBuild.packVersion) and a pack whose `Code.pul` differs is not played.
enum RetroRewindPack {
    static let directoryName = "RetroRewind6"
    static var directory: URL { GameStorage.gameDirectory.appendingPathComponent(directoryName, isDirectory: true) }

    private static let baseURL = "https://update.rwfc.net/RetroRewind/"
    private static let installURL = baseURL + "RetroRewindInstall.txt"
    private static let versionURL = baseURL + "RetroRewindVersion.txt"
    private static let deleteURL = baseURL + "RetroRewindDelete.txt"

    /// The server still lists some downloads under the host it used before; they moved, not vanished.
    private static let oldHost = "http://update.rwfc.net:8000/"
    private static let newHost = "https://update.rwfc.net/"

    /// Everything this app keeps lives under this directory inside the published zips.
    private static let packPrefix = directoryName + "/"
    private static let versionFile = "version.txt"
    private static let codePulPath = "Binaries/Code.pul"
    /// The base zip and the pack it unpacks to, both on disk while installing.
    private static let freeSpaceMargin: Int64 = 5 << 30

    /// One published update: the version it produces and the zip that gets there.
    struct Update: Equatable {
        let version: String
        let url: String
        let description: String
    }

    /// One published deletion: the version that drops `path`, relative to the pack's parent.
    struct Deletion: Equatable {
        let version: String
        let path: String
    }

    /// What is on disk, as the launcher shows it.
    enum Status: Equatable {
        case notInstalled
        /// A pack this build can play.
        case ready(version: String)
        /// The pack's Code.pul is not the one the mod was translated from; the app must be
        /// rebuilt from this pack (or the pack reinstalled at the app's version).
        case mismatch(version: String)
    }

    struct Progress: Equatable, Sendable {
        /// "Downloading Retro Rewind 6.12.8" and the like.
        let title: String
        /// 0...1, or nil while the size is unknown.
        let fraction: Double?
        let detail: String
    }

    struct Failure: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    // MARK: Feeds

    /// Reads the version feed: `<version> <url> <path> <description>` per line, oldest first.
    /// Lines that do not parse are skipped rather than failing the update, as the PC launcher does.
    static func parseUpdates(_ text: String) -> [Update] {
        text.split(whereSeparator: \.isNewline).compactMap { line -> Update? in
            let parts = line.trimmingCharacters(in: .whitespaces).split(separator: " ", maxSplits: 3, omittingEmptySubsequences: false)
            guard parts.count >= 4 else { return nil }
            let version = parts[0].trimmingCharacters(in: .whitespaces)
            let url = parts[1].trimmingCharacters(in: .whitespaces).replacingOccurrences(of: oldHost, with: newHost)
            guard isVersion(version), !url.isEmpty else { return nil }
            return Update(version: version, url: url, description: parts[3].trimmingCharacters(in: .whitespaces))
        }
    }

    /// Reads the deletion feed: `<version> <path>` per line.
    static func parseDeletions(_ text: String) -> [Deletion] {
        text.split(whereSeparator: \.isNewline).compactMap { line -> Deletion? in
            let parts = line.trimmingCharacters(in: .whitespaces).split(separator: " ", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count >= 2 else { return nil }
            let version = parts[0].trimmingCharacters(in: .whitespaces)
            let path = parts[1].trimmingCharacters(in: .whitespaces)
            guard isVersion(version), !path.isEmpty else { return nil }
            return Deletion(version: version, path: path)
        }
    }

    /// The updates to apply to an installation holding `installed` (nil: none installed), in
    /// order, stopping at `target` when the build names the pack it was made from.
    static func updatesAfter(_ installed: String?, upTo target: String?, all: [Update]) -> [Update] {
        all.filter { update in
            (installed == nil || compare(update.version, installed!) > 0)
                && (target == nil || target!.isEmpty || compare(update.version, target!) <= 0)
        }
        .sorted { compare($0.version, $1.version) < 0 }
    }

    /// The pack-relative paths the updates between `installed` and `target` delete, in order.
    /// Paths outside the pack (the loose zips and the Riivolution XML the feed also lists) are
    /// not ours.
    static func deletionsBetween(_ installed: String?, _ target: String, all: [Deletion]) -> [String] {
        all.filter { (installed == nil || compare($0.version, installed!) > 0) && compare($0.version, target) <= 0 }
            .sorted { compare($0.version, $1.version) < 0 }
            .compactMap { packRelative($0.path) }
    }

    /// The updates to apply over an installation holding `installed`, each with the
    /// pack-relative paths its version deletes, in the order the PC launcher applies them: an
    /// update, then its deletions, then the next update.
    static func steps(_ installed: String?, updates: [Update], deletions: [Deletion]) -> [(Update, [String])] {
        var previous = installed
        return updates.sorted { compare($0.version, $1.version) < 0 }.map { update in
            let dropped = deletionsBetween(previous, update.version, all: deletions)
            previous = update.version
            return (update, dropped)
        }
    }

    /// Dotted numeric comparison, which is all these versions ever are (6.12.8 > 6.9.10).
    static func compare(_ a: String, _ b: String) -> Int {
        let left = a.split(separator: ".").map { Int($0) ?? 0 }
        let right = b.split(separator: ".").map { Int($0) ?? 0 }
        for index in 0..<max(left.count, right.count) {
            let l = index < left.count ? left[index] : 0
            let r = index < right.count ? right[index] : 0
            if l != r { return l < r ? -1 : 1 }
        }
        return 0
    }

    static func isVersion(_ text: String) -> Bool {
        !text.isEmpty && text.split(separator: ".", omittingEmptySubsequences: false).allSatisfy { !$0.isEmpty && $0.allSatisfy(\.isNumber) }
    }

    /// A published path relative to the pack, or nil when it is not inside the pack. Paths that
    /// try to climb out of it are refused: nothing outside the pack directory is ever touched.
    static func packRelative(_ published: String) -> String? {
        var name = published.replacingOccurrences(of: "\\", with: "/")
        while name.hasPrefix("/") { name.removeFirst() }
        guard name.hasPrefix(packPrefix) else { return nil }
        let relative = name.dropFirst(packPrefix.count).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard !relative.isEmpty, !relative.split(separator: "/").contains("..") else { return nil }
        return relative
    }

    // MARK: On disk

    /// The version the installed pack holds, or nil when there is none.
    static func installedVersion(in pack: URL = directory) -> String? {
        guard let text = try? String(contentsOf: pack.appendingPathComponent(versionFile), encoding: .utf8) else { return nil }
        let version = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return isVersion(version) ? version : nil
    }

    /// Whether the installed Code.pul is the one this build was translated from; true when the
    /// build does not know (it was configured without the file).
    static func codePulMatchesBuild(in pack: URL = directory) -> Bool {
        guard !RetroRewindBuild.codePulSHA256.isEmpty else { return true }
        guard let data = try? Data(contentsOf: pack.appendingPathComponent(codePulPath), options: .mappedIfSafe) else { return false }
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        return digest == RetroRewindBuild.codePulSHA256.lowercased()
    }

    static var status: Status {
        guard let version = installedVersion() else { return .notInstalled }
        return codePulMatchesBuild() ? .ready(version: version) : .mismatch(version: version)
    }

    /// The version an install or update would leave on disk, or nil when the installed pack
    /// is already there. Asks the server; throws when it cannot be reached.
    static func availableUpdate() async throws -> String? {
        let updates = updatesAfter(installedVersion(), upTo: RetroRewindBuild.packVersion, all: parseUpdates(try await fetchText(versionURL)))
        return updates.last?.version
    }

    // MARK: Installing

    /// Installs or updates the pack, reporting progress; throws a Failure with the message to
    /// show, or CancellationError when the task was cancelled.
    ///
    /// A missing or version-less pack is replaced by the published base zip, unpacked beside it
    /// and swapped in only once it looks like a pack. Updates are then applied over the
    /// installed pack and the version is written last, so an interrupted update simply runs
    /// again next time.
    static func install(progress report: @escaping @Sendable (Progress) -> Void) async throws {
        let manager = FileManager.default
        let pack = directory
        let gameRoot = GameStorage.gameDirectory
        try manager.createDirectory(at: gameRoot, withIntermediateDirectories: true)
        let target = RetroRewindBuild.packVersion.isEmpty ? nil : RetroRewindBuild.packVersion

        let installed = installedVersion()
        if installed == nil, let free = freeSpace(at: gameRoot), free < freeSpaceMargin {
            throw Failure(message: "Not enough free space: installing the Retro Rewind pack needs about \(gigabytes(freeSpaceMargin)), and \(gigabytes(free)) is free.")
        }

        do {
            report(Progress(title: "Checking Retro Rewind's server", fraction: nil, detail: ""))
            let updates = updatesAfter(installed, upTo: target, all: parseUpdates(try await fetchText(versionURL)))
            if installed != nil, updates.isEmpty { return }

            if installed == nil {
                let base = try await fetchText(installURL).trimmingCharacters(in: .whitespacesAndNewlines)
                    .replacingOccurrences(of: oldHost, with: newHost)
                guard !base.isEmpty else { throw Failure(message: "Retro Rewind's server did not say where its download is.") }
                let staging = gameRoot.appendingPathComponent("\(directoryName).downloading", isDirectory: true)
                try? manager.removeItem(at: staging)
                defer { try? manager.removeItem(at: staging) }
                try await download(base, into: staging, title: "Retro Rewind", report: report)
                guard manager.fileExists(atPath: staging.appendingPathComponent(codePulPath).path),
                      manager.fileExists(atPath: staging.appendingPathComponent(versionFile).path) else {
                    throw Failure(message: "Retro Rewind's download did not contain the pack.")
                }
                report(Progress(title: "Finishing", fraction: nil, detail: ""))
                try replace(pack, with: staging)
            }

            // Every update is a partial tree that lands on top of the installed pack, so they are
            // applied in order, each followed by its own deletions before the next one can put a
            // file back, and the version is only written once all of them are in.
            let remaining = updatesAfter(installedVersion(), upTo: target, all: updates)
            if !remaining.isEmpty {
                let deletions = parseDeletions(try await fetchText(deleteURL))
                for (update, dropped) in steps(installedVersion(), updates: remaining, deletions: deletions) {
                    try await download(update.url, into: pack, title: "Retro Rewind \(update.version)", report: report)
                    for relative in dropped {
                        try? manager.removeItem(at: pack.appendingPathComponent(relative))
                    }
                }
                report(Progress(title: "Finishing", fraction: nil, detail: ""))
                try remaining.last!.version.write(to: pack.appendingPathComponent(versionFile), atomically: true, encoding: .utf8)
            }
        } catch let failure as Failure {
            throw failure
        } catch is CancellationError {
            throw CancellationError()
        } catch let error as URLError where error.code == .cancelled {
            throw CancellationError()
        } catch {
            throw Failure(message: "Retro Rewind could not be downloaded: \(error.localizedDescription)")
        }
    }

    private static func fetchText(_ url: String) async throws -> String {
        try Task.checkCancellation()
        let (data, response) = try await URLSession.shared.data(for: request(url))
        try check(response)
        return String(decoding: data, as: UTF8.self)
    }

    /// Downloads the zip at `url` and unpacks its pack directory into `target`, over whatever is there.
    private static func download(_ url: String, into target: URL, title: String,
                                 report: @escaping @Sendable (Progress) -> Void) async throws {
        try Task.checkCancellation()
        report(Progress(title: "Downloading \(title)", fraction: nil, detail: ""))
        let zip = FileManager.default.temporaryDirectory.appendingPathComponent("RetroRewind-\(UUID().uuidString).zip")
        defer { try? FileManager.default.removeItem(at: zip) }
        let response = try await Downloader.download(request(url), to: zip) { written, expected in
            let fraction = expected > 0 ? Double(written) / Double(expected) : nil
            report(Progress(title: "Downloading \(title)", fraction: fraction,
                            detail: expected > 0 ? "\(gigabytes(written)) of \(gigabytes(expected))" : gigabytes(written)))
        }
        try check(response)

        // Unpacking is file work; keep it off the cooperative pool's threads.
        try await Task.detached(priority: .utility) {
            let archive = try ZipArchive(url: zip)
            let entries = archive.entries.compactMap { entry -> (ZipArchive.Entry, String)? in
                guard !entry.isDirectory, let relative = packRelative(entry.name) else { return nil }
                return (entry, relative)
            }
            let total = entries.reduce(UInt64(0)) { $0 + $1.0.compressedSize }
            var done: UInt64 = 0
            var lastReport: UInt64 = 0
            for (index, (entry, relative)) in entries.enumerated() {
                try Task.checkCancellation()
                try archive.extract(entry, to: target.appendingPathComponent(relative)) { consumed in
                    if done + consumed - lastReport >= 8 << 20 {
                        lastReport = done + consumed
                        report(Progress(title: "Unpacking \(title)", fraction: total > 0 ? Double(lastReport) / Double(total) : nil,
                                        detail: "\(index + 1) of \(entries.count) files"))
                    }
                    return !Task.isCancelled
                }
                done += entry.compressedSize
            }
            report(Progress(title: "Unpacking \(title)", fraction: 1, detail: "\(entries.count) files"))
        }.value
    }

    private static func request(_ url: String) throws -> URLRequest {
        guard let parsed = URL(string: url) else { throw Failure(message: "Retro Rewind's server published an invalid address: \(url)") }
        var request = URLRequest(url: parsed, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 60)
        // Ask for the bytes as they are, so the length is the zip's and nothing re-encodes it.
        request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
        return request
    }

    private static func check(_ response: URLResponse) throws {
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw Failure(message: "Retro Rewind's server answered \(http.statusCode).")
        }
    }

    /// Moves `staging` to `destination`, keeping the old destination until the new one is in place.
    private static func replace(_ destination: URL, with staging: URL) throws {
        let manager = FileManager.default
        let replaced = destination.deletingLastPathComponent().appendingPathComponent("\(destination.lastPathComponent).replaced")
        try? manager.removeItem(at: replaced)
        if manager.fileExists(atPath: destination.path) {
            do {
                try manager.moveItem(at: destination, to: replaced)
            } catch {
                throw Failure(message: "The existing \(destination.lastPathComponent) folder could not be replaced. Remove it with the Files app and try again.")
            }
        }
        do {
            try manager.moveItem(at: staging, to: destination)
        } catch {
            try? manager.moveItem(at: replaced, to: destination)
            throw Failure(message: "The new files could not be moved into \(destination.path).")
        }
        // The new copy is in place; an old one that will not delete only costs space.
        try? manager.removeItem(at: replaced)
    }

    private static func freeSpace(at url: URL) -> Int64? {
        let values = try? url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return values?.volumeAvailableCapacityForImportantUsage
    }

    static func gigabytes(_ bytes: Int64) -> String {
        String(format: "%.1f GB", Double(bytes) / 1_000_000_000)
    }

    /// One download task to a file of ours, with progress, awaited; cancelling the task that
    /// awaits it cancels the transfer. URLSession calls the delegate on its own queue.
    private final class Downloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
        private let destination: URL
        private let onProgress: @Sendable (Int64, Int64) -> Void
        private let lock = NSLock()
        private var continuation: CheckedContinuation<URLResponse, Error>?
        private var lastReported: Int64 = 0
        private var moveError: Error?

        static func download(_ request: URLRequest, to destination: URL,
                             onProgress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> URLResponse {
            let downloader = Downloader(destination: destination, onProgress: onProgress)
            let session = URLSession(configuration: .default, delegate: downloader, delegateQueue: nil)
            defer { session.finishTasksAndInvalidate() }
            let task = session.downloadTask(with: request)
            return try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation { continuation in
                    downloader.continuation = continuation
                    task.resume()
                }
            } onCancel: {
                task.cancel()
            }
        }

        private init(destination: URL, onProgress: @escaping @Sendable (Int64, Int64) -> Void) {
            self.destination = destination
            self.onProgress = onProgress
        }

        private func finish(_ result: Result<URLResponse, Error>) {
            lock.lock()
            let continuation = self.continuation
            self.continuation = nil
            lock.unlock()
            continuation?.resume(with: result)
        }

        func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                        totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
            guard totalBytesWritten - lastReported >= 8 << 20 || totalBytesWritten == totalBytesExpectedToWrite else { return }
            lastReported = totalBytesWritten
            onProgress(totalBytesWritten, totalBytesExpectedToWrite)
        }

        // The system's file only lives until this returns: move it now, and finish in didComplete.
        func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
            do {
                try? FileManager.default.removeItem(at: destination)
                try FileManager.default.moveItem(at: location, to: destination)
            } catch {
                moveError = error
            }
        }

        func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
            if let error {
                finish(.failure(error))
            } else if let moveError {
                finish(.failure(moveError))
            } else if let response = task.response {
                finish(.success(response))
            } else {
                finish(.failure(URLError(.badServerResponse)))
            }
        }
    }
}
