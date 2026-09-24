package org.wiicompiled.quest.launcher

import android.content.Context
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.util.LruCache
import android.widget.ImageView
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import org.wiicompiled.quest.R

/**
 * Mii pictures for My Miis, its editor and the profiles, drawn by [MiiRenderer] off the main
 * thread and kept in a memory cache (MiiImagesSingletonService on the PC). Nothing is drawn until
 * the player has downloaded the Mii parts ([MiiRenderResource]); callers then show their
 * placeholder. Miis have their bodies once those are downloaded too.
 */
object MiiImages {
    private const val TAG = "WiiCompiledLauncher"

    private val cache = object : LruCache<String, Bitmap>((Runtime.getRuntime().maxMemory() / 8).toInt()) {
        override fun sizeOf(key: String, value: Bitmap): Int = value.allocationByteCount
    }
    private val renderers = Executors.newFixedThreadPool(2) { runnable -> Thread(runnable, "MiiImages").apply { isDaemon = true } }
    private val previewer = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "MiiPreview").apply { isDaemon = true } }
    private val previewGeneration = AtomicLong()
    /** Changes with [clear]: a picture drawn before is shown but not kept. */
    private val cacheGeneration = AtomicInteger()
    private val main by lazy { Handler(Looper.getMainLooper()) }

    /**
     * Shows [mii] in [view], [size] pixels square, with its upper body unless not [withBody]; a
     * recycled view only takes the picture it asked for last.
     */
    fun head(context: Context, view: ImageView, mii: Mii, size: Int, withBody: Boolean = true) {
        val copy = mii.copy()
        val app = context.applicationContext
        show(context, view, "${if (withBody) "mii" else "head"}:$size:${copy.lookKey()}") { resource ->
            MiiRenderer.render(resource, copy, size, bodies = if (withBody) MiiRenderResource.bodies(app) else null)
        }
    }

    /** Shows one choice of a face part in [view] (MiiRenderer.partIcon); nothing for "none". */
    fun part(context: Context, view: ImageView, mii: Mii, part: MiiRenderer.Part, index: Int, size: Int) {
        val colour = when (part) {
            MiiRenderer.Part.Eyebrow -> mii.eyebrowColor
            MiiRenderer.Part.Eye -> mii.eyeColor
            MiiRenderer.Part.Mouth -> mii.lipColor
            MiiRenderer.Part.Glasses -> mii.glassesColor
            MiiRenderer.Part.Mustache -> mii.facialHairColor
            MiiRenderer.Part.Nose -> 0
        }
        val copy = mii.copy()
        show(context, view, "part:$part:$index:$size:$colour") { resource -> MiiRenderer.partIcon(resource, copy, part, index, size) }
    }

    /**
     * [mii] turned as [pose], [size] pixels square, for [done] on the main thread; null when it
     * cannot be drawn, such as before the Mii parts are downloaded.
     */
    fun picture(context: Context, mii: Mii, size: Int, pose: MiiRenderer.Pose, done: (Bitmap?) -> Unit) {
        val copy = mii.copy()
        val key = "mii:$size:${pose.key}:${copy.lookKey()}"
        cache.get(key)?.let {
            done(it)
            return
        }
        val app = context.applicationContext
        val generation = cacheGeneration.get()
        renderers.execute {
            val bitmap = draw(app, key, generation) { resource ->
                MiiRenderer.render(resource, copy, size, pose, MiiRenderResource.bodies(app))
            }
            main.post { done(bitmap) }
        }
    }

    /**
     * The editor's picture: only the newest request is drawn, so a burst of changes costs one
     * render. [done] gets null when there is nothing to show.
     */
    fun preview(context: Context, mii: Mii, size: Int, done: (Bitmap?) -> Unit) {
        val app = context.applicationContext
        val key = "mii:$size:${mii.lookKey()}"
        cache.get(key)?.let {
            previewGeneration.incrementAndGet()
            done(it)
            return
        }
        val generation = previewGeneration.incrementAndGet()
        val installs = cacheGeneration.get()
        val copy = mii.copy()
        previewer.execute {
            if (previewGeneration.get() != generation) return@execute
            val bitmap = draw(app, key, installs) { resource ->
                MiiRenderer.render(resource, copy, size, bodies = MiiRenderResource.bodies(app))
            }
            main.post { if (previewGeneration.get() == generation) done(bitmap) }
        }
    }

    /** Forgets every picture, for when the parts were just installed and placeholders can become Miis. */
    fun clear() {
        cacheGeneration.incrementAndGet()
        cache.evictAll()
    }

    private fun show(context: Context, view: ImageView, key: String, render: (FflResource) -> IntArray?) {
        view.setTag(R.id.mii_image_key, key)
        val cached = cache.get(key)
        if (cached != null) {
            view.setImageBitmap(cached)
            return
        }
        view.setImageDrawable(null)
        val app = context.applicationContext
        val generation = cacheGeneration.get()
        renderers.execute {
            // A view scrolled away before its turn does not need its picture any more. Reading its
            // tag here is only a hint; the main thread checks again before showing the picture.
            if (view.getTag(R.id.mii_image_key) != key) return@execute
            val bitmap = draw(app, key, generation, render) ?: return@execute
            main.post { if (view.getTag(R.id.mii_image_key) == key) view.setImageBitmap(bitmap) }
        }
    }

    private fun draw(context: Context, key: String, generation: Int, render: (FflResource) -> IntArray?): Bitmap? {
        cache.get(key)?.let { return it }
        val resource = MiiRenderResource.load(context) ?: return null
        return try {
            val pixels = render(resource) ?: return null
            val size = kotlin.math.sqrt(pixels.size.toDouble()).toInt()
            Bitmap.createBitmap(pixels, size, size, Bitmap.Config.ARGB_8888).also {
                // Drawn from what was installed before a clear: shown, but not kept.
                if (cacheGeneration.get() == generation) cache.put(key, it)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Cannot draw a Mii picture", e)
            null
        } catch (e: OutOfMemoryError) {
            null
        }
    }
}
