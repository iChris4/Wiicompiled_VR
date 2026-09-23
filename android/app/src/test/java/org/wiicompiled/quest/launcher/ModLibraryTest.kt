package org.wiicompiled.quest.launcher

import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.io.InterruptedIOException
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** The Patches page keeps mods as WheelWizard does and fills the Patches folder as it does at launch. */
class ModLibraryTest {

    private fun temp(): File = Files.createTempDirectory("mods").toFile()

    private fun source(name: String, text: String) = ModLibrary.Source(name) { text.byteInputStream() }

    private fun zip(name: String, vararg entries: Pair<String, String>): ModLibrary.Source {
        val bytes = ByteArrayOutputStream().also { out ->
            ZipOutputStream(out).use { zip ->
                for ((path, text) in entries) {
                    zip.putNextEntry(ZipEntry(path))
                    zip.write(text.toByteArray())
                    zip.closeEntry()
                }
            }
        }.toByteArray()
        return ModLibrary.Source(name) { bytes.inputStream() }
    }

    @Test
    fun readsTheIniThePcWrites() {
        // IniParser's output: CRLF, spaces around '=', .NET's True/False, sometimes a BOM.
        val text = "﻿[Mod]\r\nName = Fast Karts\r\nAuthor = someone\r\nModID = 4242\r\nIsEnabled = False\r\nPriority = 3\r\n"
        assertEquals(ModLibrary.Mod("Fast Karts", enabled = false, priority = 3, author = "someone", modId = 4242), ModLibrary.parseIni(text))
        // Absent values take the PC's defaults; a nameless file is no mod.
        assertEquals(ModLibrary.Mod("Solo", enabled = true, priority = 0), ModLibrary.parseIni("[Mod]\nName=Solo\n"))
        assertNull(ModLibrary.parseIni("[Mod]\nPriority = 1\n"))
        val mod = ModLibrary.Mod("Round Trip", enabled = true, priority = 7, author = "a", modId = 9)
        assertEquals(mod, ModLibrary.parseIni(ModLibrary.iniText(mod)))
        assertTrue(ModLibrary.iniText(mod).contains("IsEnabled = True"))
    }

    @Test
    fun namesFollowThePcRules() {
        val mods = listOf(ModLibrary.Mod("Taken", true, 1))
        assertEquals(ModLibrary.NameProblem.Empty, ModLibrary.validateName("  ", mods))
        assertEquals(ModLibrary.NameProblem.Exists, ModLibrary.validateName("taken", mods))
        assertEquals(ModLibrary.NameProblem.IllegalCharacters, ModLibrary.validateName("v1.2", mods))
        assertEquals(ModLibrary.NameProblem.IllegalCharacters, ModLibrary.validateName("a:b", mods))
        assertNull(ModLibrary.validateName(" New Mod ", mods))
        assertEquals("Fire Kart", ModLibrary.suggestName("Fire Kart.tag.szs", mods))
        assertEquals("", ModLibrary.suggestName("Taken.zip", mods))
    }

    @Test
    fun importsLooseFilesAndZipsIntoOneMod() {
        val modsDir = temp()
        val first = ModLibrary.import(modsDir, " Karts ", listOf(source("Common.szs", "c"), zip("pack.zip", "Patches/Menu.szs" to "m")), emptyList())
        assertEquals(ModLibrary.Mod("Karts", enabled = true, priority = 1), first)
        assertEquals("c", File(modsDir, "Karts/Common.szs").readText())
        assertEquals("m", File(modsDir, "Karts/Patches/Menu.szs").readText())
        assertTrue(File(modsDir, "Karts/Karts.ini").isFile)
        // A new mod goes below every existing one, and nothing is left of the staging folder.
        val second = ModLibrary.import(modsDir, "Music", listOf(source("a.brstm", "a")), ModLibrary.load(modsDir))
        assertEquals(2, second.priority)
        assertEquals(listOf("Karts", "Music"), ModLibrary.load(modsDir).map { it.title })
        assertEquals(setOf("Karts", "Music"), modsDir.list()!!.toSet())
    }

    @Test
    fun refusesWhatItCannotImportAndLeavesNothing() {
        val modsDir = temp()
        for (sources in listOf(listOf(zip("evil.zip", "../escape.szs" to "x")), listOf(source("mod.rar", "r")), listOf(zip("empty.zip", "folder/" to "")))) {
            try {
                ModLibrary.import(modsDir, "Bad", sources, emptyList())
                fail("imported ${sources.map { it.name }}")
            } catch (expected: IOException) {
            }
        }
        assertFalse(File(modsDir.parentFile, "escape.szs").exists())
        assertEquals(0, modsDir.list()!!.size)
    }

