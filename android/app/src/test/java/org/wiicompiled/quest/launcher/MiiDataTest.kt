package org.wiicompiled.quest.launcher

import kotlin.random.Random
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

/** Miis are read and written as WheelWizard's MiiSerializer does. */
class MiiDataTest {

    @Test
    fun serializesTheFieldsWhereThePcPutsThem() {
        val mii = MiiFactory.male("Ab").apply {
            girl = true
            birthMonth = 4
            birthDay = 4
            favoriteColor = 3
            favorite = true
            miiId = 0x87C0DB3EL
            systemId = 0xC271D44CL
            creatorName = "Me"
        }
        val data = MiiData.serialize(mii)
        assertEquals(MiiData.SIZE, data.size)
        // Girl, April 4th, favourite colour 3, favourite.
        assertEquals(0x5087, u16(data, 0x00))
        assertArrayEquals(bytes(0x00, 0x41, 0x00, 0x62, 0x00, 0x00), data.copyOfRange(0x02, 0x08))
        assertEquals(63, data[0x16].toInt())
        assertEquals(63, data[0x17].toInt())
        assertEquals(0x87C0DB3EL, u32(data, 0x18))
        assertEquals(0xC271D44CL, u32(data, 0x1C))
        // Male's hair 33, brown, not flipped.
        assertEquals((33 shl 9) or (1 shl 6), u16(data, 0x22))
        // Eyebrows 6, rotation 6, brown, size 4, vertical 10, spacing 2.
        assertEquals((6L shl 27) or (6L shl 22) or (1L shl 13) or (4L shl 9) or (10L shl 4) or 2L, u32(data, 0x24))
        // Eyes 2, rotation 4, vertical 12, black, size 4, spacing 2.
        assertEquals((2L shl 26) or (4L shl 21) or (12L shl 16) or (4L shl 9) or (2L shl 5), u32(data, 0x28))
        assertArrayEquals(bytes(0x00, 0x4D, 0x00, 0x65, 0x00, 0x00), data.copyOfRange(0x36, 0x3C))
    }

    @Test
    fun roundTripsEveryField() {
        val random = Random(7)
        repeat(50) {
            val mii = MiiFactory.random("Mii$it", random).apply {
                miiId = 0x80000000L + it
                systemId = 0x12345678L
                birthMonth = random.nextInt(13)
                birthDay = random.nextInt(32)
                favorite = random.nextBoolean()
                mingleOff = random.nextBoolean()
                eyebrowVertical = 3 + random.nextInt(16)
                moleVertical = random.nextInt(31)
                moleHorizontal = random.nextInt(17)
                creatorName = "Creator"
            }
            assertEquals(mii, MiiData.parse(MiiData.serialize(mii)))
        }
    }

    @Test
    fun keepsABirthdayThatWasNeverSet() {
        // The PC turns month 0 into January 1st when it saves; the headset keeps what was there.
        val mii = MiiFactory.female("Nobirthday").apply {
            miiId = 5
            birthMonth = 0
            birthDay = 0
        }
        val parsed = MiiData.parse(MiiData.serialize(mii))
        assertEquals(0, parsed.birthMonth)
        assertEquals(0, parsed.birthDay)
    }

    @Test
    fun refusesWhatTheWiiNeverWrites() {
        val good = MiiData.serialize(MiiFactory.male("Good").apply { miiId = 9 })
        assertThrows(IllegalArgumentException::class.java) { MiiData.parse(ByteArray(MiiData.SIZE)) }
        assertThrows(IllegalArgumentException::class.java) { MiiData.parse(ByteArray(MiiData.SIZE) { 0xFF.toByte() }) }
        assertThrows(IllegalArgumentException::class.java) { MiiData.parse(good.copyOf(73)) }
        // No name.
        assertThrows(IllegalArgumentException::class.java) { MiiData.parse(good.copyOf().also { it.fill(0, 0x02, 0x16) }) }
        // Favourite colour 12.
        assertThrows(IllegalArgumentException::class.java) { MiiData.parse(good.copyOf().also { it[1] = (12 shl 1).toByte() }) }
        // Hair type 72.
        assertThrows(IllegalArgumentException::class.java) { MiiData.parse(good.copyOf().also { it[0x22] = (72 shl 1).toByte() }) }
        // A Mii without an ID cannot be written.
        assertThrows(IllegalArgumentException::class.java) { MiiData.serialize(MiiFactory.male("NoId").apply { miiId = 0 }) }
    }

    @Test
    fun datesAMiiByItsId() {
        // 0b100 and 4-second ticks from 2006-01-01 UTC.
        assertEquals(MiiData.EPOCH_2006_MILLIS + 1_000L * 4_000L, MiiData.creationTimeMillis(0x80000000L + 1_000L))
        assertEquals(null, MiiData.creationTimeMillis(0))
    }

    @Test
    fun randomizeKeepsWhoTheMiiIs() {
        val mii = MiiFactory.female("Keeper").apply {
            favorite = true
            miiId = 0x80001234L
            systemId = 0xAABBCCDDL
            creatorName = "Someone"
            birthMonth = 7
            birthDay = 9
        }
        val random = MiiFactory.randomLook(mii, Random(3))
        assertEquals("Keeper", random.name)
        assertEquals(true, random.favorite)
        assertEquals(0x80001234L, random.miiId)
        assertEquals(0xAABBCCDDL, random.systemId)
        assertEquals("Someone", random.creatorName)
        assertEquals(7, random.birthMonth)
        assertEquals(9, random.birthDay)
    }

    @Test
    fun mapsTheWiiMiiToStudioValuesAsThePcDoes() {
        val mii = MiiFactory.male("Studio").apply {
            hairColor = 0
            eyeColor = 2
            lipColor = 1
            glassesColor = 0
            facialHairColor = 5
            facialFeature = 4
        }
        val studio = FflCharInfo.studio(mii)
        assertEquals(8, studio[0x1B])
        assertEquals(10, studio[4])
        assertEquals(20, studio[0x24])
        assertEquals(8, studio[0x17])
        assertEquals(5, studio[0])
        // Baggy eyes are FFL's wrinkles 5, without make-up.
        assertEquals(5, studio[0x14])
        assertEquals(0, studio[0x12])
        assertEquals(mii.lookKey(), mii.copy(name = "Other", miiId = 77).lookKey())
    }

    private fun bytes(vararg values: Int) = ByteArray(values.size) { values[it].toByte() }

    private fun u16(data: ByteArray, offset: Int) = ((data[offset].toInt() and 0xFF) shl 8) or (data[offset + 1].toInt() and 0xFF)

    private fun u32(data: ByteArray, offset: Int) = (u16(data, offset).toLong() shl 16) or u16(data, offset + 2).toLong()
}
