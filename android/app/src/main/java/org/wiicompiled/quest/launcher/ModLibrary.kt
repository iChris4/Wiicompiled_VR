package org.wiicompiled.quest.launcher

import android.os.Handler
import android.os.Looper
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.InterruptedIOException
import java.util.Locale
import java.util.concurrent.Executors
import java.util.zip.ZipFile

/**
 * The mods imported or installed from the mod browser on the Patches page, kept the way
 * WheelWizard VR keeps them on a computer (Features/Mods): one folder per mod under Mods/, holding
 * the mod's files and a `<name>.ini` with its state, so a Mods folder copied from one launcher
 * reads the same in the other.
 *
 * Mods only change Retro Rewind. Before it starts, [plan] and [sync] flatten the enabled mods into
 * the pack's Patches folder, which the pack's Riivolution XML maps onto the disc (/patches, /sound),
 * as the PC launcher's ModsLaunchService does before every Retro Rewind launch. A file two mods
 * both carry comes from the one higher in the list, which is the one with the lower priority.
 */
object ModLibrary {

    /**
     * One imported mod, as its `.ini` describes it. Author and ModID (its GameBanana id) only come
     * from a mod browser, this launcher's or the PC's.
     */
    data class Mod(
        val title: String,
        val enabled: Boolean,
        val priority: Int,
        val author: String = NO_ID,
        val modId: Int = -1,
    )

    /**
     * One file for an import, picked or downloaded: its display name and how to read it, and the
     * file itself when it is already on disk, so a downloaded archive is not copied again.
     */
    class Source(val name: String, val file: File? = null, val open: () -> InputStream)

    enum class NameProblem { Empty, Exists, IllegalCharacters }

    /** The archive formats GameBanana serves, told apart by their first bytes. */
    enum class ArchiveKind { Zip, SevenZip, Rar }

    private const val SECTION = "Mod"
    const val NO_ID = "-1"

    /** ModManager._illegalChars plus Windows' invalid file name characters, so a name travels to the PC. */
    private val ILLEGAL_NAME_CHARACTERS = ".~/\\<>:\"|?*".toSet()
    private val WHITESPACE = Regex("\\s+")

    /** The archives the PC unpacks with SharpCompress: .zip here, .7z and .rar through [ModArchive]. */
    private val ARCHIVE_EXTENSIONS = listOf(".zip", ".7z", ".rar")

    private val ZIP_SIGNATURES = listOf(byteArrayOf(0x50, 0x4B, 0x03, 0x04), byteArrayOf(0x50, 0x4B, 0x05, 0x06))
    private val SEVEN_ZIP_SIGNATURE = byteArrayOf(0x37, 0x7A, 0xBC.toByte(), 0xAF.toByte(), 0x27, 0x1C)
    /** Followed by 0 for RAR 1.5 to 4.x and by 1, 0 for RAR5; nod-jni reads both. */
    private val RAR_SIGNATURE = byteArrayOf(0x52, 0x61, 0x72, 0x21, 0x1A, 0x07)
    private const val COPY_BUFFER = 64 * 1024

    // Metadata

    /** Every mod under [modsDir] (`<name>/<name>.ini`), in list order: by priority, top first. */
    fun load(modsDir: File): List<Mod> =
        modsDir.listFiles { file -> file.isDirectory }
            .orEmpty()
            .mapNotNull { folder ->
                val ini = File(folder, "${folder.name}.ini")
                if (!ini.isFile) return@mapNotNull null
                runCatching { parseIni(ini.readText()) }.getOrNull()
            }
            .sortedWith(compareBy<Mod> { it.priority }.thenBy { it.title.lowercase(Locale.ROOT) })

