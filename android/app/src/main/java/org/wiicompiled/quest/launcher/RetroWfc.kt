package org.wiicompiled.quest.launcher

import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.time.Instant
import java.time.format.DateTimeParseException
import java.util.Base64
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import org.wiicompiled.quest.BuildConfig

/**
 * Retro WFC, the server Retro Rewind plays online on, asked what WheelWizard asks it
 * (Features/RrRooms/IRwfcApi): the rooms open now, the top of the leaderboard, a player's profile,
 * which carries their Mii, and their VR history. Also WheelWizard's own badges (badges.json), by
 * friend code.
 *
 * Values are read with plain `opt` and type checks, as in [GameBanana], so Android's org.json and
 * the JVM one the tests use agree.
 */
object RetroWfc {

    private const val API = "https://rwfc.net/api"
    private const val ROOM_STATUS_URL = "$API/roomstatus"
    private const val BADGES_URL = "https://raw.githubusercontent.com/TeamWheelWizard/WheelWizard-Data/main/badges.json"
    private const val TIMEOUT_MS = 20_000

    private val userAgent: String get() = "WiiCompiledVR-Quest/${BuildConfig.VERSION_NAME}"

    /** One VR change, at [time] in epoch milliseconds, leaving the player at [total]. */
    data class HistoryEntry(val time: Long, val change: Int, val total: Int)

    /** RwfcPlayerVrHistoryResponse: the entries of a period, oldest first, and its totals. */
    data class History(
        val from: Long,
        val to: Long,
        val starting: Int,
        val ending: Int,
        val totalChange: Int,
        val entries: List<HistoryEntry>,
    )

    /** WheelWizard's BadgeVariant, with the tip its Badge component shows. */
    enum class Badge(val tip: String) {
        WhWzDev("Wheel Wizard Developer (hiii!)"),
        RrDev("Retro Rewind Developer"),
        Translator("Translator"),
        TranslatorLead("Translator Lead"),
        Heart("Heart of the Community"),
        Firestarter_GoldWinner("Firestarter Tournament Winner"),
        Firestarter_SilverWinner("Firestarter Tournament Runner-Up"),
        Firestarter_BronzeWinner("Firestarter Tournament Runner-Up"),
        SummitShowdown_GoldWinner("Summit Showdown Tournament Winner"),
        SummitShowdown_SilverWinner("Summit Showdown Tournament Runner-Up"),
        SummitShowdown_BronzeWinner("Summit Showdown Tournament Runner-Up"),
        Leafstruck_GoldWinner("Leafstruck Tournament Winner"),
        Leafstruck_SilverWinner("Leafstruck Tournament Runner-Up"),
        Leafstruck_BronzeWinner("Leafstruck Tournament Runner-Up"),
    }

    /** A player in an open room (RwfcRoomStatusPlayer); [mii] is the 74 bytes of their Mii. */
    class RoomPlayer(
        val pid: String,
        val name: String,
        val friendCode: String,
        val vr: Int?,
        val br: Int?,
        val isOpenHost: Boolean,
        val isSuspended: Boolean,
        val mii: ByteArray?,
        val connectionMap: List<String>,
    )

    /** An open room (RwfcRoomStatusRoom), created at [created] in epoch milliseconds. */
    class Room(
        val id: String,
        val type: String,
        val created: Long,
        val rk: String?,
        val suspended: Boolean,
        val players: List<RoomPlayer>,
    )

    /** A row of the leaderboard (RwfcLeaderboardEntry), as far as the rooms use it. */
    class LeaderboardEntry(val pid: String, val friendCode: String, val rank: Int?, val activeRank: Int?)

    /** RwfcLeaderboardVrStats: the VR won or lost lately. */
    class VrStats(val last24Hours: Int, val lastWeek: Int, val lastMonth: Int)

    /** PlayerProfileResponse: what Retro WFC knows of a player; [mii] is the 74 bytes of their Mii. */
    class PlayerProfile(
        val name: String,
        val friendCode: String,
        val vr: Int,
        val rank: Int,
        val lastSeen: Long?,
        val isSuspicious: Boolean,
        val vrStats: VrStats?,
        val mii: ByteArray?,
    )

    fun profileUrl(friendCode: String): String = "$API/leaderboard/player/${encode(friendCode)}"

    fun topUrl(limit: Int): String = "$API/leaderboard/top/$limit"

    fun historyUrl(friendCode: String, days: Int): String = "${profileUrl(friendCode)}/history?days=$days"

