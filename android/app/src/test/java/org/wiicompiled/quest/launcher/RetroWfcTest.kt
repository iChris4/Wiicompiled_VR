package org.wiicompiled.quest.launcher

import java.io.IOException
import java.time.Instant
import java.util.Base64
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.fail
import org.junit.Test

/** The profiles page reads Retro WFC's answers the way WheelWizard does; shapes taken from the live API. */
class RetroWfcTest {

    @Test
    fun historyEntriesComeOldestFirst() {
        val json = """
            {"playerId": "601298307", "fromDate": "2026-08-24T23:31:00.6148765Z", "toDate": "2026-09-23T23:31:00.6148765Z",
             "history": [
               {"date": "2026-09-10T00:38:50.250617Z", "vrChange": 39, "totalVR": 5832},
               {"date": "2026-09-10T00:38:49.250617Z", "vrChange": 0, "totalVR": 5793},
               {"date": "not a date", "vrChange": 1, "totalVR": 1}],
             "totalVRChange": 257, "startingVR": 5793, "endingVR": 6050}
        """.trimIndent()
        val history = RetroWfc.parseHistory(json)
        assertEquals(5793, history.starting)
        assertEquals(6050, history.ending)
        assertEquals(257, history.totalChange)
        assertEquals(Instant.parse("2026-08-24T23:31:00.6148765Z").toEpochMilli(), history.from)
        assertEquals(listOf(5793, 5832), history.entries.map { it.total })
        assertEquals(listOf(0, 39), history.entries.map { it.change })
        assertEquals(1000L, history.entries[1].time - history.entries[0].time)
    }

    @Test
    fun roomsAreReadWithTheirPlayersAndMiis() {
        val mii = MiiData.serialize(MiiFactory.male("Racer").apply { miiId = 0x80000001L })
        val json = """
            {"rooms": [
               {"id": "EVFAQV", "type": "anybody", "created": "2026-09-23T22:48:15.1641988Z", "host": "29", "rk": "vs_10",
                "players": [
                  {"pid": "166930", "name": "Racer", "friendCode": "1111-2222-3333", "vr": 13967, "br": 9999, "isOpenHost": true,
                   "isSuspended": false, "mii": {"data": "${Base64.getEncoder().encodeToString(mii)}", "name": "Racer"},
                   "connectionMap": ["0", "1"], "slotId": "29"},
                  {"pid": 204981, "name": "Other", "friendCode": "4444-5555-6666", "vr": null, "mii": [{"data": "AAEC"}],
                   "connectionMap": []}],
                "averageVR": 13967, "race": {"num": 10, "course": 324, "cc": 2, "trackName": "GCN Wario Colosseum"},
                "roomType": "Retro Tracks", "isPublic": true, "isJoinable": false, "isSuspended": true},
               {"id": "XESURM", "type": "private", "created": "2026-09-13T18:56:58Z", "rk": "unknown", "players": [], "suspend": false},
               {"type": "anybody", "created": "2026-09-13T18:56:58Z", "players": []},
               {"id": "BADDAY", "type": "anybody", "created": "not a date", "players": []}],
             "timestamp": "2026-09-23T23:22:18.9478744Z", "totalPlayers": 2}
        """.trimIndent()
        val rooms = RetroWfc.parseRoomStatus(json)
        // Rooms without their ID or creation time are left out, as the PC could not read them.
        assertEquals(listOf("EVFAQV", "XESURM"), rooms.map { it.id })
        val room = rooms[0]
        assertEquals("anybody", room.type)
        assertEquals(Instant.parse("2026-09-23T22:48:15.1641988Z").toEpochMilli(), room.created)
        assertEquals("vs_10", room.rk)
        assertEquals(true, room.suspended)
        val (racer, other) = room.players
        assertEquals("166930", racer.pid)
        assertEquals("Racer", racer.name)
        assertEquals("1111-2222-3333", racer.friendCode)
        assertEquals(13967, racer.vr)
        assertEquals(9999, racer.br)
        assertEquals(true, racer.isOpenHost)
        assertArrayEquals(mii, racer.mii)
        assertEquals(listOf("0", "1"), racer.connectionMap)
        // A numeric ID, no VR, and a Mii too short to be one.
        assertEquals("204981", other.pid)
        assertNull(other.vr)
        assertNull(other.br)
        assertNull(other.mii)
        assertEquals(false, rooms[1].suspended)
        assertEquals(emptyList<RetroWfc.Room>(), RetroWfc.parseRoomStatus("""{"rooms": null}"""))
    }

