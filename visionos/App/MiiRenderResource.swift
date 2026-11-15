// SPDX-License-Identifier: GPL-3.0-or-later

import CryptoKit
import Foundation

/// What Mii pictures are drawn from: FFL's Mii parts (FFLResHigh.dat), the PC launcher's
/// MiiRenderingResourceInstaller, and the 3DS Mii bodies the PC carries (MiiBodies). Both are
/// Nintendo's, so the app never carries them; the player downloads them once, the parts from the
/// Internet Archive's copy of Miitomo's files as on the PC, the bodies from Wheel Wizard's own
/// repository, and each is checked against the SHA-256 of the copy the PC and Quest launchers use.
/// A port of the Quest launcher's MiiRenderResource.kt.
///
/// They are kept in the app's Application Support folder, out of the Documents folder the Files
/// app shows: they are the launcher's, not the game's.
enum MiiRenderResource {
    /// Endpoints.MiiRenderingArchive on the PC.
    private static let archiveURL = "https://web.archive.org/web/20180502054513id_/" +
        "http://download-cdn.miitomo.com/native/20180125111639/android/v2/asset_model_character_mii_AFLResHigh_2_3_dat.zip"
    private static let entry = "asset/model/character/mii/AFLResHigh_2_3.dat"
    private static let archiveBytes: Int64 = 4_393_464
    private static let fileBytes: Int64 = 4_579_008

    /// The PC's embedded body models, at the Wheel Wizard commit that added them.
    private static let bodyURL = "https://raw.githubusercontent.com/TeamWheelWizard/WheelWizard/" +
        "cc4c2e9df2ef5f7717e9b1fbe18a94acc4076b5f/WheelWizard/Features/MiiRendering/Resources/"
    private static let maleBody = BodyFile(name: "mii_static_body_3ds_male_LE.rmdl", bytes: 14_320,
                                           sha256: "f17b764f4c42729572548f1cf760ab6e2eb6420cde5898b904be7eaea68cf837")
    private static let femaleBody = BodyFile(name: "mii_static_body_3ds_female_LE.rmdl", bytes: 14_736,
                                             sha256: "371639d2b73280bc2e23e6aac44b49403a026a0f019ad287ac2c7c996451742f")
    private static var bodyFiles: [BodyFile] { [maleBody, femaleBody] }

    private static let attempts = 3
    private static let timeout: TimeInterval = 30

    private struct BodyFile {
        let name: String
        let bytes: Int64
        let sha256: String
    }

    /// What has been read so far, shared by every thread that draws.
    private static let lock = NSLock()
    private static var loaded: FflResource?
    private enum BodiesState { case unread, missing, read(MiiBodies) }
    private static var loadedBodies = BodiesState.unread

    static var directory: URL {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        return support.appendingPathComponent("MiiRendering", isDirectory: true)
    }

    static var partsFile: URL { directory.appendingPathComponent("FFLResHigh.dat") }

    private static func file(_ body: BodyFile) -> URL { directory.appendingPathComponent(body.name) }

