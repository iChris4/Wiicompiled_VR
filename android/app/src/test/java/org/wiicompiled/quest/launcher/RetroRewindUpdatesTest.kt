package org.wiicompiled.quest.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.wiicompiled.quest.GameLibrary

/** When the launcher checks for a newer Retro Rewind pack, and what it offers once it has one. */
class RetroRewindUpdatesTest {

    private val packA = "a".repeat(64)
    private val packB = "b".repeat(64)

    @Test
    fun checksAtMostEveryFewHours() {
        val now = 10L * RetroRewindUpdates.CHECK_INTERVAL_MS
        assertTrue(RetroRewindUpdates.checkDue(now, 0))
        assertFalse(RetroRewindUpdates.checkDue(now, now - 1000))
        assertTrue(RetroRewindUpdates.checkDue(now, now - RetroRewindUpdates.CHECK_INTERVAL_MS))
        // A clock set back must not silence the check for days.
        assertTrue(RetroRewindUpdates.checkDue(now, now + 1000))
    }

    @Test
    fun aFailedCheckIsTriedAgainSoonerThanASuccessfulOne() {
        // checkInBackground stamps a failure this far back, so it is due after RETRY_INTERVAL_MS.
        val failedAt = 5L * RetroRewindUpdates.CHECK_INTERVAL_MS
        val stamp = failedAt - RetroRewindUpdates.CHECK_INTERVAL_MS + RetroRewindUpdates.RETRY_INTERVAL_MS
        assertFalse(RetroRewindUpdates.checkDue(failedAt + RetroRewindUpdates.RETRY_INTERVAL_MS - 1, stamp))
        assertTrue(RetroRewindUpdates.checkDue(failedAt + RetroRewindUpdates.RETRY_INTERVAL_MS, stamp))
    }

    @Test
    fun offersEachNewerVersionOnce() {
        assertEquals("6.13.4", RetroRewindUpdates.updateToOffer("6.13.3", "6.13.4", null))
        assertEquals("6.13.4", RetroRewindUpdates.updateToOffer("6.13.3", "6.13.4", "6.13.3"))
        assertNull(RetroRewindUpdates.updateToOffer("6.13.3", "6.13.4", "6.13.4"))
        // Versions compare by number: 6.13.10 is newer than 6.13.9.
        assertEquals("6.13.10", RetroRewindUpdates.updateToOffer("6.13.9", "6.13.10", "6.13.9"))
    }

    @Test
    fun offersNoUpdateWithoutAPackOrANewerVersion() {
        assertNull(RetroRewindUpdates.updateToOffer(null, "6.13.4", null))
        assertNull(RetroRewindUpdates.updateToOffer("6.13.3", null, null))
        assertNull(RetroRewindUpdates.updateToOffer("6.13.4", "6.13.4", null))
        assertNull(RetroRewindUpdates.updateToOffer("6.13.4", "6.13.3", null))
        assertFalse(RetroRewindUpdates.updateAvailable("6.13.4", "6.13.4"))
        assertTrue(RetroRewindUpdates.updateAvailable("6.13.3", "6.13.4"))
    }

    @Test
    fun offersARebuildWhenTheGameIsBehindItsPackOncePerCodePul() {
        val behind = GameLibrary.Status.PackChanged
        assertTrue(RetroRewindUpdates.rebuildToOffer(behind, recordsCodePul = true, afterUpdate = false, packCodePul = packB, offered = null))
        assertTrue(RetroRewindUpdates.rebuildToOffer(behind, recordsCodePul = true, afterUpdate = true, packCodePul = packB, offered = packA))
        assertFalse(RetroRewindUpdates.rebuildToOffer(behind, recordsCodePul = true, afterUpdate = false, packCodePul = packB, offered = packB))
        assertFalse(RetroRewindUpdates.rebuildToOffer(behind, recordsCodePul = true, afterUpdate = false, packCodePul = null, offered = null))
    }

    @Test
    fun offersARebuildAfterAnUpdateOnlyWhenTheGameCannotBeChecked() {
        val ready = GameLibrary.Status.Ready
        // A game that records its Code.pul and is Ready already matches the updated pack.
        assertFalse(RetroRewindUpdates.rebuildToOffer(ready, recordsCodePul = true, afterUpdate = true, packCodePul = packB, offered = null))
        // An older game.json says nothing, so an update that may have changed the code is worth a build.
        assertTrue(RetroRewindUpdates.rebuildToOffer(ready, recordsCodePul = false, afterUpdate = true, packCodePul = packB, offered = null))
        assertFalse(RetroRewindUpdates.rebuildToOffer(ready, recordsCodePul = false, afterUpdate = false, packCodePul = packB, offered = null))
        // Nothing to rebuild when there is no game, or one this app cannot load anyway.
        assertFalse(RetroRewindUpdates.rebuildToOffer(GameLibrary.Status.Missing, recordsCodePul = false, afterUpdate = true, packCodePul = packB, offered = null))
        assertFalse(RetroRewindUpdates.rebuildToOffer(GameLibrary.Status.Stale, recordsCodePul = false, afterUpdate = true, packCodePul = packB, offered = null))
    }

    @Test
    fun aGameIsBehindOnlyWhenBothCodePulsAreKnownAndDiffer() {
        assertTrue(GameLibrary.packChanged(packA, packB))
        assertFalse(GameLibrary.packChanged(packA, packA.uppercase()))
        assertFalse(GameLibrary.packChanged(null, packB))
        assertFalse(GameLibrary.packChanged("", packB))
        assertFalse(GameLibrary.packChanged(packA, null))
    }

    @Test
    fun theNewestPublishedVersionIsTheHighestNotTheLastListed() {
        val updates = RetroRewindPack.parseUpdates(
            """
            6.13.10 https://update.rwfc.net/RetroRewind/zip/6.13.10.zip /RetroRewind6 Later
            6.13.9 https://update.rwfc.net/RetroRewind/zip/6.13.9.zip /RetroRewind6 Earlier
            """.trimIndent(),
        )
        assertEquals("6.13.10", RetroRewindPack.latest(updates))
        assertNull(RetroRewindPack.latest(emptyList()))
    }
}
