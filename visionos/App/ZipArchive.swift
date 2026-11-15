// SPDX-License-Identifier: GPL-3.0-or-later

import Compression
import Foundation

/// A zip file on disk, read through its central directory: what the Retro Rewind server
/// publishes (RetroRewindPack), which Foundation has no reader for. Entries are stored or
/// deflated, as every zip tool writes them; ZIP64 sizes and offsets are understood. Entries
/// stream to disk in chunks, so a 2 GB pack never needs to be in memory.
struct ZipArchive {
    struct Entry {
        let name: String
        let isDirectory: Bool
        let method: UInt16
        let compressedSize: UInt64
        let uncompressedSize: UInt64
        let localHeaderOffset: UInt64
    }

    enum ZipError: LocalizedError {
        case notAZip
        case truncated
        case unsupportedMethod(UInt16, String)
        case corrupt(String)

        var errorDescription: String? {
            switch self {
            case .notAZip: return "The download is not a zip file."
            case .truncated: return "The download is incomplete."
            case .unsupportedMethod(let method, let name): return "\(name) uses an unsupported compression (\(method))."
            case .corrupt(let name): return "\(name) is damaged in the download."
            }
        }
    }

    private static let endOfCentralDirectorySignature: UInt32 = 0x0605_4b50
    private static let zip64LocatorSignature: UInt32 = 0x0706_4b50
    private static let zip64EndOfCentralDirectorySignature: UInt32 = 0x0606_4b50
    private static let centralHeaderSignature: UInt32 = 0x0201_4b50
    private static let localHeaderSignature: UInt32 = 0x0403_4b50
    private static let chunkSize = 1 << 20

    let url: URL
    let entries: [Entry]

