package org.wiicompiled.quest.launcher

import java.security.MessageDigest
import java.util.zip.CRC32

/**
 * The friend lists of a Mario Kart Wii save, read and changed as WheelWizard's GameLicenseService
 * does (ParseFriends, AddFriend, RemoveFriend). Each licence (RKPD block) keeps 30 friend slots of
 * 0x1C0 bytes at 0x56D0, and at 0x8B50 twelve bytes more per slot for the Wii's friend
 * registration. A slot holds the friend's profile ID, the wins and losses against them, their VR
 * and BR, and their Mii. The whole save is checked by a CRC-32 over its first 0x27FFC bytes,
 * stored right after them, which every change here writes again.
 */
object RksysFriends {
    const val MAX = 30
    /** A whole save's size; the PC changes nothing in a shorter file. */
    const val SAVE_SIZE = 0x2BC000

    private const val MAGIC = "RKSD0006"
    private const val LICENSE_MAGIC = "RKPD"
    private const val LICENSE_SIZE = 0x8CC0
    private const val CRC_OFFSET = 0x27FFC
    private const val SLOTS_OFFSET = 0x56D0
    private const val SLOT_SIZE = 0x1C0
    private const val REGISTRATION_OFFSET = 0x8B50
    private const val REGISTRATION_SIZE = 0x0C
    private const val MII_OFFSET = 0x1A
    /** A friend added here and not yet added back: bit 0 of the slot's state without bit 1. */
    private const val STATE_ADDED = 0x0001
    private const val CONTROL_INVALID = 0x00
    private const val CONTROL_FRIEND_KEY = 0x10
    private const val DEFAULT_COUNTRY = 0xFF
    private const val DEFAULT_REGION = 0xFF
    /** The ten records after the slot's fixed part start as "none" (0xFFFFFFFF, every 8 bytes). */
    private const val RECORDS_OFFSET = 0x70
    private const val RECORDS = 10

    /** A friend in a licence's list (FriendProfile). */
    data class Friend(
        val slot: Int,
        val profileId: Long,
        val friendCode: String,
        val mii: Mii,
        val vr: Int,
        val br: Int,
        val wins: Int,
        val losses: Int,
        val countryCode: Int,
        val regionId: Int,
        /** Added from this licence but not yet added back (the PC's IsPending). */
        val isPending: Boolean,
    ) {
        /** The Mii's name as the PC shows it; a NUL inside one ("matt\0ias") is not drawn there. */
        val name: String get() = mii.name.replace("\u0000", "")
    }

    /** Why a friend cannot be added or removed, with the PC's messages in the launcher's strings. */
    enum class Problem { Empty, NotTwelveDigits, Invalid, Own, Duplicate, Full, NoLicense, NotFound, NotASave, CannotWrite }

    /**
     * ParseFriends: the friends of licence [slot] in [save] (the licences are enough), in slot
     * order. A slot without a Mii is empty, and one whose Mii cannot be read is left out.
     */
    fun parse(save: ByteArray, slot: Int): List<Friend> {
        val base = licenseBase(slot)
        if (save.size < base + LICENSE_SIZE || String(save, base, LICENSE_MAGIC.length, Charsets.US_ASCII) != LICENSE_MAGIC) return emptyList()
        return (0 until MAX).mapNotNull { index ->
            val offset = base + SLOTS_OFFSET + index * SLOT_SIZE
            if (!hasMii(save, offset)) return@mapNotNull null
            val mii = runCatching { MiiData.parse(save.copyOfRange(offset + MII_OFFSET, offset + MII_OFFSET + MiiData.SIZE)) }.getOrNull()
                ?: return@mapNotNull null
            val state = u16(save, offset + 0x10) and 0x0003
            val control = save[base + REGISTRATION_OFFSET + index * REGISTRATION_SIZE + 2].toInt() and 0xFF
            val profileId = u32(save, offset + 0x04)
            Friend(
                slot = index,
                profileId = profileId,
                friendCode = RksysProfiles.friendCode(profileId),
                mii = mii,
                vr = u16(save, offset + 0x16),
                br = u16(save, offset + 0x18),
                wins = u16(save, offset + 0x14),
                losses = u16(save, offset + 0x12),
                countryCode = save[offset + 0x68].toInt() and 0xFF,
                regionId = save[offset + 0x69].toInt() and 0xFF,
                // Old one-sided entries left the control byte at 0 and are pending too.
                isPending = state == STATE_ADDED && (control == CONTROL_FRIEND_KEY || control == CONTROL_INVALID),
            )
        }
    }

