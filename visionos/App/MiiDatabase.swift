// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// The Wii's Mii database, `shared2/menu/FaceLib/RFL_DB.dat` in the runtime's NAND, which the game
/// reads its Miis from. A port of the Quest launcher's MiiDatabase.kt, itself the PC launcher's
/// MiiRepositoryService and MiiDbService: 100 slots of 74 bytes after the "RNOD" magic, an empty
/// slot being all zeros, and a CRC-16 over the first 0x1F1DE bytes stored after them.
///
/// A new NAND has none, so the Miis tab creates the Wii's empty database the first time it is
/// opened. Writes go through a temporary file, so a failure never leaves half a file.
enum MiiDatabase {
    static let slots = 100
    static let fileSize = 779_968
    private static let header = 4
    private static let crcOffset = 0x1F1DE
    private static let hiddenEntries = 10_000
    private static let hiddenEntrySize = 12
    /// The first hidden entry's pair of 0x7FFF links.
    private static let hiddenLinks = 0x1D10

    static func file(nand: URL) -> URL {
        nand.appendingPathComponent("shared2/menu/FaceLib/RFL_DB.dat")
    }

    /// A new database: the PC's (both magics, the hidden database's empty markers and the CRC), and
    /// like one the Wii formats, the hidden database's 10,000 entries linked to nothing (0x7FFF).
    /// The PC leaves those links zero; the game's Mii library reads this file, so it gets the Wii's.
    static func empty() -> [UInt8] {
        var db = [UInt8](repeating: 0, count: fileSize)
        db.replaceSubrange(0..<4, with: Array("RNOD".utf8))
        db[0x1CE0 + 0x0C] = 0x80
        db.replaceSubrange(0x1D00..<0x1D04, with: Array("RNHD".utf8))
        for i in 0x1D04...0x1D07 { db[i] = 0xFF }
        for entry in 0..<hiddenEntries {
            let at = hiddenLinks + entry * hiddenEntrySize
            db[at] = 0x7F
            db[at + 1] = 0xFF
            db[at + 2] = 0x7F
            db[at + 3] = 0xFF
        }
        writeCrc(&db)
        return db
    }

    /// CRC-16/XMODEM, CrcHelper.ComputeCrc16Ccitt on the PC.
    static func crc16(_ data: [UInt8], _ offset: Int, _ length: Int) -> UInt16 {
        var crc: UInt16 = 0
        for i in offset..<(offset + length) {
            crc ^= UInt16(data[i]) << 8
            for _ in 0..<8 {
                crc = crc & 0x8000 != 0 ? (crc << 1) ^ 0x1021 : crc << 1
            }
        }
        return crc
    }

    /// Creates the empty database unless there is one.
    static func create(_ file: URL) throws {
        let manager = FileManager.default
        guard !manager.fileExists(atPath: file.path) else { return }
        do {
            try manager.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        } catch {
            throw MiiError("Cannot create \(file.deletingLastPathComponent().path): \(error.localizedDescription)")
        }
        try write(file, empty())
    }

    /// The 100 slots, nil where empty.
    static func slots(_ file: URL) throws -> [[UInt8]?] {
        slots(try read(file))
    }

    static func slots(_ db: [UInt8]) -> [[UInt8]?] {
        (0..<slots).map { slot in
            let start = offset(slot)
            guard start + MiiData.size <= db.count else { return nil }
            let block = Array(db[start..<(start + MiiData.size)])
            return block.allSatisfy { $0 == 0 } ? nil : block
        }
    }

    /// Every Mii in the database, in slot order. A slot the PC could not read either is left out;
    /// it stays in the file untouched.
    static func miis(_ file: URL) throws -> [Mii] {
        try slots(file).compactMap { block in block.flatMap { try? MiiData.parse(Data($0)) } }
    }

    /// The database's Miis by ID, as the PC's GetByAvatarId finds them: the first slot holding an ID
    /// is that ID's Mii, and one the PC could not read either is left out.
    static func byId(_ file: URL) throws -> [UInt32: Mii] {
        var found: [UInt32: Mii?] = [:]
        for case let block? in try slots(file) where found[readId(block, 0)] == nil {
            found[readId(block, 0)] = .some(try? MiiData.parse(Data(block)))
        }
        return found.compactMapValues { $0 }
    }

