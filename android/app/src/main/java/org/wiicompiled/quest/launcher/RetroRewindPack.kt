package org.wiicompiled.quest.launcher

import android.content.Context
import android.os.StatFs
import android.util.Log
import java.io.BufferedInputStream
import java.io.File
import java.io.IOException
import java.io.InterruptedIOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.zip.ZipInputStream
import org.wiicompiled.quest.GameStorage

/**
 * Fetches the Retro Rewind pack the modded game reads, from Retro Rewind's own distribution server,
 * the way WheelWizard does on a PC: the server publishes the URL of a full install zip, a list of
 * update zips with their versions, and a list of files each update deletes. An installation is the
 * base zip plus every update newer than the version it holds.
 *
 * None of that content is part of this app; this only downloads what Retro Rewind publishes. Only
 * the pack's own `RetroRewind6` tree is kept, which is what `[paths] retro_rewind_root` names; the
 * Riivolution XML beside it belongs to a Wii setup and means nothing here.
 */
object RetroRewindPack {

    private const val TAG = "WiiCompiledLauncher"

    private const val BASE_URL = "https://update.rwfc.net/RetroRewind/"
    private const val INSTALL_URL = BASE_URL + "RetroRewindInstall.txt"
    private const val VERSION_URL = BASE_URL + "RetroRewindVersion.txt"
    private const val DELETE_URL = BASE_URL + "RetroRewindDelete.txt"

    /** The server still lists some downloads under the host it used before; they moved, not vanished. */
    private const val OLD_HOST = "http://update.rwfc.net:8000/"
    private const val NEW_HOST = "https://update.rwfc.net/"

    /** Everything this app keeps lives under this directory inside the published zips. */
    private const val PACK_PREFIX = GameStorage.MOD_DIRECTORY + "/"

    private const val VERSION_FILE = "version.txt"
    private const val FREE_SPACE_MARGIN = 4L shl 30

    /** One published update: the version it produces and the zip that gets there. */
    data class Update(val version: String, val url: String, val description: String)

    /** One published deletion: the version that drops [path], relative to the pack's parent. */
    data class Deletion(val version: String, val path: String)

    /**
     * Reads the version feed: `<version> <url> <path> <description>` per line, oldest first. Lines
     * that do not parse are skipped rather than failing the update, as the PC launcher does.
     */
    fun parseUpdates(text: String): List<Update> =
        text.lineSequence().mapNotNull { line ->
            val parts = line.trim().split(' ', limit = 4)
            if (parts.size < 4) return@mapNotNull null
            val version = parts[0].trim()
            val url = parts[1].trim().replace(OLD_HOST, NEW_HOST)
            if (!isVersion(version) || url.isEmpty()) return@mapNotNull null
            Update(version, url, parts[3].trim())
        }.toList()

    /** Reads the deletion feed: `<version> <path>` per line. */
    fun parseDeletions(text: String): List<Deletion> =
        text.lineSequence().mapNotNull { line ->
            val parts = line.trim().split(' ', limit = 2)
            if (parts.size < 2) return@mapNotNull null
            val version = parts[0].trim()
            val path = parts[1].trim()
            if (!isVersion(version) || path.isEmpty()) return@mapNotNull null
            Deletion(version, path)
        }.toList()

    /** The updates to apply to an installation holding [installed] (null: none installed), in order. */
    fun updatesAfter(installed: String?, all: List<Update>): List<Update> =
        all.filter { installed == null || compare(it.version, installed) > 0 }.sortedWith { a, b -> compare(a.version, b.version) }

    /**
     * The pack-relative paths the updates between [installed] and [target] delete, in order. Paths
     * outside the pack (the loose zips and the Riivolution XML the feed also lists) are not ours.
     */
    fun deletionsBetween(installed: String?, target: String, all: List<Deletion>): List<String> =
        all
            .filter { (installed == null || compare(it.version, installed) > 0) && compare(it.version, target) <= 0 }
            .sortedWith { a, b -> compare(a.version, b.version) }
            .mapNotNull { packRelative(it.path) }

