package org.wiicompiled.quest.launcher

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.util.zip.Deflater
import kotlin.math.abs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The renderer on a made-up parts file in FFL's format: the real one is Nintendo's and never in
 * this repository. Its only faceline is a square facing the camera, and its only texture a white
 * eye, which is enough to follow a Mii from the file to a picture.
 */
class MiiRendererTest {

    private val textureCounts = intArrayOf(3, 132, 80, 28, 12, 12, 20, 2, 52, 6, 18)
    private val shapeCounts = intArrayOf(4, 132, 132, 12, 1, 12, 18, 18, 132, 132, 132, 132)

    /** A big-endian FFRA file with [shapes] and [textures] (type to index to payload), zlib-compressed. */
    private fun archive(shapes: Map<Pair<Int, Int>, ByteArray>, textures: Map<Pair<Int, Int>, ByteArray>): ByteArray {
        val tableSize = 0x14 + textureCounts.size * 4 + textureCounts.sum() * 16 + shapeCounts.size * 4 + shapeCounts.sum() * 16
        val header = ByteBuffer.allocate(maxOf(tableSize, 0x4A00))
        header.putInt(0, 0x46465241)
        header.putInt(4, 0x00070000)
        // AFL 2.3's expanded size, which picks its part counts.
        header.putInt(12, 0x02502DE0)
        val payload = ByteArrayOutputStream()
        fun table(start: Int, counts: IntArray, parts: Map<Pair<Int, Int>, ByteArray>) {
            var offset = start
            for ((type, count) in counts.withIndex()) {
                for (index in 0 until count) {
                    parts[type to index]?.let { data ->
                        val compressed = deflate(data)
                        header.putInt(offset, header.capacity() + payload.size())
                        header.putInt(offset + 4, data.size)
                        header.putInt(offset + 8, compressed.size)
                        header.put(offset + 13, 5)
                        payload.write(compressed)
                    }
                    offset += 16
                }
            }
        }
        val textureTable = 0x14 + textureCounts.size * 4
        table(textureTable, textureCounts, textures)
        table(textureTable + textureCounts.sum() * 16 + shapeCounts.size * 4, shapeCounts, shapes)
        return header.array() + payload.toByteArray()
    }

    private fun deflate(data: ByteArray): ByteArray {
        val deflater = Deflater()
        deflater.setInput(data)
        deflater.finish()
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(4096)
        while (!deflater.finished()) out.write(buffer, 0, deflater.deflate(buffer))
        deflater.end()
        return out.toByteArray()
    }

    /** A shape: a square of side [size] around (0, [centreY]), facing +Z, with its three anchors. */
    private fun square(size: Float, centreY: Float): ByteArray {
        val half = size / 2
        val positions = listOf(-half to half, half to half, -half to -half, half to -half)
        val buffer = ByteBuffer.allocate(0x100 + 4 * 16 + 6 * 2)
        val positionAt = 0x100
        val indexAt = positionAt + 4 * 16
        buffer.putInt(0, positionAt)
        buffer.putInt(20, indexAt)
        buffer.putInt(24, 4 * 16)
        buffer.putInt(44, 6)
        // The faceline's hair, nose and beard anchors.
        for (anchor in 0 until 3) buffer.putFloat(0x48 + anchor * 12 + 4, centreY)
        for ((i, position) in positions.withIndex()) {
            buffer.putFloat(positionAt + i * 16, position.first)
            buffer.putFloat(positionAt + i * 16 + 4, centreY + position.second)
            buffer.putFloat(positionAt + i * 16 + 8, 0f)
        }
        // Counter-clockwise on screen, so back-face culling keeps them.
        for ((i, index) in listOf(0, 2, 1, 1, 2, 3).withIndex()) buffer.putShort(indexAt + i * 2, index.toShort())
        return buffer.array()
    }

    /** A texture part: [width] x [height] RGBA8 pixels of [value], then FFL's 12-byte footer. */
    private fun texture(width: Int, height: Int, value: Int): ByteArray {
        val buffer = ByteBuffer.allocate(width * height * 4 + 12)
        for (i in 0 until width * height * 4) buffer.put(i, value.toByte())
        val footer = width * height * 4
        buffer.putShort(footer + 4, width.toShort())
        buffer.putShort(footer + 6, height.toShort())
        buffer.put(footer + 8, 1)
        buffer.put(footer + 9, 2)
        return buffer.array()
    }

    private fun resource() = FflResource.parse(
        archive(
            shapes = mapOf((FflResource.SHAPE_FACELINE to 5) to square(20f, 34.32143f)),
            textures = mapOf((FflResource.TEXTURE_EYE to 2) to texture(16, 8, 255)),
        ),
    )

    @Test
    fun drawsTheFacelineInTheSkinColour() {
        val mii = MiiFactory.male("Square").apply {
            faceShape = 5
            skinColor = 0
        }
        val pixels = MiiRenderer.render(resource(), mii, 64)
        assertEquals(64 * 64, pixels.size)
        // Nothing around the head...
        assertEquals(0, pixels[0] ushr 24)
        assertEquals(0, pixels[64 * 64 - 1] ushr 24)
        // ...and the lit, opaque skin colour in its middle.
        val middle = pixels[32 * 64 + 32]
        assertEquals(0xFF, middle ushr 24)
        val red = (middle shr 16) and 0xFF
        val blue = middle and 0xFF
        assertTrue("skin is warm: $red vs $blue", red > blue)
        // The face view shows about 108 units at the head, so the 20-unit square is about 12 pixels wide.
        val covered = pixels.count { it ushr 24 == 0xFF }
        assertTrue("covered $covered", covered in 100..200)
    }

