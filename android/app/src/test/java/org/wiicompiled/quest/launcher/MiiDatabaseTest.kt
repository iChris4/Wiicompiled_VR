package org.wiicompiled.quest.launcher

import java.io.File
import java.io.IOException
import java.nio.file.Files
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/** The Mii database is created and changed as WheelWizard's MiiRepositoryService does. */
class MiiDatabaseTest {

    private fun newDatabase(): File {
        val file = File(Files.createTempDirectory("miidb").toFile(), "shared2/menu/FaceLib/RFL_DB.dat")
        MiiDatabase.create(file)
        return file
    }

    private fun mii(name: String, id: Long) = MiiFactory.male(name).apply {
        miiId = id
        systemId = 0xC8112233L
    }

    @Test
    fun crcIsXmodem() {
        assertEquals(0x31C3, MiiDatabase.crc16("123456789".toByteArray(), 0, 9))
    }

    @Test
    fun createsThePcsEmptyDatabase() {
        val db = newDatabase().readBytes()
        assertEquals(MiiDatabase.FILE_SIZE, db.size)
        assertEquals("RNOD", String(db, 0, 4, Charsets.US_ASCII))
        assertEquals("RNHD", String(db, 0x1D00, 4, Charsets.US_ASCII))
        assertEquals(0x80, db[0x1CEC].toInt() and 0xFF)
        assertTrue((0x1D04..0x1D07).all { db[it] == 0xFF.toByte() })
        // Like a database the Wii formatted: 10,000 hidden entries, each linked to nothing.
        for (entry in 0 until 10_000) {
            val at = 0x1D08 + entry * 12
            assertArrayEquals(
                byteArrayOf(0, 0, 0, 0, 0, 0, 0, 0, 0x7F, 0xFF.toByte(), 0x7F, 0xFF.toByte()),
                db.copyOfRange(at, at + 12),
            )
        }
        assertTrue((0x1D08 + 10_000 * 12 until 0x1F1DE).all { db[it] == 0.toByte() })
        assertTrue((0x1F1E0 until db.size).all { db[it] == 0.toByte() })
        val stored = ((db[0x1F1DE].toInt() and 0xFF) shl 8) or (db[0x1F1DF].toInt() and 0xFF)
        assertEquals(MiiDatabase.crc16(db, 0, 0x1F1DE), stored)
        assertTrue(MiiDatabase.slots(db).all { it == null })
    }

    @Test
    fun createLeavesAnExistingDatabaseAlone() {
        val file = newDatabase()
        MiiDatabase.add(file, mii("Kept", 0x80000001L))
        MiiDatabase.create(file)
        assertEquals(listOf("Kept"), MiiDatabase.miis(file).map { it.name })
    }

    @Test
    fun addsUpdatesAndRemovesBySlot() {
        val file = newDatabase()
        MiiDatabase.add(file, mii("First", 0x80000001L))
        MiiDatabase.add(file, mii("Second", 0x80000002L))
        MiiDatabase.add(file, mii("Third", 0x80000003L))
        assertEquals(listOf("First", "Second", "Third"), MiiDatabase.miis(file).map { it.name })

        MiiDatabase.remove(file, 0x80000002L)
        assertNull(MiiDatabase.slots(file)[1])
        // A new Mii takes the first free slot, as on the PC.
        MiiDatabase.add(file, mii("Fourth", 0x80000004L))
        assertEquals(listOf("First", "Fourth", "Third"), MiiDatabase.miis(file).map { it.name })

        MiiDatabase.update(file, mii("Renamed", 0x80000003L).apply { favorite = true })
        val third = MiiDatabase.miis(file)[2]
        assertEquals("Renamed", third.name)
        assertEquals(true, third.favorite)

        val db = file.readBytes()
        val stored = ((db[0x1F1DE].toInt() and 0xFF) shl 8) or (db[0x1F1DF].toInt() and 0xFF)
        assertEquals(MiiDatabase.crc16(db, 0, 0x1F1DE), stored)
        assertArrayEquals(MiiData.serialize(mii("First", 0x80000001L)), db.copyOfRange(4, 4 + MiiData.SIZE))
    }

    @Test
    fun refusesChangesItCannotMake() {
        val file = newDatabase()
        assertThrows(IOException::class.java) { MiiDatabase.update(file, mii("Missing", 0x80000009L)) }
        assertThrows(IOException::class.java) { MiiDatabase.remove(file, 0) }
        for (slot in 0 until MiiDatabase.SLOTS) MiiDatabase.add(file, mii("M$slot", 0x80000100L + slot))
        assertThrows(IOException::class.java) { MiiDatabase.add(file, mii("Extra", 0x80000999L)) }
        assertEquals(MiiDatabase.SLOTS, MiiDatabase.miis(file).size)
    }

    @Test
    fun refusesToWriteOverACorruptDatabase() {
        val file = newDatabase()
        val db = file.readBytes()
        db[0x100] = 1
        file.writeBytes(db)
        assertThrows(IOException::class.java) { MiiDatabase.add(file, mii("Nope", 0x80000001L)) }
        assertEquals(1, file.readBytes()[0x100].toInt())
    }

    @Test
    fun findsMiisByIdAsThePcDoes() {
        val file = newDatabase()
        MiiDatabase.add(file, mii("First", 0x80000001L))
        MiiDatabase.add(file, mii("Second", 0x80000002L))
        val db = file.readBytes()
        // A later slot with the same ID loses to the first, as GetByAvatarId finds it.
        MiiData.serialize(mii("Later", 0x80000001L)).copyInto(db, 4 + 2 * MiiData.SIZE)
        file.writeBytes(db)
        val byId = MiiDatabase.byId(file)
        assertEquals(setOf(0x80000001L, 0x80000002L), byId.keys)
        assertEquals("First", byId[0x80000001L]!!.name)
        assertEquals("Second", byId[0x80000002L]!!.name)
    }

    @Test
    fun listsOnlyTheSlotsItCanRead() {
        val file = newDatabase()
        MiiDatabase.add(file, mii("Good", 0x80000001L))
        val db = file.readBytes()
        // A second slot the PC could not read either: favourite colour 15.
        MiiData.serialize(mii("Bad", 0x80000002L)).also { it[1] = (15 shl 1).toByte() }.copyInto(db, 4 + MiiData.SIZE)
        file.writeBytes(db)
        assertEquals(listOf("Good"), MiiDatabase.miis(file).map { it.name })
    }
}
