package org.wiicompiled.quest.launcher

import android.content.Context
import android.os.ParcelFileDescriptor
import android.os.StatFs
import android.util.Log
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.FilterInputStream
import java.io.InputStream
import java.io.InterruptedIOException
import java.security.MessageDigest
import java.util.zip.ZipInputStream
import org.json.JSONObject
import org.wiicompiled.quest.BuildConfig
import org.wiicompiled.quest.GameLibrary
import org.wiicompiled.quest.GameProfile
import org.wiicompiled.quest.GameStorage

/**
 * A .wcgame built on a PC (android/Build-QuestGame.ps1, the PC launcher's "Build for Quest")
 * becomes one of this headset's games: libmain.so and game.json, plus DATA when the package
 * carries the extracted disc and the Retro Rewind pack when it carries that.
 *
 * The package says which game it is (`profile`), so importing one installs that game and leaves
 * the other alone. It is only accepted for the disc and app it was built for: game.json must name
 * the pinned game and hashes, the kit this APK carries, and a library whose hash matches the bytes
 * in the package. Every part is staged and checked before any of it replaces what is installed.
 */
object GamePackageImport {

    private const val TAG = "WiiCompiledLauncher"
    private const val SCHEMA = 1
    private const val DATA_PREFIX = "DATA/"
    private const val MOD_PREFIX = "MOD/"
    private const val FREE_SPACE_MARGIN = 256L * 1024 * 1024

    /** Null on success, otherwise the message to show. */
    fun run(context: Context, descriptor: ParcelFileDescriptor, progress: GameSetup.Progress, finishing: () -> Unit): String? {
        val total = descriptor.statSize
        val gameRoot = GameStorage.gameRoot(context)
        val available = StatFs(gameRoot.absolutePath).availableBytes
        if (total > 0 && available < total + FREE_SPACE_MARGIN) {
            return "Not enough free space: importing needs ${GameFiles.gigabytes(total + FREE_SPACE_MARGIN)}, and ${GameFiles.gigabytes(available)} is free."
        }

        // The game's destination is only known once game.json has been read, so the library is
        // staged under a name of its own first.
        val libraryStaging = File(context.filesDir, "game/.importing")
        val dataStaging = File(gameRoot, "DATA.importing")
        val modStaging = File(gameRoot, "${GameStorage.MOD_DIRECTORY}.importing")
        for (staging in listOf(libraryStaging, dataStaging, modStaging)) staging.deleteRecursively()
        try {
            libraryStaging.mkdirs()
            var manifestText: String? = null
            var librarySha256: String? = null
            var hasData = false
            var hasMod = false

            val counted = CountingInputStream(FileInputStream(descriptor.fileDescriptor))
            ZipInputStream(BufferedInputStream(counted, 1 shl 20)).use { zip ->
                val buffer = ByteArray(1 shl 20)
                var lastReport = 0L
                fun extract(target: File) {
                    target.parentFile?.mkdirs()
                    target.outputStream().use { out ->
                        while (true) {
                            val read = zip.read(buffer)
                            if (read < 0) break
                            out.write(buffer, 0, read)
                            if (counted.count - lastReport >= 8L shl 20) {
                                lastReport = counted.count
                                if (!progress.update(counted.count, total)) throw InterruptedIOException("Import cancelled")
                            }
                        }
                    }
                }
                while (true) {
                    val entry = zip.nextEntry ?: break
                    val name = entry.name
                    if (entry.isDirectory) continue
                    if (name.startsWith("/") || name.split('/').any { it == ".." }) {
                        return "The game file contains an invalid path: $name"
                    }
                    when {
                        name == GameLibrary.MANIFEST_NAME -> manifestText = zip.readBytes().toString(Charsets.UTF_8)
                        name == GameLibrary.LIBRARY_NAME -> {
                            val digest = MessageDigest.getInstance("SHA-256")
                            File(libraryStaging, GameLibrary.LIBRARY_NAME).outputStream().use { out ->
                                while (true) {
                                    val read = zip.read(buffer)
                                    if (read < 0) break
                                    digest.update(buffer, 0, read)
                                    out.write(buffer, 0, read)
                                }
                            }
                            librarySha256 = digest.digest().joinToString("") { "%02x".format(it) }
                        }
                        name.startsWith(DATA_PREFIX) -> {
                            hasData = true
                            extract(File(dataStaging, name.removePrefix(DATA_PREFIX)))
                        }
                        name.startsWith(MOD_PREFIX) -> {
                            hasMod = true
                            extract(File(modStaging, name.removePrefix(MOD_PREFIX)))
                        }
                    }
                    if (!progress.update(counted.count, total)) throw InterruptedIOException("Import cancelled")
                }
            }

            finishing()
            val manifestJson = manifestText?.let { runCatching { JSONObject(it) }.getOrNull() }
                ?: return "This is not a WiiCompiled game file (game.json is missing)."
            val profile = GameProfile.of(manifestJson.optString("profile"))
                ?.takeIf { it in GameProfile.available(context) }
                ?: return "This game file is for ${manifestJson.optString("profile")}, which this app does not carry a kit for."
            manifestError(context, manifestJson, librarySha256)?.let { return it }
            if (hasData) {
                GameFiles.validateData(dataStaging)?.let { return it }
            } else if (GameStorage.discStatus(context) != GameStorage.DiscStatus.Ready) {
                return "This game file has no game files (DATA). Build it again with the disc files included, or select your disc image first."
            }
            if (hasMod && !File(modStaging, "Binaries/Code.pul").isFile) {
                return "The Retro Rewind pack in this game file is incomplete (no Binaries/Code.pul)."
            }

            File(libraryStaging, GameLibrary.MANIFEST_NAME).writeText(manifestText!!)
            if (hasData) {
                GameFiles.replace(GameStorage.discDirectory(context), dataStaging)?.let { return it }
            }
            if (hasMod) {
                GameFiles.replace(GameStorage.modDirectory(context), modStaging)?.let { return it }
            }
            val libraryDir = GameLibrary.directory(context, profile)
            libraryDir.parentFile?.mkdirs()
            GameFiles.replace(libraryDir, libraryStaging)?.let { return it }
            Log.i(TAG, "Imported ${profile.id} built by ${manifestJson.optString("builtBy")} at ${manifestJson.optString("builtAt")}")
            GameProfile.select(context, profile)
            return null
        } finally {
            for (staging in listOf(libraryStaging, dataStaging, modStaging)) staging.deleteRecursively()
        }
    }