    /**
     * The updates to apply over an installation holding [installed], each with the pack-relative
     * paths its version deletes, in the order the PC launcher applies them: an update, then its
     * deletions, then the next update.
     */
    fun steps(installed: String?, updates: List<Update>, deletions: List<Deletion>): List<Pair<Update, List<String>>> {
        var previous = installed
        return updates.sortedWith { a, b -> compare(a.version, b.version) }.map { update ->
            val dropped = deletionsBetween(previous, update.version, deletions)
            previous = update.version
            update to dropped
        }
    }

    /** Dotted numeric comparison, which is all these versions ever are (6.12.7). */
    fun compare(left: String, right: String): Int {
        val a = left.split('.')
        val b = right.split('.')
        for (i in 0 until maxOf(a.size, b.size)) {
            val difference = (a.getOrNull(i)?.toIntOrNull() ?: 0) - (b.getOrNull(i)?.toIntOrNull() ?: 0)
            if (difference != 0) return difference
        }
        return 0
    }

    private fun isVersion(value: String): Boolean =
        value.isNotEmpty() && value.split('.').let { it.size >= 2 && it.all { part -> part.toIntOrNull() != null } }

    /**
     * A published path relative to the pack, or null when it is not inside the pack. Paths that try
     * to climb out of it are refused: nothing outside the pack directory is ever touched.
     */
    fun packRelative(published: String): String? {
        val name = published.replace('\\', '/').trimStart('/')
        if (!name.startsWith(PACK_PREFIX)) return null
        val relative = name.removePrefix(PACK_PREFIX).trim('/')
        if (relative.isEmpty() || relative.split('/').any { it == ".." }) return null
        return relative
    }

    /** The newest version among [updates], or null when the feed listed none. */
    fun latest(updates: List<Update>): String? =
        updates.map { it.version }.maxWithOrNull { a, b -> compare(a, b) }

    /**
     * The newest version Retro Rewind publishes, read from the same version feed an update applies.
     * Network work: call it off the main thread. Throws IOException when the server cannot be read.
     */
    fun latestPublishedVersion(): String? = latest(parseUpdates(fetchText(VERSION_URL) { false }))

    /** The version the installed pack holds, or null when there is none. */
    fun installedVersion(context: Context): String? =
        File(GameStorage.modDirectory(context), VERSION_FILE)
            .takeIf { it.isFile }
            ?.readText()
            ?.trim()
            ?.takeIf { isVersion(it) }

    /**
     * Installs or updates the pack. Null on success, otherwise the message to show.
     *
     * A missing or version-less pack is replaced by the published base zip, staged beside it and
     * swapped in only once it looks like a pack. Updates are then applied over the installed pack
     * and the version is written last, so an interrupted update simply runs again next time.
     */
    fun run(context: Context, progress: GameSetup.Progress, cancelled: () -> Boolean, finishing: () -> Unit): String? {
        val pack = GameStorage.modDirectory(context)
        val gameRoot = GameStorage.gameRoot(context)
        gameRoot.mkdirs()

        val installed = installedVersion(context)
        val available = StatFs(gameRoot.absolutePath).availableBytes
        if (installed == null && available < FREE_SPACE_MARGIN) {
            return "Not enough free space: the Retro Rewind pack needs about ${GameFiles.gigabytes(FREE_SPACE_MARGIN)}, and ${GameFiles.gigabytes(available)} is free."
        }

        return try {
            val updates = updatesAfter(installed, parseUpdates(fetchText(VERSION_URL, cancelled)))
            if (installed != null && updates.isEmpty()) {
                Log.i(TAG, "Retro Rewind $installed is already the published version")
                return null
            }

            if (installed == null) {
                val base = fetchText(INSTALL_URL, cancelled).trim().replace(OLD_HOST, NEW_HOST)
                if (base.isEmpty()) return "Retro Rewind's server did not say where its download is."
                Log.i(TAG, "Installing the Retro Rewind pack from $base")
                val staging = File(gameRoot, "${GameStorage.MOD_DIRECTORY}.downloading")
                staging.deleteRecursively()
                try {
                    download(base, staging, progress, cancelled)
                    if (!File(staging, "Binaries/Code.pul").isFile || !File(staging, VERSION_FILE).isFile) {
                        return "Retro Rewind's download did not contain the pack."
                    }
                    finishing()
                    GameFiles.replace(pack, staging)?.let { return it }
                } finally {
                    staging.deleteRecursively()
                }
            }

            // Every update is a partial tree that lands on top of the installed pack, so they are
            // applied in order, each followed by its own deletions before the next one can put a
            // file back, and the version is only written once all of them are in.
            val remaining = updatesAfter(installedVersion(context), updates)
            if (remaining.isNotEmpty()) {
                val deletions = parseDeletions(fetchText(DELETE_URL, cancelled))
                for ((update, dropped) in steps(installedVersion(context), remaining, deletions)) {
                    Log.i(TAG, "Applying Retro Rewind ${update.version}")
                    download(update.url, pack, progress, cancelled)
                    for (relative in dropped) File(pack, relative).deleteRecursively()
                }
                finishing()
                File(pack, VERSION_FILE).writeText(remaining.last().version)
            }
            Log.i(TAG, "Retro Rewind pack ready: ${installedVersion(context)}")
            null
        } catch (interrupted: InterruptedIOException) {
            throw interrupted
        } catch (failure: IOException) {
            "Retro Rewind could not be downloaded: ${failure.message}"
        }
    }

