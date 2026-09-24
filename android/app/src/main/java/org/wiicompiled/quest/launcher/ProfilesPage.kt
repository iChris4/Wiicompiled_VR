package org.wiicompiled.quest.launcher

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.TextView
import android.widget.Toast
import org.wiicompiled.quest.R

/**
 * The launcher's My profiles page, WheelWizard's UserProfilePage (Views/Pages/UserProfilePage.axaml.cs):
 * the four licences of Retro Rewind's save, one shown at a time with its Mii, name, friend code and
 * badges, beside a carousel of its VR history (VrHistoryGraph) and its numbers. As on the PC, a
 * licence is Online, with a glow, while its friend code is in a Retro WFC room, and one licence is
 * primary: the one the sidebar's card shows.
 *
 * A licence's Mii is drawn as the PC draws it, from the headset's Mii database, or else from the Mii
 * Retro WFC last saw the licence play with (ProfileStore.miiPicture). It only reads: nothing renames
 * a licence or changes its Mii. The PC's region picker is left out too, since the Quest only runs
 * PAL.
 */
class ProfilesPage(
    private val activity: Activity,
    root: View,
    /** The save was read again or the primary licence changed: the sidebar card follows. */
    private val changed: (ProfileStore.Snapshot?) -> Unit,
) {

    private val content: View = root.findViewById(R.id.profiles_content)
    private val empty: View = root.findViewById(R.id.profiles_empty)
    private val card: View = root.findViewById(R.id.profiles_card)
    private val slots: LinearLayout = root.findViewById(R.id.profiles_slots)
    private val face: View = root.findViewById(R.id.profiles_face)
    private val glow: View = root.findViewById(R.id.profiles_glow)
    private val mii: ImageView = root.findViewById(R.id.profiles_mii)
    private val placeholder: View = root.findViewById(R.id.profiles_placeholder)
    private val name: TextView = root.findViewById(R.id.profiles_name)
    private val codeRow: View = root.findViewById(R.id.profiles_code_row)
    private val code: TextView = root.findViewById(R.id.profiles_code)
    private val primaryRadio: RadioButton = root.findViewById(R.id.profiles_primary_radio)
    private val badgeRow: LinearLayout = root.findViewById(R.id.profiles_badges)
    private val historyPage: View = root.findViewById(R.id.profiles_history)
    private val statsPage: View = root.findViewById(R.id.profiles_stats)
    private val dots: List<View> = listOf(root.findViewById(R.id.profiles_dot0), root.findViewById(R.id.profiles_dot1))

    private val history = VrHistoryPanel(activity, historyPage)

    private val statsVr: TextView = root.findViewById(R.id.stats_vr)
    private val statsBr: TextView = root.findViewById(R.id.stats_br)
    private val statsWins: TextView = root.findViewById(R.id.stats_wins)
    private val statsRaces: TextView = root.findViewById(R.id.stats_races)

    private var snapshot: ProfileStore.Snapshot? = null
    private var slot = -1
    private val current: RksysProfiles.License? get() = snapshot?.licenses?.getOrNull(slot)
    private var badges: Map<String, List<RetroWfc.Badge>> = emptyMap()
    private var carouselPage = 0

    private var visible = false
    private val roomsChanged: () -> Unit = { renderOnline() }

    private val slotTabs: List<TextView> = (0 until RksysProfiles.SLOTS).map { index ->
        TextView(activity).apply {
            gravity = Gravity.CENTER
            textSize = 15f
            isSingleLine = true
            ellipsize = TextUtils.TruncateAt.END
            setPadding(dp(8), 0, dp(8), 0)
            setTextColor(activity.getColorStateList(R.color.profile_slot_text))
            setBackgroundResource(R.drawable.bg_profile_slot)
            isClickable = true
            isFocusable = true
            setOnClickListener {
                if (index != slot) {
                    slot = index
                    render()
                }
            }
            slots.addView(this, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f))
        }
    }

    init {
        root.findViewById<View>(R.id.profiles_copy).setOnClickListener { copyFriendCode() }
        root.findViewById<View>(R.id.profiles_primary).setOnClickListener { makePrimary() }
        root.findViewById<View>(R.id.profiles_previous).setOnClickListener { moveCarousel(-1) }
        root.findViewById<View>(R.id.profiles_next).setOnClickListener { moveCarousel(1) }

        renderCarousel()
    }

    /** While the page is on screen, who is online follows Retro WFC's rooms, as the PC's does. */
    fun setVisible(shown: Boolean) {
        if (shown == visible) return
        visible = shown
        if (shown) {
            LiveRooms.addListener(roomsChanged)
            renderOnline()
        } else {
            LiveRooms.removeListener(roomsChanged)
        }
    }

    /** Reads the save again, which the game may have changed since, and shows the licence chosen. */
    fun refresh() {
        ProfileStore.load(activity) { loaded ->
            if (activity.isDestroyed) return@load
            snapshot = loaded
            history.forget()
            val licenses = loaded?.licenses.orEmpty()
            if (licenses.getOrNull(slot) == null) {
                // UserProfilePage opens on the focused user, here the primary licence.
                val primary = ProfileStore.primarySlot(activity)
                slot = if (licenses.getOrNull(primary) != null) primary else licenses.indexOfFirst { it != null }
            }
            render()
            changed(loaded)
        }
        ProfileStore.badges { loaded ->
            badges = loaded
            if (!activity.isDestroyed) renderBadges()
        }
    }

    private fun render() {
        val license = current
        val hasProfiles = snapshot?.any == true && license != null
        content.visibility = if (hasProfiles) View.VISIBLE else View.GONE
        empty.visibility = if (hasProfiles) View.GONE else View.VISIBLE
        if (license == null) return

        slotTabs.forEachIndexed { index, tab ->
            val entry = snapshot?.licenses?.getOrNull(index)
            tab.text = entry?.let { ProfileStore.displayName(activity, it) } ?: activity.getString(R.string.profiles_no_license)
            tab.isEnabled = entry != null
            tab.isSelected = index == slot
            tab.setTypeface(null, if (entry == null) Typeface.ITALIC else Typeface.NORMAL)
        }
        name.text = ProfileStore.displayName(activity, license)
        code.text = license.friendCode
        codeRow.visibility = if (license.friendCode.isEmpty()) View.GONE else View.VISIBLE
        primaryRadio.isChecked = ProfileStore.primarySlot(activity) == license.slot
        showMii(license)
        renderOnline()
        renderBadges()

        statsVr.text = license.vr.toString()
        statsBr.text = license.br.toString()
        statsWins.text = license.wins.toString()
        statsRaces.text = license.races.toString()
        history.show(license.friendCode)
    }

    /** The licence's Mii, drawn at the size it is shown at (UserProfilePage's CurrentUserSideProfile). */
    private fun showMii(license: RksysProfiles.License) {
        val loaded = snapshot ?: return
        val key = "${license.slot}:${license.miiId}:${license.friendCode}"
        if (mii.tag != key) {
            mii.tag = key
            mii.setImageDrawable(null)
            placeholder.visibility = View.VISIBLE
        }
        val size = (PICTURE_DP * activity.resources.displayMetrics.density).toInt() and 1.inv()
        ProfileStore.miiPicture(activity, loaded, license, size) { bitmap ->
            if (mii.tag != key) return@miiPicture
            mii.setImageBitmap(bitmap)
            placeholder.visibility = if (bitmap == null) View.VISIBLE else View.GONE
        }
    }

    // Online, as UserProfilePage.UpdateOnlineBorders shows it.

    private fun renderOnline() {
        if (activity.isDestroyed) return
        val license = current ?: return
        val isOnline = license.friendCode.isNotEmpty() && license.friendCode in LiveRooms.onlineFriendCodes
        card.background = box(R.color.neutral_900, if (isOnline) R.color.primary_400 else R.color.neutral_900, 8)
        face.background = box(R.color.neutral_950, if (isOnline) R.color.primary_400 else R.color.neutral_600, 4)
        glow.visibility = if (isOnline) View.VISIBLE else View.GONE
        face.contentDescription = if (isOnline) activity.getString(R.string.profiles_online) else null
    }

    private fun renderBadges() {
        badgeRow.removeAllViews()
        val license = current ?: return
        for (badge in badges[license.friendCode].orEmpty()) {
            badgeRow.addView(BadgeView(activity, badge), LinearLayout.LayoutParams(dp(30), dp(30)).apply { marginStart = dp(3) })
        }
    }

    // Actions

    private fun copyFriendCode() {
        val friendCode = current?.friendCode?.takeIf { it.isNotEmpty() } ?: return
        activity.getSystemService(ClipboardManager::class.java)
            ?.setPrimaryClip(ClipData.newPlainText(activity.getString(R.string.profiles_code_label), friendCode))
        Toast.makeText(activity, R.string.profiles_code_copied, Toast.LENGTH_SHORT).show()
    }

    /** UserProfilePage.SetUserAsPrimary: the sidebar's card follows the new choice. */
    private fun makePrimary() {
        val license = current ?: return
        if (ProfileStore.primarySlot(activity) == license.slot) return
        ProfileStore.setPrimarySlot(activity, license.slot)
        primaryRadio.isChecked = true
        Toast.makeText(activity, R.string.profiles_primary_set, Toast.LENGTH_SHORT).show()
        changed(snapshot)
    }

    private fun moveCarousel(offset: Int) {
        carouselPage = Math.floorMod(carouselPage + offset, CAROUSEL_PAGES)
        renderCarousel()
    }

    private fun renderCarousel() {
        historyPage.visibility = if (carouselPage == 0) View.VISIBLE else View.GONE
        statsPage.visibility = if (carouselPage == 1) View.VISIBLE else View.GONE
        dots.forEachIndexed { index, dot ->
            val active = index == carouselPage
            dot.layoutParams = dot.layoutParams.apply { width = dp(if (active) 26 else 10) }
            dot.background = GradientDrawable().apply {
                setColor(activity.getColor(if (active) R.color.primary_300 else R.color.neutral_500))
                cornerRadius = dp(5).toFloat()
                alpha = if (active) 255 else 191
            }
        }
    }

    private fun box(fill: Int, stroke: Int, radius: Int) = GradientDrawable().apply {
        setColor(activity.getColor(fill))
        setStroke(dp(1), activity.getColor(stroke))
        cornerRadius = dp(radius).toFloat()
    }

    private fun dp(value: Int): Int = PatchesWidgets.dp(activity, value)

    private companion object {
        /** The Mii picture's frame in page_profiles.xml. */
        const val PICTURE_DP = 320
        const val CAROUSEL_PAGES = 2
    }
}
