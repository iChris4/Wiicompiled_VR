package org.wiicompiled.quest.launcher

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.util.LruCache
import android.widget.ImageView
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors
import org.wiicompiled.quest.BuildConfig
import org.wiicompiled.quest.R

/**
 * GameBanana's pictures for the mod browser: fetched off the main thread, scaled down to the width
 * they are shown at, and kept in a memory cache so scrolling back or reopening a mod does not
 * fetch them again. A picture that cannot be read is simply not shown, as in WheelWizard.
 */
object RemoteImages {

    private const val MAX_BYTES = 8 shl 20
    private const val TIMEOUT_MS = 20_000

    private val cache = object : LruCache<String, Bitmap>((Runtime.getRuntime().maxMemory() / 8).toInt()) {
        override fun sizeOf(key: String, value: Bitmap): Int = value.allocationByteCount
    }
    private val fetchers = Executors.newFixedThreadPool(3) { runnable -> Thread(runnable, "RemoteImages").apply { isDaemon = true } }
    private val main by lazy { Handler(Looper.getMainLooper()) }

    /**
     * Shows the picture at [url] in [view], at most [maxWidth] pixels wide. A recycled list row
     * only ever takes the picture it asked for last.
     */
    fun into(view: ImageView, url: String?, maxWidth: Int) {
        view.setTag(R.id.remote_image_url, url)
        view.setImageDrawable(null)
        if (url == null) return
        fetch(url, maxWidth) { bitmap ->
            if (view.getTag(R.id.remote_image_url) == url) view.setImageBitmap(bitmap)
        }
    }

    /** Hands the picture at [url] to [done] on the main thread; nothing happens when it cannot be read. */
    fun fetch(url: String, maxWidth: Int, done: (Bitmap) -> Unit) {
        val key = "$maxWidth:$url"
        cache.get(key)?.let {
            done(it)
            return
        }
        fetchers.execute {
            val bitmap = try {
                decode(read(url), maxWidth)
            } catch (e: IOException) {
                null
            } catch (e: OutOfMemoryError) {
                null
            } ?: return@execute
            cache.put(key, bitmap)
            main.post { done(bitmap) }
        }
    }

    private fun read(url: String): ByteArray {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = TIMEOUT_MS
            readTimeout = TIMEOUT_MS
            setRequestProperty("User-Agent", "WiiCompiledVR-Quest/${BuildConfig.VERSION_NAME}")
        }
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) throw IOException("HTTP ${connection.responseCode}")
            connection.inputStream.use { input ->
                val bytes = ByteArrayOutputStream()
                val buffer = ByteArray(32 * 1024)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    bytes.write(buffer, 0, count)
                    if (bytes.size() > MAX_BYTES) throw IOException("$url is too large to show")
                }
                return bytes.toByteArray()
            }
        } finally {
            connection.disconnect()
        }
    }

    /** Decodes at the smallest power-of-two reduction that still covers [maxWidth]. */
    private fun decode(bytes: ByteArray, maxWidth: Int): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        var sample = 1
        while (bounds.outWidth / (sample * 2) >= maxWidth) sample *= 2
        return BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
    }
}
