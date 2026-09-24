package org.wiicompiled.quest.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** The rooms are worked out from Retro WFC's answers as WheelWizard's RRLiveRooms and RrRoom do. */
class LiveRoomsTest {

    private fun player(pid: String, friendCode: String = "", name: String = pid, vr: Int? = null, map: List<String> = emptyList(), mii: ByteArray? = null) =
        RetroWfc.RoomPlayer(pid, name, friendCode, vr, null, isOpenHost = false, isSuspended = false, mii = mii, connectionMap = map)

    private fun room(id: String, vararg players: RetroWfc.RoomPlayer, type: String = "anybody", rk: String? = "vs_10") =
        RetroWfc.Room(id, type, created = 0L, rk = rk, suspended = false, players = players.toList())

    @Test
    fun roomsRetroWfcMergedAreSplitWhereTheirPlayersAreNotLinked() {
        // Two pairs linked both ways, the map listing every player, each player included.
        val merged = room(
            "MERGED",
            player("a", map = listOf("0", "1", "0", "0")),
            player("b", map = listOf("1", "0", "0", "0")),
            // c points at a, but a does not point back: no link.
            player("c", map = listOf("1", "0", "0", "1")),
            player("d", map = listOf("0", "0", "1", "0")),
        )
        val split = LiveRooms.splitMergedRooms(listOf(merged))
        assertEquals(listOf(listOf("a", "b"), listOf("c", "d")), split.map { r -> r.players.map { it.pid } })
        assertEquals(listOf("MERGED", "MERGED"), split.map { it.id })

        // A map can leave the player out: b's first entry is a, a's first is b.
        val withoutSelf = room(
            "THREE",
            player("a", map = listOf("1", "0")),
            player("b", map = listOf("1", "0")),
            player("c", map = listOf("0", "0")),
        )
        assertEquals(listOf(listOf("a", "b"), listOf("c")), LiveRooms.splitMergedRooms(listOf(withoutSelf)).map { r -> r.players.map { it.pid } })

        // Without any link, or all linked, a room stays whole.
        val unlinked = room("WHOLE", player("a"), player("b"))
        assertEquals(listOf(unlinked), LiveRooms.splitMergedRooms(listOf(unlinked)))
        val linked = room("PAIR", player("a", map = listOf("0", "1")), player("b", map = listOf("1", "0")))
        assertEquals(listOf(linked), LiveRooms.splitMergedRooms(listOf(linked)))
    }

    @Test
    fun playersCarryTheirLeaderboardPlaceAndMii() {
        val mii = MiiFactory.male("Racer")
        val rooms = listOf(
            room(
                "ROOM",
                player("1", "1111-1111-1111", vr = 9000, mii = MiiData.serialize(mii)),
                player("2", "2222-2222-2222", vr = 5001, mii = ByteArray(MiiData.SIZE) { 0xFF.toByte() }),
                player("3", "3333-3333-3333"),
            ),
        )
        val leaderboard = listOf(
            RetroWfc.LeaderboardEntry("1", "", rank = 4, activeRank = null),
            // Found by friend code, placed by its active rank.
            RetroWfc.LeaderboardEntry("99", "2222-2222-2222", rank = null, activeRank = 12),
        )
        val (first, second, third) = LiveRooms.build(rooms, leaderboard).single().players
        assertEquals(4, first.leaderboardRank)
        assertEquals("#4", first.topLabel)
        assertEquals(mii, first.mii)
        assertEquals(12, second.leaderboardRank)
        // Bytes the PC cannot read as a Mii are no Mii.
        assertNull(second.mii)
        assertNull(third.leaderboardRank)
        assertEquals("", third.topLabel)
        assertEquals("--", third.vrText)
        // Without the leaderboard, nobody has a place.
        assertEquals(listOf(null, null, null), LiveRooms.build(rooms, null).single().players.map { it.leaderboardRank })
        // The average leaves out players without a VR and drops the fraction.
        assertEquals(7000, LiveRooms.build(rooms, null).single().averageVr)
        assertEquals(0, LiveRooms.build(listOf(room("EMPTY")), null).single().averageVr)
    }

    @Test
    fun searchFindsNamesAndFriendCodesOnce() {
        val rooms = LiveRooms.build(
            listOf(
                room("A", player("1", "1111-2222-3333", name = "Mario"), player("2", "4444-5555-6666", name = "Luigi")),
                room("B", player("1", "1111-2222-3333", name = "Mario"), player("3", "7777-8888-9999", name = "Peach")),
            ),
            null,
        )
        assertEquals(listOf("Mario"), LiveRooms.search(rooms, "mar").map { it.name })
        assertEquals(listOf("Luigi"), LiveRooms.search(rooms, "5555").map { it.name })
        assertEquals(emptyList<LiveRooms.Player>(), LiveRooms.search(rooms, "   "))
    }

    @Test
    fun timeOnlineIsItsTwoLargestUnits() {
        fun parts(seconds: Long) = LiveRooms.timeParts(seconds * 1000)
        assertEquals(listOf(LiveRooms.TimeUnit.Seconds to 45), parts(45))
        assertEquals(listOf(LiveRooms.TimeUnit.Minutes to 1, LiveRooms.TimeUnit.Seconds to 30), parts(90))
        assertEquals(listOf(LiveRooms.TimeUnit.Hours to 1), parts(3_600))
        assertEquals(listOf(LiveRooms.TimeUnit.Hours to 23, LiveRooms.TimeUnit.Minutes to 59), parts(86_399))
        // A day and five minutes shows the day alone: its hours are zero.
        assertEquals(listOf(LiveRooms.TimeUnit.Days to 1), parts(86_700))
        assertEquals(listOf(LiveRooms.TimeUnit.Days to 2, LiveRooms.TimeUnit.Hours to 3), parts(2 * 86_400 + 3 * 3_600 + 59))
        // A room from a clock ahead of the headset's has just opened.
        assertEquals(listOf(LiveRooms.TimeUnit.Seconds to 0), parts(-5))
    }

    @Test
    fun gameModesAreThePcs() {
        assertEquals("RR 150CC", LiveRooms.gameMode("vs_10", isPublic = true))
        assertEquals("RR Elim Battle", LiveRooms.gameMode("vs_15", isPublic = true))
        assertEquals("Regular", LiveRooms.gameMode("vs", isPublic = true))
        assertEquals("Unknown Mode", LiveRooms.gameMode("unknown", isPublic = true))
        assertEquals("Private Room", LiveRooms.gameMode(null, isPublic = false))
        val private = LiveRooms.build(listOf(room("P", type = "private", rk = "unknown")), null).single()
        assertEquals(false, private.isPublic)
        assertEquals("Private Room", private.gameMode)
    }
}