    /**
     * AddFriend: writes [friendCode] (as [normalize] gives it) with the 74 bytes of its Mii into the
     * first empty slot of licence [slot] as a request the friend has not yet answered, and the
     * save's CRC again. Null when it was added, else why not.
     */
    fun add(save: ByteArray, slot: Int, friendCode: String, mii: ByteArray, vr: Int, br: Int = 5000): Problem? {
        if (!isSave(save)) return Problem.NotASave
        val profileId = profileId(friendCode)
        if (profileId == 0L) return Problem.Invalid
        val base = licenseBase(slot)
        if (String(save, base, LICENSE_MAGIC.length, Charsets.US_ASCII) != LICENSE_MAGIC) return Problem.NoLicense
        val own = u32(save, base + 0x5C)
        if (own == 0L) return Problem.NoLicense
        if (own == profileId) return Problem.Own
        if (parse(save, slot).any { it.profileId == profileId }) return Problem.Duplicate
        val index = (0 until MAX).firstOrNull { isEmpty(save, base + SLOTS_OFFSET + it * SLOT_SIZE) } ?: return Problem.Full
        require(mii.size == MiiData.SIZE) { "A Mii is ${MiiData.SIZE} bytes." }

        val offset = base + SLOTS_OFFSET + index * SLOT_SIZE
        val registration = base + REGISTRATION_OFFSET + index * REGISTRATION_SIZE
        save.fill(0, offset, offset + SLOT_SIZE)
        save.fill(0, registration, registration + REGISTRATION_SIZE)
        // The friend's key: the friend code's upper half, then the profile ID.
        val code = friendCode.filter { it in '0'..'9' }.toLong()
        putU32(save, offset, code ushr 32)
        putU32(save, offset + 0x04, profileId)
        putU16(save, offset + 0x10, STATE_ADDED)
        putU16(save, offset + 0x16, vr.coerceIn(0, 0xFFFF))
        putU16(save, offset + 0x18, br.coerceIn(0, 0xFFFF))
        mii.copyInto(save, offset + MII_OFFSET)
        putU16(save, offset + 0x64, MiiDatabase.crc16(mii, 0, mii.size))
        save[offset + 0x66] = index.toByte()
        save[offset + 0x68] = DEFAULT_COUNTRY.toByte()
        save[offset + 0x69] = DEFAULT_REGION.toByte()
        for (record in 0 until RECORDS) putU32(save, offset + RECORDS_OFFSET + record * 8, 0xFFFFFFFFL)
        // The request the Wii sends until the friend adds this licence back.
        save[registration + 2] = CONTROL_FRIEND_KEY.toByte()
        putU32(save, registration + 4, profileId)
        fixCrc(save)
        return null
    }

    /** RemoveFriend: empties the slot of [profileId] in licence [slot]; false when it is not there. */
    fun remove(save: ByteArray, slot: Int, profileId: Long): Boolean {
        if (!isSave(save) || profileId == 0L) return false
        val base = licenseBase(slot)
        val index = (0 until MAX).firstOrNull { u32(save, base + SLOTS_OFFSET + it * SLOT_SIZE + 0x04) == profileId } ?: return false
        val offset = base + SLOTS_OFFSET + index * SLOT_SIZE
        val registration = base + REGISTRATION_OFFSET + index * REGISTRATION_SIZE
        save.fill(0, offset, offset + SLOT_SIZE)
        save.fill(0, registration, registration + REGISTRATION_SIZE)
        fixCrc(save)
        return true
    }

