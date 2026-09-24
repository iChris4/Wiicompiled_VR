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
 * Retro WFC, the server Retro Rewind plays online on, asked what WheelWizard's profile page asks it
 * (Features/RrRooms/IRwfcApi): a player's profile, which carries a picture of their Mii, their VR
 * history, and the rooms open now. Also WheelWizard's own badges (badges.json), by friend code.
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

    fun profileUrl(friendCode: String): String = "$API/leaderboard/player/${encode(friendCode)}"

    fun historyUrl(friendCode: String, days: Int): String = "${profileUrl(friendCode)}/history?days=$days"

    private fun encode(text: String): String = URLEncoder.encode(text, "UTF-8")

    // Parsing

    /** What Retro WFC last saw of a profile's Mii: its 74 bytes, and its own picture of it. */
    class PlayerMii(val data: ByteArray?, val image: ByteArray?)

    /**
     * The 74 bytes of the Mii a profile last played with (the Wii's RFLCharData, as the game sent
     * it), or null without one.
     */
    fun parseMiiData(json: String): ByteArray? = parsing {
        val encoded = JSONObject(json).text("miiData") ?: return@parsing null
        try {
            Base64.getMimeDecoder().decode(encoded).takeIf { it.size >= MiiData.SIZE }?.copyOf(MiiData.SIZE)
        } catch (e: IllegalArgumentException) {
            null
        }
    }

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

    /** The friend codes of everyone in a room now (RRLiveRooms, as GameLicenseService.RefreshOnlineStatus uses it). */
    fun parseOnlineFriendCodes(json: String): Set<String> = parsing {
        val rooms = JSONObject(json).opt("rooms") as? JSONArray ?: return@parsing emptySet()
        (0 until rooms.length()).flatMap { index ->
            val players = (rooms.opt(index) as? JSONObject)?.opt("players") as? JSONArray ?: return@flatMap emptyList()
            (0 until players.length()).mapNotNull { (players.opt(it) as? JSONObject)?.text("friendCode") }
        }.toSet()
    }

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

    // Network; everything below blocks, so callers run it off the main thread.

    /** The Mii of [friendCode]'s profile, or null when Retro WFC has never seen it. */
    fun playerMii(friendCode: String): PlayerMii? =
        get(profileUrl(friendCode), missingIsNull = true)?.let { json -> PlayerMii(parseMiiData(json), parseMiiImage(json)) }

    fun history(friendCode: String, days: Int): History {
        val json = get(historyUrl(friendCode, days), missingIsNull = true)
            ?: throw IOException("Retro WFC has no VR history for $friendCode.")
        return parseHistory(json)
    }

    fun onlineFriendCodes(): Set<String> = parseOnlineFriendCodes(get(ROOM_STATUS_URL) ?: "{}")

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