    private fun manifestError(context: Context, manifest: JSONObject, librarySha256: String?): String? {
        if (manifest.optInt("schema") != SCHEMA) {
            return "This game file comes from a newer or older launcher (format ${manifest.optInt("schema")}). Build it again with the current PC launcher."
        }
        if (!manifest.optString("gameId").equals(BuildConfig.DISC_GAME_ID, ignoreCase = true) ||
            !manifest.optString("dolSha256").equals(BuildConfig.DISC_DOL_SHA256, ignoreCase = true) ||
            !manifest.optString("relSha256").equals(BuildConfig.DISC_REL_SHA256, ignoreCase = true)
        ) {
            return "This game file was not built from the supported clean PAL Mario Kart Wii (${BuildConfig.DISC_GAME_ID})."
        }
        if (manifest.optString("kitFingerprint") != GameLibrary.kitFingerprint(context)) {
            return "This game file was built for a different version of this app. Build it again against the version installed on the headset."
        }
        if (manifest.optString("androidCpu") != BuildConfig.ANDROID_CPU) {
            return "This game file targets ${manifest.optString("androidCpu")}, but this app requires ${BuildConfig.ANDROID_CPU}. Build it again for the correct headset."
        }
        val expected = manifest.optString("librarySha256")
        if (librarySha256 == null) {
            return "This game file has no game library (libmain.so)."
        }
        if (!expected.equals(librarySha256, ignoreCase = true)) {
            return "The game library in this file is damaged (its checksum does not match). Copy the file again."
        }
        return null
    }

    private class CountingInputStream(input: InputStream) : FilterInputStream(input) {
        @Volatile
        var count = 0L
            private set

        override fun read(): Int = super.read().also { if (it >= 0) count++ }

        override fun read(b: ByteArray, off: Int, len: Int): Int = super.read(b, off, len).also { if (it > 0) count += it }

        override fun skip(n: Long): Long = super.skip(n).also { count += it }
    }
}