    private fun fetchText(url: String, cancelled: () -> Boolean): String {
        if (cancelled()) throw InterruptedIOException("Download cancelled")
        return open(url).use { connection ->
            connection.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
        }
    }

    /** Unpacks the pack directory of the zip at [url] into [target], over whatever is there. */
    private fun download(url: String, target: File, progress: GameSetup.Progress, cancelled: () -> Boolean) {
        if (cancelled()) throw InterruptedIOException("Download cancelled")
        open(url).use { connection ->
            val total = connection.contentLengthLong
            val counted = CountingInputStream(connection.inputStream)
            ZipInputStream(BufferedInputStream(counted, 1 shl 20)).use { zip ->
                val buffer = ByteArray(1 shl 20)
                var lastReport = 0L
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (entry.isDirectory) continue
                    val relative = packRelative(entry.name) ?: continue
                    val file = File(target, relative)
                    file.parentFile?.mkdirs()
                    file.outputStream().use { out ->
                        while (true) {
                            val read = zip.read(buffer)
                            if (read < 0) break
                            out.write(buffer, 0, read)
                            if (counted.count - lastReport >= 8L shl 20) {
                                lastReport = counted.count
                                if (!progress.update(counted.count, total) || cancelled()) {
                                    throw InterruptedIOException("Download cancelled")
                                }
                            }
                        }
                    }
                }
            }
            progress.update(total.coerceAtLeast(0), total)
        }
    }

    private fun open(url: String): HttpURLConnection {
        val connection = URL(url).openConnection() as HttpURLConnection
        connection.connectTimeout = 30_000
        connection.readTimeout = 60_000
        connection.instanceFollowRedirects = true
        // Android asks for gzip by default, which would hide the real length and re-encode the zip.
        connection.setRequestProperty("Accept-Encoding", "identity")
        val code = connection.responseCode
        if (code != HttpURLConnection.HTTP_OK) {
            connection.disconnect()
            throw IOException("Retro Rewind's server answered $code")
        }
        return connection
    }

    private fun <T> HttpURLConnection.use(block: (HttpURLConnection) -> T): T {
        try {
            return block(this)
        } finally {
            disconnect()
        }
    }

    /** Counts what has actually come down the wire, which is what the progress bar reports. */
    private class CountingInputStream(input: java.io.InputStream) : java.io.FilterInputStream(input) {
        @Volatile
        var count = 0L
            private set

        override fun read(): Int = super.read().also { if (it >= 0) count++ }

        override fun read(bytes: ByteArray, offset: Int, length: Int): Int =
            super.read(bytes, offset, length).also { if (it > 0) count += it }
    }
}
