package org.wiicompiled.quest.launcher

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.io.IOException
import java.util.concurrent.Executors
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.R

/**
 * What the profiles page and the sidebar's profile card show: the licences of Retro Rewind's save,
 * which one is primary, and what Retro WFC knows of them (their Mii pictures, who is in a room
 * now, their VR history), plus WheelWizard's badges. Files are read and the network asked off the
 * main thread, and every answer arrives on it. Mii pictures are also kept on disk, so the sidebar
 * shows them at once, even offline.
 */
object ProfileStore {

    /** The four licence slots of the save, an empty one null. */
    data class Snapshot(val licenses: List<RksysProfiles.License?>) {
        val any: Boolean get() = licenses.any { it != null }
    }

    private const val TAG = "WiiCompiledLauncher"
    // LauncherActivity's preferences file, where the launcher keeps its other choices.
    private const val PREFERENCES = "launcher"
    private const val KEY_PRIMARY = "primaryProfile"
    private const val IMAGE_DIRECTORY = "profile-miis"
    /** SettingValues.NoName: the name the game's guest Miis carry. */
    private const val GUEST_NAME = "no name"
    /** Who is in a room is asked again after this long; WheelWizard polls its live rooms similarly. */
    private const val ONLINE_REFRESH_MS = 30_000L

    private val worker = Executors.newFixedThreadPool(2) { runnable -> Thread(runnable, "Profiles").apply { isDaemon = true } }
    private val main by lazy { Handler(Looper.getMainLooper()) }

    // Main thread only.
    private val images = HashMap<String, Bitmap>()
    private val imagesAsked = HashSet<String>()
    private val waiting = HashMap<String, MutableList<(Bitmap?) -> Unit>>()
    private var online: Set<String> = emptySet()
    private var onlineAt = 0L
    private var badges: Map<String, List<RetroWfc.Badge>>? = null

    /** Reads Retro Rewind's save; [done] gets null when there is none, or it is not readable. */
    fun load(context: Context, done: (Snapshot?) -> Unit) {
        val app = context.applicationContext
        worker.execute {
            val snapshot = read(app)
            main.post { done(snapshot) }
        }
    }

    private fun read(context: Context): Snapshot? {
        val save = RksysProfiles.readLicenses(GameStorage.retroRewindSave(context)) ?: return null
        val ratings = try {
            GameStorage.retroRewindRatings(context).takeIf { it.isFile }?.readBytes()?.let(RksysProfiles::parseRatings).orEmpty()
        } catch (e: IOException) {
            emptyMap()
        }
        return RksysProfiles.parse(save, ratings)?.let(::Snapshot)
    }

    /** GameLicenseService's FOCUSED_USER: the licence the sidebar shows and the page opens on. */
    fun primarySlot(context: Context): Int =
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).getInt(KEY_PRIMARY, 0).coerceIn(0, RksysProfiles.SLOTS - 1)

    fun setPrimarySlot(context: Context, slot: Int) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit().putInt(KEY_PRIMARY, slot).apply()
    }

    /** A licence's name as the PC shows it: a guest Mii's "no name" reads No name. */
    fun displayName(context: Context, license: RksysProfiles.License): String =
        if (license.name.isBlank() || license.name == GUEST_NAME) context.getString(R.string.profiles_no_name) else license.name

    /** The licence the sidebar shows: the primary one, or the first there is when its slot is empty. */
    fun sidebarLicense(context: Context, snapshot: Snapshot): RksysProfiles.License? =
        snapshot.licenses.getOrNull(primarySlot(context)) ?: snapshot.licenses.firstOrNull { it != null }

    /**
     * The Mii picture Retro WFC keeps for [friendCode]. [done] gets the one already known at once
     * (from memory or disk), then Retro WFC's own the first time this session it differs, or null
     * when there is none at all. A licence never taken online has no friend code, and no picture.
     */
    fun miiImage(context: Context, friendCode: String, done: (Bitmap?) -> Unit) {
        if (friendCode.isEmpty()) {
            done(null)
            return
        }
        val known = images[friendCode]
        if (known != null) done(known)
        // A fetch already on its way answers everyone who asked meanwhile.
        waiting[friendCode]?.let {
            it += done
            return
        }
        if (!imagesAsked.add(friendCode)) {
            if (known == null) done(null)
            return
        }
        waiting[friendCode] = mutableListOf(done)
        val file = File(File(context.applicationContext.cacheDir, IMAGE_DIRECTORY), "$friendCode.png")
        worker.execute {
            val cached = if (file.isFile) runCatching { file.readBytes() }.getOrNull() else null
            if (known == null) cached?.let(::decode)?.let { bitmap -> main.post { remember(friendCode, bitmap, finished = false) } }
            val fresh = try {
                RetroWfc.miiImage(friendCode)
            } catch (e: IOException) {
                Log.i(TAG, "No Mii picture for $friendCode from Retro WFC: ${e.message}")
                null
            }
            val bitmap = fresh?.takeIf { !it.contentEquals(cached ?: byteArrayOf()) }?.let(::decode)
            if (fresh != null && bitmap != null) {
                runCatching {
                    file.parentFile?.mkdirs()
                    file.writeBytes(fresh)
                }
            }
            main.post { remember(friendCode, bitmap, finished = true) }
        }
    }

    /** Hands a picture to everyone waiting for [friendCode]; the last answer also tells those with none. */
    private fun remember(friendCode: String, bitmap: Bitmap?, finished: Boolean) {
        if (bitmap != null) images[friendCode] = bitmap
        val callbacks = (if (finished) waiting.remove(friendCode) else waiting[friendCode]?.toList()).orEmpty()
        when {
            bitmap != null -> callbacks.forEach { it(bitmap) }
            finished && images[friendCode] == null -> callbacks.forEach { it(null) }
        }
    }

    private fun decode(bytes: ByteArray): Bitmap? = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)

    /** The friend codes in a room now, asked again when the last answer is old; the last known ones when that fails. */
    fun online(done: (Set<String>) -> Unit) {
        if (onlineAt != 0L && SystemClock.elapsedRealtime() - onlineAt < ONLINE_REFRESH_MS) {
            done(online)
            return
        }
        worker.execute {
            val fresh = try {
                RetroWfc.onlineFriendCodes()
            } catch (e: IOException) {
                Log.i(TAG, "Retro WFC's rooms are unavailable: ${e.message}")
                null
            }
            main.post {
                if (fresh != null) {
                    online = fresh
                    onlineAt = SystemClock.elapsedRealtime()
                }
                done(online)
            }
        }
    }

    /** WheelWizard's badges by friend code, fetched once a session. */
    fun badges(done: (Map<String, List<RetroWfc.Badge>>) -> Unit) {
        badges?.let {
            done(it)
            return
        }
        worker.execute {
            val fresh = try {
                RetroWfc.badges()
            } catch (e: IOException) {
                Log.i(TAG, "WheelWizard's badges are unavailable: ${e.message}")
                null
            }
            main.post {
                if (fresh != null) badges = fresh
                done(badges.orEmpty())
            }
        }
    }

    /** [friendCode]'s VR over the last [days] days (999 for all of it). */
    fun history(friendCode: String, days: Int, done: (Result<RetroWfc.History>) -> Unit) {
        worker.execute {
            val result = runCatching { RetroWfc.history(friendCode, days) }
            main.post { done(result) }
        }
    }
}
