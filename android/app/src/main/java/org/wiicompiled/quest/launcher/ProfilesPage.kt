package org.wiicompiled.quest.launcher

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.graphics.PointF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.text.NumberFormat
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import org.wiicompiled.quest.R

/**
 * The launcher's My profiles page, WheelWizard's UserProfilePage (Views/Pages/UserProfilePage.axaml.cs):
 * the four licences of Retro Rewind's save, one shown at a time with its Mii, name, friend code and
 * badges, beside a carousel of its VR history (VrHistoryGraph) and its numbers. As on the PC, a
 * licence is Online, with a glow, while its friend code is in a Retro WFC room, and one licence is
 * primary: the one the sidebar's card shows.
 *
 * It only reads. The headset has no Mii database, so a licence's Mii is the picture Retro WFC keeps
 * of it (a licence never taken online has none), and nothing renames a licence or changes its Mii.
 * The PC's region picker is left out too, since the Quest only runs PAL.
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

    private val historyStart: TextView = root.findViewById(R.id.history_start)
    private val historyEnd: TextView = root.findViewById(R.id.history_end)
    private val historyChange: TextView = root.findViewById(R.id.history_change)
    private val historyRange: TextView = root.findViewById(R.id.history_range)
    private val historyLoading: View = root.findViewById(R.id.history_loading)
    private val historyMatches: Switch = root.findViewById(R.id.history_matches)
    private val historyGraph: View = root.findViewById(R.id.history_graph)
    private val historyEmpty: View = root.findViewById(R.id.history_empty)
    private val historyEmptyMessage: TextView = root.findViewById(R.id.history_empty_message)
    private val historyMax: TextView = root.findViewById(R.id.history_max)
    private val historyMin: TextView = root.findViewById(R.id.history_min)
    private val historyChart: VrHistoryChart = root.findViewById(R.id.history_chart)
    private val historyLabels: List<TextView> = listOf(
        root.findViewById(R.id.history_label_start),
        root.findViewById(R.id.history_label_mid),
        root.findViewById(R.id.history_label_end),
    )

    private val statsVr: TextView = root.findViewById(R.id.stats_vr)
    private val statsBr: TextView = root.findViewById(R.id.stats_br)
    private val statsWins: TextView = root.findViewById(R.id.stats_wins)
    private val statsRaces: TextView = root.findViewById(R.id.stats_races)

    private var snapshot: ProfileStore.Snapshot? = null
    private var slot = -1
    private val current: RksysProfiles.License? get() = snapshot?.licenses?.getOrNull(slot)
    private var online: Set<String> = emptySet()
    private var badges: Map<String, List<RetroWfc.Badge>> = emptyMap()
    private var carouselPage = 0
    private var days = DEFAULT_DAYS
    private var history: RetroWfc.History? = null
    /** The friend code and period the history on screen, or on its way, is for. */
    private var historyFor: Pair<String, Int>? = null
    private var historyGeneration = 0

    private val main = Handler(Looper.getMainLooper())
    private var visible = false
    private val onlineTicker = object : Runnable {
        override fun run() {
            refreshOnline()
            main.postDelayed(this, ONLINE_REFRESH_MS)
        }
    }

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

        PatchesWidgets.styleSwitch(activity, historyMatches)
        historyMatches.setOnCheckedChangeListener { _, _ -> renderHistory() }
        root.findViewById<View>(R.id.history_matches_row).setOnClickListener { historyMatches.toggle() }

        root.findViewById<Spinner>(R.id.history_days).apply {
            background = activity.getDrawable(R.drawable.bg_dropdown)
            setPopupBackgroundDrawable(activity.getDrawable(R.drawable.bg_dropdown_popup))
            dropDownVerticalOffset = dp(4)
            adapter = ArrayAdapter(activity, R.layout.item_dropdown, DAY_LABELS.map { activity.getString(it) }).apply {
                setDropDownViewResource(R.layout.item_dropdown_popup)
            }
            setSelection(DAY_OPTIONS.indexOf(DEFAULT_DAYS), false)
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    val chosen = DAY_OPTIONS[position]
                    if (chosen == days) return
                    days = chosen
                    current?.let(::loadHistory)
                }

                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }
        renderCarousel()
    }

    /** While the page is on screen, who is online is asked again now and then, as the PC polls its rooms. */
    fun setVisible(shown: Boolean) {
        if (shown == visible) return
        visible = shown
        main.removeCallbacks(onlineTicker)
        if (shown) main.post(onlineTicker)
    }

    /** Reads the save again, which the game may have changed since, and shows the licence chosen. */
    fun refresh() {
        ProfileStore.load(activity) { loaded ->
            if (activity.isDestroyed) return@load
            snapshot = loaded
            // VrHistoryGraph reloads each time it is shown: races since then count.
            historyFor = null
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
        loadHistory(license)
    }

    private fun showMii(license: RksysProfiles.License) {
        val friendCode = license.friendCode
        if (mii.tag != friendCode) {
            mii.tag = friendCode
            mii.setImageDrawable(null)
            placeholder.visibility = View.VISIBLE
        }
        ProfileStore.miiImage(activity, friendCode) { bitmap ->
            if (mii.tag != friendCode) return@miiImage
            mii.setImageBitmap(bitmap)
            placeholder.visibility = if (bitmap == null) View.VISIBLE else View.GONE
        }
    }

    // Online, as UserProfilePage.UpdateOnlineBorders shows it.

    private fun refreshOnline() {
        ProfileStore.online { codes ->
            online = codes
            if (!activity.isDestroyed) renderOnline()
        }
    }

    private fun renderOnline() {
        val license = current ?: return
        val isOnline = license.friendCode.isNotEmpty() && license.friendCode in online
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

    // VR history, as VrHistoryGraph loads and draws it.

    private fun loadHistory(license: RksysProfiles.License) {
        val friendCode = license.friendCode
        if (historyFor == friendCode to days) return
        historyFor = friendCode to days
        val generation = ++historyGeneration
        history = null
        if (friendCode.isEmpty() || friendCode.none { it in '1'..'9' }) {
            // SetNoFriendCodeState: a licence never taken online has nothing to show.
            historyLoading.visibility = View.GONE
            showHistoryState(activity.getString(R.string.history_no_code))
            historyRange.text = ""
            return
        }
        historyLoading.visibility = View.VISIBLE
        ProfileStore.history(friendCode, days) { result ->
            if (generation != historyGeneration || activity.isDestroyed) return@history
            historyLoading.visibility = View.GONE
            result.onSuccess { loaded ->
                history = loaded
                renderHistory()
            }.onFailure { failure ->
                historyRange.text = ""
                showHistoryState(failure.message ?: failure.toString())
            }
        }
    }

    /** VrHistoryGraph.ApplyHistoryData: the totals, the range, then the plot by time or by match. */
    private fun renderHistory() {
        val data = history ?: return
        historyStart.text = grouped(data.starting)
        historyEnd.text = grouped(data.ending)
        historyChange.text = if (data.totalChange > 0) "+${grouped(data.totalChange)}" else grouped(data.totalChange)
        historyChange.setTextColor(
            activity.getColor(
                when {
                    data.totalChange > 0 -> R.color.primary_400
                    data.totalChange < 0 -> R.color.danger_400
                    else -> R.color.neutral_100
                },
            ),
        )
        historyRange.text = if (days == LIFETIME_DAYS) {
            activity.getString(R.string.history_range_all)
        } else {
            val fromPattern = if (year(data.from) != year(data.to)) "MMM d, yyyy" else "MMM d"
            activity.getString(R.string.history_range, days, date(data.from, fromPattern), date(data.to, "MMM d"))
        }

        val entries = data.entries
        if (entries.isEmpty()) {
            showHistoryState(activity.getString(R.string.history_none_range))
            return
        }
        historyGraph.visibility = View.VISIBLE
        historyEmpty.visibility = View.GONE
        val start = entries.first().time
        val span = maxOf(1_000L, entries.last().time - start)
        val lowest = entries.minOf { it.total }
        val highest = entries.maxOf { it.total }
        val vrRange = maxOf(1, highest - lowest).toFloat()
        historyMax.text = grouped(highest)
        historyMin.text = grouped(lowest)
        val byMatch = historyMatches.isChecked
        val labels = if (byMatch) {
            listOf(1, entries.size / 2 + 1, entries.size).map { activity.getString(R.string.history_match, it) }
        } else {
            val pattern = if (days >= 7) "MMM d" else "MMM d HH:mm"
            listOf(start, start + span / 2, entries.last().time).map { date(it, pattern) }
        }
        historyLabels.zip(labels).forEach { (view, text) -> view.text = text }
        historyChart.setPoints(
            entries.mapIndexed { index, entry ->
                val x = if (byMatch) {
                    if (entries.size > 1) index.toFloat() / (entries.size - 1) else 0f
                } else {
                    (entry.time - start).toFloat() / span
                }
                PointF(x, (entry.total - lowest) / vrRange)
            },
        )
    }

    /** VrHistoryGraph's empty and error states, which share one look. */
    private fun showHistoryState(message: String) {
        historyGraph.visibility = View.GONE
        historyEmpty.visibility = View.VISIBLE
        historyEmptyMessage.text = message
        for (view in listOf(historyStart, historyEnd, historyChange)) view.text = "0"
        historyChange.setTextColor(activity.getColor(R.color.neutral_100))
        historyChart.setPoints(emptyList())
    }

    private fun grouped(value: Int): String = NumberFormat.getIntegerInstance(Locale.getDefault()).format(value)

    private fun date(time: Long, pattern: String): String =
        DateTimeFormatter.ofPattern(pattern, Locale.getDefault()).withZone(ZoneId.systemDefault()).format(Instant.ofEpochMilli(time))

    private fun year(time: Long): Int = Instant.ofEpochMilli(time).atZone(ZoneId.systemDefault()).year

    private fun box(fill: Int, stroke: Int, radius: Int) = GradientDrawable().apply {
        setColor(activity.getColor(fill))
        setStroke(dp(1), activity.getColor(stroke))
        cornerRadius = dp(radius).toFloat()
    }

    private fun dp(value: Int): Int = PatchesWidgets.dp(activity, value)

    private companion object {
        const val CAROUSEL_PAGES = 2
        const val DEFAULT_DAYS = 30
        /** What VrHistoryGraph asks for "Lifetime". */
        const val LIFETIME_DAYS = 999
        val DAY_OPTIONS = listOf(1, 7, 30, 60, LIFETIME_DAYS)
        val DAY_LABELS = listOf(R.string.history_days_1, R.string.history_days_7, R.string.history_days_30, R.string.history_days_60, R.string.history_days_all)
        /** How often who is online is asked again while the page is shown. */
        const val ONLINE_REFRESH_MS = 30_000L
    }
}
