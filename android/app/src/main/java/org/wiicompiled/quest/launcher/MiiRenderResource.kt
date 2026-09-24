package org.wiicompiled.quest.launcher

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.concurrent.Executors
import java.util.zip.ZipInputStream
import org.wiicompiled.quest.BuildConfig

/**
 * FFL's Mii parts (FFLResHigh.dat), which every Mii picture is drawn from: the PC launcher's
 * MiiRenderingResourceInstaller. The file is Nintendo's, so the app never carries it; like the PC,
 * the player downloads it once from the Internet Archive's copy of Miitomo's files, and it is
 * checked against the SHA-256 of the copy both launchers install.
 */
object MiiRenderResource {
    /** Endpoints.MiiRenderingArchive on the PC. */
    private const val ARCHIVE_URL = "https://web.archive.org/web/20180502054513id_/" +
        "http://download-cdn.miitomo.com/native/20180125111639/android/v2/asset_model_character_mii_AFLResHigh_2_3_dat.zip"
    private const val ENTRY = "asset/model/character/mii/AFLResHigh_2_3.dat"
    const val DOWNLOAD_BYTES = 4_393_464L
    private const val FILE_BYTES = 4_579_008L
    private const val ATTEMPTS = 3
    private const val TIMEOUT_MS = 30_000
    private const val TAG = "WiiCompiledLauncher"

    private val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "MiiResource").apply { isDaemon = true } }
    private val main by lazy { Handler(Looper.getMainLooper()) }

    @Volatile
    private var loaded: FflResource? = null

    /** Whether a download is running; main thread only. */
    var installing = false
        private set

    fun file(context: Context): File = File(context.filesDir, "MiiRendering/FFLResHigh.dat")

    fun installed(context: Context): Boolean = file(context).length() == FILE_BYTES

    /** The parts, read once; null when they are not installed or cannot be read. Not on the main thread. */
    fun load(context: Context): FflResource? {
        loaded?.let { return it }
        synchronized(this) {
            loaded?.let { return it }
            if (!installed(context)) return null
            return try {
                FflResource.load(file(context)).also { loaded = it }
            } catch (e: IOException) {
                Log.w(TAG, "Cannot read the Mii parts", e)
                null
            }
        }
    }

    /**
     * Downloads and installs the parts. [progress] gets the bytes downloaded so far, and [done]
     * null or the reason it failed, both on the main thread.
     */
    fun install(context: Context, progress: (Long) -> Unit, done: (String?) -> Unit) {
        if (installing) return
        installing = true
        val app = context.applicationContext
        worker.execute {
            val error = try {
                download(app) { bytes -> main.post { progress(bytes) } }
                null
            } catch (e: IOException) {
                Log.w(TAG, "Cannot install the Mii parts", e)
                e.message ?: e.toString()
            }
            main.post {
                installing = false
                done(error)
            }
        }
    }

    private fun download(context: Context, progress: (Long) -> Unit) {
        val target = file(context)
        target.parentFile?.mkdirs()
        var failure: IOException? = null
        for (attempt in 1..ATTEMPTS) {
            try {
                val archive = fetch(progress)
                val partial = File(target.parentFile, target.name + ".partial")
                try {
                    val digest = extract(archive, partial)
                    if (digest != FflResource.SHA256) throw IOException("The downloaded Mii parts are not the expected file (SHA-256 $digest).")
                    FflResource.load(partial)
                    if (!partial.renameTo(target)) throw IOException("Cannot store ${target.path}")
                } finally {
                    partial.delete()
                }
                Log.i(TAG, "Installed the Mii parts to ${target.path}")
                return
            } catch (e: IOException) {
                Log.w(TAG, "Mii parts download attempt $attempt/$ATTEMPTS failed: ${e.message}")
                failure = e
            }
        }
        throw failure ?: IOException("The Mii parts could not be downloaded.")
    }

    private fun fetch(progress: (Long) -> Unit): ByteArray {
        val connection = (URL(ARCHIVE_URL).openConnection() as HttpURLConnection).apply {
            connectTimeout = TIMEOUT_MS
            readTimeout = TIMEOUT_MS
            setRequestProperty("User-Agent", "WiiCompiledVR-Quest/${BuildConfig.VERSION_NAME}")
        }
        try {
            val code = connection.responseCode
            if (code != HttpURLConnection.HTTP_OK) throw IOException("The Internet Archive answered $code.")
            val expected = connection.contentLengthLong
            val bytes = ByteArrayOutputStream(if (expected > 0) expected.toInt() else DOWNLOAD_BYTES.toInt())
            connection.inputStream.use { input ->
                val buffer = ByteArray(64 * 1024)
                var total = 0L
                var reported = 0L
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    bytes.write(buffer, 0, count)
                    total += count
                    if (total - reported >= 64 * 1024) {
                        reported = total
                        progress(total)
                    }
                }
                progress(total)
                if (expected > 0 && total != expected) throw IOException("The download stopped after $total of $expected bytes.")
            }
            val archive = bytes.toByteArray()
            if (archive.size < 4 || archive[0] != 'P'.code.toByte() || archive[1] != 'K'.code.toByte()) {
                throw IOException("The download is not a ZIP archive.")
            }
            return archive
        } finally {
            connection.disconnect()
        }
    }

    /** Writes the archive's resource entry to [to] and returns its SHA-256. */
    private fun extract(archive: ByteArray, to: File): String {
        ZipInputStream(ByteArrayInputStream(archive)).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: throw IOException("The archive does not contain $ENTRY.")
                if (entry.name != ENTRY) continue
                val digest = MessageDigest.getInstance("SHA-256")
                to.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val count = zip.read(buffer)
                        if (count < 0) break
                        digest.update(buffer, 0, count)
                        output.write(buffer, 0, count)
                    }
                }
                return digest.digest().joinToString("") { "%02x".format(it) }
            }
        }
    }
}