    private fun encode(text: String): String = URLEncoder.encode(text, "UTF-8")

    // Parsing

    /** What Retro WFC last saw of a profile's Mii: its 74 bytes, and its own picture of it. */
    class PlayerMii(val data: ByteArray?, val image: ByteArray?)

    /**
     * The 74 bytes of the Mii a profile last played with (the Wii's RFLCharData, as the game sent
     * it), or null without one.
     */
    fun parseMiiData(json: String): ByteArray? = parsing { miiBytes(JSONObject(json).text("miiData")) }

    /** The PNG of the player's Mii a profile carries (64 by 64 pixels), or null without one. */
    fun parseMiiImage(json: String): ByteArray? = parsing {
        val encoded = JSONObject(json).text("miiImageBase64") ?: return@parsing null
        try {
            // The MIME decoder skips line breaks, should a server ever wrap the text, and anything
            // else that is not Base64, which leaves nothing.
            Base64.getMimeDecoder().decode(encoded).takeIf { it.isNotEmpty() }
        } catch (e: IllegalArgumentException) {
            null
        }
    }

    fun parseHistory(json: String): History = parsing {
        val root = JSONObject(json)
        val entries = (root.opt("history") as? JSONArray)?.let { array ->
            (0 until array.length()).mapNotNull { index ->
                val entry = array.opt(index) as? JSONObject ?: return@mapNotNull null
                HistoryEntry(
                    time = time(entry.text("date")) ?: return@mapNotNull null,
                    change = entry.number("vrChange"),
                    total = entry.number("totalVR"),
                )
            }
        }.orEmpty().sortedBy { it.time }
        History(
            from = time(root.text("fromDate")) ?: entries.firstOrNull()?.time ?: 0L,
            to = time(root.text("toDate")) ?: entries.lastOrNull()?.time ?: 0L,
            starting = root.number("startingVR"),
            ending = root.number("endingVR"),
            totalChange = root.number("totalVRChange"),
            entries = entries,
        )
    }

    /**
     * The rooms open now (RwfcRoomStatusResponse). A room or player without its ID, or a room
     * without its type or creation time, which the PC could not read either, is left out.
     */
    fun parseRoomStatus(json: String): List<Room> = parsing {
        objects(JSONObject(json).opt("rooms")).mapNotNull { room ->
            Room(
                id = room.text("id") ?: return@mapNotNull null,
                type = room.text("type") ?: return@mapNotNull null,
                created = time(room.text("created")) ?: return@mapNotNull null,
                rk = room.text("rk"),
                // The PC's "suspend"; Retro WFC now calls it "isSuspended".
                suspended = (room.opt("isSuspended") ?: room.opt("suspend")) as? Boolean ?: false,
                players = objects(room.opt("players")).mapNotNull { player ->
                    // The Mii is one object, or was a list of them.
                    val mii = player.opt("mii").let { it as? JSONObject ?: objects(it).firstOrNull() }
                    RoomPlayer(
                        pid = player.id("pid") ?: return@mapNotNull null,
                        name = player.opt("name") as? String ?: "",
                        friendCode = player.opt("friendCode") as? String ?: "",
                        vr = (player.opt("vr") as? Number)?.toInt(),
                        br = (player.opt("br") as? Number)?.toInt(),
                        isOpenHost = player.opt("isOpenHost") as? Boolean ?: false,
                        isSuspended = player.opt("isSuspended") as? Boolean ?: false,
                        mii = miiBytes(mii?.text("data")),
                        connectionMap = (player.opt("connectionMap") as? JSONArray)?.let { map ->
                            (0 until map.length()).map { map.opt(it)?.toString().orEmpty() }
                        }.orEmpty(),
                    )
                },
            )
        }
    }

    /** The top of the leaderboard, best first. */
    fun parseLeaderboard(json: String): List<LeaderboardEntry> = parsing {
        objects(JSONArray(json)).mapNotNull { entry ->
            LeaderboardEntry(
                pid = entry.id("pid") ?: return@mapNotNull null,
                friendCode = entry.opt("friendCode") as? String ?: "",
                rank = (entry.opt("rank") as? Number)?.toInt(),
                activeRank = (entry.opt("activeRank") as? Number)?.toInt(),
            )
        }
    }

