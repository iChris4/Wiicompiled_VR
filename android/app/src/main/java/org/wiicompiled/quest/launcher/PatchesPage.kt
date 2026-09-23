package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.AlertDialog
import android.content.res.ColorStateList
import android.net.Uri
import android.provider.OpenableColumns
import android.text.TextUtils
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.io.IOException
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.R

/**
 * The launcher's Patches page, WheelWizard's mods page (Views/Pages/ModsPage.axaml.cs): Browse
 * opens the mod browser ([ModBrowser]) in place of the list, Import turns picked files into a named
 * mod, and each mod can be switched on or off, moved up or down the list, renamed or deleted, and
 * viewed in the browser when it came from there. What the list means for the game happens at Play,
 * in [ModLibrary.prepareForLaunch].
 */
class PatchesPage(
    private val activity: Activity,
    root: View,
    private val pickFiles: () -> Unit,
) {

    private val listPage: View = root.findViewById(R.id.patches_list)
    private val browserPage: View = root.findViewById(R.id.patches_browser)
    private val empty: View = root.findViewById(R.id.patches_empty)
    private val content: View = root.findViewById(R.id.patches_content)
    private val rows: LinearLayout = root.findViewById(R.id.patches_rows)
    private val count: TextView = root.findViewById(R.id.patches_count)
    private val enableAll: Switch = root.findViewById(R.id.patches_enable_all)
    private val headerButtons: View = root.findViewById(R.id.patches_header_buttons)
    private val importButtons = listOf(root.findViewById<TextView>(R.id.patches_import), root.findViewById<TextView>(R.id.patches_empty_import))

    private val modsDir: File get() = GameStorage.modsDirectory(activity)

    /** The list as read for the rows on screen, top first. */
    private var mods: List<ModLibrary.Mod> = emptyList()

    private val browser = ModBrowser(activity, browserPage, close = { showBrowser(false) }, modsChanged = { refresh() })

    /** Whether the browser is open in place of the list; it stays open while other pages are shown. */
    private var browsing = false

    private var installRunning = false

    private val installListener: (ModInstaller.State) -> Unit = { state ->
        browser.installStateChanged()
        // The list page's Import waits while a browser install holds the import slot; it only
        // changes when an install starts or ends, not with every progress step.
        val running = state != ModInstaller.State.Idle
        if (running != installRunning && !browsing) refresh()
        installRunning = running
    }

    init {
        for (button in importButtons) {
            button.setOnClickListener { if (!ModLibrary.importing) pickFiles() }
        }
        for (id in listOf(R.id.patches_browse, R.id.patches_empty_browse)) {
            root.findViewById<View>(id).setOnClickListener { showBrowser(true) }
        }
        PatchesWidgets.styleSwitch(activity, enableAll)
        // A click, not a checked change: refresh() sets the switch without meaning to change every mod.
        enableAll.setOnClickListener { setAllEnabled(enableAll.isChecked) }
        root.findViewById<View>(R.id.patches_enable_all_label).setOnClickListener {
            enableAll.toggle()
            setAllEnabled(enableAll.isChecked)
        }
    }

    /** While the launcher is in front: follows a browser install that may be running. */
    fun attach() {
        installRunning = ModInstaller.busy
        ModInstaller.addListener(installListener)
    }

    fun detach() {
        ModInstaller.removeListener(installListener)
    }

    /** Back closes the browser first; false when there was nothing to close. */
    fun back(): Boolean {
        if (!browsing) return false
        showBrowser(false)
        return true
    }

    /**
     * Opens the browser, on the mod with GameBanana id [modId] when one is given (View Mod), and
     * for unattended tests installs that mod as soon as it has loaded when [install] is set.
     */
    fun showBrowser(open: Boolean, modId: Int? = null, install: Boolean = false) {
        browsing = open
        listPage.visibility = if (open) View.GONE else View.VISIBLE
        browserPage.visibility = if (open) View.VISIBLE else View.GONE
        when {
            open && modId != null -> browser.showMod(modId, install)
            open -> browser.opened()
            // ModBrowserWindow.BeforeClose returns to ModsPage so the list shows what was installed.
            else -> refresh()
        }
    }

    /** Rebuilds the page from the Mods folder, which adb or the PC launcher's files may have changed. */
    fun refresh() {
        if (browsing) {
            browser.opened()
            return
        }
        mods = ModLibrary.load(modsDir)
        val hasMods = mods.isNotEmpty()
        // As on the PC, the page's own Browse and Import move to the top bar once there is a list.
        empty.visibility = if (hasMods) View.GONE else View.VISIBLE
        content.visibility = if (hasMods) View.VISIBLE else View.GONE
        headerButtons.visibility = if (hasMods) View.VISIBLE else View.GONE
        count.text = mods.size.toString()
        enableAll.isChecked = mods.all { it.enabled }

        val importing = ModLibrary.importing
        for (button in importButtons) {
            button.isEnabled = !importing
            button.alpha = if (importing) 0.45f else 1f
            button.setText(if (importing) R.string.patches_importing else R.string.patches_import)
        }

        rows.removeAllViews()
        if (!hasMods) return
        rows.addView(
            note(),
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(12)
            },
        )
        mods.forEachIndexed { index, mod ->
            rows.addView(
                row(mod, index),
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    if (index > 0) topMargin = dp(3)
                },
            )
        }
    }

    /** The files the player picked for Import: asks for the mod's name, then copies them in the background. */
    fun importPicked(uris: List<Uri>) {
        if (uris.isEmpty() || ModLibrary.importing) return
        mods = ModLibrary.load(modsDir)
        val sources = uris.map { uri ->
            ModLibrary.Source(displayName(uri)) {
                activity.contentResolver.openInputStream(uri) ?: throw IOException("Cannot read $uri")
            }
        }
        // Typing on a headset is slow, so a single file offers its own name to start from.
        val suggested = if (sources.size == 1) ModLibrary.suggestName(sources[0].name, mods) else ""
        PatchesWidgets.nameDialog(activity, R.string.patches_name_title, null, suggested, R.string.patches_import, { ModLibrary.validateName(it, mods) }) { name ->
            Log.i(TAG, "Importing ${sources.size} file(s) as mod $name")
            ModLibrary.importInBackground(modsDir, name, sources) { result ->
                if (activity.isDestroyed) return@importInBackground
                refresh()
                result.onSuccess { mod ->
                    Toast.makeText(activity, activity.getString(R.string.patches_installed, mod.title), Toast.LENGTH_LONG).show()
                }.onFailure { failure ->
                    Log.w(TAG, "Mod import failed", failure)
                    AlertDialog.Builder(activity)
                        .setTitle(R.string.patches_import_failed)
                        .setMessage(failure.message ?: failure.toString())
                        .setPositiveButton(android.R.string.ok, null)
                        .show()
                }
            }
            refresh()
        }
    }

    private fun row(mod: ModLibrary.Mod, index: Int): View {
        val last = mods.size - 1
        val toggle = Switch(activity).apply {
            isChecked = mod.enabled
            PatchesWidgets.styleSwitch(activity, this)
            contentDescription = mod.title
            setOnCheckedChangeListener { _, checked -> setModEnabled(mod, checked) }
        }
        val title = TextView(activity).apply {
            text = mod.title
            setTextColor(activity.getColor(R.color.neutral_100))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            isSingleLine = true
            ellipsize = TextUtils.TruncateAt.END
        }
        return LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = dp(58)
            setPadding(dp(14), dp(6), dp(8), dp(6))
            background = activity.getDrawable(
                when {
                    last == 0 -> R.drawable.bg_row_single
                    index == 0 -> R.drawable.bg_row_top
                    index == last -> R.drawable.bg_row_bottom
                    else -> R.drawable.bg_row_middle
                },
            )
            addView(toggle)
            addView(title, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = dp(14)
                marginEnd = dp(8)
            })
            addView(iconButton(R.drawable.ic_chevron_up, R.string.patches_move_up, index > 0) { move(mod, up = true) })
            addView(iconButton(R.drawable.ic_chevron_down, R.string.patches_move_down, index < last) { move(mod, up = false) })
            addView(iconButton(R.drawable.ic_more, R.string.patches_more, true) { anchor -> showMenu(anchor, mod) })
            setOnClickListener { toggle.toggle() }
        }
    }

    private fun iconButton(icon: Int, description: Int, enabled: Boolean, onClick: (View) -> Unit) = ImageView(activity).apply {
        setImageResource(icon)
        imageTintList = ColorStateList.valueOf(activity.getColor(R.color.neutral_300))
        background = activity.getDrawable(R.drawable.bg_nav_item)
        contentDescription = activity.getString(description)
        setPadding(dp(10), dp(10), dp(10), dp(10))
        isEnabled = enabled
        alpha = if (enabled) 1f else 0.3f
        setOnClickListener(onClick)
        layoutParams = LinearLayout.LayoutParams(dp(42), dp(42))
    }

    private fun note() = LinearLayout(activity).apply {
        orientation = LinearLayout.HORIZONTAL
        background = activity.getDrawable(R.drawable.bg_banner_info)
        setPadding(dp(14), dp(10), dp(14), dp(10))
        addView(
            TextView(activity).apply {
                setText(R.string.patches_note)
                setTextColor(activity.getColor(R.color.neutral_300))
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            },
        )
    }

    private fun showMenu(anchor: View, mod: ModLibrary.Mod) {
        PopupMenu(activity, anchor).apply {
            // ModsPage's View Mod, offered only for mods a browser installed: the PC's refusal
            // ("Cannot view mod that was not installed through the mod browser") is all it shows otherwise.
            if (mod.modId >= 0) menu.add(0, MENU_VIEW, 0, R.string.patches_view_mod)
            menu.add(0, MENU_RENAME, 1, R.string.patches_rename)
            menu.add(0, MENU_DELETE, 2, R.string.patches_delete)
            setOnMenuItemClickListener { item ->
                when (item.itemId) {
                    MENU_VIEW -> showBrowser(true, mod.modId)
                    MENU_RENAME -> rename(mod)
                    MENU_DELETE -> delete(mod)
                }
                true
            }
            show()
        }
    }

    private fun setModEnabled(mod: ModLibrary.Mod, enabled: Boolean) {
        // Saved in place rather than rebuilt, so the switch keeps its animation.
        val changed = mod.copy(enabled = enabled)
        if (!change { ModLibrary.save(modsDir, changed) }) {
            refresh()
            return
        }
        mods = mods.map { if (it.title == mod.title) changed else it }
        enableAll.isChecked = mods.all { it.enabled }
    }

    private fun setAllEnabled(enabled: Boolean) {
        change {
            for (mod in mods) {
                if (mod.enabled != enabled) ModLibrary.save(modsDir, mod.copy(enabled = enabled))
            }
        }
        refresh()
    }

    private fun move(mod: ModLibrary.Mod, up: Boolean) {
        change { ModLibrary.move(mods, mod, up).forEach { ModLibrary.save(modsDir, it) } }
        refresh()
    }

    private fun rename(mod: ModLibrary.Mod) {
        PatchesWidgets.nameDialog(
            activity,
            R.string.patches_rename_title,
            activity.getString(R.string.patches_rename_message, mod.title),
            mod.title,
            R.string.patches_rename,
            { name -> if (name.trim() == mod.title) null else ModLibrary.validateName(name, mods) },
        ) { name ->
            change { ModLibrary.rename(modsDir, mod, name, mods) }
            refresh()
        }
    }

    private fun delete(mod: ModLibrary.Mod) {
        AlertDialog.Builder(activity)
            .setTitle(activity.getString(R.string.patches_delete_title, mod.title))
            .setMessage(R.string.patches_delete_message)
            .setPositiveButton(R.string.patches_delete) { _, _ ->
                change { ModLibrary.delete(modsDir, mod) }
                refresh()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /** Runs one change to the Mods folder; false, with the reason shown, when it failed. */
    private fun change(edit: () -> Unit): Boolean = try {
        edit()
        true
    } catch (e: IOException) {
        Log.w(TAG, "Mod change failed", e)
        Toast.makeText(activity, activity.getString(R.string.patches_change_failed, e.message ?: e.toString()), Toast.LENGTH_LONG).show()
        false
    }

    private fun displayName(uri: Uri): String {
        runCatching {
            activity.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0)?.takeIf { it.isNotBlank() }?.let { return it }
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() } ?: "file"
    }

    private fun dp(value: Int): Int = PatchesWidgets.dp(activity, value)

    private companion object {
        const val TAG = "WiiCompiledLauncher"
        const val MENU_RENAME = 1
        const val MENU_DELETE = 2
        const val MENU_VIEW = 3
    }
}
