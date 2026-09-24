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
 * What Mii pictures are drawn from: FFL's Mii parts (FFLResHigh.dat), the PC launcher's
 * MiiRenderingResourceInstaller, and the 3DS Mii bodies the PC carries ([MiiBodies]). Both are
 * Nintendo's, so the app never carries them; the player downloads them once, the parts from the
 * Internet Archive's copy of Miitomo's files as on the PC, the bodies from Wheel Wizard's own
 * repository, and each is checked against the SHA-256 of the copy both launchers use.
 */
object MiiRenderResource {
    /** Endpoints.MiiRenderingArchive on the PC. */
    private const val ARCHIVE_URL = "https://web.archive.org/web/20180502054513id_/" +
        "http://download-cdn.miitomo.com/native/20180125111639/android/v2/asset_model_character_mii_AFLResHigh_2_3_dat.zip"
    private const val ENTRY = "asset/model/character/mii/AFLResHigh_2_3.dat"
    private const val ARCHIVE_BYTES = 4_393_464L
    private const val FILE_BYTES = 4_579_008L

    /** The PC's embedded body models, at the Wheel Wizard commit that added them. */
    private const val BODY_URL = "https://raw.githubusercontent.com/TeamWheelWizard/WheelWizard/" +
        "cc4c2e9df2ef5f7717e9b1fbe18a94acc4076b5f/WheelWizard/Features/MiiRendering/Resources/"
    private val MALE_BODY = BodyFile("mii_static_body_3ds_male_LE.rmdl", 14_320L, "f17b764f4c42729572548f1cf760ab6e2eb6420cde5898b904be7eaea68cf837")
    private val FEMALE_BODY = BodyFile("mii_static_body_3ds_female_LE.rmdl", 14_736L, "371639d2b73280bc2e23e6aac44b49403a026a0f019ad287ac2c7c996451742f")

    private const val ATTEMPTS = 3
    private const val TIMEOUT_MS = 30_000
    private const val TAG = "WiiCompiledLauncher"

    private class BodyFile(val name: String, val bytes: Long, val sha256: String)

    private val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "MiiResource").apply { isDaemon = true } }
    private val main by lazy { Handler(Looper.getMainLooper()) }

    @Volatile
    private var loaded: FflResource? = null

    /** The bodies once read, or null while they have not been; [NO_BODIES] when they could not be. */
    @Volatile
    private var loadedBodies: MiiBodies? = null
    private val NO_BODIES = MiiBodies(emptyList(), emptyList())

    /** Whether a download is running; main thread only. */
    var installing = false
        private set

    fun file(context: Context): File = File(context.filesDir, "MiiRendering/FFLResHigh.dat")

    private fun bodyFile(context: Context, body: BodyFile) = File(context.filesDir, "MiiRendering/${body.name}")

    /** Whether Miis can be drawn: the parts are here. */
    fun installed(context: Context): Boolean = file(context).length() == FILE_BYTES

    /** Whether Miis are drawn with their bodies, as on the PC: the parts and the bodies are here. */
    fun complete(context: Context): Boolean =
        installed(context) && listOf(MALE_BODY, FEMALE_BODY).all { bodyFile(context, it).length() == it.bytes }

    /** How much [install] downloads: what is not here yet. */
    fun downloadBytes(context: Context): Long =
        (if (installed(context)) 0L else ARCHIVE_BYTES) +
            listOf(MALE_BODY, FEMALE_BODY).filter { bodyFile(context, it).length() != it.bytes }.sumOf { it.bytes }

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

    /** The bodies, read once; null when they are not installed or cannot be read. Not on the main thread. */
    fun bodies(context: Context): MiiBodies? {
        loadedBodies?.let { return it.takeIf { it !== NO_BODIES } }
        synchronized(this) {
            loadedBodies?.let { return it.takeIf { it !== NO_BODIES } }
            if (!complete(context)) return null
            val bodies = try {
                MiiBodies.load(bodyFile(context, MALE_BODY), bodyFile(context, FEMALE_BODY))
            } catch (e: IOException) {
                Log.w(TAG, "Cannot read the Mii bodies", e)
                NO_BODIES
            }
            loadedBodies = bodies
            return bodies.takeIf { it !== NO_BODIES }
        }
    }

    /**
     * Downloads and installs what is missing. [progress] gets the bytes downloaded so far, and
     * [done] null or the reason it failed, both on the main thread.
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
            // Read the bodies again, now that they may be here.
            loadedBodies = null
            main.post {
                installing = false
                done(error)
            }
        }
    }

    private fun download(context: Context, progress: (Long) -> Unit) {
        var before = 0L
        if (!installed(context)) {
            retrying("Mii parts") { downloadParts(context) { bytes -> progress(before + bytes) } }
            before += ARCHIVE_BYTES
        }
        for (body in listOf(MALE_BODY, FEMALE_BODY)) {
            if (bodyFile(context, body).length() == body.bytes) continue
            retrying("Mii body ${body.name}") { downloadBody(context, body) { bytes -> progress(before + bytes) } }
            before += body.bytes
        }
    }

    private fun retrying(what: String, attempt: () -> Unit) {
        var failure: IOException? = null
        for (number in 1..ATTEMPTS) {
            try {
                attempt()
                return
            } catch (e: IOException) {
                Log.w(TAG, "$what download attempt $number/$ATTEMPTS failed: ${e.message}")
                failure = e
            }
        }
        throw failure ?: IOException("The $what could not be downloaded.")
    }

    private fun downloadParts(context: Context, progress: (Long) -> Unit) {
        val target = file(context)
        target.parentFile?.mkdirs()
        val archive = fetch(ARCHIVE_URL, "The Internet Archive", ARCHIVE_BYTES, progress)
        if (archive.size < 4 || archive[0] != 'P'.code.toByte() || archive[1] != 'K'.code.toByte()) {
            throw IOException("The download is not a ZIP archive.")
        }
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
    }

    private fun downloadBody(context: Context, body: BodyFile, progress: (Long) -> Unit) {
        val target = bodyFile(context, body)
        target.parentFile?.mkdirs()
        val bytes = fetch(BODY_URL + body.name, "GitHub", body.bytes, progress)
        val digest = sha256(bytes)
        if (digest != body.sha256) throw IOException("The downloaded Mii body is not the expected file (SHA-256 $digest).")
        MiiBodies.parse(bytes)
        val partial = File(target.parentFile, target.name + ".partial")
        try {
            partial.writeBytes(bytes)
            if (!partial.renameTo(target)) throw IOException("Cannot store ${target.path}")
        } finally {
            partial.delete()
        }
        Log.i(TAG, "Installed the Mii body ${target.path}")
    }

    private fun fetch(url: String, server: String, size: Long, progress: (Long) -> Unit): ByteArray {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = TIMEOUT_MS
            readTimeout = TIMEOUT_MS
            setRequestProperty("User-Agent", "WiiCompiledVR-Quest/${BuildConfig.VERSION_NAME}")
        }
        try {
            val code = connection.responseCode
            if (code != HttpURLConnection.HTTP_OK) throw IOException("$server answered $code.")
            val expected = connection.contentLengthLong
            val bytes = ByteArrayOutputStream(if (expected > 0) expected.toInt() else size.toInt())
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
            return bytes.toByteArray()
        } finally {
            connection.disconnect()
        }
    }

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

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
