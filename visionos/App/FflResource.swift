// SPDX-License-Identifier: GPL-3.0-or-later

import Compression
import Foundation

/// FFL's Mii part resource (FFLResHigh.dat): the head shapes and part textures Mii pictures are
/// made of. A port of the Quest launcher's FflResource.kt, itself a port of the PC launcher's
/// ManagedFflResourceArchive and of NativeMiiRenderer's shape and texture decoding. The file is
/// Nintendo's, so the app never ships it: the player downloads the same copy as the PC does.
///
/// Everything read from the file is immutable once parsed. Decoded parts are cached behind a lock,
/// so one resource serves pictures drawn on several threads at once.
final class FflResource: @unchecked Sendable {
    private struct PartInfo {
        let position: Int
        let size: Int
        let compressedSize: Int
        let windowBits: Int
        let strategy: Int
    }

    private let bytes: Data
    private let bigEndian: Bool
    private let textureParts: [[PartInfo]]
    private let shapeParts: [[PartInfo]]
    /// AFL's resources (Miitomo's, which the download is) store linear textures and scale glasses differently.
    let linearTextures: Bool
    private let halfFloatLayout: Bool

    /// Guards the two caches below; parts are decoded outside it, as the Kotlin decodes them
    /// outside its synchronized blocks, so two threads may decode the same part once each.
    private let lock = NSLock()
    /// Decoded parts by type and index. A nil value records a part that is empty or cannot be used.
    private var shapes: [Int64: FflShape?] = [:]
    private var textures: [Int64: FflTexture?] = [:]

    private init(bytes: Data, bigEndian: Bool, textureParts: [[PartInfo]], shapeParts: [[PartInfo]],
                 linearTextures: Bool, halfFloatLayout: Bool) {
        self.bytes = bytes
        self.bigEndian = bigEndian
        self.textureParts = textureParts
        self.shapeParts = shapeParts
        self.linearTextures = linearTextures
        self.halfFloatLayout = halfFloatLayout
    }

    /// A shape, or nil when the part is empty or cannot be used, which drawing skips.
    func shape(_ partType: Int, _ index: Int) -> FflShape? {
        let key = Int64(partType) << 32 | Int64(UInt32(truncatingIfNeeded: index))
        lock.lock()
        if let cached = shapes[key] {
            lock.unlock()
            return cached
        }
        lock.unlock()
        // Any failure, from reading the part to decoding it, leaves the part out.
        let shape = try? decodeShape(loadPart(shapeParts, partType, index), partType)
        lock.lock()
        shapes.updateValue(shape, forKey: key)
        lock.unlock()
        return shape
    }

    /// A texture, or nil for a negative index or an empty part (which drawing skips). A part that
    /// cannot be read throws, and so does the picture, as on the PC.
    func texture(_ partType: Int, _ index: Int) throws -> FflTexture? {
        if index < 0 { return nil }
        let key = Int64(partType) << 32 | Int64(index)
        lock.lock()
        if let cached = textures[key] {
            lock.unlock()
            return cached
        }
        lock.unlock()
        let data = try loadPart(textureParts, partType, index)
        let texture = data.count <= 12 ? nil : try decodeTexture(data)
        lock.lock()
        textures.updateValue(texture, forKey: key)
        lock.unlock()
        return texture
    }