    init(url: URL) throws {
        self.url = url
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        let size = try file.seekToEnd()

        // The end record sits in the last 64 KiB (its comment can push it back that far).
        let tailLength = min(size, UInt64(22 + 65535))
        try file.seek(toOffset: size - tailLength)
        let tail = try file.readToEnd() ?? Data()
        guard let endIndex = ZipArchive.findEndRecord(in: tail) else { throw ZipError.notAZip }
        var entryCount = UInt64(tail.readUInt16(at: endIndex + 10))
        var directoryOffset = UInt64(tail.readUInt32(at: endIndex + 16))
        let directorySize = UInt64(tail.readUInt32(at: endIndex + 12))

        // ZIP64: a locator just before the end record points at the 64-bit end record.
        if entryCount == 0xFFFF || directoryOffset == 0xFFFF_FFFF || directorySize == 0xFFFF_FFFF,
           endIndex >= 20, tail.readUInt32(at: endIndex - 20) == ZipArchive.zip64LocatorSignature {
            let zip64Offset = tail.readUInt64(at: endIndex - 20 + 8)
            try file.seek(toOffset: zip64Offset)
            let record = try file.read(upToCount: 56) ?? Data()
            guard record.count == 56, record.readUInt32(at: 0) == ZipArchive.zip64EndOfCentralDirectorySignature else {
                throw ZipError.corrupt("central directory")
            }
            entryCount = record.readUInt64(at: 32)
            directoryOffset = record.readUInt64(at: 48)
        }

        try file.seek(toOffset: directoryOffset)
        guard let directory = try file.read(upToCount: Int(min(size - directoryOffset, UInt64(Int.max)))) else {
            throw ZipError.truncated
        }
        var entries: [Entry] = []
        entries.reserveCapacity(Int(min(entryCount, 1 << 20)))
        var cursor = 0
        for _ in 0..<entryCount {
            guard cursor + 46 <= directory.count, directory.readUInt32(at: cursor) == ZipArchive.centralHeaderSignature else {
                throw ZipError.corrupt("central directory")
            }
            let flags = directory.readUInt16(at: cursor + 8)
            let method = directory.readUInt16(at: cursor + 10)
            var compressed = UInt64(directory.readUInt32(at: cursor + 20))
            var uncompressed = UInt64(directory.readUInt32(at: cursor + 24))
            let nameLength = Int(directory.readUInt16(at: cursor + 28))
            let extraLength = Int(directory.readUInt16(at: cursor + 30))
            let commentLength = Int(directory.readUInt16(at: cursor + 32))
            var localOffset = UInt64(directory.readUInt32(at: cursor + 42))
            let nameStart = cursor + 46
            guard nameStart + nameLength + extraLength + commentLength <= directory.count else { throw ZipError.truncated }
            let nameData = directory.subdata(in: nameStart..<nameStart + nameLength)
            // Bit 11 marks UTF-8 names; older tools wrote the platform's code page, which for
            // the ASCII names these packs use reads the same either way.
            let name = String(data: nameData, encoding: flags & 0x800 != 0 ? .utf8 : .isoLatin1)
                ?? String(decoding: nameData, as: UTF8.self)

            // The ZIP64 extra field carries whichever of the sizes and offset overflowed, in order.
            var extraCursor = nameStart + nameLength
            let extraEnd = extraCursor + extraLength
            while extraCursor + 4 <= extraEnd {
                let id = directory.readUInt16(at: extraCursor)
                let length = Int(directory.readUInt16(at: extraCursor + 2))
                if id == 0x0001 {
                    var field = extraCursor + 4
                    let fieldEnd = min(field + length, extraEnd)
                    if uncompressed == 0xFFFF_FFFF, field + 8 <= fieldEnd { uncompressed = directory.readUInt64(at: field); field += 8 }
                    if compressed == 0xFFFF_FFFF, field + 8 <= fieldEnd { compressed = directory.readUInt64(at: field); field += 8 }
                    if localOffset == 0xFFFF_FFFF, field + 8 <= fieldEnd { localOffset = directory.readUInt64(at: field) }
                }
                extraCursor += 4 + length
            }
            entries.append(Entry(name: name, isDirectory: name.hasSuffix("/"), method: method,
                                 compressedSize: compressed, uncompressedSize: uncompressed, localHeaderOffset: localOffset))
            cursor = nameStart + nameLength + extraLength + commentLength
        }
        self.entries = entries
    }

    private static func findEndRecord(in tail: Data) -> Int? {
        guard tail.count >= 22 else { return nil }
        var index = tail.count - 22
        while index >= 0 {
            if tail.readUInt32(at: index) == endOfCentralDirectorySignature,
               index + 22 + Int(tail.readUInt16(at: index + 20)) == tail.count {
                return index
            }
            index -= 1
        }
        return nil
    }

    /// Writes an entry's contents to `destination`, creating its folders and replacing the file.
    /// `progress` receives the compressed bytes consumed so far; return false from it to stop.
    func extract(_ entry: Entry, to destination: URL, progress: (UInt64) -> Bool = { _ in true }) throws {
        let manager = FileManager.default
        try manager.createDirectory(at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        try file.seek(toOffset: entry.localHeaderOffset)
        guard let header = try file.read(upToCount: 30), header.count == 30,
              header.readUInt32(at: 0) == ZipArchive.localHeaderSignature else {
            throw ZipError.corrupt(entry.name)
        }
        // The local header repeats the name and carries its own extra field; the sizes come
        // from the central directory, which is right even when the local ones were deferred.
        let dataStart = entry.localHeaderOffset + 30
            + UInt64(header.readUInt16(at: 26)) + UInt64(header.readUInt16(at: 28))
        try file.seek(toOffset: dataStart)

        if manager.fileExists(atPath: destination.path) {
            try manager.removeItem(at: destination)
        }
        guard manager.createFile(atPath: destination.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: destination.path])
        }
        let output = try FileHandle(forWritingTo: destination)
        defer { try? output.close() }

