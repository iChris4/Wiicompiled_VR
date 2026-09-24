package org.wiicompiled.quest.launcher

import android.app.Activity
import android.graphics.PointF
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.ProgressBar
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import java.text.NumberFormat
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import org.wiicompiled.quest.R

/**
 * WheelWizard's VrHistoryGraph (Views/Patterns/VrHistoryGraph): a player's VR over the period
 * chosen, with its start, end and change, drawn by time or by match. [root] is a view_vr_history
 * layout; My profiles shows its licence's, the Rooms page a player's.
 */
class VrHistoryPanel(private val activity: Activity, root: View) {

    private val start: TextView = root.findViewById(R.id.history_start)
    private val end: TextView = root.findViewById(R.id.history_end)
    private val change: TextView = root.findViewById(R.id.history_change)
    private val range: TextView = root.findViewById(R.id.history_range)
    private val loading: ProgressBar = root.findViewById(R.id.history_loading)
    private val matches: Switch = root.findViewById(R.id.history_matches)
    private val graph: View = root.findViewById(R.id.history_graph)
    private val empty: View = root.findViewById(R.id.history_empty)
    private val emptyMessage: TextView = root.findViewById(R.id.history_empty_message)
    private val max: TextView = root.findViewById(R.id.history_max)
    private val min: TextView = root.findViewById(R.id.history_min)
    private val chart: VrHistoryChart = root.findViewById(R.id.history_chart)
    private val labels: List<TextView> = listOf(
        root.findViewById(R.id.history_label_start),
        root.findViewById(R.id.history_label_mid),
        root.findViewById(R.id.history_label_end),
    )

    private var days = DEFAULT_DAYS
    private var friendCode: String? = null
    private var history: RetroWfc.History? = null
    /** The friend code and period the history on screen, or on its way, is for. */
    private var shownFor: Pair<String, Int>? = null
    private var generation = 0

    init {
        PatchesWidgets.styleSwitch(activity, matches)
        matches.setOnCheckedChangeListener { _, _ -> render() }
        root.findViewById<View>(R.id.history_matches_row).setOnClickListener { matches.toggle() }

        root.findViewById<Spinner>(R.id.history_days).apply {
            background = activity.getDrawable(R.drawable.bg_dropdown)
            setPopupBackgroundDrawable(activity.getDrawable(R.drawable.bg_dropdown_popup))
            dropDownVerticalOffset = PatchesWidgets.dp(activity, 4)
            adapter = ArrayAdapter(activity, R.layout.item_dropdown, DAY_LABELS.map { activity.getString(it) }).apply {
                setDropDownViewResource(R.layout.item_dropdown_popup)
            }
            setSelection(DAY_OPTIONS.indexOf(DEFAULT_DAYS), false)
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    val chosen = DAY_OPTIONS[position]
                    if (chosen == days) return
                    days = chosen
                    friendCode?.let(::show)
                }

                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }
    }

    /** Shows [code]'s history, loading it unless it is already the one shown for this period. */
    fun show(code: String) {
        friendCode = code
        if (shownFor == code to days) return
        shownFor = code to days
        val loadGeneration = ++generation
        history = null
        if (code.isEmpty() || code.none { it in '1'..'9' }) {
            // SetNoFriendCodeState: a licence never taken online has nothing to show.
            loading.visibility = View.GONE
            showState(activity.getString(R.string.history_no_code))
            range.text = ""
            return
        }
        loading.visibility = View.VISIBLE
        ProfileStore.history(code, days) { result ->
            if (loadGeneration != generation || activity.isDestroyed) return@history
            loading.visibility = View.GONE
            result.onSuccess { loaded ->
                history = loaded
                render()
            }.onFailure { failure ->
                range.text = ""
                showState(failure.message ?: failure.toString())
            }
        }
    }

    /** VrHistoryGraph reloads each time it is shown: races since then count. */
    fun forget() {
        shownFor = null
    }

    /** VrHistoryGraph.ApplyHistoryData: the totals, the range, then the plot by time or by match. */
    private fun render() {
        val data = history ?: return
        start.text = grouped(data.starting)
        end.text = grouped(data.ending)
        change.text = if (data.totalChange > 0) "+${grouped(data.totalChange)}" else grouped(data.totalChange)
        change.setTextColor(
            activity.getColor(
                when {
                    data.totalChange > 0 -> R.color.primary_400
                    data.totalChange < 0 -> R.color.danger_400
                    else -> R.color.neutral_100
                },
            ),
        )
        range.text = if (days == LIFETIME_DAYS) {
            activity.getString(R.string.history_range_all)
        } else {
            val fromPattern = if (year(data.from) != year(data.to)) "MMM d, yyyy" else "MMM d"
            activity.getString(R.string.history_range, days, date(data.from, fromPattern), date(data.to, "MMM d"))
        }

        val entries = data.entries
        if (entries.isEmpty()) {
            showState(activity.getString(R.string.history_none_range))
            return
        }
        graph.visibility = View.VISIBLE
        empty.visibility = View.GONE
        val first = entries.first().time
        val span = maxOf(1_000L, entries.last().time - first)
        val lowest = entries.minOf { it.total }
        val highest = entries.maxOf { it.total }
        val vrRange = maxOf(1, highest - lowest).toFloat()
        max.text = grouped(highest)
        min.text = grouped(lowest)
        val byMatch = matches.isChecked
        val texts = if (byMatch) {
            listOf(1, entries.size / 2 + 1, entries.size).map { activity.getString(R.string.history_match, it) }
        } else {
            val pattern = if (days >= 7) "MMM d" else "MMM d HH:mm"
            listOf(first, first + span / 2, entries.last().time).map { date(it, pattern) }
        }
        labels.zip(texts).forEach { (view, text) -> view.text = text }
        chart.setPoints(
            entries.mapIndexed { index, entry ->
                val x = if (byMatch) {
                    if (entries.size > 1) index.toFloat() / (entries.size - 1) else 0f
                } else {
                    (entry.time - first).toFloat() / span
                }
                PointF(x, (entry.total - lowest) / vrRange)
            },
        )
    }

    /** VrHistoryGraph's empty and error states, which share one look. */
    private fun showState(message: String) {
        graph.visibility = View.GONE
        empty.visibility = View.VISIBLE
        emptyMessage.text = message
        for (view in listOf(start, end, change)) view.text = "0"
        change.setTextColor(activity.getColor(R.color.neutral_100))
        chart.setPoints(emptyList())
    }

    private fun grouped(value: Int): String = NumberFormat.getIntegerInstance(Locale.getDefault()).format(value)

    private fun date(time: Long, pattern: String): String =
        DateTimeFormatter.ofPattern(pattern, Locale.getDefault()).withZone(ZoneId.systemDefault()).format(Instant.ofEpochMilli(time))

    private fun year(time: Long): Int = Instant.ofEpochMilli(time).atZone(ZoneId.systemDefault()).year

    private companion object {
        const val DEFAULT_DAYS = 30
        /** What VrHistoryGraph asks for "Lifetime". */
        const val LIFETIME_DAYS = 999
        val DAY_OPTIONS = listOf(1, 7, 30, 60, LIFETIME_DAYS)
        val DAY_LABELS = listOf(R.string.history_days_1, R.string.history_days_7, R.string.history_days_30, R.string.history_days_60, R.string.history_days_all)
    }
}