    @Test
    fun movingSwapsPriorityWithTheNeighbour() {
        val mods = listOf(ModLibrary.Mod("A", true, 1), ModLibrary.Mod("B", true, 2), ModLibrary.Mod("C", true, 5))
        assertEquals(listOf(ModLibrary.Mod("B", true, 1), ModLibrary.Mod("A", true, 2)), ModLibrary.move(mods, mods[1], up = true))
        assertEquals(listOf(ModLibrary.Mod("B", true, 5), ModLibrary.Mod("C", true, 2)), ModLibrary.move(mods, mods[1], up = false))
        assertTrue(ModLibrary.move(mods, mods[0], up = true).isEmpty())
    }

    @Test
    fun renameMovesTheFolderAndItsIni() {
        val modsDir = temp()
        val mod = ModLibrary.import(modsDir, "Old", listOf(source("x.szs", "x")), emptyList())
        val renamed = ModLibrary.rename(modsDir, mod, "New", ModLibrary.load(modsDir))
        assertEquals(listOf(renamed), ModLibrary.load(modsDir))
        assertTrue(File(modsDir, "New/x.szs").isFile)
        assertFalse(File(modsDir, "New/Old.ini").exists())
    }

    @Test
    fun modArchivesTakeTheirModsPriority() {
        assertEquals("3.Kart.tag.szs", ModLibrary.launchName(3, "Kart.tag.szs"))
        assertEquals("3.Kart.tag.szs", ModLibrary.launchName(3, "12.Kart.tag.szs"))
        assertEquals("Common.szs", ModLibrary.launchName(3, "Common.szs"))
        assertEquals("track.brstm", ModLibrary.launchName(3, "track.brstm"))
    }

    @Test
    fun theTopOfTheListWinsAndDisabledModsAreLeftOut() {
        val modsDir = temp()
        val top = ModLibrary.import(modsDir, "Top", listOf(source("Common.szs", "top"), source("Top.brstm", "t")), emptyList())
        val bottom = ModLibrary.import(modsDir, "Bottom", listOf(source("COMMON.szs", "bottom"), source("Bottom.brstm", "b")), listOf(top))
        val off = ModLibrary.Mod("Off", enabled = false, priority = 3)
        File(modsDir, "Off").mkdirs()
        File(modsDir, "Off/Off.brstm").writeText("o")
        ModLibrary.save(modsDir, off)

        val plan = ModLibrary.plan(modsDir, ModLibrary.load(modsDir))
        assertEquals(setOf("common.szs", "top.brstm", "bottom.brstm"), plan.keys.map { it.lowercase() }.toSet())
        assertEquals("top", plan.entries.single { it.key.equals("common.szs", ignoreCase = true) }.value.readText())
        assertEquals(listOf(top, bottom, off), ModLibrary.load(modsDir))
    }

    @Test
    fun syncLeavesExactlyThePlanInPatches() {
        val modsDir = temp()
        val mod = ModLibrary.import(modsDir, "Mod", listOf(source("New.szs", "new")), emptyList())
        val patches = File(temp(), "Patches").apply { mkdirs() }
        File(patches, "Stale.szs").writeText("stale")
        File(patches, "keep-folder").mkdirs()

        ModLibrary.sync(patches, ModLibrary.plan(modsDir, listOf(mod)))
        assertEquals(setOf("New.szs", "keep-folder"), patches.list()!!.toSet())
        assertEquals("new", File(patches, "New.szs").readText())

        // Nothing enabled: the folder is only cleared when the player agrees.
        val disabled = listOf(mod.copy(enabled = false))
        assertTrue(ModLibrary.shouldAskToClear(disabled, patches))
        assertNull(ModLibrary.prepareForLaunch(modsDir, patches, disabled, clear = false))
        assertTrue(File(patches, "New.szs").isFile)
        assertNull(ModLibrary.prepareForLaunch(modsDir, patches, disabled, clear = true))
        assertFalse(patches.exists())
        assertFalse(ModLibrary.shouldAskToClear(disabled, patches))
    }