    /**
     * Reads what Mod.LoadFromIniAsync reads, with its defaults: enabled unless it says otherwise,
     * priority 0 and ModID -1 when absent. Null without a name, which the PC skips as well.
     */
    fun parseIni(text: String): Mod? {
        val values = mutableMapOf<String, String>()
        var section = ""
        for (raw in text.removePrefix("﻿").lineSequence()) {
            val line = raw.trim()
            when {
                line.isEmpty() || line.startsWith(";") || line.startsWith("#") -> Unit
                line.startsWith("[") && line.endsWith("]") -> section = line.substring(1, line.length - 1).trim()
                section.equals(SECTION, ignoreCase = true) && '=' in line ->
                    values[line.substringBefore('=').trim().lowercase(Locale.ROOT)] = line.substringAfter('=').trim()
            }
        }
        val title = values["name"]?.takeIf { it.isNotBlank() } ?: return null
        return Mod(
            title = title,
            // bool.TryParse: either word in any case, and anything else keeps the default.
            enabled = when (values["isenabled"]?.lowercase(Locale.ROOT)) {
                "false" -> false
                else -> true
            },
            priority = values["priority"]?.toIntOrNull() ?: 0,
            author = values["author"] ?: NO_ID,
            modId = values["modid"]?.toIntOrNull() ?: -1,
        )
    }

    /** What Mod.SaveToIniAsync writes: the same keys, with .NET's True/False. */
    fun iniText(mod: Mod): String = buildString {
        append("[").append(SECTION).append("]\n")
        append("Name = ").append(mod.title).append('\n')
        append("Author = ").append(mod.author).append('\n')
        append("ModID = ").append(mod.modId).append('\n')
        append("IsEnabled = ").append(if (mod.enabled) "True" else "False").append('\n')
        append("Priority = ").append(mod.priority).append('\n')
    }

    fun save(modsDir: File, mod: Mod) {
        val folder = folder(modsDir, mod)
        if (!folder.isDirectory) throw IOException("The folder of ${mod.title} is missing: ${folder.absolutePath}")
        val ini = File(folder, "${mod.title}.ini")
        val temporary = File(folder, "${mod.title}.ini.tmp")
        temporary.writeText(iniText(mod))
        if (!temporary.renameTo(ini)) {
            temporary.delete()
            ini.writeText(iniText(mod))
        }
    }

    fun folder(modsDir: File, mod: Mod): File = File(modsDir, mod.title)

    /** ModManager.ValidateModName: a new name must be non-empty, unused (any case) and a valid folder name. */
    fun validateName(name: String, mods: List<Mod>): NameProblem? {
        val trimmed = name.trim()
        return when {
            trimmed.isEmpty() -> NameProblem.Empty
            mods.any { it.title.equals(trimmed, ignoreCase = true) } -> NameProblem.Exists
            trimmed.any { it in ILLEGAL_NAME_CHARACTERS || it.code < 32 } -> NameProblem.IllegalCharacters
            else -> null
        }
    }

    /** A starting name for the import dialog: the file's name without its extensions, made valid. */
    fun suggestName(fileName: String, mods: List<Mod>): String {
        val base = legalName(fileName.substringAfterLast('/').substringBefore('.'))
        return base.takeIf { validateName(it, mods) == null } ?: ""
    }

    /**
     * A starting name for the browser's install dialog: the mod's GameBanana name with what a
     * folder name cannot hold turned into spaces. The PC offers the raw name, which a headset
     * keyboard would then have to fix; a name already taken is left for the dialog to point out.
     */
    fun nameFrom(title: String): String = legalName(title)

    private fun legalName(text: String): String =
        text.map { if (it in ILLEGAL_NAME_CHARACTERS || it.code < 32) ' ' else it }
            .joinToString("")
            .replace(WHITESPACE, " ")
            .trim()

    /** ModManager.IsModInstalled: the mod a browser installed with this GameBanana id, if any. */
    fun installed(mods: List<Mod>, modId: Int): Mod? = if (modId < 0) null else mods.firstOrNull { it.modId == modId }

    // Changes

