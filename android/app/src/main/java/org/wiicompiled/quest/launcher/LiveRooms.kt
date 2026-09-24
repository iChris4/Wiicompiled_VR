package org.wiicompiled.quest.launcher

import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.IOException
import java.util.concurrent.Executors

/**
 * Who plays on Retro WFC now, for the Rooms page, the sidebar's player count and the profiles'
 * Online glow: WheelWizard's RRLiveRooms. While the launcher is on screen it asks every 40 seconds
 * for the open rooms and the top 50 of the leaderboard ([Leaderboard], shared with its page),
 * splits the rooms Retro WFC merged by mistake, and marks the players of the top 50 with their rank.
 */
object LiveRooms {
    private const val TAG = "WiiCompiledLauncher"
    private const val REFRESH_MS = 40_000L

    /** A player in a room (RrPlayer). Two are the same player when their profile and friend code are. */
    class Player(
        val pid: String,
        val name: String,
        val friendCode: String,
        val vr: Int?,
        val br: Int?,
        val isOpenHost: Boolean,
        val isSuspended: Boolean,
        val mii: Mii?,
        /** Their place in the top 50, or null outside it. */
        val leaderboardRank: Int?,
    ) {
        val vrText: String get() = vr?.toString() ?: "--"
        val topLabel: String get() = leaderboardRank?.let { "#$it" }.orEmpty()

        override fun equals(other: Any?): Boolean = other is Player && pid == other.pid && friendCode == other.friendCode
        override fun hashCode(): Int = 31 * pid.hashCode() + friendCode.hashCode()
    }

    /** A room (RrRoom), created at [created] in epoch milliseconds. */
    class Room(val id: String, val created: Long, val type: String, val rk: String?, val players: List<Player>) {
        val isPublic: Boolean get() = type != "private"
        val gameMode: String get() = gameMode(rk, isPublic)

        /** The players' VR averaged and truncated, as the PC shows it; 0 when none has a VR. */
        val averageVr: Int
            get() = players.mapNotNull { it.vr }.takeIf { it.isNotEmpty() }?.average()?.toInt() ?: 0
    }

    enum class TimeUnit { Days, Hours, Minutes, Seconds }

