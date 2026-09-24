package org.wiicompiled.quest.launcher

import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors
import org.json.JSONException
import org.json.JSONObject
import org.wiicompiled.quest.BuildConfig

/**
 * The Wheel Wizard team's status message (WhWzStatusManager), such as "Unstable servers!", which
 * the sidebar shows beside the version: WheelWizard-Data's status.json, asked every 90 seconds
 * while the launcher is on screen. As on the PC, a status that cannot be had is shown as an
 * error ([Status.failed]).
 */
object WheelWizardStatus {
    private const val TAG = "WiiCompiledLauncher"
    private const val URL_STATUS = "https://raw.githubusercontent.com/TeamWheelWizard/WheelWizard-Data/main/status.json"
    private const val REFRESH_MS = 90_000L
    private const val TIMEOUT_MS = 20_000

    /** WhWzStatusVariant: the preset icons. */
    enum class Variant { None, Warning, Error, Success, Info, Party, Question }

    /**
     * WhWzStatus: a preset [variant], or an [icon] of its own (SVG path data) in [color], and the
     * [message] its tip shows; [failed] when there was none to read.
     */
    class Status(
        val variant: Variant?,
        val message: String,
        val icon: String? = null,
        val color: String? = null,
        val failed: Boolean = false,
    ) {
        /** UpdateLiveAlert shows the icon for a variant other than None, or an icon of its own. */
        val visible: Boolean get() = (variant != null && variant != Variant.None) || !icon.isNullOrEmpty()
    }

    private val main by lazy { Handler(Looper.getMainLooper()) }
    private val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "WhWzStatus").apply { isDaemon = true } }
    private val listeners = LinkedHashSet<() -> Unit>()
    private var running = false

    /** The last status read, null until the first answer; main thread only. */
    var status: Status? = null
        private set

    private val ticker = object : Runnable {
        override fun run() {
            fetch()
            main.postDelayed(this, REFRESH_MS)
        }
    }

    fun addListener(listener: () -> Unit) {
        listeners += listener
    }

    fun removeListener(listener: () -> Unit) {
        listeners -= listener
    }

    fun start() {
        if (running) return
        running = true
        main.post(ticker)
    }

    fun stop() {
        running = false
        main.removeCallbacks(ticker)
    }

    private fun fetch() {
        worker.execute {
            val fresh = try {
                parse(download())
            } catch (e: IOException) {
                Log.i(TAG, "Wheel Wizard's status is unavailable: ${e.message}")
                Status(Variant.Error, "", failed = true)
            }
            main.post {
                status = fresh
                listeners.toList().forEach { it() }
            }
        }
    }

    /**
     * status.json as the PC reads it: its message is required and its variant, when there, one of
     * the known names in any case; anything else is no status, which the PC shows as an error.
     */
    fun parse(json: String): Status = try {
        val root = JSONObject(json)
        val message = root.opt("message") as? String ?: throw IOException("Wheel Wizard's status has no message.")
        val variant = when (val name = root.opt("variant")) {
            null, JSONObject.NULL -> null
            is String -> Variant.entries.firstOrNull { it.name.equals(name, ignoreCase = true) }
                ?: throw IOException("Unknown status variant $name")
            else -> throw IOException("Unknown status variant $name")
        }
        Status(variant, message, icon = root.opt("icon") as? String, color = root.opt("color") as? String)
    } catch (e: JSONException) {
        throw IOException("Wheel Wizard's status cannot be read.", e)
    }

    private fun download(): String {
        val connection = (URL(URL_STATUS).openConnection() as HttpURLConnection).apply {
            connectTimeout = TIMEOUT_MS
            readTimeout = TIMEOUT_MS
            setRequestProperty("User-Agent", "WiiCompiledVR-Quest/${BuildConfig.VERSION_NAME}")
        }
        try {
            val code = connection.responseCode
            if (code !in 200..299) throw IOException("GitHub answered $code.")
            return connection.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
        } finally {
            connection.disconnect()
        }
    }
}