    /// Whether `db` is a Mii database the game reads: the magic, the size and the CRC.
    static func isValid(_ db: [UInt8]) -> Bool {
        db.count >= crcOffset + 2 && Array(db[0..<4]) == Array("RNOD".utf8)
            && UInt16(db[crcOffset]) << 8 | UInt16(db[crcOffset + 1]) == crc16(db, 0, crcOffset)
    }

    /// Adds another database's Miis to this one block for block, as the Wii stored them (nothing is
    /// parsed and written again, so no bit is lost): a Mii whose ID is here already takes its slot,
    /// the others the free ones. Returns how many were added, replaced, and left out for want of a slot.
    @discardableResult
    static func merge(_ file: URL, from other: [UInt8]) throws -> (added: Int, replaced: Int, skipped: Int) {
        var added = 0
        var replaced = 0
        var skipped = 0
        let blocks = slots(other).compactMap { $0 }
        try edit(file) { db in
            for block in blocks {
                let id = readId(block, 0)
                if id != 0, let slot = (0..<slots).first(where: { !isEmpty(db, $0) && readId(db, offset($0)) == id }) {
                    db.replaceSubrange(offset(slot)..<(offset(slot) + MiiData.size), with: block)
                    replaced += 1
                } else if let free = (0..<slots).first(where: { isEmpty(db, $0) }) {
                    db.replaceSubrange(offset(free)..<(offset(free) + MiiData.size), with: block)
                    added += 1
                } else {
                    skipped += 1
                }
            }
        }
        return (added, replaced, skipped)
    }

    /// Adds a Mii in the first free slot.
    static func add(_ file: URL, _ mii: Mii) throws {
        let block = try MiiData.serialize(mii)
        try edit(file) { db in
            guard let free = (0..<slots).first(where: { isEmpty(db, $0) }) else {
                throw MiiError("No empty Mii slot available.")
            }
            db.replaceSubrange(offset(free)..<(offset(free) + MiiData.size), with: block)
        }
    }

    /// Replaces the Mii with the same ID.
    static func update(_ file: URL, _ mii: Mii) throws {
        let block = try MiiData.serialize(mii)
        try edit(file) { db in
            let start = offset(try slotOf(db, mii.miiId))
            db.replaceSubrange(start..<(start + MiiData.size), with: block)
        }
    }

    /// Empties the Mii's slot.
    static func remove(_ file: URL, miiId: UInt32) throws {
        try edit(file) { db in
            let start = offset(try slotOf(db, miiId))
            db.replaceSubrange(start..<(start + MiiData.size), with: repeatElement(0, count: MiiData.size))
        }
    }

    private static func edit(_ file: URL, _ change: (inout [UInt8]) throws -> Void) throws {
        guard FileManager.default.fileExists(atPath: file.path) else { throw MiiError("RFL_DB.dat not found.") }
        var db = try read(file)
        guard db.count >= crcOffset + 2 else { throw MiiError("RFL_DB.dat is too short (\(db.count) bytes).") }
        let stored = UInt16(db[crcOffset]) << 8 | UInt16(db[crcOffset + 1])
        let computed = crc16(db, 0, crcOffset)
        guard stored == computed else {
            throw MiiError(String(format: "Corrupt Mii database (bad CRC 0x%04X, expected 0x%04X).", stored, computed))
        }
        try change(&db)
        writeCrc(&db)
        try write(file, db)
    }

    private static func slotOf(_ db: [UInt8], _ miiId: UInt32) throws -> Int {
        guard miiId != 0 else { throw MiiError("Invalid Client ID.") }
        guard let slot = (0..<slots).first(where: { !isEmpty(db, $0) && readId(db, offset($0)) == miiId }) else {
            throw MiiError("Mii not found.")
        }
        return slot
    }

    private static func offset(_ slot: Int) -> Int { header + slot * MiiData.size }

    private static func isEmpty(_ db: [UInt8], _ slot: Int) -> Bool {
        let start = offset(slot)
        return db[start..<(start + MiiData.size)].allSatisfy { $0 == 0 }
    }

    private static func readId(_ db: [UInt8], _ block: Int) -> UInt32 {
        (0..<4).reduce(UInt32(0)) { id, i in id << 8 | UInt32(db[block + 0x18 + i]) }
    }

    private static func writeCrc(_ db: inout [UInt8]) {
        let crc = crc16(db, 0, crcOffset)
        db[crcOffset] = UInt8(crc >> 8)
        db[crcOffset + 1] = UInt8(crc & 0xFF)
    }

