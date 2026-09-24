package org.wiicompiled.quest.launcher

import android.os.SystemClock
import java.io.IOException

/**
 * The top 50 of Retro WFC's leaderboard: WheelWizard's RrLeaderboardSingletonService, which the
 * Rooms page ([LiveRooms]) and the Leaderboard page share, so one answer serves both for 90
 * seconds and the last one stands in while Retro WFC fails; and how LeaderboardPage ranks it.
 */
object Leaderboard {
    const val SIZE = 50
    private const val FRESH_MS = 90_000L
    /** ResolveRank trusts a rank this far down; beyond it the row's place in the answer is used. */
    private const val MAX_RANK = 50_000

    /** A player of the top 50 as the page shows them (LeaderboardPlayerItem); [name] can be blank. */
    class Row(
        val rank: Int,
        val name: String,
        val friendCode: String,
        val vr: Int?,
        val isSuspicious: Boolean,
        val mii: Mii?,
    )

    private var cached: List<RetroWfc.LeaderboardEntry>? = null
    private var cachedAt = 0L

    /**
     * The top 50, from the cache while it is fresh, else from Retro WFC, else the last answer;
     * throws when Retro WFC fails and never answered. Blocks, so callers run it off the main thread.
     */
    @Synchronized
    fun top(): List<RetroWfc.LeaderboardEntry> {
        val now = SystemClock.elapsedRealtime()
        cached?.let { if (now - cachedAt < FRESH_MS) return it }
        return try {
            RetroWfc.topPlayers(SIZE).also {
                cached = it
                cachedAt = now
            }
        } catch (e: IOException) {
            cached ?: throw e
        }
    }

    // What the page shows, worked out apart from Android so it can be tested.

    /** ReloadLeaderboardAsync: the rows by rank, at most 50, each with its Mii when it has a valid one. */
    fun rows(entries: List<RetroWfc.LeaderboardEntry>): List<Row> =
        entries.mapIndexed { index, entry -> rank(entry, index) to entry }
            // Stable, as LINQ's OrderBy: players sharing a rank keep Retro WFC's order.
            .sortedBy { it.first }
            .take(SIZE)
            .map { (rank, entry) ->
                Row(
                    rank = rank,
                    name = entry.name,
                    friendCode = entry.friendCode,
                    vr = entry.vr,
                    isSuspicious = entry.isSuspicious,
                    mii = entry.mii?.let { data -> runCatching { MiiData.parse(data) }.getOrNull() },
                )
            }

    /** ResolveRank: the rank, else the active rank, else the place in Retro WFC's answer. */
    private fun rank(entry: RetroWfc.LeaderboardEntry, index: Int): Int =
        entry.rank?.takeIf { it in 1..MAX_RANK } ?: entry.activeRank?.takeIf { it in 1..MAX_RANK } ?: (index + 1)

    /**
     * The room [friendCode] plays in now, for the row's View Room (JoinRoom_OnClick), or null. Codes
     * are compared by their digits, as the PC compares the profile IDs they stand for.
     */
    fun roomOf(rooms: List<LiveRooms.Room>, friendCode: String): LiveRooms.Room? {
        val digits = digits(friendCode)
        if (digits.isEmpty()) return null
        return rooms.firstOrNull { room -> room.players.any { digits(it.friendCode) == digits } }
    }

    private fun digits(friendCode: String): String = friendCode.filter { it in '0'..'9' }
}
