package org.wiicompiled.quest.launcher

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** New Miis get their IDs as on the PC, from the console identity the runtime uses. */
class MiiIdsTest {

    /** A NAND with setting.txt as RuntimeNandSettings::EncodeNew writes it. */
    private fun nand(serial: String): File {
        val root = Files.createTempDirectory("nand").toFile()
        val text = "AREA=EUR\r\nMODEL=RVL-001(EUR)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LEH\r\nSERNO=$serial\r\nVIDEO=PAL\r\nGAME=EU\r\n"
        val bytes = ByteArray(256)
        var key = 0x73B5DBFA
        for ((i, char) in text.withIndex()) {
            bytes[i] = (char.code xor (key and 0xFF)).toByte()
            key = (key shl 1) or (key ushr 31)
        }
        File(root, "title/00000001/00000002/data").mkdirs()
        File(root, "title/00000001/00000002/data/setting.txt").writeBytes(bytes)
        return root
    }

    @Test
    fun readsTheSerialFromSettingTxt() {
        val root = nand("123456789")
        assertEquals("123456789", MiiIds.consoleSerial(File(root, "title/00000001/00000002/data/setting.txt")))
    }

    @Test
    fun derivesTheMacAsTheRuntimeDoes() {
        // FNV-1a of the serial's digits, below Nintendo's 00:09:BF.
        assertArrayEquals(bytes(0x00, 0x09, 0xBF, 0x86, 0xB1, 0x1C), MiiIds.consoleMac(nand("123456789")))
        assertArrayEquals(bytes(0x00, 0x09, 0xBF, 0x45, 0x62, 0x31), MiiIds.consoleMac(nand("000000042")))
    }

    @Test
    fun hasNoMacBeforeTheGameMadeOne() {
        assertNull(MiiIds.consoleMac(Files.createTempDirectory("nand").toFile()))
        // An all-zero serial is not an identity.
        assertNull(MiiIds.consoleMac(nand("000000000")))
    }

    @Test
    fun systemIdSumsTheMacVendorBytes() {
        assertEquals(0xC886B11CL, MiiIds.systemId(bytes(0x00, 0x09, 0xBF, 0x86, 0xB1, 0x1C)))
        // The address the PC gives imported Miis.
        assertEquals(0x24111111L, MiiIds.systemId(MiiIds.IMPORT_MAC))
    }

    @Test
    fun newIdsCountFourSecondTicksFrom2006() {
        val now = MiiData.EPOCH_2006_MILLIS + 4_000L * 1_000_000L + 1_500L
        val first = MiiIds.newMiiId(now)
        assertEquals(0b100L, first ushr 29)
        assertEquals(1_000_000L, first and 0x1FFFFFFF)
        // Two Miis in the same tick still get different IDs.
        assertEquals(1_000_001L, MiiIds.newMiiId(now) and 0x1FFFFFFF)
        assertEquals(2_000_000L, MiiIds.newMiiId(now + 4_000L * 1_000_000L) and 0x1FFFFFFF)
    }

    private fun bytes(vararg values: Int) = ByteArray(values.size) { values[it].toByte() }
}
