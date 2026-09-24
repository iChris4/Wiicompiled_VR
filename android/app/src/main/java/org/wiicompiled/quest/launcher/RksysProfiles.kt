package org.wiicompiled.quest.launcher

import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.security.MessageDigest

/**
 * The licences of a Mario Kart Wii save, read as WheelWizard's GameLicenseService reads rksys.dat
 * (after kazuki-4ys' FaceThief and https://wiki.tockdom.com/wiki/Rksys.dat): after the RKSD0006
 * magic come four RKPD blocks, one per licence, each holding its Mii's name and ID, the profile ID
 * its friend code derives from, its VR and BR, and race counts. Retro Rewind keeps the ratings it plays
 * with in Pulsar's RRRating.pul, which outranks the save's own when it knows the profile. Each
 * licence's friend list is [RksysFriends]', which also changes it.
 */
object RksysProfiles {

    const val SLOTS = 4

    private const val MAGIC = "RKSD0006"
    private const val LICENSE_MAGIC = "RKPD"
    private const val LICENSE_SIZE = 0x8CC0
    /** Where the licences end: the magic, then the four blocks. The rest of the save is not read. */
    const val LICENSES_END = 8 + SLOTS * LICENSE_SIZE

    private const val NAME_OFFSET = 0x14
    private const val NAME_CHARS = 10
    /** The Mii's ID in the Mii database, which the PC calls its avatar ID. */
    private const val MII_ID_OFFSET = 0x28
    private const val PROFILE_ID_OFFSET = 0x5C
    private const val VR_OFFSET = 0xB0
    private const val BR_OFFSET = 0xB2
    private const val RACES_OFFSET = 0xB4
    private const val WINS_OFFSET = 0xDC

    private const val RATING_MAGIC = 0x52525254L // "RRRT"
    private const val RATING_VERSION = 1
    private const val RATING_ENTRIES = 100
    private const val RATING_ENTRY_SIZE = 16
    private const val RATING_HAS_DATA = 1L

    /**
     * One licence as the profiles page shows it. [friendCode] is empty for a licence never taken
     * online, which has no profile ID yet.
     */
    data class License(
        val slot: Int,
        val name: String,
        val profileId: Long,
        val friendCode: String,
        val vr: Int,
        val br: Int,
        val races: Long,
        val wins: Long,
        /** The licence's Mii in the Mii database; a guest Mii's (0x80000001 and on) is in none. */
        val miiId: Long = 0,
        val friends: List<RksysFriends.Friend> = emptyList(),
    )

    /** A rating from RRRating.pul, already as the game shows it (60.50 is 6050). */
    data class Rating(val vr: Int, val br: Int)

    /**
     * The four licence slots of [save], an empty one null, or null when [save] is not a Mario Kart
     * Wii save. [ratings] are RRRating.pul's, by profile ID.
     */
    fun parse(save: ByteArray, ratings: Map<Long, Rating> = emptyMap()): List<License?>? {
        if (save.size < LICENSES_END || String(save, 0, MAGIC.length, Charsets.US_ASCII) != MAGIC) return null
        return (0 until SLOTS).map { slot ->
            val base = MAGIC.length + slot * LICENSE_SIZE
            if (String(save, base, LICENSE_MAGIC.length, Charsets.US_ASCII) != LICENSE_MAGIC) return@map null
            val profileId = u32(save, base + PROFILE_ID_OFFSET)
            val rating = if (profileId != 0L) ratings[profileId] else null
            License(
                slot = slot,
                name = String(save, base + NAME_OFFSET, NAME_CHARS * 2, Charsets.UTF_16BE).substringBefore('\u0000').trim(),
                profileId = profileId,
                friendCode = friendCode(profileId),
                vr = rating?.vr ?: u16(save, base + VR_OFFSET),
                br = rating?.br ?: u16(save, base + BR_OFFSET),
                races = u32(save, base + RACES_OFFSET),
                wins = u32(save, base + WINS_OFFSET),
                miiId = u32(save, base + MII_ID_OFFSET),
                friends = RksysFriends.parse(save, slot),
            )
        }
    }

    /**
     * FriendCodeGenerator.GetFriendCode: the profile ID with, above it, the top seven bits of the
     * MD5 of the ID (little-endian) followed by "JCMR", written as three groups of four digits.
     * Empty for ID 0.
     */
    fun friendCode(profileId: Long): String {
        if (profileId == 0L) return ""
        val id = profileId.toInt()
        val input = byteArrayOf(id.toByte(), (id ushr 8).toByte(), (id ushr 16).toByte(), (id ushr 24).toByte(), 0x4A, 0x43, 0x4D, 0x52)
        val check = (MessageDigest.getInstance("MD5").digest(input)[0].toInt() and 0xFF) ushr 1
        val digits = "%012d".format((check.toLong() shl 32) or profileId)
        return "${digits.substring(0, 4)}-${digits.substring(4, 8)}-${digits.substring(8)}"
    }

    /**
     * RRratingReader: RRRating.pul is "RRRT", version 1 and 100 entries of profile ID, VR and BR
     * as floats, and flags. The PC multiplies by 100 and rounds half to even, as .NET's Math.Round.
     */
    fun parseRatings(pul: ByteArray): Map<Long, Rating> {
        if (pul.size < 8 + RATING_ENTRIES * RATING_ENTRY_SIZE) return emptyMap()
        if (u32(pul, 0) != RATING_MAGIC || u16(pul, 4) != RATING_VERSION || u16(pul, 6) != RATING_ENTRIES) return emptyMap()
        val ratings = HashMap<Long, Rating>()
        for (index in 0 until RATING_ENTRIES) {
            val entry = 8 + index * RATING_ENTRY_SIZE
            val profileId = u32(pul, entry)
            if (u32(pul, entry + 12) and RATING_HAS_DATA == 0L || profileId == 0L) continue
            ratings[profileId] = Rating(rating(pul, entry + 4), rating(pul, entry + 8))
        }
        return ratings
    }

    private fun rating(data: ByteArray, offset: Int): Int = Math.rint((Float.fromBits(u32(data, offset).toInt()) * 100f).toDouble()).toInt()

    /** The part of [file] the licences occupy, or null when it is not there or too short to be a save. */
    fun readLicenses(file: File): ByteArray? = try {
        RandomAccessFile(file, "r").use { input ->
            if (input.length() < LICENSES_END) return null
            ByteArray(LICENSES_END).also { input.readFully(it) }
        }
    } catch (e: IOException) {
        null
    }

    private fun u16(data: ByteArray, offset: Int): Int = ((data[offset].toInt() and 0xFF) shl 8) or (data[offset + 1].toInt() and 0xFF)

    private fun u32(data: ByteArray, offset: Int): Long =
        ((data[offset].toLong() and 0xFF) shl 24) or ((data[offset + 1].toLong() and 0xFF) shl 16) or
            ((data[offset + 2].toLong() and 0xFF) shl 8) or (data[offset + 3].toLong() and 0xFF)
}