    fun parsePlayerProfile(json: String): PlayerProfile = parsing {
        val root = JSONObject(json)
        PlayerProfile(
            name = root.opt("name") as? String ?: "",
            friendCode = root.opt("friendCode") as? String ?: "",
            vr = root.number("vr"),
            rank = root.number("rank"),
            lastSeen = time(root.text("lastSeen")),
            isSuspicious = root.opt("isSuspicious") as? Boolean ?: false,
            vrStats = (root.opt("vrStats") as? JSONObject)?.let { stats ->
                VrStats(stats.number("last24Hours"), stats.number("lastWeek"), stats.number("lastMonth"))
            },
            mii = miiBytes(root.text("miiData")),
        )
    }

    /** The 74 bytes of a Mii Retro WFC sent in Base64, or null for none or anything shorter. */
    private fun miiBytes(encoded: String?): ByteArray? = try {
        // The MIME decoder skips line breaks and anything else that is not Base64.
        encoded?.let { Base64.getMimeDecoder().decode(it) }?.takeIf { it.size >= MiiData.SIZE }?.copyOf(MiiData.SIZE)
    } catch (e: IllegalArgumentException) {
        null
    }

    private fun objects(value: Any?): List<JSONObject> =
        (value as? JSONArray)?.let { array -> (0 until array.length()).mapNotNull { array.opt(it) as? JSONObject } }.orEmpty()

    /** WhWzDataSingletonService.LoadBadgesAsync: friend code to badges, unknown badge names left out. */
    fun parseBadges(json: String): Map<String, List<Badge>> = parsing {
        val root = JSONObject(json)
        root.keys().asSequence().associateWith { key ->
            val names = root.opt(key) as? JSONArray ?: return@associateWith emptyList()
            (0 until names.length()).mapNotNull { index ->
                (names.opt(index) as? String)?.let { name -> Badge.entries.firstOrNull { it.name == name } }
            }
        }
    }

    private fun time(text: String?): Long? = try {
        text?.let { Instant.parse(it).toEpochMilli() }
    } catch (e: DateTimeParseException) {
        null
    }

    private fun <T> parsing(block: () -> T): T = try {
        block()
    } catch (e: JSONException) {
        throw IOException("Retro WFC sent something this launcher cannot read.", e)
    }

    private fun JSONObject.text(key: String): String? = (opt(key) as? String)?.takeIf { it.isNotBlank() }
    private fun JSONObject.number(key: String): Int = (opt(key) as? Number)?.toInt() ?: 0

    /** An ID Retro WFC sends as text, or might send as a number. */
    private fun JSONObject.id(key: String): String? = when (val value = opt(key)) {
        is String -> value.takeIf { it.isNotBlank() }
        is Number -> value.toLong().toString()
        else -> null
    }

    // Network; everything below blocks, so callers run it off the main thread.

    /** The Mii of [friendCode]'s profile, or null when Retro WFC has never seen it. */
    fun playerMii(friendCode: String): PlayerMii? =
        get(profileUrl(friendCode), missingIsNull = true)?.let { json -> PlayerMii(parseMiiData(json), parseMiiImage(json)) }

    fun history(friendCode: String, days: Int): History {
        val json = get(historyUrl(friendCode, days), missingIsNull = true)
            ?: throw IOException("Retro WFC has no VR history for $friendCode.")
        return parseHistory(json)
    }

    fun roomStatus(): List<Room> = parseRoomStatus(get(ROOM_STATUS_URL) ?: "{}")

    fun topPlayers(limit: Int): List<LeaderboardEntry> = parseLeaderboard(get(topUrl(limit)) ?: "[]")

    /** What Retro WFC knows of [friendCode], or null when it has never seen them. */
    fun playerProfile(friendCode: String): PlayerProfile? =
        get(profileUrl(friendCode), missingIsNull = true)?.let(::parsePlayerProfile)

    fun badges(): Map<String, List<Badge>> = parseBadges(get(BADGES_URL) ?: "{}")

    private fun get(url: String, missingIsNull: Boolean = false): String? {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = TIMEOUT_MS
            readTimeout = TIMEOUT_MS
            setRequestProperty("User-Agent", userAgent)
        }
        try {
            val code = try {
                connection.responseCode
            } catch (e: IOException) {
                throw IOException("Retro WFC could not be reached. Check the headset's internet connection.", e)
            }
            if (code == HttpURLConnection.HTTP_NOT_FOUND && missingIsNull) return null
            if (code !in 200..299) throw IOException("${URL(url).host} answered $code.")
            return connection.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
        } finally {
            connection.disconnect()
        }
    }
}