    private func loadPart(_ table: [[PartInfo]], _ partType: Int, _ index: Int) throws -> [UInt8] {
        guard table.indices.contains(partType) else { throw MiiError("Part type \(partType) is out of range.") }
        let parts = table[partType]
        guard parts.indices.contains(index) else {
            throw MiiError("Part index \(index) is out of range for part type \(partType).")
        }
        let info = parts[index]
        if info.size <= 0 { return [] }
        if info.position < 0 || info.position >= bytes.count { throw MiiError("dataPos out of range: \(info.position)") }
        if info.strategy == 5 {
            if info.position + info.size > bytes.count { throw MiiError("Uncompressed part range is out of file bounds.") }
            return bytes.withUnsafeBytes { Array($0[info.position..<info.position + info.size]) }
        }
        if info.compressedSize <= 0 { throw MiiError("Compressed part has empty compressedSize.") }
        if info.position + info.compressedSize > bytes.count { throw MiiError("Compressed part range is out of file bounds.") }
        var decoded: [UInt8] = try bytes.withUnsafeBytes { raw in
            let compressed = UnsafeRawBufferPointer(rebasing: raw[info.position..<info.position + info.compressedSize])
            switch info.windowBits {
            case 8...15:
                return try FflResource.gunzip(compressed, sizeHint: info.size)
            case 16:
                do {
                    return try FflResource.inflate(compressed, sizeHint: info.size)
                } catch {
                    return try FflResource.gunzip(compressed, sizeHint: info.size)
                }
            default:
                return try FflResource.inflate(compressed, sizeHint: info.size)
            }
        }
        // Kotlin's copyOf: cut to the declared size, or padded to it with zeros.
        if decoded.count > info.size {
            decoded.removeLast(decoded.count - info.size)
        } else if decoded.count < info.size {
            decoded.append(contentsOf: repeatElement(0, count: info.size - decoded.count))
        }
        return decoded
    }

    private func decodeShape(_ data: [UInt8], _ partType: Int) -> FflShape? {
        if data.count < 0x90 { return nil }
        let positionOf = (0..<6).map { Int(Int32(bitPattern: u32(data, $0 * 4))) }
        let sizeOf = (0..<6).map { Int(Int32(bitPattern: u32(data, 24 + $0 * 4))) }
        for i in 0..<6 {
            if sizeOf[i] == 0 { continue }
            // An index "size" counts u16 indices; the others are byte sizes.
            let byteSize = i == 5 ? sizeOf[i] * 2 : Int(UInt32(truncatingIfNeeded: sizeOf[i]))
            if positionOf[i] < 0 || positionOf[i] >= data.count || positionOf[i] + byteSize > data.count { return nil }
        }
        let positionSize = sizeOf[0]
        if positionSize <= 0 { return nil }
        var positionStride = halfFloatLayout ? 6 : 16
        if !halfFloatLayout && positionSize % positionStride != 0 && positionSize % 12 == 0 { positionStride = 12 }
        let count = positionSize / positionStride
        if count <= 0 { return nil }

        var positions = [Float](repeating: 0, count: count * 3)
        var texcoords = [Float](repeating: 0, count: count * 2)
        var normals = (0..<count * 3).map { $0 % 3 == 2 ? Float(1) : 0 }
        var tangents = [Float](repeating: 0, count: count * 3)
        var parameters = (0..<count * 4).map { $0 % 4 == 2 ? Float(0) : 1 }

        for i in 0..<count {
            let o = positionOf[0] + i * positionStride
            for axis in 0..<3 {
                positions[i * 3 + axis] = halfFloatLayout ? FflResource.half(u16(data, o + axis * 2)) : f32(data, o + axis * 4)
            }
        }
        if sizeOf[2] > 0 {
            let stride = halfFloatLayout ? 4 : 8
            for i in 0..<min(count, sizeOf[2] / stride) {
                let o = positionOf[2] + i * stride
                texcoords[i * 2] = halfFloatLayout ? FflResource.half(u16(data, o)) : f32(data, o)
                texcoords[i * 2 + 1] = halfFloatLayout ? FflResource.half(u16(data, o + 2)) : f32(data, o + 4)
            }
        }
        if sizeOf[1] > 0 {
            for i in 0..<min(count, sizeOf[1] / 4) {
                let o = positionOf[1] + i * 4
                let normal = halfFloatLayout
                    ? FflResource.snorm8Normal(data, o, zeroFallback: false)
                    : FflResource.decodeInt2101010(Int(Int32(bitPattern: u32(data, o))))
                normals[i * 3] = normal.0
                normals[i * 3 + 1] = normal.1
                normals[i * 3 + 2] = normal.2
            }
        }
        if sizeOf[3] > 0 {
            for i in 0..<min(count, sizeOf[3] / 4) {
                let tangent = FflResource.snorm8Normal(data, positionOf[3] + i * 4, zeroFallback: true)
                tangents[i * 3] = tangent.0
                tangents[i * 3 + 1] = tangent.1
                tangents[i * 3 + 2] = tangent.2
            }
        }
        if sizeOf[4] > 0 {
            for i in 0..<min(count, sizeOf[4] / 4) {
                for c in 0..<4 { parameters[i * 4 + c] = Float(data[positionOf[4] + i * 4 + c]) / 255 }
            }
        }
        if sizeOf[5] <= 0 { return nil }
        let indices = (0..<sizeOf[5]).map { u16(data, positionOf[5] + $0 * 2) }
        if indices.contains(where: { $0 >= count }) { return nil }

        var translates: [[Float]]?
        if partType == FflResource.shapeFaceline && data.count >= 0x48 + 0x24 {
            translates = (0..<3).map { t in (0..<3).map { axis in f32(data, 0x48 + t * 12 + axis * 4) } }
        }
        return FflShape(positions: positions, texcoords: texcoords, normals: normals, tangents: tangents,
                        parameters: parameters, indices: indices, translates: translates)
    }

