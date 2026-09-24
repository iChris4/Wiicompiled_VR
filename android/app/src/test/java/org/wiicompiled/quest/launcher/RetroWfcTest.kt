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
    fun onlineIsBeingInARoomNow() {
        val json = """
            {"rooms": [
               {"id": "XESURM", "players": []},
               {"id": "A", "players": [{"pid": "166930", "friendCode": "4123-1702-7346", "mii": {"data": "", "name": "x"}},
                                       {"pid": "204981", "friendCode": "4939-2144-4021"}]},
               {"id": "B", "players": [{"pid": "1", "friendCode": null}]}],
             "totalPlayers": 3}
        """.trimIndent()
        assertEquals(setOf("4123-1702-7346", "4939-2144-4021"), RetroWfc.parseOnlineFriendCodes(json))
        assertEquals(emptySet<String>(), RetroWfc.parseOnlineFriendCodes("""{"rooms": null}"""))
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