    /**
     * ModManager.ImportModFilesAsync and InstallModFromFileAsync: the files become one new mod,
     * enabled, below every existing one. A .zip, .7z or .rar among them is unpacked into it, as the
     * PC does with a mod it downloads; [author] and [modId] are what the mod browser knows of it.
     * The files are gathered beside the Mods folder's other entries and only moved into place once
     * all are in, so a failed import leaves nothing behind. [progress] follows the unpacking of
     * archives and returns false to cancel it.
     */
    fun import(
        modsDir: File,
        title: String,
        sources: List<Source>,
        mods: List<Mod>,
        author: String = NO_ID,
        modId: Int = -1,
        progress: ModArchive.Listener? = null,
    ): Mod {
        val name = title.trim()
        validateName(name, mods)?.let { throw IOException("The name $name cannot be used ($it).") }
        if (sources.isEmpty()) throw IOException("No files were chosen.")
        modsDir.mkdirs()
        val staging = File(modsDir, ".$name.importing")
        staging.deleteRecursively()
        if (!staging.mkdirs()) throw IOException("Could not create ${staging.absolutePath}")
        try {
            for (source in sources) {
                val fileName = source.name.substringAfterLast('/').substringAfterLast('\\')
                if (fileName.isBlank() || fileName == "." || fileName == "..") throw IOException("A chosen file has no usable name.")
                if (ARCHIVE_EXTENSIONS.any { fileName.endsWith(it, ignoreCase = true) }) {
                    unpack(source, staging, progress)
                } else {
                    source.open().use { input -> File(staging, fileName).outputStream().use { input.copyTo(it) } }
                }
            }
            if (staging.walkTopDown().none { it.isFile }) throw IOException("There was nothing to import.")
            val mod = Mod(name, enabled = true, priority = (mods.maxOfOrNull { it.priority } ?: 0) + 1, author = author, modId = modId)
            val target = folder(modsDir, mod)
            // No mod has this name, so a folder under it is what an interrupted import left.
            target.deleteRecursively()
            if (!staging.renameTo(target)) throw IOException("Could not move the mod into ${target.absolutePath}")
            save(modsDir, mod)
            return mod
        } finally {
            staging.deleteRecursively()
        }
    }

    /** The archive format [header], a file's first bytes, starts with; null for anything else. */
    fun archiveKind(header: ByteArray): ArchiveKind? {
        fun startsWith(signature: ByteArray) = header.size >= signature.size && signature.indices.all { header[it] == signature[it] }
        return when {
            ZIP_SIGNATURES.any(::startsWith) -> ArchiveKind.Zip
            startsWith(SEVEN_ZIP_SIGNATURE) -> ArchiveKind.SevenZip
            startsWith(RAR_SIGNATURE) -> ArchiveKind.Rar
            else -> null
        }
    }

    /**
     * Unpacks an archive into [destination], by what it holds rather than its name, refusing
     * entries that would land outside it. Every reader needs the archive as a file, so a picked
     * one is copied beside the staging folder first; a downloaded one is read where it is.
     */
    private fun unpack(source: Source, destination: File, progress: ModArchive.Listener?) {
        val copy = if (source.file == null) File(destination.parentFile, "${destination.name}.archive") else null
        val archive = source.file ?: copy!!
        try {
            if (copy != null) source.open().use { input -> copy.outputStream().use { input.copyTo(it) } }
            val header = ByteArray(8)
            val read = archive.inputStream().use { it.read(header) }
            when (archiveKind(header.copyOf(maxOf(read, 0)))) {
                ArchiveKind.Zip -> unzip(archive, source.name, destination, progress)
                ArchiveKind.SevenZip, ArchiveKind.Rar -> try {
                    ModArchive.extract(archive.absolutePath, destination.absolutePath, progress ?: ModArchive.Listener { _, _ -> true })
                } catch (e: InterruptedIOException) {
                    throw e
                } catch (e: IOException) {
                    throw IOException("${source.name}: ${e.message}", e)
                }
                null -> throw IOException("${source.name} is not a zip, 7z or RAR archive.")
            }
        } finally {
            copy?.delete()
        }
    }