    private func decodeTexture(_ data: [UInt8]) throws -> FflTexture {
        let footer = data.count - 12
        let width = u16(data, footer + 4)
        let height = u16(data, footer + 6)
        let format = Int(data[footer + 9])
        if width == 0 || height == 0 { throw MiiError("Texture part has invalid dimensions.") }
        let stride: Int
        switch format {
        case FflTexture.r8: stride = 1
        case FflTexture.rg8: stride = 2
        case FflTexture.rgba8: stride = 4
        default: throw MiiError("Unsupported texture format \(format).")
        }
        let imageSize = width * height * stride
        if imageSize > footer { throw MiiError("Texture image payload is truncated.") }
        // Mipmaps follow the base level; the PC samples only that.
        return FflTexture(width: width, height: height, format: format, pixels: Array(data[0..<imageSize]))
    }

    private func u16(_ data: [UInt8], _ offset: Int) -> Int {
        let a = Int(data[offset])
        let b = Int(data[offset + 1])
        return bigEndian ? (a << 8) | b : (b << 8) | a
    }

    private func u32(_ data: [UInt8], _ offset: Int) -> UInt32 {
        let high = UInt32(u16(data, bigEndian ? offset : offset + 2))
        let low = UInt32(u16(data, bigEndian ? offset + 2 : offset))
        return (high << 16) | low
    }

    private func f32(_ data: [UInt8], _ offset: Int) -> Float { Float(bitPattern: u32(data, offset)) }

    /// The SHA-256 of AFLResHigh_2_3.dat, the file every launcher installs as FFLResHigh.dat.
    static let sha256 = "4a4be71d75162c20b48720ef89cd3d4e6cd4e8e21bcbc2aed63ee08d96de7722"

    static let shapeBeard = 0
    static let shapeHat = 1
    static let shapeFaceline = 3
    static let shapeGlass = 4
    static let shapeMask = 5
    static let shapeNoseline = 6
    static let shapeNose = 7
    static let shapeHair = 8
    static let shapeForehead = 10

    static let textureBeard = 0
    static let textureCap = 1
    static let textureEye = 2
    static let textureEyebrow = 3
    static let textureFaceline = 4
    static let textureMakeup = 5
    static let textureGlass = 6
    static let textureMole = 7
    static let textureMouth = 8
    static let textureMustache = 9
    static let textureNoseline = 10

    private static let magic: UInt32 = 0x4646_5241
    private static let version: UInt32 = 0x0007_0000
    private static let headerSize = 0x4A00
    private static let textureHeader = 0x14
    private static let expandedAfl: UInt32 = 0x0239_D5E0
    private static let expandedAfl23: UInt32 = 0x0250_2DE0