        switch entry.method {
        case 0:
            try copyStored(from: file, to: output, count: entry.compressedSize, name: entry.name, progress: progress)
        case 8:
            try inflate(from: file, to: output, count: entry.compressedSize, expected: entry.uncompressedSize,
                        name: entry.name, progress: progress)
        default:
            throw ZipError.unsupportedMethod(entry.method, entry.name)
        }
    }

    private func copyStored(from input: FileHandle, to output: FileHandle, count: UInt64, name: String,
                            progress: (UInt64) -> Bool) throws {
        var remaining = count
        var consumed: UInt64 = 0
        while remaining > 0 {
            let want = Int(min(remaining, UInt64(ZipArchive.chunkSize)))
            guard let chunk = try input.read(upToCount: want), !chunk.isEmpty else { throw ZipError.truncated }
            try output.write(contentsOf: chunk)
            remaining -= UInt64(chunk.count)
            consumed += UInt64(chunk.count)
            guard progress(consumed) else { throw CancellationError() }
        }
    }

    /// Raw deflate, which is what COMPRESSION_ZLIB decodes (no zlib header, as in zip files).
    private func inflate(from input: FileHandle, to output: FileHandle, count: UInt64, expected: UInt64, name: String,
                         progress: (UInt64) -> Bool) throws {
        let streamPointer = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer { streamPointer.deallocate() }
        guard compression_stream_init(streamPointer, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
            throw ZipError.corrupt(name)
        }
        defer { compression_stream_destroy(streamPointer) }

        let outputBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: ZipArchive.chunkSize)
        defer { outputBuffer.deallocate() }
        var remaining = count
        var consumed: UInt64 = 0
        var produced: UInt64 = 0
        var pending = Data()
        var finished = false
        while !finished {
            if pending.isEmpty, remaining > 0 {
                let want = Int(min(remaining, UInt64(ZipArchive.chunkSize)))
                guard let chunk = try input.read(upToCount: want), !chunk.isEmpty else { throw ZipError.truncated }
                remaining -= UInt64(chunk.count)
                consumed += UInt64(chunk.count)
                pending = chunk
                guard progress(consumed) else { throw CancellationError() }
            }
            let flags = remaining == 0 ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0
            let (status, used): (compression_status, Int) = try pending.withUnsafeBytes { bytes in
                streamPointer.pointee.src_ptr = bytes.bindMemory(to: UInt8.self).baseAddress ?? UnsafePointer(outputBuffer)
                streamPointer.pointee.src_size = bytes.count
                streamPointer.pointee.dst_ptr = outputBuffer
                streamPointer.pointee.dst_size = ZipArchive.chunkSize
                let status = compression_stream_process(streamPointer, flags)
                let written = ZipArchive.chunkSize - streamPointer.pointee.dst_size
                if written > 0 {
                    try output.write(contentsOf: Data(bytesNoCopy: outputBuffer, count: written, deallocator: .none))
                    produced += UInt64(written)
                }
                return (status, bytes.count - streamPointer.pointee.src_size)
            }
            pending = used == pending.count ? Data() : pending.subdata(in: used..<pending.count)
            switch status {
            case COMPRESSION_STATUS_END: finished = true
            case COMPRESSION_STATUS_OK: break
            default: throw ZipError.corrupt(name)
            }
        }
        guard produced == expected else { throw ZipError.corrupt(name) }
    }
}

private extension Data {
    func readUInt16(at offset: Int) -> UInt16 {
        UInt16(self[startIndex + offset]) | UInt16(self[startIndex + offset + 1]) << 8
    }

    func readUInt32(at offset: Int) -> UInt32 {
        UInt32(readUInt16(at: offset)) | UInt32(readUInt16(at: offset + 2)) << 16
    }

    func readUInt64(at offset: Int) -> UInt64 {
        UInt64(readUInt32(at: offset)) | UInt64(readUInt32(at: offset + 4)) << 32
    }
}