    fun isSave(save: ByteArray): Boolean = save.size >= SAVE_SIZE && String(save, 0, MAGIC.length, Charsets.US_ASCII) == MAGIC

    /** FixRksysCrc: CRC-32 over everything before 0x27FFC, big-endian at 0x27FFC. */
    fun fixCrc(save: ByteArray) {
        val crc = CRC32().apply { update(save, 0, CRC_OFFSET) }.value
        putU32(save, CRC_OFFSET, crc)
    }

    /**
     * NormalizeFriendCode: the twelve digits of [text] as 0000-0000-0000, or why it is not a friend
     * code, as the PC checks one typed in.
     */
    fun normalize(text: String): Result {
        if (text.isBlank()) return Result(problem = Problem.Empty)
        val digits = text.filter { it in '0'..'9' }
        if (digits.length != 12) return Result(problem = Problem.NotTwelveDigits)
        val formatted = "${digits.substring(0, 4)}-${digits.substring(4, 8)}-${digits.substring(8)}"
        if (profileId(formatted) == 0L) return Result(problem = Problem.Invalid)
        return Result(friendCode = formatted)
    }

    class Result(val friendCode: String? = null, val problem: Problem? = null)

    /**
     * FriendCodeToProfileId: the profile ID a friend code stands for, its lower 32 bits, once its
     * upper bits match the checksum the ID gives; 0 for anything else.
     */
    fun profileId(friendCode: String): Long {
        val digits = friendCode.replace("-", "")
        if (digits.length != 12 || !digits.all { it in '0'..'9' }) return 0L
        val value = digits.toLong()
        val profileId = value and 0xFFFFFFFFL
        if (profileId == 0L) return 0L
        return if (value ushr 32 == checksum(profileId)) profileId else 0L
    }

    /** ProfileIdToFriendCode's checksum: the top seven bits of the MD5 of the ID (little-endian) and "JCMR". */
    private fun checksum(profileId: Long): Long {
        val id = profileId.toInt()
        val input = byteArrayOf(id.toByte(), (id ushr 8).toByte(), (id ushr 16).toByte(), (id ushr 24).toByte(), 0x4A, 0x43, 0x4D, 0x52)
        return ((MessageDigest.getInstance("MD5").digest(input)[0].toInt() and 0xFF) ushr 1).toLong()
    }

    private fun licenseBase(slot: Int): Int = MAGIC.length + slot * LICENSE_SIZE

    private fun hasMii(save: ByteArray, slotOffset: Int): Boolean =
        (slotOffset + MII_OFFSET until slotOffset + MII_OFFSET + MiiData.SIZE).any { save[it] != 0.toByte() }

    /** IsFriendSlotEmpty: no profile ID, no state and no Mii. */
    private fun isEmpty(save: ByteArray, slotOffset: Int): Boolean =
        u32(save, slotOffset + 0x04) == 0L && u16(save, slotOffset + 0x10) and 0x0003 == 0 && !hasMii(save, slotOffset)

    private fun u16(data: ByteArray, offset: Int): Int = ((data[offset].toInt() and 0xFF) shl 8) or (data[offset + 1].toInt() and 0xFF)

    private fun u32(data: ByteArray, offset: Int): Long =
        ((data[offset].toLong() and 0xFF) shl 24) or ((data[offset + 1].toLong() and 0xFF) shl 16) or
            ((data[offset + 2].toLong() and 0xFF) shl 8) or (data[offset + 3].toLong() and 0xFF)

    private fun putU16(data: ByteArray, offset: Int, value: Int) {
        data[offset] = (value ushr 8).toByte()
        data[offset + 1] = value.toByte()
    }

    private fun putU32(data: ByteArray, offset: Int, value: Long) {
        data[offset] = (value ushr 24).toByte()
        data[offset + 1] = (value ushr 16).toByte()
        data[offset + 2] = (value ushr 8).toByte()
        data[offset + 3] = value.toByte()
    }
}