    private static let textureCountsFfl = [3, 132, 62, 24, 12, 12, 9, 2, 37, 6, 18]
    private static let textureCountsAfl = [3, 132, 80, 28, 12, 12, 9, 2, 52, 6, 18]
    private static let textureCountsAfl23 = [3, 132, 80, 28, 12, 12, 20, 2, 52, 6, 18]
    private static let shapeCounts = [4, 132, 132, 12, 1, 12, 18, 18, 132, 132, 132, 132]

    static func load(_ url: URL) throws -> FflResource {
        try parse(Data(contentsOf: url, options: .mappedIfSafe))
    }

    static func parse(_ data: Data) throws -> FflResource {
        if data.count < headerSize { throw MiiError("FFL resource file is too small (\(data.count) bytes).") }
        return try data.withUnsafeBytes { raw -> FflResource in
            let bigEndian: Bool
            if readU32(raw, 0, true) == magic {
                bigEndian = true
            } else if readU32(raw, 0, false) == magic {
                bigEndian = false
            } else {
                throw MiiError("FFL resource has an invalid magic.")
            }
            let version = readU32(raw, 4, bigEndian)
            if version != self.version { throw MiiError(String(format: "FFL resource has unsupported version 0x%08X.", version)) }
            let expanded = readU32(raw, 12, bigEndian)
            let halfFloat = readU32(raw, 16, bigEndian) == 0x841F_10A7
            let hint = expanded >> 29
            let expandedSize = expanded & 0x1FFF_FFFF
            let afl23 = hint == 3 || expandedSize == expandedAfl23
            let afl = afl23 || hint == 2 || expandedSize == expandedAfl
            let textureCounts = afl23 ? textureCountsAfl23 : afl ? textureCountsAfl : textureCountsFfl
            let textureTable = textureHeader + textureCounts.count * 4
            let textureParts = try readParts(raw, bigEndian, textureTable, textureCounts)
            let shapeHeader = textureHeader + textureCounts.count * 4 + textureCounts.reduce(0, +) * 16
            let shapeParts = try readParts(raw, bigEndian, shapeHeader + shapeCounts.count * 4, shapeCounts)
            return FflResource(bytes: data, bigEndian: bigEndian, textureParts: textureParts, shapeParts: shapeParts,
                               linearTextures: afl, halfFloatLayout: halfFloat)
        }
    }

    private static func readParts(_ bytes: UnsafeRawBufferPointer, _ bigEndian: Bool, _ start: Int,
                                  _ counts: [Int]) throws -> [[PartInfo]] {
        var offset = start
        return try counts.map { count in
            try (0..<count).map { _ in
                if offset + 16 > bytes.count { throw MiiError("Resource header is truncated while parsing parts info.") }
                let info = PartInfo(
                    position: Int(Int32(bitPattern: readU32(bytes, offset, bigEndian))),
                    size: Int(Int32(bitPattern: readU32(bytes, offset + 4, bigEndian))),
                    compressedSize: Int(Int32(bitPattern: readU32(bytes, offset + 8, bigEndian))),
                    windowBits: Int(bytes[offset + 13]),
                    strategy: Int(bytes[offset + 15]))
                offset += 16
                return info
            }
        }
    }

    private static func readU32(_ bytes: UnsafeRawBufferPointer, _ offset: Int, _ bigEndian: Bool) -> UInt32 {
        var value: UInt32 = 0
        for i in 0..<4 {
            value = (value << 8) | UInt32(bytes[offset + (bigEndian ? i : 3 - i)])
        }
        return value
    }

    // MARK: Decompression