    private fun unzip(archive: File, name: String, destination: File, progress: ModArchive.Listener?) {
        // ZipFile reads the central directory, which every zip has; streaming fails on some.
        ZipFile(archive).use { zip ->
            val entries = zip.entries().toList().filter { !it.isDirectory }
            val total = entries.sumOf { maxOf(it.size, 0L) }
            var done = 0L
            if (progress?.update(done, total) == false) throw InterruptedIOException("Install cancelled")
            for (entry in entries) {
                val relative = entry.name.replace('\\', '/').trimStart('/')
                if (relative.isEmpty() || relative.split('/').any { it == ".." }) {
                    throw IOException("$name has a file outside its own folder (${entry.name}).")
                }
                val file = File(destination, relative)
                file.parentFile?.mkdirs()
                zip.getInputStream(entry).use { input ->
                    file.outputStream().use { output ->
                        val buffer = ByteArray(COPY_BUFFER)
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            output.write(buffer, 0, count)
                            done += count
                            if (progress?.update(done, total) == false) throw InterruptedIOException("Install cancelled")
                        }
                    }
                }
            }
        }
    }

    fun delete(modsDir: File, mod: Mod) {
        val folder = folder(modsDir, mod)
        if (folder.exists() && !folder.deleteRecursively()) throw IOException("Could not delete ${folder.absolutePath}")
    }

    /** ModManager.RenameModAsync: the folder and its `.ini` take the new name. */
    fun rename(modsDir: File, mod: Mod, newTitle: String, mods: List<Mod>): Mod {
        val name = newTitle.trim()
        if (name == mod.title) return mod
        validateName(name, mods)?.let { throw IOException("The name $name cannot be used ($it).") }
        val renamed = mod.copy(title = name)
        val from = folder(modsDir, mod)
        val to = folder(modsDir, renamed)
        if (!from.renameTo(to)) throw IOException("Could not rename ${from.absolutePath}")
        File(to, "${mod.title}.ini").delete()
        save(modsDir, renamed)
        return renamed
    }

    /**
     * ModManager.DecreasePriorityAsync (up) and IncreasePriorityAsync (down): the mod swaps
     * priorities with its neighbour. Returns the two changed mods, or nothing at either end.
     */
    fun move(mods: List<Mod>, mod: Mod, up: Boolean): List<Mod> {
        val neighbour = if (up) {
            mods.filter { it.priority < mod.priority }.maxByOrNull { it.priority }
        } else {
            mods.filter { it.priority > mod.priority }.minByOrNull { it.priority }
        } ?: return emptyList()
        return listOf(mod.copy(priority = neighbour.priority), neighbour.copy(priority = mod.priority))
    }

    // Launch

    /**
     * ModsLaunchService.PrepareModsForLaunch: the file name each enabled mod's files take in the
     * Patches folder, and where each comes from. Mods are walked from the bottom of the list up and
     * a later one replaces an earlier one's file, so the top of the list wins. Names compare
     * without case, as on the PC and on the headset's shared storage.
     */
    fun plan(modsDir: File, mods: List<Mod>): Map<String, File> {
        val files = LinkedHashMap<String, Pair<String, File>>()
        for (mod in mods.sortedWith(compareByDescending<Mod> { it.priority }.thenByDescending { it.title.lowercase(Locale.ROOT) })) {
            if (!mod.enabled) continue
            val folder = folder(modsDir, mod)
            if (!folder.isDirectory) continue
            val metadata = File(folder, "${mod.title}.ini").absolutePath
            val contents = folder.walkTopDown()
                .filter { it.isFile && !it.absolutePath.equals(metadata, ignoreCase = true) }
                .sortedBy { it.relativeTo(folder).path.lowercase(Locale.ROOT) }
            for (file in contents) {
                val name = launchName(mod.priority, file.name)
                val key = name.lowercase(Locale.ROOT)
                files[key] = (files[key]?.first ?: name) to file
            }
        }
        return files.values.associate { it }
    }

    /**
     * ModsLaunchService.GetLaunchPatchFileName: a modding archive (`<name>.<tag>.szs`) is prefixed
     * with its mod's priority, so Pulsar can resolve two mods patching the same archive.
     */
    fun launchName(priority: Int, fileName: String): String {
        if (!isModdingArchive(fileName)) return fileName
        return "$priority.${stripPriorityPrefix(fileName)}"
    }

    private fun isModdingArchive(fileName: String): Boolean {
        if (!fileName.endsWith(".szs", ignoreCase = true)) return false
        val stem = fileName.substring(0, fileName.length - ".szs".length)
        val separator = stem.lastIndexOf('.')
        return separator > 0 && separator + 1 < stem.length
    }

    private fun stripPriorityPrefix(fileName: String): String {
        val digits = fileName.takeWhile { it.isDigit() }.length
        return if (digits > 0 && digits < fileName.length && fileName[digits] == '.') fileName.substring(digits + 1) else fileName
    }

    /** ModsLaunchService.ShouldAskToClearTargetFolder: no mod enabled, yet the Patches folder holds files. */
    fun shouldAskToClear(mods: List<Mod>, patchesDir: File): Boolean =
        mods.none { it.enabled } && patchesDir.listFiles { file -> file.isFile }.orEmpty().isNotEmpty()

    /**
     * ModsLaunchService.CopyFinalFiles: the Patches folder ends up holding exactly [plan]'s files.
     * Loose files no mod provides go; a file whose size and time already match is not copied again.
     */
    fun sync(patchesDir: File, plan: Map<String, File>) {
        patchesDir.mkdirs()
        if (!patchesDir.isDirectory) throw IOException("Could not create ${patchesDir.absolutePath}")
        val wanted = plan.keys.map { it.lowercase(Locale.ROOT) }.toSet()
        for (file in patchesDir.listFiles { file -> file.isFile }.orEmpty()) {
            if (file.name.lowercase(Locale.ROOT) !in wanted && !file.delete()) {
                throw IOException("Could not remove ${file.absolutePath}")
            }
        }
        for ((name, source) in plan) {
            val target = File(patchesDir, name)
            if (target.isFile && target.length() == source.length() && target.lastModified() == source.lastModified()) continue
            source.copyTo(target, overwrite = true)
            // Without the time, every start would copy everything again; that is all it costs.
            target.setLastModified(source.lastModified())
        }
    }

    /** ModsLaunchService with the folder-clearing answer: null on success, otherwise the message. */
    fun prepareForLaunch(modsDir: File, patchesDir: File, mods: List<Mod>, clear: Boolean): String? = try {
        when {
            mods.any { it.enabled } -> sync(patchesDir, plan(modsDir, mods))
            clear && patchesDir.exists() && !patchesDir.deleteRecursively() ->
                throw IOException("Could not clear ${patchesDir.absolutePath}")
        }
        null
    } catch (e: IOException) {
        e.message ?: e.toString()
    }

    // Background work

    private val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "ModLibrary").apply { isDaemon = true } }
    private val main by lazy { Handler(Looper.getMainLooper()) }

    /**
     * True while an import runs, including a mod browser install from the start of its download
     * ([ModInstaller]); imports and launch preparation take turns on one thread.
     */
    @Volatile
    var importing = false
        internal set

    /** Runs [work] off the main thread, one task at a time, and hands its result to [done] on the main thread. */
    fun <T> background(work: () -> T, done: (Result<T>) -> Unit) {
        worker.execute {
            val result = runCatching(work)
            main.post { done(result) }
        }
    }

    fun importInBackground(modsDir: File, title: String, sources: List<Source>, done: (Result<Mod>) -> Unit) {
        importing = true
        background({ import(modsDir, title, sources, load(modsDir)) }) { result ->
            importing = false
            done(result)
        }
    }
}
