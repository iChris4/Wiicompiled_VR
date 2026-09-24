package org.wiicompiled.quest.launcher

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.File
import java.io.IOException
import java.util.concurrent.Executors
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.R

/**
 * What the profiles page and the sidebar's profile card show: the licences of Retro Rewind's save
 * and their Miis, which one is primary, and what Retro WFC knows of them (the Miis they last played
 * with, their VR history), plus WheelWizard's badges; who is online comes from [LiveRooms]. Files
 * are read and the network asked off the main thread, and every answer arrives on it. What Retro
 * WFC saw of a Mii is also kept on disk, so the sidebar shows it at once, even offline.
 */
object ProfileStore {

    /**
     * The four licence slots of the save, an empty one null, and the licences' Miis the headset's
     * Mii database holds, by ID.
     */
    data class Snapshot(val licenses: List<RksysProfiles.License?>, val miis: Map<Long, Mii> = emptyMap()) {
        val any: Boolean get() = licenses.any { it != null }
    }

    /** What Retro WFC last saw of a profile's Mii: the Mii, to draw it, and its own 64-pixel picture of it. */
    private class Remote(val mii: Mii?, val picture: Bitmap?, val data: ByteArray?, val image: ByteArray?)

    private const val TAG = "WiiCompiledLauncher"
    // LauncherActivity's preferences file, where the launcher keeps its other choices.
    private const val PREFERENCES = "launcher"
    private const val KEY_PRIMARY = "primaryProfile"
    private const val IMAGE_DIRECTORY = "profile-miis"
    /** SettingValues.NoName: the name the game's guest Miis carry. */
    private const val GUEST_NAME = "no name"

    private val worker = Executors.newFixedThreadPool(2) { runnable -> Thread(runnable, "Profiles").apply { isDaemon = true } }
    private val main by lazy { Handler(Looper.getMainLooper()) }

    // Main thread only.
    private val remotes = HashMap<String, Remote>()
    private val remotesAsked = HashSet<String>()
    private val waiting = HashMap<String, MutableList<(Remote?) -> Unit>>()
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
        val licenses = RksysProfiles.parse(save, ratings) ?: return null
        // GameLicenseService.ParseMiiData: a licence's Mii is looked up in the Mii database by ID.
        val ids = licenses.mapNotNull { it?.miiId }.toSet()
        val miis = try {
            MiiDatabase.file(GameStorage.nandDirectory(context)).takeIf { it.isFile }?.let(MiiDatabase::byId).orEmpty()
        } catch (e: IOException) {
            emptyMap()
        }
        return Snapshot(licenses, miis.filterKeys { it in ids })
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
     * [license]'s Mii, [size] pixels square and turned as the PC's profile page and sidebar show it.
     * Like the PC, the Mii is the one of the headset's Mii database (the game's own, which My Miis
     * edits) with the licence's ID. A licence whose Mii is not there shows the Mii Retro WFC last
     * saw it play with, while that is still the licence's Mii. Both are drawn from the Mii parts,
     * with their upper bodies, once My Miis downloaded them; until then Retro WFC's 64-pixel
     * picture stands in. [done] can be called twice: with what is known at once, then with what
     * Retro WFC answers.
     */
    fun miiPicture(
        context: Context,
        snapshot: Snapshot,
        license: RksysProfiles.License,
        size: Int,
        done: (Bitmap?) -> Unit,
    ) {
        val drawable = MiiRenderResource.installed(context)
        val pose = MiiRenderer.Pose.SIDE
        val local = snapshot.miis[license.miiId]
        if (local != null && drawable) {
            MiiImages.picture(context, local, size, pose, done)
            return
        }
        val friendCode = license.friendCode
        remote(context, friendCode) { remote ->
            // What Retro WFC saw is of another Mii once the licence took a new one.
            val stale = remote?.mii != null && remote.mii.miiId != license.miiId
            when {
                remote == null || stale -> done(null)
                remote.mii != null && drawable -> MiiImages.picture(context, remote.mii, size, pose) { bitmap ->
                    // A newer answer may have arrived while this one was drawn.
                    if (remotes[friendCode] === remote) done(bitmap ?: remote.picture)
                }
                else -> done(remote.picture)
            }
        }
    }

    /**
     * What Retro WFC last saw of [friendCode]'s Mii. [done] gets what is already known at once (from
     * memory or disk), then Retro WFC's answer the first time this session it differs, or null when
     * there is nothing at all. A licence never taken online has no friend code, and nothing here.
     */
    private fun remote(context: Context, friendCode: String, done: (Remote?) -> Unit) {
        if (friendCode.isEmpty()) {
            done(null)
            return
        }
        val known = remotes[friendCode]
        if (known != null) done(known)
        // A fetch already on its way answers everyone who asked meanwhile.
        waiting[friendCode]?.let {
            it += done
            return
        }
        if (!remotesAsked.add(friendCode)) {
            if (known == null) done(null)
            return
        }
        waiting[friendCode] = mutableListOf(done)
        val directory = File(context.applicationContext.cacheDir, IMAGE_DIRECTORY)
        val imageFile = File(directory, "$friendCode.png")
        val dataFile = File(directory, "$friendCode.mii")
        worker.execute {
            val cached = remoteOf(read(dataFile), read(imageFile))
            if (known == null && cached != null) main.post { remember(friendCode, cached, finished = false) }
            val fresh = try {
                RetroWfc.playerMii(friendCode)
            } catch (e: IOException) {
                Log.i(TAG, "No Mii for $friendCode from Retro WFC: ${e.message}")
                null
            }
            val changed = fresh != null && (
                !(fresh.data ?: NONE).contentEquals(cached?.data ?: NONE) || !(fresh.image ?: NONE).contentEquals(cached?.image ?: NONE)
                )
            val remote = if (fresh != null && changed) remoteOf(fresh.data, fresh.image) else null
            if (fresh != null && changed) {
                runCatching {
                    directory.mkdirs()
                    store(dataFile, fresh.data)
                    store(imageFile, fresh.image)
                }
            }
            main.post { remember(friendCode, remote, finished = true) }
        }
    }

    private fun remoteOf(data: ByteArray?, image: ByteArray?): Remote? {
        val mii = data?.let { runCatching { MiiData.parse(it) }.getOrNull() }
        val picture = image?.let(::decode)
        return if (mii == null && picture == null) null else Remote(mii, picture, data, image)
    }

    private fun read(file: File): ByteArray? = if (file.isFile) runCatching { file.readBytes() }.getOrNull() else null

    private fun store(file: File, bytes: ByteArray?) {
        if (bytes == null) file.delete() else file.writeBytes(bytes)
    }

    /** Hands what Retro WFC saw to everyone waiting for [friendCode]; the last answer also tells those with none. */
    private fun remember(friendCode: String, remote: Remote?, finished: Boolean) {
        if (remote != null) remotes[friendCode] = remote
        val callbacks = (if (finished) waiting.remove(friendCode) else waiting[friendCode]?.toList()).orEmpty()
        when {
            remote != null -> callbacks.forEach { it(remote) }
            finished && remotes[friendCode] == null -> callbacks.forEach { it(null) }
        }
    }

    private fun decode(bytes: ByteArray): Bitmap? = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)

    private val NONE = ByteArray(0)

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
