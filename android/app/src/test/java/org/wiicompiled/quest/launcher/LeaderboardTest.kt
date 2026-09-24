package org.wiicompiled.quest.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

/** The leaderboard page ranks Retro WFC's top players as WheelWizard's LeaderboardPage does. */
class LeaderboardTest {

    private fun entry(pid: String, rank: Int? = null, activeRank: Int? = null, name: String = pid, mii: ByteArray? = null) =
        RetroWfc.LeaderboardEntry(pid, "$pid-0000-0000", rank, activeRank, name = name, vr = 1000, mii = mii)

    @Test
    fun rowsAreOrderedByTheirResolvedRank() {
        val rows = Leaderboard.rows(
            listOf(
                entry("a", rank = 3),
                // No rank: its active rank.
                entry("b", activeRank = 1),
                // Neither, or ranks beyond 50,000: its place in the answer, here the third.
                entry("c", rank = 0, activeRank = 50_001),
                // Sharing a rank keeps Retro WFC's order.
                entry("d", rank = 3),
            ),
        )
        assertEquals(listOf("b", "a", "c", "d"), rows.map { it.name })
        assertEquals(listOf(1, 3, 3, 3), rows.map { it.rank })
    }

    @Test
    fun atMostFiftyRowsWithTheirMiiAndBlankNamesKept() {
        val mii = MiiFactory.female("Racer")
        val entries = (1..60).map { index -> entry("p$index", rank = index) } +
            listOf(entry("first", rank = 1, name = " ", mii = MiiData.serialize(mii)), entry("broken", rank = 2, mii = ByteArray(MiiData.SIZE) { 0xFF.toByte() }))
        val rows = Leaderboard.rows(entries)
        assertEquals(50, rows.size)
        assertEquals(listOf("p1", " ", "p2", "broken"), rows.take(4).map { it.name })
        assertEquals(mii, rows[1].mii)
        // Bytes the PC cannot read as a Mii are no Mii.
        assertNull(rows[3].mii)
        assertNull(rows[0].mii)
    }

    @Test
    fun viewRoomFindsThePlayersRoomByTheirFriendCodesDigits() {
        fun player(friendCode: String) = LiveRooms.Player("1", "Racer", friendCode, 5000, null, false, false, null, null)
        val first = LiveRooms.Room("AAAA", 0L, "anybody", "vs_10", listOf(player("1111-2222-3333")))
        val second = LiveRooms.Room("BBBB", 0L, "anybody", "vs_10", listOf(player("4444-5555-6666")))
        val rooms = listOf(first, second)
        assertSame(second, Leaderboard.roomOf(rooms, "444455556666"))
        assertSame(first, Leaderboard.roomOf(rooms, "1111-2222-3333"))
        assertNull(Leaderboard.roomOf(rooms, "7777-8888-9999"))
        assertNull(Leaderboard.roomOf(rooms, ""))
    }
}
