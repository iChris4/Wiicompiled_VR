package org.wiicompiled.quest.launcher

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.util.zip.GZIPInputStream
import java.util.zip.Inflater
import kotlin.math.sqrt

/**
 * FFL's Mii part resource (FFLResHigh.dat): the head shapes and part textures Mii pictures are
 * made of. A port of the PC launcher's ManagedFflResourceArchive and of NativeMiiRenderer's shape
 * and texture decoding. The file is Nintendo's, so the app never ships it: the player downloads
 * the same copy as the PC does (see [MiiRenderResource]).
 */
class FflResource private constructor(
    private val bytes: ByteArray,
    private val bigEndian: Boolean,
    private val textureParts: Array<Array<PartInfo>>,
    private val shapeParts: Array<Array<PartInfo>>,
    /** AFL's resources (Miitomo's, which the download is) store linear textures and scale glasses differently. */
    val linearTextures: Boolean,
    private val halfFloatLayout: Boolean,
) {
    private class PartInfo(val position: Int, val size: Int, val compressedSize: Int, val windowBits: Int, val strategy: Int)

    private val shapes = HashMap<Long, Any>()
    private val textures = HashMap<Long, Any>()

    /** A shape, or null when the part is empty or cannot be used, which drawing skips. */
    internal fun shape(partType: Int, index: Int): FflShape? {
        val key = (partType.toLong() shl 32) or (index.toLong() and 0xFFFFFFFFL)
        synchronized(shapes) { shapes[key]?.let { return it as? FflShape } }
        val shape = runCatching { decodeShape(loadPart(shapeParts, partType, index), partType) }.getOrNull()
        synchronized(shapes) { shapes[key] = shape ?: MISSING }
        return shape
    }

    /**
     * A texture, or null for a negative index or an empty part (which drawing skips). A part that
     * cannot be read throws, and so does the picture, as on the PC.
     */
    internal fun texture(partType: Int, index: Int): FflTexture? {
        if (index < 0) return null
        val key = (partType.toLong() shl 32) or index.toLong()
        synchronized(textures) { textures[key]?.let { return it as? FflTexture } }
        val data = loadPart(textureParts, partType, index)
        val texture = if (data.size <= 12) null else decodeTexture(data)
        synchronized(textures) { textures[key] = texture ?: MISSING }
        return texture
    }

    private fun loadPart(table: Array<Array<PartInfo>>, partType: Int, index: Int): ByteArray {
        if (partType !in table.indices) throw IOException("Part type $partType is out of range.")
        val parts = table[partType]
        if (index !in parts.indices) throw IOException("Part index $index is out of range for part type $partType.")
        val info = parts[index]
        if (info.size <= 0) return ByteArray(0)
        if (info.position < 0 || info.position >= bytes.size) throw IOException("dataPos out of range: ${info.position}")
        if (info.strategy == 5) {
            if (info.position + info.size > bytes.size) throw IOException("Uncompressed part range is out of file bounds.")
            return bytes.copyOfRange(info.position, info.position + info.size)
        }
        if (info.compressedSize <= 0) throw IOException("Compressed part has empty compressedSize.")
        if (info.position + info.compressedSize > bytes.size) throw IOException("Compressed part range is out of file bounds.")
        val compressed = bytes.copyOfRange(info.position, info.position + info.compressedSize)
        val decoded = when (info.windowBits) {
            in 8..15 -> gunzip(compressed)
            16 -> runCatching { inflate(compressed) }.getOrElse { gunzip(compressed) }
            else -> inflate(compressed)
        }
        return if (decoded.size == info.size) decoded else decoded.copyOf(info.size)
    }

    private fun decodeShape(data: ByteArray, partType: Int): FflShape? {
        if (data.size < 0x90) return null
        val positionOf = IntArray(6) { u32(data, it * 4).toInt() }
        val sizeOf = IntArray(6) { u32(data, 24 + it * 4).toInt() }
        for (i in 0 until 6) {
            if (sizeOf[i] == 0) continue
            // An index "size" counts u16 indices; the others are byte sizes.
            val byteSize = if (i == 5) sizeOf[i].toLong() * 2 else sizeOf[i].toLong() and 0xFFFFFFFFL
            if (positionOf[i] < 0 || positionOf[i] >= data.size || positionOf[i] + byteSize > data.size) return null
        }
        val positionSize = sizeOf[0]
        if (positionSize <= 0) return null
        var positionStride = if (halfFloatLayout) 6 else 16
        if (!halfFloatLayout && positionSize % positionStride != 0 && positionSize % 12 == 0) positionStride = 12
        val count = positionSize / positionStride
        if (count <= 0) return null

        val positions = FloatArray(count * 3)
        val texcoords = FloatArray(count * 2)
        val normals = FloatArray(count * 3) { if (it % 3 == 2) 1f else 0f }
        val tangents = FloatArray(count * 3)
        val parameters = FloatArray(count * 4) { if (it % 4 == 2) 0f else 1f }

        for (i in 0 until count) {
            val o = positionOf[0] + i * positionStride
            for (axis in 0 until 3) {
                positions[i * 3 + axis] = if (halfFloatLayout) half(u16(data, o + axis * 2)) else f32(data, o + axis * 4)
            }
        }
        if (sizeOf[2] > 0) {
            val stride = if (halfFloatLayout) 4 else 8
            for (i in 0 until minOf(count, sizeOf[2] / stride)) {
                val o = positionOf[2] + i * stride
                texcoords[i * 2] = if (halfFloatLayout) half(u16(data, o)) else f32(data, o)
                texcoords[i * 2 + 1] = if (halfFloatLayout) half(u16(data, o + 2)) else f32(data, o + 4)
            }
        }
        if (sizeOf[1] > 0) {
            for (i in 0 until minOf(count, sizeOf[1] / 4)) {
                val o = positionOf[1] + i * 4
                if (halfFloatLayout) {
                    snorm8Normal(data, o, normals, i * 3, zeroFallback = false)
                } else {
                    decodeInt2101010(u32(data, o).toInt(), normals, i * 3)
                }
            }
        }
        if (sizeOf[3] > 0) {
            for (i in 0 until minOf(count, sizeOf[3] / 4)) snorm8Normal(data, positionOf[3] + i * 4, tangents, i * 3, zeroFallback = true)
        }
        if (sizeOf[4] > 0) {
            for (i in 0 until minOf(count, sizeOf[4] / 4)) {
                for (c in 0 until 4) parameters[i * 4 + c] = (data[positionOf[4] + i * 4 + c].toInt() and 0xFF) / 255f
            }
        }
        if (sizeOf[5] <= 0) return null
        val indices = IntArray(sizeOf[5]) { u16(data, positionOf[5] + it * 2) }
        if (indices.any { it >= count }) return null

        var translates: Array<FloatArray>? = null
        if (partType == SHAPE_FACELINE && data.size >= 0x48 + 0x24) {
            translates = Array(3) { t -> FloatArray(3) { axis -> f32(data, 0x48 + t * 12 + axis * 4) } }
        }
        return FflShape(positions, texcoords, normals, tangents, parameters, indices, translates)
    }

    private fun decodeTexture(data: ByteArray): FflTexture {
        val footer = data.size - 12
        val width = u16(data, footer + 4)
        val height = u16(data, footer + 6)
        val format = data[footer + 9].toInt() and 0xFF
        if (width == 0 || height == 0) throw IOException("Texture part has invalid dimensions.")
        val stride = when (format) {
            FflTexture.R8 -> 1
            FflTexture.RG8 -> 2
            FflTexture.RGBA8 -> 4
            else -> throw IOException("Unsupported texture format $format.")
        }
        val imageSize = width.toLong() * height * stride
        if (imageSize > footer) throw IOException("Texture image payload is truncated.")
        // Mipmaps follow the base level; the PC samples only that.
        return FflTexture(width, height, format, data.copyOf(imageSize.toInt()))
    }

    private fun u16(data: ByteArray, offset: Int): Int {
        val a = data[offset].toInt() and 0xFF
        val b = data[offset + 1].toInt() and 0xFF
        return if (bigEndian) (a shl 8) or b else (b shl 8) or a
    }

    private fun u32(data: ByteArray, offset: Int): Long {
        val high = u16(data, if (bigEndian) offset else offset + 2).toLong()
        val low = u16(data, if (bigEndian) offset + 2 else offset).toLong()
        return (high shl 16) or low
    }

    private fun f32(data: ByteArray, offset: Int): Float = java.lang.Float.intBitsToFloat(u32(data, offset).toInt())

    companion object {
        /** The SHA-256 of AFLResHigh_2_3.dat, the file both launchers install as FFLResHigh.dat. */
        const val SHA256 = "4a4be71d75162c20b48720ef89cd3d4e6cd4e8e21bcbc2aed63ee08d96de7722"

        internal const val SHAPE_BEARD = 0
        internal const val SHAPE_HAT = 1
        internal const val SHAPE_FACELINE = 3
        internal const val SHAPE_GLASS = 4
        internal const val SHAPE_MASK = 5
        internal const val SHAPE_NOSELINE = 6
        internal const val SHAPE_NOSE = 7
        internal const val SHAPE_HAIR = 8
        internal const val SHAPE_FOREHEAD = 10

        internal const val TEXTURE_BEARD = 0
        internal const val TEXTURE_CAP = 1
        internal const val TEXTURE_EYE = 2
        internal const val TEXTURE_EYEBROW = 3
        internal const val TEXTURE_FACELINE = 4
        internal const val TEXTURE_MAKEUP = 5
        internal const val TEXTURE_GLASS = 6
        internal const val TEXTURE_MOLE = 7
        internal const val TEXTURE_MOUTH = 8
        internal const val TEXTURE_MUSTACHE = 9
        internal const val TEXTURE_NOSELINE = 10

        private const val MAGIC = 0x46465241L
        private const val VERSION = 0x00070000L
        private const val HEADER_SIZE = 0x4A00
        private const val TEXTURE_HEADER = 0x14
        private const val EXPANDED_AFL = 0x0239D5E0L
        private const val EXPANDED_AFL_23 = 0x02502DE0L
        private val MISSING = Any()

        private val TEXTURE_COUNTS = intArrayOf(3, 132, 62, 24, 12, 12, 9, 2, 37, 6, 18)
        private val TEXTURE_COUNTS_AFL = intArrayOf(3, 132, 80, 28, 12, 12, 9, 2, 52, 6, 18)
        private val TEXTURE_COUNTS_AFL_23 = intArrayOf(3, 132, 80, 28, 12, 12, 20, 2, 52, 6, 18)
        private val SHAPE_COUNTS = intArrayOf(4, 132, 132, 12, 1, 12, 18, 18, 132, 132, 132, 132)

        @Throws(IOException::class)
        fun load(file: File): FflResource = parse(file.readBytes())

        @Throws(IOException::class)
        fun parse(bytes: ByteArray): FflResource {
            if (bytes.size < HEADER_SIZE) throw IOException("FFL resource file is too small (${bytes.size} bytes).")
            val bigEndian = when (MAGIC) {
                readU32(bytes, 0, true) -> true
                readU32(bytes, 0, false) -> false
                else -> throw IOException("FFL resource has an invalid magic.")
            }
            val version = readU32(bytes, 4, bigEndian)
            if (version != VERSION) throw IOException("FFL resource has unsupported version 0x%08X.".format(version))
            val expanded = readU32(bytes, 12, bigEndian)
            val halfFloat = readU32(bytes, 16, bigEndian) == 0x841F10A7L
            val hint = expanded ushr 29
            val expandedSize = expanded and 0x1FFFFFFF
            val afl23 = hint == 3L || expandedSize == EXPANDED_AFL_23
            val afl = afl23 || hint == 2L || expandedSize == EXPANDED_AFL
            val textureCounts = when {
                afl23 -> TEXTURE_COUNTS_AFL_23
                afl -> TEXTURE_COUNTS_AFL
                else -> TEXTURE_COUNTS
            }
            val textureTable = TEXTURE_HEADER + textureCounts.size * 4
            val textureParts = readParts(bytes, bigEndian, textureTable, textureCounts)
            val shapeHeader = TEXTURE_HEADER + textureCounts.size * 4 + textureCounts.sum() * 16
            val shapeParts = readParts(bytes, bigEndian, shapeHeader + SHAPE_COUNTS.size * 4, SHAPE_COUNTS)
            return FflResource(bytes, bigEndian, textureParts, shapeParts, linearTextures = afl, halfFloatLayout = halfFloat)
        }

        private fun readParts(bytes: ByteArray, bigEndian: Boolean, start: Int, counts: IntArray): Array<Array<PartInfo>> {
            var offset = start
            return Array(counts.size) { type ->
                Array(counts[type]) {
                    if (offset + 16 > bytes.size) throw IOException("Resource header is truncated while parsing parts info.")
                    PartInfo(
                        position = readU32(bytes, offset, bigEndian).toInt(),
                        size = readU32(bytes, offset + 4, bigEndian).toInt(),
                        compressedSize = readU32(bytes, offset + 8, bigEndian).toInt(),
                        windowBits = bytes[offset + 13].toInt() and 0xFF,
                        strategy = bytes[offset + 15].toInt() and 0xFF,
                    ).also { offset += 16 }
                }
            }
        }

        private fun readU32(bytes: ByteArray, offset: Int, bigEndian: Boolean): Long {
            var value = 0L
            for (i in 0 until 4) {
                val byte = bytes[offset + if (bigEndian) i else 3 - i].toLong() and 0xFF
                value = (value shl 8) or byte
            }
            return value
        }

        private fun inflate(compressed: ByteArray): ByteArray {
            val inflater = Inflater()
            try {
                inflater.setInput(compressed)
                val output = ByteArrayOutputStream(compressed.size * 4)
                val buffer = ByteArray(64 * 1024)
                while (!inflater.finished()) {
                    val count = inflater.inflate(buffer)
                    if (count == 0 && (inflater.needsInput() || inflater.needsDictionary())) throw IOException("Truncated zlib part.")
                    output.write(buffer, 0, count)
                }
                return output.toByteArray()
            } finally {
                inflater.end()
            }
        }

        private fun gunzip(compressed: ByteArray): ByteArray = GZIPInputStream(ByteArrayInputStream(compressed)).use { it.readBytes() }

        private fun half(bits: Int): Float {
            val exponent = (bits shr 10) and 0x1F
            val mantissa = bits and 0x3FF
            val magnitude = when (exponent) {
                0 -> mantissa * (1f / (1 shl 24))
                0x1F -> if (mantissa == 0) Float.POSITIVE_INFINITY else Float.NaN
                else -> java.lang.Float.intBitsToFloat(((exponent + 112) shl 23) or (mantissa shl 13))
            }
            return if (bits and 0x8000 != 0) -magnitude else magnitude
        }

        /** Snorm8 vectors; a zero-length normal becomes +Z and a zero-length tangent stays zero. */
        private fun snorm8Normal(data: ByteArray, offset: Int, out: FloatArray, at: Int, zeroFallback: Boolean) {
            val x = data[offset] / 127f
            val y = data[offset + 1] / 127f
            val z = data[offset + 2] / 127f
            normalizeInto(x, y, z, out, at, 1e-8f, if (zeroFallback) 0f else 1f)
        }

        /** Int2101010 normals: NativeMiiRenderer.DecodeInt2101010. */
        internal fun decodeInt2101010(packed: Int, out: FloatArray, at: Int) {
            val x = signExtend10(packed) / 511f
            val y = signExtend10(packed shr 10) / 511f
            val z = signExtend10(packed shr 20) / 511f
            normalizeInto(x, y, z, out, at, 1e-5f, 1f)
        }

        private fun signExtend10(value: Int): Int {
            val bits = value and 0x3FF
            return if (bits and 0x200 != 0) bits - 0x400 else bits
        }

        /** Normalizes, or writes (0, 0, [fallbackZ]) when shorter than the threshold. */
        internal fun normalizeInto(x: Float, y: Float, z: Float, out: FloatArray, at: Int, threshold: Float, fallbackZ: Float) {
            val lengthSquared = x * x + y * y + z * z
            if (lengthSquared < threshold) {
                out[at] = 0f
                out[at + 1] = 0f
                out[at + 2] = fallbackZ
                return
            }
            val length = sqrt(lengthSquared)
            out[at] = x / length
            out[at + 1] = y / length
            out[at + 2] = z / length
        }
    }
}

internal class FflTexture(val width: Int, val height: Int, val format: Int, val pixels: ByteArray) {
    val stride = when (format) {
        R8 -> 1
        RG8 -> 2
        else -> 4
    }

    companion object {
        const val R8 = 0
        const val RG8 = 1
        const val RGBA8 = 2
    }
}

/** A decoded shape, three floats per position, normal and tangent, two per texcoord, four per parameter. */
internal class FflShape(
    val positions: FloatArray,
    val texcoords: FloatArray,
    val normals: FloatArray,
    val tangents: FloatArray,
    val parameters: FloatArray,
    val indices: IntArray,
    /** The faceline's hair, nose and beard anchors. */
    val translates: Array<FloatArray>?,
) {
    val vertexCount get() = positions.size / 3
}