    @Test
    fun theLeaderboardGivesRanksByProfileAndFriendCode() {
        val json = """
            [{"pid": "592986326", "name": "s", "friendCode": "1111-2222-3333", "vr": 161570, "rank": 1, "isSuspicious": false},
             {"pid": "2", "friendCode": "", "rank": null, "activeRank": 7},
             {"name": "no pid", "rank": 3}]
        """.trimIndent()
        val entries = RetroWfc.parseLeaderboard(json)
        assertEquals(listOf("592986326", "2"), entries.map { it.pid })
        assertEquals(1, entries[0].rank)
        assertEquals("1111-2222-3333", entries[0].friendCode)
        assertNull(entries[1].rank)
        assertEquals(7, entries[1].activeRank)
    }

    @Test
    fun aPlayersProfileIsReadAsThePcReadsIt() {
        val mii = MiiData.serialize(MiiFactory.female("Profile"))
        val json = """
            {"pid": "601298307", "name": "Profile", "friendCode": "1111-2222-3333", "vr": 6050, "rank": 38439,
             "lastSeen": "2026-09-21T02:43:42.584862Z", "isSuspicious": true,
             "vrStats": {"last24Hours": 0, "lastWeek": -4967, "lastMonth": 257},
             "miiImageBase64": null, "miiData": "${Base64.getEncoder().encodeToString(mii)}", "badges": null}
        """.trimIndent()
        val profile = RetroWfc.parsePlayerProfile(json)
        assertEquals("Profile", profile.name)
        assertEquals("1111-2222-3333", profile.friendCode)
        assertEquals(6050, profile.vr)
        assertEquals(38439, profile.rank)
        assertEquals(Instant.parse("2026-09-21T02:43:42.584862Z").toEpochMilli(), profile.lastSeen)
        assertEquals(true, profile.isSuspicious)
        assertEquals(-4967, profile.vrStats!!.lastWeek)
        assertEquals(257, profile.vrStats!!.lastMonth)
        assertArrayEquals(mii, profile.mii)
        // A profile without stats or a Mii still reads.
        val bare = RetroWfc.parsePlayerProfile("""{"name": "x", "friendCode": "1111-2222-3333"}""")
        assertNull(bare.vrStats)
        assertNull(bare.mii)
        assertNull(bare.lastSeen)
    }

    @Test
    fun badgesAreListedByFriendCode() {
        val json = """
            {"all": ["WhWzDev", "RrDev", "Translator", "TranslatorLead", "Heart"],
             "4343-3434-3434": ["Firestarter_GoldWinner", "SomethingNew", "SummitShowdown_SilverWinner"]}
        """.trimIndent()
        val badges = RetroWfc.parseBadges(json)
        assertEquals(
            listOf(RetroWfc.Badge.Firestarter_GoldWinner, RetroWfc.Badge.SummitShowdown_SilverWinner),
            badges["4343-3434-3434"],
        )
        assertEquals(5, badges["all"]!!.size)
        assertNull(badges["0349-6103-6675"])
    }

    @Test
    fun aProfileCarriesItsMiiPicture() {
        val png = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 1, 2, 3)
        val json = """{"pid": "601298307", "name": "no name", "miiImageBase64": "${Base64.getEncoder().encodeToString(png)}"}"""
        assertArrayEquals(png, RetroWfc.parseMiiImage(json))
        assertNull(RetroWfc.parseMiiImage("""{"pid": "1", "miiImageBase64": null}"""))
        assertNull(RetroWfc.parseMiiImage("""{"pid": "1", "miiImageBase64": "***"}"""))
    }

    @Test
    fun aProfileCarriesItsMii() {
        // The Mii Retro WFC keeps is the game's 74-byte one: here a Mii made in My Miis.
        val mii = MiiFactory.female("Quest").apply { miiId = 0x89BF58D1L; systemId = 0xC8C6363CL }
        val data = MiiData.serialize(mii)
        val json = """{"pid": "601298307", "miiData": "${Base64.getEncoder().encodeToString(data)}"}"""
        assertArrayEquals(data, RetroWfc.parseMiiData(json))
        assertEquals(mii, MiiData.parse(RetroWfc.parseMiiData(json)!!))
        assertNull(RetroWfc.parseMiiData("""{"pid": "1", "miiData": null}"""))
        assertNull(RetroWfc.parseMiiData("""{"pid": "1", "miiData": "AAEC"}"""))
        assertNull(RetroWfc.parseMiiData("""{"pid": "1"}"""))
    }

    @Test
    fun requestsAreTheOnesWheelWizardMakes() {
        assertEquals("https://rwfc.net/api/leaderboard/player/0349-6103-6675", RetroWfc.profileUrl("0349-6103-6675"))
        assertEquals("https://rwfc.net/api/leaderboard/player/0349-6103-6675/history?days=30", RetroWfc.historyUrl("0349-6103-6675", 30))
        assertEquals("https://rwfc.net/api/leaderboard/top/50", RetroWfc.topUrl(50))
    }

    @Test
    fun unreadableAnswersAreIoErrors() {
        try {
            RetroWfc.parseHistory("Player with friend code '0000-0000-0001' not found")
            fail("parsed a plain-text answer")
        } catch (expected: IOException) {
        }
    }
}
