// SPDX-License-Identifier: GPL-3.0-or-later

import Compression
import Foundation

/// A zip file made in memory, the counterpart of ZipArchive for the few megabytes a profile
/// export holds (ProfileTransfer). Each entry is deflated with Compression's raw DEFLATE, or
/// stored when that does not make it smaller, under a UTF-8 name, so the Files app, Finder and
/// every zip tool open it.
struct ZipWriter {
    private var body = Data()
    private var directory = Data()
    private var count = 0
    private let time: UInt16
    private let date: UInt16

    init(date now: Date = Date()) {
        // MS-DOS time and date, in local time as zip tools show them.
        let parts = Calendar.current.dateComponents([.year, .month, .day, .hour, .minute, .second], from: now)
        time = UInt16((parts.hour ?? 0) << 11 | (parts.minute ?? 0) << 5 | (parts.second ?? 0) / 2)
        date = UInt16(max((parts.year ?? 1980) - 1980, 0) << 9 | (parts.month ?? 1) << 5 | (parts.day ?? 1))
    }

    /// Adds `data` as the file `name`, a path with forward slashes.
    mutating func add(_ name: String, _ data: Data) throws {
        let nameBytes = Data(name.utf8)
        let crc = ZipWriter.crc32(data)
        var method: UInt16 = 8
        var stored = ZipWriter.deflate(data)
        if stored == nil || stored!.count >= data.count {
            method = 0
            stored = data
        }
        guard let contents = stored, data.count < 0xFFFF_FFFF, body.count < 0xFFFF_FFFF, count < 0xFFFF else {
            throw ZipArchive.ZipError.corrupt(name)
        }
        let offset = UInt32(body.count)

        var local = Data()
        local.appendUInt32(0x0403_4b50)
        local.appendUInt16(20)
        local.appendUInt16(0x0800)
        local.appendUInt16(method)
        local.appendUInt16(time)
        local.appendUInt16(date)
        local.appendUInt32(crc)
        local.appendUInt32(UInt32(contents.count))
        local.appendUInt32(UInt32(data.count))
        local.appendUInt16(UInt16(nameBytes.count))
        local.appendUInt16(0)
        body.append(local)
        body.append(nameBytes)
        body.append(contents)

        directory.appendUInt32(0x0201_4b50)
        directory.appendUInt16(20)
        directory.appendUInt16(20)
        directory.appendUInt16(0x0800)
        directory.appendUInt16(method)
        directory.appendUInt16(time)
        directory.appendUInt16(date)
        directory.appendUInt32(crc)
        directory.appendUInt32(UInt32(contents.count))
        directory.appendUInt32(UInt32(data.count))
        directory.appendUInt16(UInt16(nameBytes.count))
        directory.appendUInt16(0)
        directory.appendUInt16(0)
        directory.appendUInt16(0)
        directory.appendUInt16(0)
        directory.appendUInt32(0)
        directory.appendUInt32(offset)
        directory.append(nameBytes)
        count += 1
    }

    /// The finished archive: the entries, their central directory and its end record.
    func finish() -> Data {
        var archive = body
        archive.append(directory)
        archive.appendUInt32(0x0605_4b50)
        archive.appendUInt16(0)
        archive.appendUInt16(0)
        archive.appendUInt16(UInt16(count))
        archive.appendUInt16(UInt16(count))
        archive.appendUInt32(UInt32(directory.count))
        archive.appendUInt32(UInt32(body.count))
        archive.appendUInt16(0)
        return archive
    }

    /// Raw DEFLATE (Compression's COMPRESSION_ZLIB has no zlib header), or nil when it does not fit.
    private static func deflate(_ data: Data) -> Data? {
        guard !data.isEmpty else { return nil }
        let capacity = data.count + 1024
        var output = Data(count: capacity)
        let written = output.withUnsafeMutableBytes { destination in
            data.withUnsafeBytes { source in
                compression_encode_buffer(destination.bindMemory(to: UInt8.self).baseAddress!, capacity,
                                          source.bindMemory(to: UInt8.self).baseAddress!, data.count,
                                          nil, COMPRESSION_ZLIB)
            }
        }
        guard written > 0 else { return nil }
        output.count = written
        return output
    }

    private static let crcTable: [UInt32] = (0..<256).map { index in
        (0..<8).reduce(UInt32(index)) { crc, _ in crc & 1 != 0 ? 0xEDB8_8320 ^ (crc >> 1) : crc >> 1 }
    }

    /// CRC-32 (IEEE), as zip stores it.
    static func crc32(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        data.withUnsafeBytes { bytes in
            for byte in bytes {
                crc = crcTable[Int((crc ^ UInt32(byte)) & 0xFF)] ^ (crc >> 8)
            }
        }
        return crc ^ 0xFFFF_FFFF
    }
}

private extension Data {
    mutating func appendUInt16(_ value: UInt16) {
        append(UInt8(value & 0xFF))
        append(UInt8(value >> 8))
    }

    mutating func appendUInt32(_ value: UInt32) {
        appendUInt16(UInt16(value & 0xFFFF))
        appendUInt16(UInt16(value >> 16))
    }
}
