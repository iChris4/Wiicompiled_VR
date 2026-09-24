package org.wiicompiled.quest.launcher

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.ImageView
import android.widget.ListPopupWindow
import android.widget.TextView
import android.widget.Toast
import org.wiicompiled.quest.R

/**
 * WheelWizard's sidebar bottom bar (Layout.axaml's SidebarBottomBar): the Wheel Wizard team's
 * status ([WheelWizardStatus]) as an icon whose tip is its message, the info menu (who made this,
 * About, and the Discord, GitHub and support links), Settings, and the version.
 */
class SidebarFooter(
    private val activity: Activity,
    root: View,
    private val openSettings: () -> Unit,
    private val openAbout: () -> Unit,
) {
    private val statusIcon: ImageView = root.findViewById(R.id.sidebar_status)
    private val info: View = root.findViewById(R.id.sidebar_info)
    private val statusChanged: () -> Unit = { showStatus() }

    init {
        info.setOnClickListener { showInfo() }
        root.findViewById<View>(R.id.sidebar_settings).setOnClickListener { openSettings() }
        // The PC only shows the message under the pointer; a tap shows it too.
        statusIcon.setOnClickListener { statusIcon.tooltipText?.let { Toast.makeText(activity, it, Toast.LENGTH_LONG).show() } }
        WheelWizardStatus.addListener(statusChanged)
        showStatus()
    }

    fun destroy() = WheelWizardStatus.removeListener(statusChanged)

    /** UpdateLiveAlert. */
    private fun showStatus() {
        val status = WheelWizardStatus.status
        if (status == null || !status.visible) {
            statusIcon.visibility = View.GONE
            return
        }
        statusIcon.visibility = View.VISIBLE
        statusIcon.tooltipText = if (status.failed) activity.getString(R.string.sidebar_status_failed) else status.message
        statusIcon.contentDescription = statusIcon.tooltipText
        val custom = status.icon?.takeIf { it.isNotEmpty() }?.let { data -> runCatching { SvgPath.toPath(SvgPath.parse(data)) }.getOrNull() }
        if (custom != null) {
            // An icon of its own, in its colour or white.
            val color = status.color?.let { runCatching { Color.parseColor(it) }.getOrNull() } ?: Color.WHITE
            statusIcon.imageTintList = null
            statusIcon.setImageDrawable(SvgPath.IconDrawable(custom, color))
            return
        }
        val (icon, color) = when (status.variant) {
            WheelWizardStatus.Variant.Warning -> R.drawable.ic_warning_tip to R.color.warning_500
            WheelWizardStatus.Variant.Error -> R.drawable.ic_error_tip to R.color.danger_500
            WheelWizardStatus.Variant.Success -> R.drawable.ic_success_tip to R.color.primary_300
            WheelWizardStatus.Variant.Info -> R.drawable.ic_info to R.color.neutral_200
            WheelWizardStatus.Variant.Question -> R.drawable.ic_question_tip to R.color.warning_600
            WheelWizardStatus.Variant.Party -> R.drawable.ic_star to R.color.danger_300
            // A custom icon that could not be read leaves an empty space, as the PC's would.
            else -> {
                statusIcon.setImageDrawable(null)
                return
            }
        }
        statusIcon.setImageResource(icon)
        statusIcon.imageTintList = activity.getColorStateList(color)
    }

    /** SidebarInfoContextMenu. */
    private fun showInfo() {
        val entries = listOf(
            Entry.Header,
            Entry.Divider,
            Entry.Action(R.string.sidebar_about, R.drawable.ic_info) { openAbout() },
            Entry.Action(R.string.sidebar_discord, R.drawable.ic_discord) { open(DISCORD_URL) },
            Entry.Action(R.string.sidebar_github, R.drawable.ic_github) { open(GITHUB_URL) },
            Entry.Action(R.string.sidebar_support, R.drawable.ic_coffee) { open(SUPPORT_URL) },
        )
        ListPopupWindow(activity).apply {
            anchorView = info
            setAdapter(MenuAdapter(entries))
            setBackgroundDrawable(activity.getDrawable(R.drawable.bg_dropdown_popup))
            width = PatchesWidgets.dp(activity, 220)
            isModal = true
            setOnItemClickListener { _, _, position, _ ->
                val entry = entries[position] as? Entry.Action ?: return@setOnItemClickListener
                dismiss()
                entry.run()
            }
            show()
        }
    }

    private fun open(url: String) {
        try {
            activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (e: ActivityNotFoundException) {
            Toast.makeText(activity, R.string.browser_no_browser, Toast.LENGTH_LONG).show()
        }
    }

    private sealed class Entry {
        object Header : Entry()
        object Divider : Entry()
        class Action(val label: Int, val icon: Int, val run: () -> Unit) : Entry()
    }

    private inner class MenuAdapter(private val entries: List<Entry>) : BaseAdapter() {
        override fun getCount(): Int = entries.size
        override fun getItem(position: Int): Any = entries[position]
        override fun getItemId(position: Int): Long = position.toLong()
        override fun getViewTypeCount(): Int = 3
        override fun getItemViewType(position: Int): Int = when (entries[position]) {
            Entry.Header -> 0
            Entry.Divider -> 1
            is Entry.Action -> 2
        }
        override fun areAllItemsEnabled(): Boolean = false
        override fun isEnabled(position: Int): Boolean = entries[position] is Entry.Action

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View = when (val entry = entries[position]) {
            // The PC's disabled "Made by" header.
            Entry.Header -> (convertView as? TextView ?: TextView(activity)).apply {
                setText(R.string.sidebar_made_by)
                setTextColor(activity.getColor(R.color.neutral_400))
                textSize = 12f
                setPadding(dp(12), dp(8), dp(12), dp(6))
            }
            Entry.Divider -> convertView ?: View(activity).apply {
                layoutParams = ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(9))
                background = activity.getDrawable(R.drawable.bg_menu_divider)
            }
            is Entry.Action -> ((convertView as? TextView) ?: activity.layoutInflater.inflate(R.layout.item_dropdown_popup, parent, false) as TextView).apply {
                setText(entry.label)
                val size = dp(16)
                val icon = activity.getDrawable(entry.icon)?.mutate()?.apply {
                    setBounds(0, 0, size, size)
                    setTint(activity.getColor(R.color.neutral_300))
                }
                setCompoundDrawablesRelative(icon, null, null, null)
                compoundDrawablePadding = dp(10)
            }
        }
    }

    private fun dp(value: Int): Int = PatchesWidgets.dp(activity, value)

    private companion object {
        /** The links the user chose: this project's repository, and Wheel Wizard's Discord and Ko-fi as on the PC. */
        const val GITHUB_URL = "https://github.com/iChris4/Wiicompiled_VR"
        const val DISCORD_URL = "https://discord.gg/vZ7T2wJnsq"
        const val SUPPORT_URL = "https://ko-fi.com/wheelwizard"
    }
}
