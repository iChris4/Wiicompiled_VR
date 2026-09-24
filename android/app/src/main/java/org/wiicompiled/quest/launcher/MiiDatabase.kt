package org.wiicompiled.quest.launcher

import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * The Wii's Mii database, `shared2/menu/FaceLib/RFL_DB.dat` in the runtime's NAND, which the game
 * reads its Miis from. A port of the PC launcher's MiiRepositoryService and MiiDbService: 100
 * slots of 74 bytes after the "RNOD" magic, an empty slot being all zeros, and a CRC-16 over the
 * first 0x1F1DE bytes stored after them.
 *
 * The Quest's NAND starts without one, so the page creates the PC's empty database the first
 * time it is opened. Writes go through a temporary file, so a failure never leaves half a file.
 */
object MiiDatabase {
    const val SLOTS = 100
    const val FILE_SIZE = 779_968
    private const val HEADER = 4
    private const val CRC_OFFSET = 0x1F1DE
    private const val HIDDEN_ENTRIES = 10_000
    private const val HIDDEN_ENTRY_SIZE = 12
    /** The first hidden entry's pair of 0x7FFF links. */
    private const val HIDDEN_LINKS = 0x1D10

    fun file(nand: File): File = File(nand, "shared2/menu/FaceLib/RFL_DB.dat")

    /**
     * A new database: the PC's (both magics, the hidden database's empty markers and the CRC), and
     * like one the Wii formats, the hidden database's 10,000 entries linked to nothing (0x7FFF).
     * The PC leaves those links zero; the game's Mii library reads this file, so it gets the Wii's.
     */
    fun empty(): ByteArray {
        val db = ByteArray(FILE_SIZE)
        "RNOD".toByteArray(Charsets.US_ASCII).copyInto(db, 0)
        db[0x1CE0 + 0x0C] = 0x80.toByte()
        "RNHD".toByteArray(Charsets.US_ASCII).copyInto(db, 0x1D00)
        for (i in 0x1D04..0x1D07) db[i] = 0xFF.toByte()
        for (entry in 0 until HIDDEN_ENTRIES) {
            val at = HIDDEN_LINKS + entry * HIDDEN_ENTRY_SIZE
            db[at] = 0x7F
            db[at + 1] = 0xFF.toByte()
            db[at + 2] = 0x7F
            db[at + 3] = 0xFF.toByte()
        }
        writeCrc(db)
        return db
    }

    /** CRC-16/XMODEM, CrcHelper.ComputeCrc16Ccitt on the PC. */
    fun crc16(data: ByteArray, offset: Int, length: Int): Int {
        var crc = 0
        for (i in offset until offset + length) {
            crc = crc xor ((data[i].toInt() and 0xFF) shl 8)
            repeat(8) { crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) and 0xFFFF else (crc shl 1) and 0xFFFF }
        }
        return crc
    }

    /** Creates the empty database unless there is one. */
    @Throws(IOException::class)
    fun create(file: File) {
        if (file.exists()) return
        file.parentFile?.let { if (!it.isDirectory && !it.mkdirs()) throw IOException("Cannot create ${it.path}") }
        write(file, empty())
    }

    /** The 100 slots, null where empty. */
    @Throws(IOException::class)
    fun slots(file: File): List<ByteArray?> = slots(file.readBytes())

    fun slots(db: ByteArray): List<ByteArray?> = (0 until SLOTS).map { slot ->
        val start = HEADER + slot * MiiData.SIZE
        if (start + MiiData.SIZE > db.size) return@map null
        db.copyOfRange(start, start + MiiData.SIZE).takeUnless { block -> block.all { it == 0.toByte() } }
    }

    /**
     * Every Mii in the database, in slot order. A slot the PC could not read either is left out;
     * it stays in the file untouched.
     */
    @Throws(IOException::class)
    fun miis(file: File): List<Mii> = slots(file).mapNotNull { block -> block?.let { runCatching { MiiData.parse(it) }.getOrNull() } }

    /**
     * The database's Miis by ID, as the PC's GetByAvatarId finds them: the first slot holding an ID
     * is that ID's Mii, and one the PC could not read either is left out.
     */
    @Throws(IOException::class)
    fun byId(file: File): Map<Long, Mii> {
        val found = LinkedHashMap<Long, Mii?>()
        for (block in slots(file)) {
            if (block == null) continue
            val id = readId(block, 0)
            if (id !in found) found[id] = runCatching { MiiData.parse(block) }.getOrNull()
        }
        return found.mapNotNull { (id, mii) -> mii?.let { id to it } }.toMap()
    }

    /** Adds a Mii in the first free slot. */
    @Throws(IOException::class)
    fun add(file: File, mii: Mii) = edit(file) { db ->
        val block = MiiData.serialize(mii)
        val free = (0 until SLOTS).firstOrNull { slot -> isEmpty(db, slot) } ?: throw IOException("No empty Mii slot available.")
        block.copyInto(db, offset(free))
    }

    /** Replaces the Mii with the same ID. */
    @Throws(IOException::class)
    fun update(file: File, mii: Mii) {
        val block = MiiData.serialize(mii)
        edit(file) { db -> block.copyInto(db, offset(slotOf(db, mii.miiId))) }
    }

    /** Empties the Mii's slot. */
    @Throws(IOException::class)
    fun remove(file: File, miiId: Long) = edit(file) { db -> ByteArray(MiiData.SIZE).copyInto(db, offset(slotOf(db, miiId))) }

    private fun edit(file: File, change: (ByteArray) -> Unit) {
        if (!file.isFile) throw IOException("RFL_DB.dat not found.")
        val db = file.readBytes()
        if (db.size < CRC_OFFSET + 2) throw IOException("RFL_DB.dat is too short (${db.size} bytes).")
        val stored = ((db[CRC_OFFSET].toInt() and 0xFF) shl 8) or (db[CRC_OFFSET + 1].toInt() and 0xFF)
        val computed = crc16(db, 0, CRC_OFFSET)
        if (stored != computed) {
            throw IOException("Corrupt Mii database (bad CRC 0x%04X, expected 0x%04X).".format(stored, computed))
        }
        change(db)
        writeCrc(db)
        write(file, db)
    }

    private fun slotOf(db: ByteArray, miiId: Long): Int {
        if (miiId == 0L) throw IOException("Invalid Client ID.")
        return (0 until SLOTS).firstOrNull { slot -> !isEmpty(db, slot) && readId(db, offset(slot)) == miiId }
            ?: throw IOException("Mii not found.")
    }

    private fun offset(slot: Int) = HEADER + slot * MiiData.SIZE

    private fun isEmpty(db: ByteArray, slot: Int): Boolean {
        val start = offset(slot)
        return (start until start + MiiData.SIZE).all { db[it] == 0.toByte() }
    }

    private fun readId(db: ByteArray, block: Int): Long =
        (0 until 4).fold(0L) { id, i -> (id shl 8) or (db[block + 0x18 + i].toLong() and 0xFF) }

    private fun writeCrc(db: ByteArray) {
        val crc = crc16(db, 0, CRC_OFFSET)
        db[CRC_OFFSET] = (crc shr 8).toByte()
        db[CRC_OFFSET + 1] = crc.toByte()
    }

    private fun write(file: File, db: ByteArray) {
        val temporary = File(file.parentFile, file.name + ".partial")
        try {
            temporary.writeBytes(db)
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        } finally {
            temporary.delete()
        }
    }
}

