package org.wiicompiled.quest.launcher

import android.content.Context
import android.content.SharedPreferences
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.IOException
import org.wiicompiled.quest.GameLibrary

/**
 * Keeps the headset's Retro Rewind current, as WheelWizard does on a PC. When the launcher opens,
 * Retro Rewind's version feed (the one [RetroRewindPack] updates from) is read in the background,
 * at most every [CHECK_INTERVAL_MS], and a newer pack is offered once per published version.
 *
 * Retro Rewind's code is translated from the pack's Code.pul, so an update that changes it leaves
 * the installed game behind ([GameLibrary.Status.PackChanged]), out of step with the new pack and
 * likely refused online. Building it again is offered once per Code.pul, after an update or
 * whenever the launcher finds the game behind.
 */
object RetroRewindUpdates {

    const val CHECK_INTERVAL_MS = 4L * 60 * 60 * 1000
    /** A check that could not reach the server is tried again after this long, not at every open. */
    const val RETRY_INTERVAL_MS = 30L * 60 * 1000

    private const val TAG = "WiiCompiledLauncher"
    private const val PREFERENCES = "retro_rewind_updates"
    private const val KEY_CHECKED_AT = "checkedAt"
    private const val KEY_LATEST = "latest"
    private const val KEY_OFFERED_UPDATE = "offeredUpdate"
    private const val KEY_OFFERED_REBUILD = "offeredRebuild"

    // Lazy, so the rules above stay usable where there is no main looper (the JVM unit tests).
    private val main by lazy { Handler(Looper.getMainLooper()) }

    @Volatile
    private var checking = false

    /** Whether a check is due at [now]. A clock that went backwards checks again. */
    fun checkDue(now: Long, checkedAt: Long): Boolean =
        checkedAt <= 0 || now < checkedAt || now - checkedAt >= CHECK_INTERVAL_MS

    /** Whether [latest] is newer than the [installed] pack. Without a pack there is nothing to update. */
    fun updateAvailable(installed: String?, latest: String?): Boolean =
        installed != null && latest != null && RetroRewindPack.compare(latest, installed) > 0

    /** The version to offer: newer than the installed pack, and not offered before. */
    fun updateToOffer(installed: String?, latest: String?, offered: String?): String? =
        latest?.takeIf { updateAvailable(installed, it) && it != offered }

    /**
     * Whether to offer building Retro Rewind again for the pack hashed [packCodePul]: its game is
     * behind the pack, or, right after a pack update, it predates game.json recording its Code.pul
     * and so cannot be checked. Never twice for one Code.pul.
     */
    fun rebuildToOffer(
        status: GameLibrary.Status,
        recordsCodePul: Boolean,
        afterUpdate: Boolean,
        packCodePul: String?,
        offered: String?,
    ): Boolean {
        if (packCodePul == null || packCodePul == offered) return false
        return status == GameLibrary.Status.PackChanged ||
            (afterUpdate && status == GameLibrary.Status.Ready && !recordsCodePul)
    }

    /** The newest version the last successful check found, or null before one has. */
    fun latestKnown(context: Context): String? = preferences(context).getString(KEY_LATEST, null)

    fun offeredUpdate(context: Context): String? = preferences(context).getString(KEY_OFFERED_UPDATE, null)

    fun markUpdateOffered(context: Context, version: String) {
        preferences(context).edit().putString(KEY_OFFERED_UPDATE, version).apply()
    }

    fun offeredRebuild(context: Context): String? = preferences(context).getString(KEY_OFFERED_REBUILD, null)

    fun markRebuildOffered(context: Context, codePul: String) {
        preferences(context).edit().putString(KEY_OFFERED_REBUILD, codePul).apply()
    }

    /**
     * Reads the version feed off the main thread when a check is due, then calls [onChecked] on the
     * main thread with the newest published version (null when the server could not be read). Does
     * nothing when no check is due or one is already running.
     */
    fun checkInBackground(context: Context, onChecked: (String?) -> Unit) {
        val preferences = preferences(context)
        val now = System.currentTimeMillis()
        if (checking || !checkDue(now, preferences.getLong(KEY_CHECKED_AT, 0))) return
        checking = true
        Thread({
            val latest = try {
                RetroRewindPack.latestPublishedVersion()
            } catch (e: IOException) {
                Log.w(TAG, "Could not read Retro Rewind's version feed: ${e.message}")
                null
            }
            val editor = preferences.edit()
            if (latest != null) {
                Log.i(TAG, "Retro Rewind's newest published pack is $latest")
                editor.putLong(KEY_CHECKED_AT, now).putString(KEY_LATEST, latest)
            } else {
                // Due again after RETRY_INTERVAL_MS rather than at every open while offline.
                editor.putLong(KEY_CHECKED_AT, now - CHECK_INTERVAL_MS + RETRY_INTERVAL_MS)
            }
            editor.apply()
            checking = false
            main.post { onChecked(latest) }
        }, "RetroRewindUpdateCheck").start()
    }

    private fun preferences(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
}