    /// A zlib stream (RFC 1950), as java.util.zip.Inflater reads it in the Quest launcher: its
    /// two-byte header is checked with zlib's own messages, then Compression's COMPRESSION_ZLIB,
    /// which is raw DEFLATE, decodes the rest. The Adler-32 trailer is not checked, because
    /// Compression reads past the end of the DEFLATE data and so cannot say where the trailer is.
    private static func inflate(_ compressed: UnsafeRawBufferPointer, sizeHint: Int) throws -> [UInt8] {
        // Inflater waits for more input on a header it does not have whole, which the Kotlin reports so.
        if compressed.count < 2 { throw MiiError("Truncated zlib part.") }
        let cmf = Int(compressed[0])
        let flg = Int(compressed[1])
        if ((cmf << 8) | flg) % 31 != 0 { throw MiiError("incorrect header check") }
        if cmf & 0x0F != 8 { throw MiiError("unknown compression method") }
        if (cmf >> 4) + 8 > 15 { throw MiiError("invalid window size") }
        // A preset dictionary, which FFL never uses: Inflater asks for one and the Kotlin gives up
        // with the same message as for a truncated part.
        if flg & 0x20 != 0 { throw MiiError("Truncated zlib part.") }
        return try inflateRaw(UnsafeRawBufferPointer(rebasing: compressed[2...]), sizeHint: sizeHint,
                              truncated: "Truncated zlib part.")
    }

    /// A gzip stream (RFC 1952), as java.util.zip.GZIPInputStream reads it: the header with its
    /// optional extra field, name, comment and header CRC (which is skipped, not checked), then
    /// raw DEFLATE. As with zlib the trailer's CRC-32 and size are not checked, and only the first
    /// member is read, where GZIPInputStream would go on to any members concatenated after it.
    private static func gunzip(_ compressed: UnsafeRawBufferPointer, sizeHint: Int) throws -> [UInt8] {
        var at = 0
        func byte() throws -> Int {
            if at >= compressed.count { throw MiiError("Unexpected end of gzip part header.") }
            at += 1
            return Int(compressed[at - 1])
        }
        func u16() throws -> Int {
            let low = try byte()
            let high = try byte()
            return low | (high << 8)
        }
        if try u16() != 0x8B1F { throw MiiError("Not in GZIP format") }
        if try byte() != 8 { throw MiiError("Unsupported compression method") }
        let flags = try byte()
        for _ in 0..<6 { _ = try byte() }
        if flags & 0x04 != 0 {
            let length = try u16()
            for _ in 0..<length { _ = try byte() }
        }
        if flags & 0x08 != 0 { while try byte() != 0 {} }
        if flags & 0x10 != 0 { while try byte() != 0 {} }
        if flags & 0x02 != 0 { _ = try u16() }
        return try inflateRaw(UnsafeRawBufferPointer(rebasing: compressed[at...]), sizeHint: sizeHint,
                              truncated: "Unexpected end of ZLIB input stream")
    }