/**
 * The IDs that make a Mii this headset's own: MiiDbService.AddToDatabase and GenerateMiiId on the
 * PC. The system ID comes from the console's MAC address, which the runtime derives from the NAND's
 * serial number (RuntimeConsoleIdentity::FromSerial), so a Mii made here belongs to the same
 * console as the game's saves.
 */
object MiiIds {
    /** The PC gives imported Miis this address, so they never pass for the console's own. */
    val IMPORT_MAC = byteArrayOf(0x02, 0x11, 0x11, 0x11, 0x11, 0x11)

    private var lastCounter = -1L
    private var sequenceOffset = 0L

    fun systemId(mac: ByteArray): Long {
        val first = ((mac[0].toInt() and 0xFF) + (mac[1].toInt() and 0xFF) + (mac[2].toInt() and 0xFF)) and 0xFF
        return (first.toLong() shl 24) or ((mac[3].toLong() and 0xFF) shl 16) or ((mac[4].toLong() and 0xFF) shl 8) or (mac[5].toLong() and 0xFF)
    }

    /** A new Mii ID: 0b100 and a 4-second counter from 2006, bumped when two share a tick. */
    @Synchronized
    fun newMiiId(nowMillis: Long = System.currentTimeMillis()): Long {
        val base = ((nowMillis - MiiData.EPOCH_2006_MILLIS) / 4_000L) and 0xFFFFFFFFL
        if (base == lastCounter) {
            sequenceOffset++
        } else {
            lastCounter = base
            sequenceOffset = 0
        }
        return (0b100L shl 29) or ((base + sequenceOffset) and 0x1FFFFFFF)
    }

    /**
     * The console's MAC address, as the runtime computes it from `setting.txt` in the NAND, or
     * null before the game has created that file on its first start.
     */
    fun consoleMac(nand: File): ByteArray? {
        val serial = consoleSerial(File(nand, "title/00000001/00000002/data/setting.txt")) ?: return null
        var hash = 2166136261L
        for (byte in serial.toByteArray(Charsets.US_ASCII)) {
            hash = ((hash xor (byte.toLong() and 0xFF)) * 16777619L) and 0xFFFFFFFFL
        }
        var suffix = hash and 0x00FFFFFF
        if (suffix == 0L || suffix == 0x00FFFFFFL) suffix = suffix xor 0x005A17C3
        return byteArrayOf(0x00, 0x09, 0xBF.toByte(), (suffix shr 16).toByte(), (suffix shr 8).toByte(), suffix.toByte())
    }

    /** SERNO from the Wii's setting.txt, a 256-byte buffer under a rotating XOR key. */
    fun consoleSerial(file: File): String? {
        val bytes = runCatching { file.readBytes() }.getOrNull()?.takeIf { it.size >= 256 } ?: return null
        var key = 0x73B5DBFA
        val text = StringBuilder()
        for (i in 0 until 256) {
            val value = ((bytes[i].toInt() xor key) and 0xFF).toChar()
            key = (key shl 1) or (key ushr 31)
            if (value == '\u0000') break
            if (value != '\r') text.append(value)
        }
        val serial = text.lineSequence()
            .mapNotNull { line -> line.split('=', limit = 2).takeIf { it.size == 2 && it[0] == "SERNO" }?.get(1) }
            .firstOrNull()
            ?: return null
        val valid = serial.length in 1..9 && serial.all { it in '0'..'9' } && serial.any { it != '0' }
        return serial.takeIf { valid }
    }
}