    private static func read(_ file: URL) throws -> [UInt8] {
        do {
            return [UInt8](try Data(contentsOf: file))
        } catch {
            throw MiiError("Cannot read \(file.lastPathComponent): \(error.localizedDescription)")
        }
    }

    private static func write(_ file: URL, _ db: [UInt8]) throws {
        do {
            // Written beside the file and renamed over it, which Data's atomic write does.
            try Data(db).write(to: file, options: .atomic)
        } catch {
            throw MiiError("Cannot write \(file.lastPathComponent): \(error.localizedDescription)")
        }
    }
}

/// The IDs that make a Mii this headset's own: MiiDbService.AddToDatabase and GenerateMiiId on the
/// PC, MiiIds in the Quest launcher. The system ID comes from the console's MAC address, which the
/// runtime derives from the NAND's serial number (RuntimeConsoleIdentity::FromSerial in
/// runtime/include/console_identity.h), so a Mii made here belongs to the same console as the
/// game's saves.
enum MiiIds {
    /// The PC gives imported Miis this address, so they never pass for the console's own.
    static let importMac: [UInt8] = [0x02, 0x11, 0x11, 0x11, 0x11, 0x11]

    private static let lock = NSLock()
    private static var lastCounter: Int64 = -1
    private static var sequenceOffset: Int64 = 0

    static func systemId(mac: [UInt8]) -> UInt32 {
        let first = (UInt32(mac[0]) + UInt32(mac[1]) + UInt32(mac[2])) & 0xFF
        return first << 24 | UInt32(mac[3]) << 16 | UInt32(mac[4]) << 8 | UInt32(mac[5])
    }

    /// A new Mii ID: 0b100 and a 4-second counter from 2006, bumped when two share a tick.
    static func newMiiId(now: Date = Date()) -> UInt32 {
        lock.lock()
        defer { lock.unlock() }
        let base = Int64(floor(now.timeIntervalSince(MiiData.epoch2006) / 4)) & 0xFFFF_FFFF
        if base == lastCounter {
            sequenceOffset += 1
        } else {
            lastCounter = base
            sequenceOffset = 0
        }
        return 0b100 << 29 | UInt32((base + sequenceOffset) & 0x1FFF_FFFF)
    }

    static func settingFile(nand: URL) -> URL {
        nand.appendingPathComponent("title/00000001/00000002/data/setting.txt")
    }

    /// The console's MAC address, as the runtime computes it from `setting.txt` in the NAND, or
    /// nil when there is no readable one.
    static func consoleMac(nand: URL) -> [UInt8]? {
        guard let serial = consoleSerial(settingFile(nand: nand)) else { return nil }
        var hash: UInt32 = 2_166_136_261
        for byte in serial.utf8 {
            hash = (hash ^ UInt32(byte)) &* 16_777_619
        }
        var suffix = hash & 0x00FF_FFFF
        if suffix == 0 || suffix == 0x00FF_FFFF { suffix ^= 0x005A_17C3 }
        return [0x00, 0x09, 0xBF, UInt8(suffix >> 16), UInt8((suffix >> 8) & 0xFF), UInt8(suffix & 0xFF)]
    }

    /// SERNO from the Wii's setting.txt, a 256-byte buffer under a rotating XOR key.
    static func consoleSerial(_ file: URL) -> String? {
        guard let data = try? Data(contentsOf: file), let serial = settings(data)?["SERNO"],
              (1...9).contains(serial.count), serial.allSatisfy({ ("0"..."9").contains($0) }),
              serial.contains(where: { $0 != "0" }) else { return nil }
        return serial
    }