    private val main by lazy { Handler(Looper.getMainLooper()) }
    private val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "LiveRooms").apply { isDaemon = true } }
    private val listeners = LinkedHashSet<() -> Unit>()
    private var running = false
    private var fetching = false

    /** The rooms of the last answer, empty until one comes or when Retro WFC failed; main thread only. */
    var rooms: List<Room> = emptyList()
        private set

    val playerCount: Int get() = rooms.sumOf { it.players.size }

    /** Everyone in a room, as GameLicenseService.RefreshOnlineStatus finds a licence online. */
    val onlineFriendCodes: Set<String> get() = rooms.flatMap { room -> room.players.map { it.friendCode } }.toSet()

    private val ticker = object : Runnable {
        override fun run() {
            fetch()
            main.postDelayed(this, REFRESH_MS)
        }
    }

    /** [listener] is called on the main thread after every answer, until it is removed. */
    fun addListener(listener: () -> Unit) {
        listeners += listener
    }

    fun removeListener(listener: () -> Unit) {
        listeners -= listener
    }

    /** Asks now, then every 40 seconds, while the launcher is on screen. */
    fun start() {
        if (running) return
        running = true
        main.post(ticker)
    }

    fun stop() {
        running = false
        main.removeCallbacks(ticker)
    }

    private fun fetch() {
        if (fetching) return
        fetching = true
        worker.execute {
            val fresh = try {
                build(RetroWfc.roomStatus(), topPlayers())
            } catch (e: IOException) {
                Log.i(TAG, "Retro WFC's rooms are unavailable: ${e.message}")
                emptyList()
            }
            main.post {
                fetching = false
                rooms = fresh
                listeners.toList().forEach { it() }
            }
        }
    }

    /** The rooms go on without ranks while the leaderboard is unavailable. */
    private fun topPlayers(): List<RetroWfc.LeaderboardEntry>? = try {
        Leaderboard.top()
    } catch (e: IOException) {
        Log.i(TAG, "Retro WFC's leaderboard is unavailable: ${e.message}")
        null
    }

    // What the pages show, worked out apart from Android so it can be tested.

    /** RRLiveRooms.ExecuteTaskAsync: rooms split where they were merged, their players with their Miis and ranks. */
    internal fun build(rooms: List<RetroWfc.Room>, leaderboard: List<RetroWfc.LeaderboardEntry>?): List<Room> {
        val byPid = leaderboard.orEmpty().filter { it.pid.isNotBlank() }.groupBy { it.pid }.mapValues { it.value.first() }
        val byFriendCode = leaderboard.orEmpty().filter { it.friendCode.isNotBlank() }.groupBy { it.friendCode }
            .mapValues { it.value.first() }
        return splitMergedRooms(rooms).map { room ->
            Room(
                id = room.id,
                created = room.created,
                type = room.type,
                rk = room.rk,
                players = room.players.map { player ->
                    val entry = byPid[player.pid] ?: player.friendCode.takeIf { it.isNotBlank() }?.let { byFriendCode[it] }
                    Player(
                        pid = player.pid,
                        name = player.name,
                        friendCode = player.friendCode,
                        vr = player.vr,
                        br = player.br,
                        isOpenHost = player.isOpenHost,
                        isSuspended = player.isSuspended,
                        mii = player.mii?.let { data -> runCatching { MiiData.parse(data) }.getOrNull() },
                        leaderboardRank = entry?.rank ?: entry?.activeRank,
                    )
                },
            )
        }
    }

    /**
     * SplitMergedRooms (after kevinvg207's rr-rooms): players only linked both ways in their
     * connection maps share a room, so a room whose players fall into several such groups is
     * really several rooms. A room without any link is left whole.
     */
    internal fun splitMergedRooms(rooms: List<RetroWfc.Room>): List<RetroWfc.Room> = rooms.flatMap { room ->
        val count = room.players.size
        val links = List(count) { ArrayList<Int>() }
        var anyLink = false
        for (i in 0 until count) {
            val map = room.players[i].connectionMap
            if (map.isEmpty()) continue
            // The map usually lists every player, the player included, but can leave them out.
            val includesSelf = map.size == count
            val excludesSelf = map.size == count - 1
            if (!includesSelf && !excludesSelf) continue
            for (j in map.indices) {
                if ((map[j].firstOrNull() ?: '0') == '0') continue
                if (includesSelf && j == i) continue
                val other = if (includesSelf) j else if (j >= i) j + 1 else j
                if (other !in 0 until count) continue
                links[i] += other
                anyLink = true
            }
        }
        if (!anyLink) return@flatMap listOf(room)

        val seen = BooleanArray(count)
        val groups = ArrayList<List<Int>>()
        for (start in 0 until count) {
            if (seen[start]) continue
            val stack = ArrayDeque<Int>().apply { addLast(start) }
            val group = ArrayList<Int>()
            while (stack.isNotEmpty()) {
                val u = stack.removeLast()
                if (seen[u]) continue
                seen[u] = true
                group += u
                for (v in links[u]) if (u in links[v]) stack.addLast(v)
            }
            groups += group.sorted()
        }
        if (groups.size == 1) {
            listOf(room)
        } else {
            groups.map { group -> RetroWfc.Room(room.id, room.type, room.created, room.rk, room.suspended, group.map { room.players[it] }) }
        }
    }

    /** RoomsPage.PerformSearch: every player whose name or friend code holds [query], once each. */
    fun search(rooms: List<Room>, query: String): List<Player> {
        val wanted = query.trim()
        if (wanted.isEmpty()) return emptyList()
        return rooms.flatMap { it.players }
            .filter { it.name.contains(wanted, ignoreCase = true) || it.friendCode.contains(wanted, ignoreCase = true) }
            .distinct()
    }

    /** tTime: a duration by its two largest units, the second left out when it is zero. */
    fun timeParts(millis: Long): List<Pair<TimeUnit, Int>> {
        val total = maxOf(0L, millis / 1000)
        val days = (total / 86_400).toInt()
        val hours = (total / 3_600 % 24).toInt()
        val minutes = (total / 60 % 60).toInt()
        val seconds = (total % 60).toInt()
        fun pair(first: Pair<TimeUnit, Int>, second: Pair<TimeUnit, Int>) = if (second.second == 0) listOf(first) else listOf(first, second)
        return when {
            days >= 1 -> pair(TimeUnit.Days to days, TimeUnit.Hours to hours)
            total >= 3_600 -> pair(TimeUnit.Hours to hours, TimeUnit.Minutes to minutes)
            total >= 60 -> pair(TimeUnit.Minutes to minutes, TimeUnit.Seconds to seconds)
            else -> listOf(TimeUnit.Seconds to seconds)
        }
    }

    /** RrRoom.GameMode: the name of a room's ranking, which tells the pack and the mode. */
    fun gameMode(rk: String?, isPublic: Boolean): String = GAME_MODES[rk] ?: if (isPublic) "Unknown Mode" else "Private Room"

    private val GAME_MODES = mapOf(
        // Retro Rewind
        "vs_10" to "RR 150CC",
        "vs_11" to "RR Time Tr",
        "vs_12" to "RR 200CC",
        "vs_13" to "RR Item Rain",
        "vs_14" to "RR Battle",
        "vs_15" to "RR Elim Battle",
        "vs_20" to "RR 150CC CTs",
        "vs_21" to "RR Vanilla",
        // CTGP
        "vs_668" to "CTGP-C",
        // Insane Kart Wii
        "vs_69" to "Insane Kart",
        "vs_70" to "Ultras VS",
        "vs_71" to "Crazy Items",
        "vs_72" to "Bob-omb Blast",
        "vs_73" to "Inf Accel",
        "vs_74" to "Banan Slip",
        "vs_75" to "Rand Items",
        "vs_76" to "Unfair Items",
        "vs_77" to "Blue Madness",
        "vs_78" to "Mush Dash",
        "vs_79" to "Bumper Karts",
        "vs_80" to "Item Rampage",
        "vs_81" to "Item Rain",
        "vs_82" to "Shell Break",
        "vs_83" to "Riibalanced",
        // Luminous
        "vs_666" to "Luminous",
        "vs_667" to "Luminous TT",
        // OptPack
        "vs_875" to "OP 150",
        "vs_876" to "OP TT",
        "vs_877" to "OP R1",
        "vs_878" to "OP R2",
        "vs_879" to "OP R3",
        "vs_880" to "OP R4",
        // WTP
        "vs_1312" to "WTP 150CC",
        "vs_1313" to "WTP 200CC",
        "vs_1314" to "WTP Time Trial",
        "vs_1315" to "WTP Item Rain",
        "vs_1316" to "WTP STYD",
        // Generic
        "vs_751" to "Versus",
        "vs_-1" to "Regular",
        "vs" to "Regular",
    )
}