    private static func size(_ url: URL) -> Int64 {
        ((try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? NSNumber)?.int64Value ?? -1
    }

    /// Whether Miis can be drawn: the parts are here.
    static var installed: Bool { size(partsFile) == fileBytes }

    /// Whether Miis are drawn with their bodies, as on the PC: the parts and the bodies are here.
    static var complete: Bool { installed && bodyFiles.allSatisfy { size(file($0)) == $0.bytes } }

    /// How much `install` downloads: what is not here yet.
    static var downloadBytes: Int64 {
        (installed ? 0 : archiveBytes) + bodyFiles.filter { size(file($0)) != $0.bytes }.reduce(0) { $0 + $1.bytes }
    }

    /// The parts, read once; nil when they are not installed or cannot be read. Not on the main thread.
    static func load() -> FflResource? {
        lock.lock()
        defer { lock.unlock() }
        if let loaded { return loaded }
        guard installed else { return nil }
        loaded = try? FflResource.load(partsFile)
        return loaded
    }

    /// The bodies, read once; nil when they are not installed or cannot be read. Not on the main thread.
    static func bodies() -> MiiBodies? {
        lock.lock()
        defer { lock.unlock() }
        switch loadedBodies {
        case .read(let bodies): return bodies
        case .missing: return nil
        case .unread:
            guard complete else { return nil }
            if let bodies = try? MiiBodies.load(male: file(maleBody), female: file(femaleBody)) {
                loadedBodies = .read(bodies)
                return bodies
            }
            loadedBodies = .missing
            return nil
        }
    }

    /// Downloads and installs what is missing. `progress` gets the bytes downloaded so far, from
    /// the download's task.
    static func install(progress: @escaping @Sendable (Int64) -> Void) async throws {
        defer {
            // Read the bodies again, now that they may be here.
            lock.withLock { loadedBodies = .unread }
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var before: Int64 = 0
        if !installed {
            let done = before
            try await retrying("Mii parts") { try await downloadParts { progress(done + $0) } }
            before += archiveBytes
        }
        for body in bodyFiles where size(file(body)) != body.bytes {
            let done = before
            try await retrying("Mii body \(body.name)") { try await downloadBody(body) { progress(done + $0) } }
            before += body.bytes
        }
    }

    private static func retrying(_ what: String, _ attempt: () async throws -> Void) async throws {
        var failure: Error?
        for _ in 1...attempts {
            try Task.checkCancellation()
            do {
                try await attempt()
                return
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                failure = error
            }
        }
        throw failure ?? MiiError("The \(what) could not be downloaded.")
    }

    private static func downloadParts(progress: @escaping (Int64) -> Void) async throws {
        let archive = try await fetch(archiveURL, server: "The Internet Archive", size: archiveBytes, progress: progress)
        guard archive.count >= 4, archive[archive.startIndex] == UInt8(ascii: "P"), archive[archive.startIndex + 1] == UInt8(ascii: "K") else {
            throw MiiError("The download is not a ZIP archive.")
        }
        let manager = FileManager.default
        let zip = manager.temporaryDirectory.appendingPathComponent("MiiParts-\(UUID().uuidString).zip")
        let partial = directory.appendingPathComponent(partsFile.lastPathComponent + ".partial")
        defer {
            try? manager.removeItem(at: zip)
            try? manager.removeItem(at: partial)
        }
        try archive.write(to: zip)
        let zipArchive = try ZipArchive(url: zip)
        guard let found = zipArchive.entries.first(where: { $0.name == entry }) else {
            throw MiiError("The archive does not contain \(entry).")
        }
        try zipArchive.extract(found, to: partial)
        let digest = sha256(try Data(contentsOf: partial))
        guard digest == FflResource.sha256 else {
            throw MiiError("The downloaded Mii parts are not the expected file (SHA-256 \(digest)).")
        }
        _ = try FflResource.load(partial)
        try place(partial, at: partsFile)
    }

    private static func downloadBody(_ body: BodyFile, progress: @escaping (Int64) -> Void) async throws {
        let bytes = try await fetch(bodyURL + body.name, server: "GitHub", size: body.bytes, progress: progress)
        let digest = sha256(bytes)
        guard digest == body.sha256 else {
            throw MiiError("The downloaded Mii body is not the expected file (SHA-256 \(digest)).")
        }
        _ = try MiiBodies.parse(bytes)
        let partial = directory.appendingPathComponent(body.name + ".partial")
        defer { try? FileManager.default.removeItem(at: partial) }
        try bytes.write(to: partial)
        try place(partial, at: file(body))
    }

    /// Moves a checked download to where it is read from. It can be downloaded again, so it
    /// stays out of the headset's backups.
    private static func place(_ partial: URL, at target: URL) throws {
        let manager = FileManager.default
        do {
            if manager.fileExists(atPath: target.path) { try manager.removeItem(at: target) }
            try manager.moveItem(at: partial, to: target)
            var stored = target
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try? stored.setResourceValues(values)
        } catch {
            throw MiiError("Cannot store \(target.path): \(error.localizedDescription)")
        }
    }

    private static func fetch(_ url: String, server: String, size: Int64, progress: (Int64) -> Void) async throws -> Data {
        guard let address = URL(string: url) else { throw MiiError("Invalid address \(url)") }
        var request = URLRequest(url: address, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: timeout)
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
        request.setValue("WiiCompiledVR-VisionPro/\(version)", forHTTPHeaderField: "User-Agent")
        // Ask for the bytes as they are, so the length is the file's and nothing re-encodes it.
        request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
        let (stream, response) = try await URLSession.shared.bytes(for: request)
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw MiiError("\(server) answered \(http.statusCode).")
        }
        let expected = response.expectedContentLength
        var data = Data(capacity: Int(expected > 0 ? expected : size))
        var chunk = [UInt8]()
        chunk.reserveCapacity(64 * 1024)
        for try await byte in stream {
            chunk.append(byte)
            if chunk.count == 64 * 1024 {
                data.append(contentsOf: chunk)
                chunk.removeAll(keepingCapacity: true)
                progress(Int64(data.count))
            }
        }
        data.append(contentsOf: chunk)
        progress(Int64(data.count))
        if expected > 0, Int64(data.count) != expected {
            throw MiiError("The download stopped after \(data.count) of \(expected) bytes.")
        }
        return data
    }

    private static func sha256(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }
}