    @Test
    fun posesTurnTheHead() {
        val mii = MiiFactory.male("Square").apply { faceShape = 5 }
        val front = MiiRenderer.render(resource(), mii, 64)
        val side = MiiRenderer.render(resource(), mii, 64, MiiRenderer.Pose.SIDE)
        fun covered(pixels: IntArray) = pixels.count { it ushr 24 == 0xFF }
        // Turned away by 15 degrees and seen from above, the square covers other pixels, and less.
        assertTrue(!side.contentEquals(front))
        assertTrue("side ${covered(side)} front ${covered(front)}", covered(side) < covered(front))
        // The front pose is the one My Miis draws without a pose.
        assertTrue(MiiRenderer.render(resource(), mii, 64, MiiRenderer.Pose.FRONT).contentEquals(front))
    }

    /** Made-up bodies: a boy's shirt on the left and trousers on the right, both below the head; no girl's body. */
    private fun bodies() = MiiBodies(
        male = MiiBodies.parse(rioModel(quad(-15f, 0f, 40f, 76f), quad(0f, 15f, 40f, 76f))),
        female = emptyList(),
    )

    private fun opaqueIn(pixels: IntArray, rows: IntRange) = rows.sumOf { y -> (0 until 64).count { x -> pixels[y * 64 + x] ushr 24 == 0xFF } }

    @Test
    fun drawsTheUpperBodyBelowTheHeadInItsColours() {
        val mii = MiiFactory.male("Square").apply {
            faceShape = 5
            favoriteColor = 5
        }
        val alone = MiiRenderer.render(resource(), mii, 64)
        val dressed = MiiRenderer.render(resource(), mii, 64, bodies = bodies())
        // The camera rises with the head onto the shoulders, so the head is where it was...
        assertEquals(opaqueIn(alone, 0 until 48), opaqueIn(dressed, 0 until 48))
        assertEquals(0xFF, dressed[32 * 64 + 32] ushr 24)
        // ...and below it, where there was nothing, is the body.
        assertEquals(0, alone[60 * 64 + 28] ushr 24)
        fun channels(pixel: Int) = Triple((pixel shr 16) and 0xFF, (pixel shr 8) and 0xFF, pixel and 0xFF)
        // The shirt in the favourite colour, blue...
        val (shirtRed, _, shirtBlue) = channels(dressed[60 * 64 + 28])
        assertEquals(0xFF, dressed[60 * 64 + 28] ushr 24)
        assertTrue("shirt $shirtRed vs $shirtBlue", shirtBlue > shirtRed + 60)
        // ...and the trousers grey.
        val (pantsRed, _, pantsBlue) = channels(dressed[60 * 64 + 36])
        assertEquals(0xFF, dressed[60 * 64 + 36] ushr 24)
        assertTrue("trousers $pantsRed vs $pantsBlue", abs(pantsBlue - pantsRed) < 30)
    }

    @Test
    fun theBodyFollowsBuildAndGender() {
        val thin = MiiFactory.male("Thin").apply {
            faceShape = 5
            weight = 0
        }
        val heavy = thin.copy(weight = 127)
        val rows = 50 until 64
        assertTrue(opaqueIn(MiiRenderer.render(resource(), heavy, 64, bodies = bodies()), rows) >
            opaqueIn(MiiRenderer.render(resource(), thin, 64, bodies = bodies()), rows))
        // These bodies have none for a girl, who is drawn as without bodies at all.
        val girl = MiiFactory.female("Girl").apply { faceShape = 5 }
        assertTrue(MiiRenderer.render(resource(), girl, 64, bodies = bodies()).contentEquals(MiiRenderer.render(resource(), girl, 64)))
    }

    @Test
    fun aMiiWithoutItsFacelineCannotBeDrawn() {
        val mii = MiiFactory.male("Missing").apply { faceShape = 1 }
        assertThrows(java.io.IOException::class.java) { MiiRenderer.render(resource(), mii, 64) }
    }

    @Test
    fun drawsAPartIconFromItsTexture() {
        val mii = MiiFactory.male("Icons")
        val icon = MiiRenderer.partIcon(resource(), mii, MiiRenderer.Part.Eye, 2, 32)
        assertNotNull(icon)
        // The 16x8 eye is fitted across the icon and centred.
        assertEquals(0xFF, icon!![16 * 32 + 16] ushr 24)
        assertEquals(0, icon[1 * 32 + 16] ushr 24)
        // A part that is not in the file is no picture at all.
        assertNull(MiiRenderer.partIcon(resource(), mii, MiiRenderer.Part.Eye, 3, 32))
    }

    @Test
    fun refusesFilesThatAreNotFflResources() {
        assertThrows(java.io.IOException::class.java) { FflResource.parse(ByteArray(0x4A00)) }
        assertThrows(java.io.IOException::class.java) { FflResource.parse(ByteArray(16)) }
    }
}