    @Test
    fun archivesAreToldApartByTheirFirstBytes() {
        fun bytes(vararg values: Int) = ByteArray(values.size) { values[it].toByte() }
        assertEquals(ModLibrary.ArchiveKind.Zip, ModLibrary.archiveKind(bytes(0x50, 0x4B, 0x03, 0x04, 0x14)))
        assertEquals(ModLibrary.ArchiveKind.Zip, ModLibrary.archiveKind(bytes(0x50, 0x4B, 0x05, 0x06)))
        assertEquals(ModLibrary.ArchiveKind.SevenZip, ModLibrary.archiveKind(bytes(0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04)))
        // RAR 1.5 to 4.x, then RAR5.
        assertEquals(ModLibrary.ArchiveKind.Rar, ModLibrary.archiveKind(bytes(0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00)))
        assertEquals(ModLibrary.ArchiveKind.Rar, ModLibrary.archiveKind(bytes(0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00)))
        assertNull(ModLibrary.archiveKind(bytes(0x52, 0x61, 0x72)))
        assertNull(ModLibrary.archiveKind("text".toByteArray()))
    }

    @Test
    fun browserInstallsRememberTheirGameBananaMod() {
        val modsDir = temp()
        val archive = File(temp(), "vanellope.zip")
        archive.outputStream().use { out ->
            ZipOutputStream(out).use { zip ->
                zip.putNextEntry(ZipEntry("Vanellope/Patches/Driver.szs"))
                zip.write("driver".toByteArray())
                zip.closeEntry()
            }
        }
        // A downloaded archive is read where it is, not opened as a stream and copied again.
        val downloaded = ModLibrary.Source(archive.name, archive) { throw IOException("copied a downloaded archive") }
        val mod = ModLibrary.import(modsDir, "Vanellope", listOf(downloaded), emptyList(), author = "toothcakee", modId = 699980)
        assertEquals(ModLibrary.Mod("Vanellope", enabled = true, priority = 1, author = "toothcakee", modId = 699980), mod)
        assertEquals(listOf(mod), ModLibrary.load(modsDir))
        assertEquals("driver", File(modsDir, "Vanellope/Vanellope/Patches/Driver.szs").readText())
        assertTrue(archive.isFile)
        assertEquals(mod, ModLibrary.installed(ModLibrary.load(modsDir), 699980))
        assertNull(ModLibrary.installed(ModLibrary.load(modsDir), 1))
        // Imported mods have no GameBanana id, and -1 never means installed.
        assertNull(ModLibrary.installed(listOf(ModLibrary.Mod("Local", true, 1)), -1))
    }

    @Test
    fun zipProgressCountsTheBytesAndCanCancel() {
        val modsDir = temp()
        val updates = mutableListOf<Pair<Long, Long>>()
        val counting = ModArchive.Listener { done, total ->
            updates += done to total
            true
        }
        ModLibrary.import(modsDir, "Two", listOf(zip("two.zip", "a.szs" to "aaaa", "b.szs" to "bb")), emptyList(), progress = counting)
        assertEquals(0L to 6L, updates.first())
        assertEquals(6L to 6L, updates.last())

        try {
            val stop = ModArchive.Listener { _, _ -> false }
            ModLibrary.import(modsDir, "Stopped", listOf(zip("stop.zip", "a.szs" to "aaaa")), ModLibrary.load(modsDir), progress = stop)
            fail("a cancelled import finished")
        } catch (expected: InterruptedIOException) {
        }
        assertEquals(setOf("Two"), modsDir.list()!!.toSet())
    }

    @Test
    fun archivesThatAreNotWhatTheyAreCalledAreRefused() {
        val modsDir = temp()
        for (name in listOf("mod.7z", "mod.rar", "mod.zip")) {
            try {
                ModLibrary.import(modsDir, "Fake", listOf(source(name, "not an archive")), emptyList())
                fail("imported $name")
            } catch (expected: IOException) {
                assertTrue(expected.message!!, expected.message!!.contains("not a zip, 7z or RAR archive"))
            }
        }
        assertEquals(0, modsDir.list()!!.size)
    }

    @Test
    fun gameBananaNamesBecomeFolderNames() {
        assertEquals("PaRappa Award szs for CTGP", ModLibrary.nameFrom("PaRappa Award.szs for CTGP"))
        assertEquals("Mario Kart Wii", ModLibrary.nameFrom("Mario Kart: Wii"))
        assertEquals("Galacta Character Mod MKWii", ModLibrary.nameFrom("Galacta Character Mod | MKWii"))
        assertEquals("Retro Rewind - Vanellope", ModLibrary.nameFrom("  Retro Rewind - Vanellope "))
        assertNull(ModLibrary.validateName(ModLibrary.nameFrom("a/b\\c?.d"), emptyList()))
    }
}