    /// The fields of a setting.txt (RuntimeNandSettings::Read): the 256 bytes decrypted up to the
    /// first NUL, carriage returns dropped, one KEY=value per line, the first of a key kept.
    static func settings(_ data: Data) -> [String: String]? {
        guard data.count >= 256 else { return nil }
        var key: UInt32 = 0x73B5_DBFA
        var text = ""
        for byte in data.prefix(256) {
            let value = byte ^ UInt8(key & 0xFF)
            key = key << 1 | key >> 31
            if value == 0 { break }
            if value != 0x0D { text.unicodeScalars.append(Unicode.Scalar(value)) }
        }
        var fields: [String: String] = [:]
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let parts = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            if parts.count == 2, !parts[0].isEmpty, fields[String(parts[0])] == nil {
                fields[String(parts[0])] = String(parts[1])
            }
        }
        return fields
    }

    /// RuntimeNandSettings::HasIdentity: the fields the runtime refuses to start without.
    static func hasIdentity(_ settings: [String: String]) -> Bool {
        guard let serial = settings["SERNO"], (1...9).contains(serial.count),
              serial.allSatisfy({ ("0"..."9").contains($0) }), serial.contains(where: { $0 != "0" }) else { return false }
        return [("CODE", 5), ("AREA", 3), ("GAME", 2)].allSatisfy { field, limit in
            guard let value = settings[field] else { return false }
            return !value.isEmpty && value.utf8.count <= limit
        }
    }

    /// The console's MAC address, creating the console first when the NAND has none yet: the
    /// Quest asks the player to start the game once so that the runtime writes `setting.txt`,
    /// but here each game run needs the app relaunched, so the launcher writes the file the
    /// runtime would (RuntimeNandSettings::Ensure in runtime/include/nand_settings.h), which the
    /// runtime then keeps. An existing file is never replaced, even a damaged one.
    static func ensureConsoleMac(nand: URL, now: Date = Date()) throws -> [UInt8] {
        if let mac = consoleMac(nand: nand) { return mac }
        let file = settingFile(nand: nand)
        let manager = FileManager.default
        if manager.fileExists(atPath: file.path) {
            throw MiiError("The NAND's setting.txt is unreadable or invalid, and the game would refuse it too. Restore it from this console's backup.")
        }
        guard let bytes = encodeSettings(serial: serial(now: now)) else {
            throw MiiError("Cannot create the console's settings: the system clock is invalid.")
        }
        let directory = file.deletingLastPathComponent()
        let temporary = directory.appendingPathComponent(".setting-init-\(UUID().uuidString)")
        do {
            try manager.createDirectory(at: directory, withIntermediateDirectories: true)
            try Data(bytes).write(to: temporary)
            // A move never replaces an existing file, which is the runtime's guarantee too.
            try manager.moveItem(at: temporary, to: file)
        } catch {
            try? manager.removeItem(at: temporary)
            throw MiiError("Cannot create \(file.path): \(error.localizedDescription)")
        }
        guard let mac = consoleMac(nand: nand) else { throw MiiError("Cannot read the new \(file.path).") }
        return mac
    }

    /// RuntimeNandSettings::GenerateSerial: Dolphin's first-boot serial, the clock's last nine digits.
    static func serial(now: Date) -> String {
        let seconds = Int64(floor(now.timeIntervalSince1970))
        guard seconds >= 0 else { return "" }
        let digits = String(seconds % 1_000_000_000)
        return String(repeating: "0", count: 9 - digits.count) + digits
    }

    /// RuntimeNandSettings::EncodeNew: Dolphin's PAL boot settings with this serial, encrypted as
    /// the Wii's setting.txt is; nil for a serial the runtime would refuse.
    static func encodeSettings(serial: String) -> [UInt8]? {
        guard (1...9).contains(serial.count), serial.allSatisfy({ ("0"..."9").contains($0) }),
              serial.contains(where: { $0 != "0" }) else { return nil }
        var bytes = [UInt8](repeating: 0, count: 256)
        var position = 0
        var key: UInt32 = 0x73B5_DBFA
        func writeByte(_ value: UInt8) {
            bytes[position] = value ^ UInt8(key & 0xFF)
            position += 1
            key = key << 1 | key >> 31
        }
        let lines = ["AREA=EUR\r\n", "MODEL=RVL-001(EUR)\r\n", "DVD=0\r\n", "MPCH=0x7FFE\r\n", "CODE=LEH\r\n",
                     "SERNO=\(serial)\r\n", "VIDEO=PAL\r\n", "GAME=EU\r\n"]
        for line in lines {
            let characters = Array(line.utf8)
            while true {
                guard position + characters.count <= bytes.count else { return nil }
                let start = position
                let savedKey = key
                var hasNull = false
                for value in characters {
                    writeByte(value)
                    hasNull = hasNull || bytes[position - 1] == 0
                }
                if !hasNull { break }
                // Nintendo stops at an encoded NUL. Dolphin inserts an extra LF before this
                // line and retries with the shifted encryption key.
                position = start
                key = savedKey
                writeByte(0x0A)
            }
        }
        // The unused tail stays raw zero, as in Dolphin.
        return bytes
    }
}
