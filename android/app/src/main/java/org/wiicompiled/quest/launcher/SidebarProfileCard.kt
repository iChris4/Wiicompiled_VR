package org.wiicompiled.quest.launcher

import android.app.Activity
import android.view.View
import android.widget.ImageView
import android.widget.TextView
import org.wiicompiled.quest.R

/**
 * WheelWizard's sidebar profile block (Views/Patterns/CurrentUserProfile): the primary licence of
 * Retro Rewind's save, with its Mii, name and friend code, opening My profiles. Hidden while the
 * save has no licence.
 */
class SidebarProfileCard(private val activity: Activity, private val card: View, open: () -> Unit) {

    private val mii: ImageView = card.findViewById(R.id.sidebar_profile_mii)
    private val placeholder: View = card.findViewById(R.id.sidebar_profile_placeholder)
    private val name: TextView = card.findViewById(R.id.sidebar_profile_name)
    private val code: TextView = card.findViewById(R.id.sidebar_profile_code)

    init {
        card.setOnClickListener { open() }
    }

    /** Reads the save again, which the game may have changed. */
    fun refresh() = ProfileStore.load(activity, ::show)

    fun show(snapshot: ProfileStore.Snapshot?) {
        if (activity.isDestroyed) return
        val license = snapshot?.let { ProfileStore.sidebarLicense(activity, it) }
        card.visibility = if (license == null) View.GONE else View.VISIBLE
        if (snapshot == null || license == null) return
        name.text = ProfileStore.displayName(activity, license)
        code.text = license.friendCode
        code.visibility = if (license.friendCode.isEmpty()) View.GONE else View.VISIBLE
        val key = "${license.slot}:${license.miiId}:${license.friendCode}"
        if (mii.tag != key) {
            mii.tag = key
            mii.setImageDrawable(null)
            placeholder.visibility = View.VISIBLE
        }
        // FriendsSideProfile on the PC, drawn at twice its pixels for smooth edges.
        val size = (2 * PICTURE_DP * activity.resources.displayMetrics.density).toInt() and 1.inv()
        ProfileStore.miiPicture(activity, snapshot, license, size) { bitmap ->
            if (mii.tag != key) return@miiPicture
            mii.setImageBitmap(bitmap)
            placeholder.visibility = if (bitmap == null) View.VISIBLE else View.GONE
        }
    }

    private companion object {
        /** The Mii picture's size in activity_launcher.xml. */
        const val PICTURE_DP = 80
    }
}
