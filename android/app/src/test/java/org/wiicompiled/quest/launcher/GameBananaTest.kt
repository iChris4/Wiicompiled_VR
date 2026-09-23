package org.wiicompiled.quest.launcher

import java.io.IOException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** The mod browser reads GameBanana's answers the way WheelWizard's does; shapes taken from the live API. */
class GameBananaTest {

    private val page = """
        {"_aMetadata": {"_nRecordCount": 999, "_nPerpage": 15, "_bIsComplete": false},
         "_aRecords": [
          {"_idRow": 699980, "_sModelName": "Mod", "_sName": "MKWII Retro Rewind - Vanellope",
           "_aTags": ["Mod: Retro Rewind", "Character: Vanellope"], "_bHasContentRatings": false,
           "_nLikeCount": 4, "_nViewCount": 2524,
           "_aSubmitter": {"_sName": "toothcakee", "_sProfileUrl": "https://gamebanana.com/members/5730856"},
           "_aPreviewMedia": {"_aImages": [
             {"_sType": "screenshot", "_sBaseUrl": "https://images.gamebanana.com/img/ss/mods", "_sFile": "a.jpg",
              "_sFile220": "220-90_a.jpg", "_sFile530": "530-90_a.jpg"},
             {"_sType": "screenshot", "_sBaseUrl": "https://images.gamebanana.com/img/ss/mods", "_sFile": "b.jpg"}]}},
          {"_idRow": 12, "_sModelName": "Mod", "_sName": "Different Menu Style", "_aTags": ["Patches: True"],
           "_bHasContentRatings": false, "_nLikeCount": 1, "_nViewCount": 10, "_aSubmitter": {"_sName": "someone"}},
          {"_idRow": 13, "_sModelName": "Mod", "_sName": "Rated", "_aTags": [], "_bHasContentRatings": true},
          {"_idRow": 14, "_sModelName": "Question", "_sName": "How do I install mods?", "_aTags": []},
          {"_idRow": 15, "_sModelName": "Mod", "_sName": "No pictures", "_aTags": null, "_aSubmitter": null,
           "_aPreviewMedia": {}}
         ]}
    """.trimIndent()

    private val details = """
        {"_idRow": 596169, "_sName": "Sonic 06 Dark Rider", "_nLikeCount": 7, "_nViewCount": 900,
         "_nDownloadCount": 321, "_sProfileUrl": "https://gamebanana.com/mods/596169",
         "_sText": "A <b>bike</b><br><span class=\"GreenColor\">green</span>",
         "_aSubmitter": {"_sName": "rider", "_sProfileUrl": "https://gamebanana.com/members/1"},
         "_aTags": [{"_sTitle": "Patches", "_sValue": "True"}],
         "_aFiles": [
           {"_sFile": "shadow_bike_sonic006.zip", "_nFilesize": 214000, "_sDownloadUrl": "https://gamebanana.com/dl/1",
            "_sMd5Checksum": "f58e50d011bac0d1d57809340cb2810a", "_sDescription": "OLD"},
           {"_sFile": "shadow_bike_sonic06.zip", "_nFilesize": 214385, "_sDownloadUrl": "https://gamebanana.com/dl/2",
            "_sMd5Checksum": null, "_sDescription": null}],
         "_aArchivedFiles": [
           {"_sFile": "older.zip", "_nFilesize": 5, "_sDownloadUrl": "https://gamebanana.com/dl/0"}]}
    """.trimIndent()

    @Test
    fun searchPagesListOnlyMods() {
        val parsed = GameBanana.parsePage(page)
        // Questions and rated mods are left out, as WheelWizard's browser leaves them out.
        assertEquals(listOf(699980, 12, 15), parsed.mods.map { it.id })
        assertFalse(parsed.complete)
        val first = parsed.mods[0]
        assertEquals("MKWII Retro Rewind - Vanellope", first.name)
        assertEquals("toothcakee", first.author)
        assertEquals(4L, first.likes)
        assertEquals(2524L, first.views)
        assertFalse(first.usesPatches)
        assertEquals("https://images.gamebanana.com/img/ss/mods/220-90_a.jpg", first.image?.small)
        assertEquals("https://images.gamebanana.com/img/ss/mods/530-90_a.jpg", first.image?.medium)
        assertTrue(parsed.mods[1].usesPatches)
        // JSON nulls and empty objects read as nothing rather than as the text "null".
        assertEquals("", parsed.mods[2].author)
        assertNull(parsed.mods[2].image)
    }