    /// Raw DEFLATE (RFC 1951) with all of its input at hand, decoded to its end however long that
    /// is. A stream that stops before its last block throws `truncated`. The input is never
    /// finalized: that way Compression waits for more of a truncated stream, which the loop sees as
    /// a pass without progress, where finalizing would report it as damaged data.
    private static func inflateRaw(_ input: UnsafeRawBufferPointer, sizeHint: Int, truncated: String) throws -> [UInt8] {
        let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer { stream.deallocate() }
        guard compression_stream_init(stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
            throw MiiError("Could not start decompressing a Mii part.")
        }
        defer { compression_stream_destroy(stream) }
        // One chunk holds the whole part when its declared size is right.
        let chunk = max(sizeHint, 4096) + 1
        let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: chunk)
        defer { buffer.deallocate() }
        var output: [UInt8] = []
        output.reserveCapacity(sizeHint)
        let source = input.bindMemory(to: UInt8.self)
        stream.pointee.src_ptr = source.baseAddress ?? UnsafePointer(buffer)
        stream.pointee.src_size = source.count
        while true {
            stream.pointee.dst_ptr = buffer
            stream.pointee.dst_size = chunk
            let status = compression_stream_process(stream, 0)
            let written = chunk - stream.pointee.dst_size
            output.append(contentsOf: UnsafeBufferPointer(start: buffer, count: written))
            switch status {
            case COMPRESSION_STATUS_END:
                return output
            case COMPRESSION_STATUS_OK:
                // All the input was given at once, so a pass that produces nothing has run out of
                // it before the last block: Inflater's needsInput() in the Kotlin.
                if written == 0 { throw MiiError(truncated) }
            default:
                throw MiiError("Invalid ZLIB data format")
            }
        }
    }

    // MARK: Vertex formats

    private static func half(_ bits: Int) -> Float {
        let exponent = (bits >> 10) & 0x1F
        let mantissa = bits & 0x3FF
        let magnitude: Float
        switch exponent {
        case 0: magnitude = Float(mantissa) * (1 / Float(1 << 24))
        case 0x1F: magnitude = mantissa == 0 ? .infinity : .nan
        default: magnitude = Float(bitPattern: UInt32(((exponent + 112) << 23) | (mantissa << 13)))
        }
        return bits & 0x8000 != 0 ? -magnitude : magnitude
    }

    /// Snorm8 vectors; a zero-length normal becomes +Z and a zero-length tangent stays zero.
    private static func snorm8Normal(_ data: [UInt8], _ offset: Int, zeroFallback: Bool) -> (Float, Float, Float) {
        // Signed bytes, as Kotlin's Byte / Float divides them.
        let x = Float(Int8(bitPattern: data[offset])) / 127
        let y = Float(Int8(bitPattern: data[offset + 1])) / 127
        let z = Float(Int8(bitPattern: data[offset + 2])) / 127
        return normalized(x, y, z, threshold: 1e-8, fallbackZ: zeroFallback ? 0 : 1)
    }

    /// Int2101010 normals: NativeMiiRenderer.DecodeInt2101010.
    static func decodeInt2101010(_ packed: Int) -> (Float, Float, Float) {
        let x = Float(signExtend10(packed)) / 511
        let y = Float(signExtend10(packed >> 10)) / 511
        let z = Float(signExtend10(packed >> 20)) / 511
        return normalized(x, y, z, threshold: 1e-5, fallbackZ: 1)
    }

    private static func signExtend10(_ value: Int) -> Int {
        let bits = value & 0x3FF
        return bits & 0x200 != 0 ? bits - 0x400 : bits
    }

    /// Normalizes, or gives (0, 0, `fallbackZ`) when shorter than the threshold (the Kotlin's
    /// normalizeInto, returning the vector rather than writing it into an array).
    @inline(__always)
    static func normalized(_ x: Float, _ y: Float, _ z: Float, threshold: Float, fallbackZ: Float) -> (Float, Float, Float) {
        let lengthSquared = x * x + y * y + z * z
        if lengthSquared < threshold { return (0, 0, fallbackZ) }
        // The Kotlin's sqrt goes through double precision, which rounds back to this same float.
        let length = lengthSquared.squareRoot()
        return (x / length, y / length, z / length)
    }
}

/// A part texture's base level: one, two or four bytes per pixel.
final class FflTexture: Sendable {
    static let r8 = 0
    static let rg8 = 1
    static let rgba8 = 2

    let width: Int
    let height: Int
    let format: Int
    let pixels: [UInt8]
    let stride: Int

    init(width: Int, height: Int, format: Int, pixels: [UInt8]) {
        self.width = width
        self.height = height
        self.format = format
        self.pixels = pixels
        switch format {
        case FflTexture.r8: stride = 1
        case FflTexture.rg8: stride = 2
        default: stride = 4
        }
    }
}

/// A decoded shape, three floats per position, normal and tangent, two per texcoord, four per parameter.
final class FflShape: Sendable {
    let positions: [Float]
    let texcoords: [Float]
    let normals: [Float]
    let tangents: [Float]
    let parameters: [Float]
    let indices: [Int]
    /// The faceline's hair, nose and beard anchors.
    let translates: [[Float]]?

    init(positions: [Float], texcoords: [Float], normals: [Float], tangents: [Float], parameters: [Float],
         indices: [Int], translates: [[Float]]?) {
        self.positions = positions
        self.texcoords = texcoords
        self.normals = normals
        self.tangents = tangents
        self.parameters = parameters
        self.indices = indices
        self.translates = translates
    }

    var vertexCount: Int { positions.count / 3 }
}
