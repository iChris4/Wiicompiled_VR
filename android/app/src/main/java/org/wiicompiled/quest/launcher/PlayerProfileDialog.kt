package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.AlertDialog
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import java.text.NumberFormat
import java.util.Locale
import java.util.concurrent.Executors
import org.wiicompiled.quest.R

/**
 * WheelWizard's PlayerProfileWindow (Views/Popups/PlayerProfileWindow.axaml.cs): what Retro WFC
 * knows of a player, by friend code: their Mii, rank, VR and how it moved lately, and their VR
 * history. Titled with the friend code until the profile names the player.
 */
object PlayerProfileDialog {

    private val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "PlayerProfile").apply { isDaemon = true } }
    private val main by lazy { Handler(Looper.getMainLooper()) }

    fun show(activity: Activity, friendCode: String) {
        val view = activity.layoutInflater.inflate(R.layout.dialog_player_profile, null)
        val dialog = AlertDialog.Builder(activity)
            .setTitle(friendCode)
            .setView(view)
            .setPositiveButton(R.string.dialog_close, null)
            .create()
        dialog.show()
        dialog.window?.setLayout(PatchesWidgets.dp(activity, 640), ViewGroup.LayoutParams.WRAP_CONTENT)
        val history = VrHistoryPanel(activity, view.findViewById(R.id.player_profile_history))
        worker.execute {
            val result = runCatching { RetroWfc.playerProfile(friendCode) }
            main.post {
                if (!dialog.isShowing || activity.isDestroyed) return@post
                view.findViewById<View>(R.id.player_loading).visibility = View.GONE
                val profile = result.getOrNull()
                if (profile == null) {
                    view.findViewById<View>(R.id.player_error).visibility = View.VISIBLE
                    view.findViewById<TextView>(R.id.player_error_text).text =
                        result.exceptionOrNull()?.message ?: activity.getString(R.string.player_profile_unknown)
                    return@post
                }
                dialog.setTitle(profile.name)
                fill(activity, view, profile)
                history.show(profile.friendCode.ifEmpty { friendCode })
            }
        }
    }

    private fun fill(activity: Activity, view: View, profile: RetroWfc.PlayerProfile) {
        view.findViewById<View>(R.id.player_content).visibility = View.VISIBLE
        view.findViewById<TextView>(R.id.player_profile_name).text = profile.name
        view.findViewById<TextView>(R.id.player_profile_code).text = profile.friendCode
        view.findViewById<TextView>(R.id.player_profile_rank).apply {
            text = activity.getString(R.string.player_profile_rank, profile.rank)
            visibility = if (profile.rank > 0) View.VISIBLE else View.GONE
        }
        view.findViewById<View>(R.id.player_profile_suspicious).visibility = if (profile.isSuspicious) View.VISIBLE else View.GONE
        view.findViewById<TextView>(R.id.player_profile_vr).text = grouped(profile.vr)
        view.findViewById<TextView>(R.id.player_profile_seen).text = lastSeen(activity, profile.lastSeen)
        val stats = profile.vrStats
        view.findViewById<TextView>(R.id.player_profile_day).text = stats?.let { signed(it.last24Hours) } ?: "--"
        view.findViewById<TextView>(R.id.player_profile_week).text = stats?.let { signed(it.lastWeek) } ?: "--"
        view.findViewById<TextView>(R.id.player_profile_month).text = stats?.let { signed(it.lastMonth) } ?: "--"

        val mii = profile.mii?.let { data -> runCatching { MiiData.parse(data) }.getOrNull() }
        val picture = view.findViewById<ImageView>(R.id.player_profile_mii)
        val drawn = mii != null && MiiRenderResource.installed(activity)
        view.findViewById<View>(R.id.player_profile_placeholder).visibility = if (drawn) View.GONE else View.VISIBLE
        if (drawn) MiiImages.head(activity, picture, mii!!, (2 * 84 * activity.resources.displayMetrics.density).toInt() and 1.inv())
    }

    /** LastSeenText: whole days, hours or minutes ago. */
    private fun lastSeen(activity: Activity, time: Long?): String {
        if (time == null) return "--"
        val minutes = (System.currentTimeMillis() - time) / 60_000
        return when {
            minutes >= 24 * 60 -> activity.getString(R.string.player_profile_days_ago, (minutes / (24 * 60)).toInt())
            minutes >= 60 -> activity.getString(R.string.player_profile_hours_ago, (minutes / 60).toInt())
            minutes >= 1 -> activity.getString(R.string.player_profile_minutes_ago, minutes.toInt())
            else -> activity.getString(R.string.player_profile_just_now)
        }
    }

    /** FormatSignedValue: a gain with its plus sign. */
    private fun signed(value: Int): String = if (value > 0) "+${grouped(value)}" else grouped(value)

    private fun grouped(value: Int): String = NumberFormat.getIntegerInstance(Locale.getDefault()).format(value)
}