    @Test
    fun theLastPageSaysSo() {
        assertTrue(GameBanana.parsePage("""{"_aMetadata": {"_bIsComplete": true}, "_aRecords": []}""").complete)
    }

    @Test
    fun detailsCarryTheFilesToDownload() {
        val mod = GameBanana.parseDetails(details)
        assertEquals(596169, mod.id)
        assertEquals("rider", mod.author)
        assertEquals("https://gamebanana.com/members/1", mod.authorUrl)
        assertEquals(321L, mod.downloads)
        assertEquals("https://gamebanana.com/support/add?s=Mod.596169", mod.reportUrl)
        assertEquals(listOf("shadow_bike_sonic006.zip", "shadow_bike_sonic06.zip"), mod.files.map { it.name })
        assertEquals("f58e50d011bac0d1d57809340cb2810a", mod.files[0].md5)
        assertEquals("OLD", mod.files[0].description)
        assertNull(mod.files[1].md5)
        assertEquals("", mod.files[1].description)
        assertEquals(214385L, mod.files[1].size)
        assertTrue(mod.images.isEmpty())
    }

    @Test
    fun archivedFilesStandInWhenThereAreNoOthers() {
        val mod = GameBanana.parseDetails("""{"_idRow": 1, "_sName": "Old", "_aFiles": [], "_aArchivedFiles": [{"_sFile": "old.rar", "_sDownloadUrl": "u"}]}""")
        assertEquals(listOf("old.rar"), mod.files.map { it.name })
        // Mods whose files live elsewhere (Google Drive, Mega) have nothing to download here.
        assertTrue(GameBanana.parseDetails("""{"_idRow": 2, "_sName": "Elsewhere"}""").files.isEmpty())
    }

    @Test
    fun privateModsSayWhyTheyCannotBeShown() {
        // What GameBanana answers for a private mod a search still lists (N64 Mod Pack, 451155).
        val stub = """{"_idRow": 451155, "_nStatus": "2", "_bIsPrivate": true, "_bAccessorIsSubmitter": false, "_bIsTrashed": false, "_bIsWithheld": false, "_sName": "N64 Mod Pack"}"""
        try {
            GameBanana.parseDetails(stub)
            fail("showed a private mod")
        } catch (expected: IOException) {
            assertTrue(expected.message!!, expected.message!!.contains("private"))
        }
    }

    @Test
    fun patchesTagsAreTheOnesTitledPatchOrPatches() {
        assertTrue(GameBanana.usesPatches(listOf("Patches: True")))
        assertTrue(GameBanana.usesPatches(listOf(" patch ")))
        assertTrue(GameBanana.usesPatches(listOf("Patches")))
        assertFalse(GameBanana.usesPatches(listOf("Mod: Retro Rewind", "Software Used: Patch that BRSAR!")))
        assertFalse(GameBanana.usesPatches(emptyList()))
    }

    @Test
    fun emptySearchesStillListSomething() {
        assertEquals("Mod", GameBanana.effectiveSearch("  ", patchesOnly = false))
        assertEquals("Patches", GameBanana.effectiveSearch("", patchesOnly = true))
        assertEquals("Luigi", GameBanana.effectiveSearch(" Luigi ", patchesOnly = true))
        assertEquals(
            "https://gamebanana.com/apiv12/Util/Search/Results?_sSearchString=Mario%20%26%20Luigi&_idGameRow=5896&_sModelName=Mod&_nPage=2",
            GameBanana.searchUrl("Mario & Luigi", 2),
        )
    }

    @Test
    fun refusalsExplainThemselves() {
        val tooShort = """{"_sErrorCode": "INPUT_ERRORS", "_aErrorData": {"_sSearchString": {"_sErrorCode": "STRING_TOO_SHORT", "_sErrorMessage": "Must be 2 characters or more"}}}"""
        assertEquals("Must be 2 characters or more", GameBanana.errorMessage(tooShort))
        assertEquals("NOT_FOUND", GameBanana.errorMessage("""{"_sErrorCode": "NOT_FOUND"}"""))
        assertNull(GameBanana.errorMessage("<html>Bad gateway</html>"))
    }

    @Test
    fun unreadableAnswersAreIoErrors() {
        for (json in listOf("<html>", """{"_aMetadata": {}}""", """{"_idRow": 3}""")) {
            try {
                if (json.contains("_idRow")) GameBanana.parseDetails(json) else GameBanana.parsePage(json)
                fail("parsed $json")
            } catch (expected: IOException) {
            }
        }
    }
}
