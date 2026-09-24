package org.wiicompiled.quest.launcher

import java.io.File
import java.nio.ByteBuffer
import java.nio.file.Files
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The profiles page reads licences as WheelWizard's GameLicenseService does. */
class RksysProfilesTest {

    private val licenseSize = 0x8CC0

    /** A save with the magic and, for each given slot, an RKPD block. */
    private fun save(vararg licenses: Pair<Int, (ByteBuffer, Int) -> Unit>): ByteArray {
        val buffer = ByteBuffer.allocate(RksysProfiles.LICENSES_END + 0x100)
        buffer.put(0, "RKSD0006".toByteArray())
        for ((slot, fill) in licenses) {
            val base = 8 + slot * licenseSize
            buffer.put(base, "RKPD".toByteArray())
            fill(buffer, base)
        }
        return buffer.array()
    }

    private fun ByteBuffer.name(base: Int, name: String) = put(base + 0x14, name.toByteArray(Charsets.UTF_16BE))

    @Test
    fun friendCodesAreTheProfileIdWithItsChecksum() {
        // The licence on the author's headset, and a player Retro WFC listed in a room.
        assertEquals("0349-6103-6675", RksysProfiles.friendCode(0x23D71583L))
        assertEquals("4123-1702-7346", RksysProfiles.friendCode(166930L))
        assertEquals("", RksysProfiles.friendCode(0L))
    }

    @Test
    fun licencesComeWithNameCodeRatingAndRaces() {
        val bytes = save(
            0 to { buffer: ByteBuffer, base: Int ->
                buffer.name(base, "Mario")
                buffer.putInt(base + 0x5C, 0x23D71583)
                buffer.putShort(base + 0xB0, 5793.toShort())
                buffer.putShort(base + 0xB2, 5052.toShort())
                buffer.putInt(base + 0xB4, 1062)
                buffer.putInt(base + 0xDC, 416)
            },
            2 to { buffer: ByteBuffer, base: Int -> buffer.name(base, "no name") },
        )
        val licenses = RksysProfiles.parse(bytes)!!
        assertEquals(4, licenses.size)
        assertNull(licenses[1])
        assertNull(licenses[3])
        assertEquals(RksysProfiles.License(0, "Mario", 0x23D71583L, "0349-6103-6675", 5793, 5052, 1062, 416), licenses[0])
        // A licence never taken online has no profile ID, so no friend code.
        assertEquals(RksysProfiles.License(2, "no name", 0, "", 0, 0, 0, 0), licenses[2])
    }

    @Test
    fun retroRewindsRatingsOutrankTheSave() {
        val bytes = save(0 to { buffer: ByteBuffer, base: Int ->
            buffer.putInt(base + 0x5C, 0x23D71583)
            buffer.putShort(base + 0xB0, 5793.toShort())
            buffer.putShort(base + 0xB2, 5000.toShort())
        })
        val pul = ByteBuffer.allocate(8 + 100 * 16).apply {
            putInt(0x52525254)
            putShort(1)
            putShort(100)
            // Slot 3 holds the licence's rating; slot 4 a rating without its "has data" flag.
            putInt(8 + 3 * 16, 0x23D71583)
            putFloat(8 + 3 * 16 + 4, 60.50f)
            putFloat(8 + 3 * 16 + 8, 50.52f)
            putInt(8 + 3 * 16 + 12, 1)
            putInt(8 + 4 * 16, 42)
            putFloat(8 + 4 * 16 + 4, 99f)
        }.array()
        val ratings = RksysProfiles.parseRatings(pul)
        assertEquals(mapOf(0x23D71583L to RksysProfiles.Rating(6050, 5052)), ratings)
        val license = RksysProfiles.parse(bytes, ratings)!![0]!!
        assertEquals(6050, license.vr)
        assertEquals(5052, license.br)
        // Anything else is no rating file at all.
        assertTrue(RksysProfiles.parseRatings(ByteArray(8 + 100 * 16)).isEmpty())
    }

    @Test
    fun anythingElseIsNoSave() {
        assertNull(RksysProfiles.parse(ByteArray(RksysProfiles.LICENSES_END)))
        assertNull(RksysProfiles.parse("RKSD0006".toByteArray()))
    }

    @Test
    fun onlyTheLicencesAreRead() {
        val folder = Files.createTempDirectory("rksys").toFile()
        val bytes = save(1 to { buffer: ByteBuffer, base: Int -> buffer.name(base, "Luigi") })
        val file = File(folder, "rksys.dat").apply { writeBytes(bytes) }
        val read = RksysProfiles.readLicenses(file)!!
        assertArrayEquals(bytes.copyOf(RksysProfiles.LICENSES_END), read)
        assertEquals("Luigi", RksysProfiles.parse(read)!![1]!!.name)
        assertNull(RksysProfiles.readLicenses(File(folder, "missing.dat")))
        assertNull(RksysProfiles.readLicenses(File(folder, "short.dat").apply { writeBytes(ByteArray(64)) }))
    }
}
