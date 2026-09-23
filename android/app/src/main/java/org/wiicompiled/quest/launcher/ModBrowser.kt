package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Intent
import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.PixelFormat
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.net.Uri
import android.text.Html
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.format.Formatter
import android.text.method.LinkMovementMethod
import android.text.style.ClickableSpan
import android.text.style.URLSpan
import android.util.Log
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.AbsListView
import android.widget.BaseAdapter
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.io.IOException
import java.util.concurrent.Executors
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.R

/**
 * WheelWizard's mod browser (Views/Popups/ModManagement/ModBrowserWindow and ModContent), shown in
 * place of the Patches page's list: GameBanana's Mario Kart Wii mods, searched by name and listed a
 * page at a time as the list scrolls, beside the chosen mod's pictures, description and Download
 * and Install.
 *
 * As on the PC, an empty search lists "Mod" ("Patches" with Patches only on), mods that use
 * patches come first (or alone, with Patches only), and a mod the browser installed shows as
 * Installed with Uninstall beside it. Two things differ for a headset: the mod's name is asked for
 * before the download rather than after it, so the rest runs without the player, and a mod with
 * several files asks which one instead of taking the first, which is often an old version.
 */
class ModBrowser(
    private val activity: Activity,
    private val root: View,
    private val close: () -> Unit,
    /** The Mods folder changed: a mod was installed or uninstalled here. */
    private val modsChanged: () -> Unit,
) {

    private val search: EditText = root.findViewById(R.id.browser_search)
    private val patchesOnly: Switch = root.findViewById(R.id.browser_patches_only)
    private val list: ListView = root.findViewById(R.id.browser_results)
    private val listMessage: View = root.findViewById(R.id.browser_results_message)
    private val listMessageText: TextView = root.findViewById(R.id.browser_results_message_text)
    private val retry: View = root.findViewById(R.id.browser_retry)

    private val detailsLoading: View = root.findViewById(R.id.browser_details_loading)
    private val detailsEmpty: View = root.findViewById(R.id.browser_details_empty)
    private val detailsEmptyTitle: TextView = root.findViewById(R.id.browser_details_empty_title)
    private val detailsEmptyMessage: TextView = root.findViewById(R.id.browser_details_empty_message)
    private val detailsView: View = root.findViewById(R.id.browser_details)
    private val banner: ImageView = root.findViewById(R.id.browser_banner)
    private val title: TextView = root.findViewById(R.id.browser_mod_title)
    private val likes: TextView = root.findViewById(R.id.browser_mod_likes)
    private val views: TextView = root.findViewById(R.id.browser_mod_views)
    private val downloads: TextView = root.findViewById(R.id.browser_mod_downloads)
    private val author: TextView = root.findViewById(R.id.browser_mod_author)
    private val pageLink: View = root.findViewById(R.id.browser_mod_page)
    private val report: View = root.findViewById(R.id.browser_mod_report)
    private val install: TextView = root.findViewById(R.id.browser_install)
    private val cancel: View = root.findViewById(R.id.browser_cancel)
    private val uninstall: View = root.findViewById(R.id.browser_uninstall)
    private val installNote: TextView = root.findViewById(R.id.browser_install_note)
    private val detailsScroll: View = root.findViewById(R.id.browser_details_scroll)
    private val imagesLabel: View = root.findViewById(R.id.browser_images_label)
    private val imagesScroll: View = root.findViewById(R.id.browser_images_scroll)
    private val images: LinearLayout = root.findViewById(R.id.browser_images)
    private val description: TextView = root.findViewById(R.id.browser_mod_description)

    private val modsDir get() = GameStorage.modsDirectory(activity)

    // The list (ModBrowserWindow): every result loaded so far, and the ones shown.
    private val loaded = mutableListOf<GameBanana.Preview>()
    private var shown: List<GameBanana.Preview> = emptyList()
    private var searchTerm = ""
    private var nextPage = 1
    private var hasMore = true
    private var loadingList = false
    private var listError: String? = null
    /** Pages fetched in a row while Patches only has found nothing to show. */
    private var pagesWithoutPatches = 0
    /** Bumped by every new search, so an answer to an older one is dropped. */
    private var listGeneration = 0
    /** Whether the first search has run: nothing is fetched until the browser is opened. */
    private var searched = false

    // The details (ModContent).
    private var selectedId: Int? = null
    private var details: GameBanana.Details? = null
    /** The installed mod the browser put there for [details], if any. */
    private var installedMod: ModLibrary.Mod? = null
    private var detailsGeneration = 0
    /** A mod an unattended test asked to install once its details are in ([showMod]). */
    private var installWhenLoaded: Int? = null

    private val adapter = Results()

    init {
        root.findViewById<View>(R.id.browser_back).setOnClickListener { close() }
        list.adapter = adapter
        list.setOnItemClickListener { _, _, position, _ ->
            shown.getOrNull(position)?.let { mod ->
                hideKeyboard()
                // Choosing the same mod again only reloads it when it failed to load.
                if (mod.id != selectedId || (details == null && detailsLoading.visibility != View.VISIBLE)) loadDetails(mod.id)
                markSelection()
            }
        }
        list.setOnScrollListener(object : AbsListView.OnScrollListener {
            override fun onScrollStateChanged(view: AbsListView, scrollState: Int) = Unit

            override fun onScroll(view: AbsListView, first: Int, visibleCount: Int, total: Int) {
                // ModBrowserWindow loads the next page when the list nears its end. ListView also
                // calls this when the listener is set, before the browser was ever opened.
                if (searched && total > 0 && first + visibleCount >= total - SCROLL_AHEAD) loadMore()
            }
        })
        search.setOnEditorActionListener { _, action, event ->
            val enter = event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN
            if (action == EditorInfo.IME_ACTION_SEARCH || enter) {
                runSearch()
                true
            } else {
                false
            }
        }
        root.findViewById<View>(R.id.browser_search_button).setOnClickListener { runSearch() }
        PatchesWidgets.styleSwitch(activity, patchesOnly)
        patchesOnly.setOnClickListener { reload() }
        root.findViewById<View>(R.id.browser_patches_only_row).setOnClickListener {
            patchesOnly.toggle()
            reload()
        }
        retry.setOnClickListener {
            listError = null
            loadMore()
        }
        install.setOnClickListener { startInstall() }
        cancel.setOnClickListener { ModInstaller.cancel() }
        uninstall.setOnClickListener { uninstall() }
        pageLink.setOnClickListener { details?.profileUrl?.let(::openLink) }
        author.setOnClickListener { details?.authorUrl?.let(::openLink) }
        report.setOnClickListener { details?.let { openLink(it.reportUrl) } }
        description.movementMethod = LinkMovementMethod.getInstance()
    }

    /** Shown again: the first search runs once, and what is installed may have changed meanwhile. */
    fun opened() {
        if (!searched || (loaded.isEmpty() && !loadingList && listError == null)) reload() else renderList()
        refreshInstalled()
    }

    /**
     * ModsPage's View Mod: a mod the browser installed, by its GameBanana id. With [install], an
     * unattended test's, the mod is installed once it has loaded, with its first file and the
     * name the dialog would suggest.
     */
    fun showMod(modId: Int, install: Boolean = false) {
        opened()
        installWhenLoaded = if (install) modId else null
        loadDetails(modId)
        markSelection()
    }

    /** ModInstaller moved on: the chosen mod's buttons follow it. */
    fun installStateChanged() {
        if (details != null) renderInstall()
    }

    // The list

    private fun runSearch() {
        hideKeyboard()
        val term = search.text.toString().trim()
        if (term.isNotEmpty() && term.length < GameBanana.MIN_SEARCH_LENGTH) {
            Toast.makeText(activity, R.string.browser_search_too_short, Toast.LENGTH_LONG).show()
            return
        }
        searchTerm = term
        reload()
    }

    /** ModBrowserWindow.ReloadSearchResults: back to page 1 for the current search and switch. */
    private fun reload() {
        searched = true
        listGeneration++
        loaded.clear()
        nextPage = 1
        hasMore = true
        loadingList = false
        listError = null
        pagesWithoutPatches = 0
        shown = emptyList()
        renderList()
        list.setSelection(0)
        loadMore()
    }

    /**
     * Whether another page can be asked for: GameBanana has more, nothing failed, and Patches only
     * has not looked through [PATCHES_SEARCH_PAGES] pages in a row without finding one to show.
     * A short list asks again after every page, since the list scrolls to its end by itself.
     */
    private fun canLoadMore(): Boolean =
        hasMore && listError == null && !(patchesOnly.isChecked && shown.isEmpty() && pagesWithoutPatches >= PATCHES_SEARCH_PAGES)

    private fun loadMore() {
        if (loadingList || !canLoadMore()) return
        loadingList = true
        val generation = listGeneration
        val term = GameBanana.effectiveSearch(searchTerm, patchesOnly.isChecked)
        val page = nextPage
        renderList()
        network.execute {
            val result = runCatching { GameBanana.search(term, page) }
            root.post {
                if (generation != listGeneration) return@post
                loadingList = false
                result.onSuccess { found ->
                    Log.i(TAG, "Mod browser: page $page for \"$term\" has ${found.mods.size} mods" + if (found.complete) ", the last" else "")
                    loaded += found.mods.filter { mod -> loaded.none { it.id == mod.id } }
                    nextPage = page + 1
                    hasMore = !found.complete
                    shown = visibleMods()
                    // ModBrowserWindow.EnsurePatchesOnlyResultsAsync, bounded by canLoadMore: keep
                    // looking while Patches only has nothing to show yet.
                    if (patchesOnly.isChecked && shown.isEmpty()) pagesWithoutPatches++
                    renderList()
                    if (patchesOnly.isChecked && shown.isEmpty()) loadMore()
                }.onFailure { failure ->
                    Log.w(TAG, "Mod browser search failed", failure)
                    listError = failure.message ?: failure.toString()
                    renderList()
                }
            }
        }
    }

    /** ModBrowserWindow.ApplyModListFilters: Patches only keeps those; otherwise they come first. */
    private fun visibleMods(): List<GameBanana.Preview> =
        if (patchesOnly.isChecked) loaded.filter { it.usesPatches } else loaded.sortedByDescending { it.usesPatches }

    private fun renderList() {
        adapter.notifyDataSetChanged()
        markSelection()
        val error = listError
        when {
            error != null -> {
                listMessageText.text = activity.getString(R.string.browser_load_failed, error)
                retry.visibility = View.VISIBLE
                listMessage.visibility = View.VISIBLE
            }
            shown.isEmpty() && !loadingList && !canLoadMore() -> {
                listMessageText.text = activity.getString(R.string.browser_no_results, GameBanana.effectiveSearch(searchTerm, patchesOnly.isChecked))
                retry.visibility = View.GONE
                listMessage.visibility = View.VISIBLE
            }
            else -> listMessage.visibility = View.GONE
        }
    }

    private fun markSelection() {
        val position = shown.indexOfFirst { it.id == selectedId }
        if (position >= 0) list.setItemChecked(position, true) else list.clearChoices()
    }

    private inner class Results : BaseAdapter() {
        /** The mods, then a loading row while more can come (the PC's "LOADING" entry). */
        override fun getCount(): Int = shown.size + if (loadingList || canLoadMore()) 1 else 0

        override fun getItem(position: Int): Any? = shown.getOrNull(position)

        override fun getItemId(position: Int): Long = shown.getOrNull(position)?.id?.toLong() ?: -1L

        override fun hasStableIds(): Boolean = true

        override fun getViewTypeCount(): Int = 2

        override fun getItemViewType(position: Int): Int = if (position < shown.size) 0 else 1

        override fun isEnabled(position: Int): Boolean = position < shown.size

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val mod = shown.getOrNull(position)
                ?: return convertView ?: LayoutInflater.from(activity).inflate(R.layout.item_mod_browser_loading, parent, false)
            val row = convertView ?: LayoutInflater.from(activity).inflate(R.layout.item_mod_browser, parent, false)
            row.findViewById<TextView>(R.id.browser_item_title).text = mod.name
            row.findViewById<TextView>(R.id.browser_item_author).text = activity.getString(R.string.browser_by, mod.author)
            row.findViewById<TextView>(R.id.browser_item_likes).text = mod.likes.toString()
            row.findViewById<TextView>(R.id.browser_item_views).text = mod.views.toString()
            row.findViewById<View>(R.id.browser_item_patches).visibility = if (mod.usesPatches) View.VISIBLE else View.GONE
            row.setBackgroundResource(if (mod.usesPatches) R.drawable.bg_browser_item_patches else R.drawable.bg_browser_item)
            val image = row.findViewById<ImageView>(R.id.browser_item_image)
            image.setBackgroundColor(activity.getColor(if (mod.usesPatches) R.color.patches_image else R.color.neutral_700))
            RemoteImages.into(image, mod.image?.small, dp(THUMBNAIL_WIDTH_DP))
            return row
        }
    }

    // The details

    /** ModContent.LoadModDetailsAsync: the list stays usable, and only the latest choice is shown. */
    private fun loadDetails(modId: Int) {
        val generation = ++detailsGeneration
        selectedId = modId
        details = null
        showDetailsState(loading = true)
        network.execute {
            val result = runCatching { GameBanana.details(modId) }
            root.post {
                if (generation != detailsGeneration) return@post
                result.onSuccess { found ->
                    details = found
                    renderDetails(found)
                    if (installWhenLoaded == found.id) {
                        installWhenLoaded = null
                        val file = found.files.firstOrNull()
                        val name = ModLibrary.nameFrom(found.name)
                        if (file != null && installedMod == null && ModLibrary.validateName(name, ModLibrary.load(modsDir)) == null) {
                            install(found, file, name)
                        } else {
                            Log.w(TAG, "Mod ${found.id} cannot be installed unattended: no file, installed already, or its name is taken")
                        }
                    }
                }.onFailure { failure ->
                    Log.w(TAG, "Mod browser could not load mod $modId", failure)
                    detailsEmptyTitle.setText(R.string.browser_details_failed)
                    detailsEmptyMessage.text = failure.message ?: failure.toString()
                    showDetailsState(loading = false)
                }
            }
        }
    }

    private fun showDetailsState(loading: Boolean) {
        detailsLoading.visibility = if (loading) View.VISIBLE else View.GONE
        detailsView.visibility = if (!loading && details != null) View.VISIBLE else View.GONE
        detailsEmpty.visibility = if (!loading && details == null) View.VISIBLE else View.GONE
    }

    private fun renderDetails(mod: GameBanana.Details) {
        title.text = mod.name
        likes.text = mod.likes.toString()
        views.text = mod.views.toString()
        downloads.text = mod.downloads.toString()
        author.text = mod.author
        author.visibility = if (mod.author.isNotBlank()) View.VISIBLE else View.GONE
        pageLink.visibility = if (mod.profileUrl != null) View.VISIBLE else View.GONE

        // The first picture is the banner too, as in ModContent.
        RemoteImages.into(banner, mod.images.firstOrNull()?.medium, dp(BANNER_WIDTH_DP))
        images.removeAllViews()
        for (image in mod.images) {
            val view = ImageView(activity).apply {
                adjustViewBounds = true
                scaleType = ImageView.ScaleType.FIT_CENTER
                importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
            }
            images.addView(view, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.MATCH_PARENT).apply {
                marginEnd = dp(8)
            })
            RemoteImages.into(view, image.medium, dp(GALLERY_WIDTH_DP))
        }
        imagesLabel.visibility = if (mod.images.isEmpty()) View.GONE else View.VISIBLE
        imagesScroll.visibility = imagesLabel.visibility
        description.text = description(mod.text, detailsGeneration)
        detailsScroll.scrollTo(0, 0)

        refreshInstalled()
        showDetailsState(loading = false)
    }

    /** Whether the chosen mod is in the Mods folder, read again after anything that may change it. */
    private fun refreshInstalled() {
        val mod = details ?: return
        installedMod = ModLibrary.installed(ModLibrary.load(modsDir), mod.id)
        renderInstall()
    }

    /** ModContent.UpdateDownloadButtonState, plus the progress of an install that is running. */
    private fun renderInstall() {
        val mod = details ?: return
        val state = ModInstaller.state
        val running = when (state) {
            is ModInstaller.State.Downloading -> state.modId == mod.id
            is ModInstaller.State.Installing -> state.modId == mod.id
            ModInstaller.State.Idle -> false
        }
        val busy = ModInstaller.busy || ModLibrary.importing
        install.text = when {
            state is ModInstaller.State.Downloading && running -> activity.getString(R.string.browser_downloading, percent(state.done, state.total))
            state is ModInstaller.State.Installing && running && state.total > 0 ->
                activity.getString(R.string.browser_installing_percent, percent(state.done, state.total))
            running -> activity.getString(R.string.browser_installing)
            installedMod != null -> activity.getString(R.string.browser_installed)
            else -> activity.getString(R.string.browser_install)
        }
        install.isEnabled = !running && installedMod == null && mod.files.isNotEmpty() && !busy
        cancel.visibility = if (running) View.VISIBLE else View.GONE
        uninstall.visibility = if (!running && installedMod != null) View.VISIBLE else View.GONE
        uninstall.isEnabled = !busy
        val note = when {
            running || installedMod != null -> null
            mod.files.isEmpty() -> activity.getString(R.string.browser_no_files)
            busy -> activity.getString(R.string.browser_busy)
            else -> null
        }
        installNote.text = note
        installNote.visibility = if (note != null) View.VISIBLE else View.GONE
    }

    private fun percent(done: Long, total: Long): Int = if (total > 0) (done * 100 / total).toInt().coerceIn(0, 100) else 0

    // Install and uninstall

    private fun startInstall() {
        val mod = details ?: return
        if (ModInstaller.busy || ModLibrary.importing || installedMod != null) return
        when (mod.files.size) {
            0 -> return
            1 -> askName(mod, mod.files[0])
            else -> {
                val labels = mod.files.map { file ->
                    val size = Formatter.formatShortFileSize(activity, file.size)
                    if (file.description.isBlank()) "${file.name} ($size)" else "${file.name} ($size)\n${file.description}"
                }
                AlertDialog.Builder(activity)
                    .setTitle(R.string.browser_choose_file)
                    .setItems(labels.toTypedArray()) { _, which -> askName(mod, mod.files[which]) }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
            }
        }
    }

    /** WheelWizard's TextInputWindow, asked before the download so the rest runs by itself. */
    private fun askName(mod: GameBanana.Details, file: GameBanana.ModFile) {
        val message = activity.getString(R.string.browser_install_note, file.name, Formatter.formatShortFileSize(activity, file.size))
        PatchesWidgets.nameDialog(activity, R.string.patches_name_title, message, ModLibrary.nameFrom(mod.name), R.string.browser_install, {
            ModLibrary.validateName(it, ModLibrary.load(modsDir))
        }) { name -> install(mod, file, name) }
    }

    /** Downloads and installs [file] as [name]; the outcome shows as the Patches page's import's does. */
    private fun install(mod: GameBanana.Details, file: GameBanana.ModFile, name: String) {
        val started = ModInstaller.start(activity, mod, file, name) { result ->
            if (activity.isDestroyed) return@start
            modsChanged()
            refreshInstalled()
            result?.onSuccess { installed ->
                Toast.makeText(activity, activity.getString(R.string.patches_installed, installed.title), Toast.LENGTH_LONG).show()
            }?.onFailure { failure ->
                AlertDialog.Builder(activity)
                    .setTitle(R.string.browser_install_failed)
                    .setMessage(failure.message ?: failure.toString())
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
        }
        if (!started) renderInstall()
    }

    /** ModContent.UnInstall_Click, with the Patches page's confirmation, since deleting is final. */
    private fun uninstall() {
        val mod = installedMod ?: return
        if (ModInstaller.busy || ModLibrary.importing) return
        AlertDialog.Builder(activity)
            .setTitle(activity.getString(R.string.patches_delete_title, mod.title))
            .setMessage(R.string.patches_delete_message)
            .setPositiveButton(R.string.patches_delete) { _, _ ->
                try {
                    ModLibrary.delete(modsDir, mod)
                } catch (e: IOException) {
                    Log.w(TAG, "Uninstall failed", e)
                    Toast.makeText(activity, activity.getString(R.string.patches_change_failed, e.message ?: e.toString()), Toast.LENGTH_LONG).show()
                }
                modsChanged()
                refreshInstalled()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    // Description

    /**
     * The mod's description, GameBanana's HTML: its colour classes get the colours WheelWizard's
     * stylesheet gives them, pictures load in after the text, and links open in the browser.
     */
    private fun description(html: String, generation: Int): Spanned {
        var text = html
        for ((name, color) in HTML_COLORS) text = text.replace("class=\"$name\"", "style=\"color:$color\"")
        val parsed = SpannableStringBuilder(Html.fromHtml(text, Html.FROM_HTML_MODE_COMPACT, DescriptionImages(generation), null))
        for (span in parsed.getSpans(0, parsed.length, URLSpan::class.java)) {
            val url = span.url
            val start = parsed.getSpanStart(span)
            val end = parsed.getSpanEnd(span)
            parsed.removeSpan(span)
            parsed.setSpan(object : ClickableSpan() {
                override fun onClick(widget: View) = openLink(url)
            }, start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        return parsed
    }

    private inner class DescriptionImages(private val generation: Int) : Html.ImageGetter {
        override fun getDrawable(source: String?): Drawable {
            val holder = LateDrawable()
            val url = source?.trim()?.let { if (it.startsWith("//")) "https:$it" else it }
            if (url == null || !(url.startsWith("https://") || url.startsWith("http://"))) return holder
            RemoteImages.fetch(url, dp(DESCRIPTION_IMAGE_WIDTH_DP)) { bitmap ->
                if (generation != detailsGeneration) return@fetch
                val width = (description.width - description.paddingStart - description.paddingEnd).takeIf { it > 0 } ?: dp(DESCRIPTION_IMAGE_WIDTH_DP)
                holder.set(BitmapDrawable(activity.resources, bitmap), width)
                // Setting the same text again lays it out around the picture's new size.
                description.text = description.text
            }
            return holder
        }
    }

    /** A picture in the description that arrives after the text is on screen. */
    private class LateDrawable : Drawable() {
        private var picture: Drawable? = null

        fun set(drawable: Drawable, maxWidth: Int) {
            val scale = minOf(1f, maxWidth.toFloat() / drawable.intrinsicWidth.coerceAtLeast(1))
            val width = (drawable.intrinsicWidth * scale).toInt()
            val height = (drawable.intrinsicHeight * scale).toInt()
            drawable.setBounds(0, 0, width, height)
            setBounds(0, 0, width, height)
            picture = drawable
        }

        override fun draw(canvas: Canvas) {
            picture?.draw(canvas)
        }

        override fun setAlpha(alpha: Int) = Unit

        override fun setColorFilter(colorFilter: ColorFilter?) = Unit

        @Deprecated("Deprecated in Java")
        override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
    }

    // Helpers

    private fun openLink(url: String) {
        try {
            activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (e: ActivityNotFoundException) {
            Toast.makeText(activity, R.string.browser_no_browser, Toast.LENGTH_LONG).show()
        }
    }

    private fun hideKeyboard() {
        search.clearFocus()
        activity.getSystemService(InputMethodManager::class.java)?.hideSoftInputFromWindow(search.windowToken, 0)
    }

    private fun dp(value: Int): Int = PatchesWidgets.dp(activity, value)

    private companion object {
        const val TAG = "WiiCompiledLauncher"
        /** Rows from the end at which the next page is asked for. */
        const val SCROLL_AHEAD = 3
        /** How many pages Patches only looks through for a first result before it stops. */
        const val PATCHES_SEARCH_PAGES = 8
        const val THUMBNAIL_WIDTH_DP = 96
        const val BANNER_WIDTH_DP = 480
        const val GALLERY_WIDTH_DP = 300
        const val DESCRIPTION_IMAGE_WIDTH_DP = 440

        /** The classes GameBanana colours text with, in WheelWizard's HtmlPanel stylesheet colours. */
        val HTML_COLORS = listOf(
            "BlueColor" to "#3489AA",
            "GreenColor" to "#34EA3A",
            "OrangeColor" to "#EA9834",
            "GreyColor" to "#6C7389",
            "RedColor" to "#F7524D",
            "PurpleColor" to "#9A4DF7",
        )

        val network = Executors.newFixedThreadPool(2) { runnable -> Thread(runnable, "ModBrowser").apply { isDaemon = true } }
    }
}
