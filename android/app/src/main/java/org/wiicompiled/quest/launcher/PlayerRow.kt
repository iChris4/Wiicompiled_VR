package org.wiicompiled.quest.launcher

import android.app.Activity
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import org.wiicompiled.quest.R

/**
 * Fills a row of item_player.xml, WheelWizard's PlayerListItem, for the Rooms and Leaderboard
 * pages: the player's Mii, name and friend code, VR, badges, place and whether they host.
 */
object PlayerRow {
    /** The Mii's circle in item_player.xml, drawn at twice its pixels for smooth edges. */
    private const val MII_DP = 50

    fun miiSize(activity: Activity): Int = (2 * MII_DP * activity.resources.displayMetrics.density).toInt() and 1.inv()

    fun bind(
        activity: Activity,
        row: View,
        mii: Mii?,
        name: String,
        friendCode: String,
        vrText: String,
        badges: List<RetroWfc.Badge>,
        topLabel: String?,
        isOpenHost: Boolean,
    ) {
        val picture = row.findViewById<ImageView>(R.id.player_mii)
        val drawn = mii != null && MiiRenderResource.installed(activity)
        row.findViewById<View>(R.id.player_placeholder).visibility = if (drawn) View.GONE else View.VISIBLE
        if (drawn) {
            MiiImages.head(activity, picture, mii!!, miiSize(activity))
        } else {
            picture.setTag(R.id.mii_image_key, null)
            picture.setImageDrawable(null)
        }
        row.findViewById<TextView>(R.id.player_name).text = name
        row.findViewById<TextView>(R.id.player_code).text = friendCode
        row.findViewById<TextView>(R.id.player_vr).text = vrText

        row.findViewById<View>(R.id.player_badges_box).visibility = if (badges.isEmpty()) View.GONE else View.VISIBLE
        row.findViewById<LinearLayout>(R.id.player_badges).apply {
            removeAllViews()
            badges.forEachIndexed { index, badge ->
                val size = PatchesWidgets.dp(activity, 30)
                addView(BadgeView(activity, badge), LinearLayout.LayoutParams(size, size).apply {
                    if (index > 0) marginStart = PatchesWidgets.dp(activity, 3)
                })
            }
        }
        row.findViewById<TextView>(R.id.player_top).apply {
            text = topLabel.orEmpty()
            visibility = if (topLabel != null) View.VISIBLE else View.GONE
        }
        row.findViewById<View>(R.id.player_host).visibility = if (isOpenHost) View.VISIBLE else View.GONE
    }
}
