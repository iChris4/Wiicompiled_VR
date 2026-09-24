package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.AlertDialog
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.text.format.Formatter
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.io.IOException
import java.util.concurrent.Executors
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.R

/**
 * My Miis: WheelWizard's MiiListPage for the headset. It lists the Miis of the game's own Mii
 * database ([MiiDatabase], created empty the first time), favourites first, and makes, edits,
 * duplicates, imports, exports and deletes them. The game reads the same file, so a Mii made
 * here can be picked for a licence.
 *
 * The PC selects with a click and adds to the selection with Shift or Ctrl; the headset has
 * neither, so a long press adds a Mii to the selection or takes it out.
 */
class MiisPage(
    private val activity: Activity,
    private val root: View,
    private val gameRunning: () -> Boolean,
    private val pickImport: () -> Unit,
    private val pickExport: (suggestedName: String, several: Boolean) -> Unit,
) {
    private val list: View = root.findViewById(R.id.miis_list)
    private val count: TextView = root.findViewById(R.id.miis_count)
    private val buttons: View = root.findViewById(R.id.miis_buttons)
    private val favoriteButton: ImageView = root.findViewById(R.id.miis_favorite)
    private val editButton: View = root.findViewById(R.id.miis_edit)
    private val duplicateButton: View = root.findViewById(R.id.miis_duplicate)
    private val exportButton: View = root.findViewById(R.id.miis_export)
    private val deleteButton: View = root.findViewById(R.id.miis_delete)
    private val importButton: View = root.findViewById(R.id.miis_import)
    private val parts: View = root.findViewById(R.id.miis_parts)
    private val partsText: TextView = root.findViewById(R.id.miis_parts_text)
    private val partsDownload: TextView = root.findViewById(R.id.miis_parts_download)
    private val empty: View = root.findViewById(R.id.miis_empty)
    private val emptyMessage: TextView = root.findViewById(R.id.miis_empty_message)
    private val scroll: View = root.findViewById(R.id.miis_scroll)
    private val grid: TileGrid = root.findViewById(R.id.miis_grid)
    private val editor = MiiEditor(activity, root.findViewById(R.id.miis_editor), ::editorClosed)

    private var miis: List<Mii> = emptyList()
    private val selected = LinkedHashSet<Long>()
    /** The console's system ID, which marks the Miis made on this headset; null before the game's first start. */
    private var consoleSystemId: Long? = null
    /** Miis waiting for the player to choose where Export saves them. */
    private var exporting: List<Mii> = emptyList()
    /** An editor to open once the list is loaded, for unattended tests (LauncherActivity's EXTRA_DEBUG_MIIS). */
    private var debugSpec: String? = null

    init {
        grid.cellWidth = dp(104)
        grid.spacing = dp(12)
        favoriteButton.setOnClickListener { toggleFavorite(selectedMiis()) }
        editButton.setOnClickListener { selectedMiis().singleOrNull()?.let(::edit) }
        duplicateButton.setOnClickListener { duplicate(selectedMiis()) }
        exportButton.setOnClickListener { export(selectedMiis()) }
        deleteButton.setOnClickListener { delete(selectedMiis()) }
        importButton.setOnClickListener {
            if (!refuseWhileGameRuns()) pickImport()
        }
        partsDownload.setOnClickListener { downloadParts() }
    }

    fun refresh() {
        showParts()
        if (editor.isOpen) {
            editor.refreshPicture()
            return
        }
        load()
    }

    /** For unattended tests: edit:N[:Section] opens the editor on the Nth Mii listed, and that page. */
    fun debugOpen(spec: String) {
        debugSpec = spec.takeIf { it.startsWith("edit:") }
    }

    /** Back closes the editor's page, then the editor, before leaving My Miis. */
    fun back(): Boolean = editor.back()

    // --- The list ---

    private fun load() {
        val app = activity.applicationContext
        worker.execute {
            val nand = GameStorage.nandDirectory(app)
            val file = MiiDatabase.file(nand)
            val result = runCatching {
                MiiDatabase.create(file)
                MiiDatabase.miis(file)
            }
            val systemId = MiiIds.consoleMac(nand)?.let(MiiIds::systemId)
            activity.runOnUiThread {
                if (activity.isDestroyed) return@runOnUiThread
                consoleSystemId = systemId
                result.onSuccess { show(it) }.onFailure { error ->
                    Log.w(TAG, "Cannot open the Mii database", error)
                    showEmpty(error.message ?: error.toString())
                }
            }
        }
    }

    private fun show(all: List<Mii>) {
        // MiiListPage: favourites first, otherwise in the database's order.
        miis = all.sortedByDescending { it.favorite }
        selected.retainAll(miis.map { it.miiId }.toSet())
        empty.visibility = View.GONE
        scroll.visibility = View.VISIBLE
        buttons.visibility = View.VISIBLE
        count.visibility = View.VISIBLE
        count.text = miis.size.toString()
        grid.removeAllViews()
        val inflater = LayoutInflater.from(activity)
        for (mii in miis) {
            val tile = inflater.inflate(R.layout.item_mii, grid, false)
            bindTile(tile, mii)
            tile.setOnClickListener { select(mii, keepOthers = false) }
            tile.setOnLongClickListener {
                select(mii, keepOthers = true)
                true
            }
            grid.addView(tile)
        }
        if (miis.size < MiiDatabase.SLOTS) {
            val add = inflater.inflate(R.layout.item_mii, grid, false)
            add.findViewById<View>(R.id.mii_placeholder).visibility = View.GONE
            add.findViewById<View>(R.id.mii_add).visibility = View.VISIBLE
            add.contentDescription = activity.getString(R.string.miis_new_title)
            add.setOnClickListener {
                selected.clear()
                updateSelection()
                create()
            }
            grid.addView(add)
        }
        updateSelection()
        debugSpec?.let { spec ->
            debugSpec = null
            val parts = spec.split(':')
            miis.getOrNull(parts.getOrNull(1)?.toIntOrNull() ?: 0)?.let { mii ->
                openEditor(mii, isNew = false) { edited -> change({ file -> MiiDatabase.update(file, edited) }) }
                parts.getOrNull(2)?.let(editor::debugShow)
            }
        }
    }

    private fun bindTile(tile: View, mii: Mii) {
        tile.findViewById<TextView>(R.id.mii_name).text = mii.name
        tile.contentDescription = mii.name
        tile.findViewById<View>(R.id.mii_favorite).visibility = if (mii.favorite) View.VISIBLE else View.GONE
        tile.findViewById<View>(R.id.mii_foreign).visibility = if (isForeign(mii)) View.VISIBLE else View.GONE
        tile.isActivated = mii.favorite
        tile.tag = mii.miiId
        tile.findViewById<View>(R.id.mii_placeholder).visibility =
            if (MiiRenderResource.installed(activity)) View.GONE else View.VISIBLE
        val picture = tile.findViewById<ImageView>(R.id.mii_picture)
        MiiImages.head(activity, picture, mii, TILE_PICTURE)
    }

    /** MiiExtensions.IsGlobal: a special Mii, or one another console made. */
    private fun isForeign(mii: Mii): Boolean {
        if ((mii.miiId ushr 29) == 0b110L) return true
        val own = consoleSystemId ?: return false
        return mii.systemId != own
    }

    private fun showEmpty(reason: String) {
        miis = emptyList()
        selected.clear()
        grid.removeAllViews()
        scroll.visibility = View.GONE
        buttons.visibility = View.GONE
        count.visibility = View.GONE
        empty.visibility = View.VISIBLE
        emptyMessage.text = activity.getString(R.string.miis_empty_message, reason)
    }

    /** A click selects only that Mii, or nothing when it was the one selected; a long press adds or removes it. */
    private fun select(mii: Mii, keepOthers: Boolean) {
        val wasSelected = mii.miiId in selected
        if (!keepOthers) selected.clear()
        if (wasSelected) selected.remove(mii.miiId) else selected.add(mii.miiId)
        updateSelection()
    }

    private fun selectedMiis(): List<Mii> = miis.filter { it.miiId in selected }

    /** MiiListPage.ChangeTopButtons. */
    private fun updateSelection() {
        for (i in 0 until grid.childCount) {
            val tile = grid.getChildAt(i)
            tile.isSelected = tile.tag in selected
        }
        val chosen = selectedMiis()
        val any = chosen.isNotEmpty()
        favoriteButton.visibility = if (any) View.VISIBLE else View.GONE
        editButton.visibility = if (chosen.size == 1) View.VISIBLE else View.GONE
        duplicateButton.visibility = if (any) View.VISIBLE else View.GONE
        exportButton.visibility = if (any) View.VISIBLE else View.GONE
        deleteButton.visibility = if (any) View.VISIBLE else View.GONE
        importButton.visibility = if (any) View.GONE else View.VISIBLE
        val unfavorite = any && chosen.all { it.favorite }
        favoriteButton.setImageResource(if (unfavorite) R.drawable.ic_star_empty else R.drawable.ic_star)
        val label = activity.getString(if (unfavorite) R.string.miis_unfavorite else R.string.miis_favorite)
        favoriteButton.contentDescription = label
        favoriteButton.tooltipText = label
    }

    // --- Actions ---

    private fun toggleFavorite(chosen: List<Mii>) {
        if (chosen.isEmpty()) return
        val favorite = !chosen.all { it.favorite }
        change({ file -> chosen.forEach { MiiDatabase.update(file, it.copy(favorite = favorite)) } })
    }

    private fun edit(mii: Mii) {
        // Refused before editing rather than at Save, which would lose the edits.
        if (refuseWhileGameRuns()) return
        openEditor(mii, isNew = false) { edited -> change({ file -> MiiDatabase.update(file, edited) }) }
    }

    /** MiiListPage.CreateNewMii: a random, male or female start, then the editor. */
    private fun create() {
        if (refuseWhileGameRuns()) return
        val mac = consoleMac() ?: return
        val name = activity.getString(R.string.miis_default_name)
        val choices = arrayOf(
            activity.getString(R.string.miis_new_random),
            activity.getString(R.string.miis_new_male),
            activity.getString(R.string.miis_new_female),
        )
        AlertDialog.Builder(activity)
            .setTitle(R.string.miis_new_title)
            .setItems(choices) { _, which ->
                val mii = when (which) {
                    0 -> MiiFactory.random(name)
                    1 -> MiiFactory.male(name)
                    else -> MiiFactory.female(name)
                }
                openEditor(mii, isNew = true) { edited -> change({ file -> MiiDatabase.add(file, stamped(edited, mac)) }) }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /** MiiListPage.DuplicateMii: copies with new IDs, as this headset's own Miis. */
    private fun duplicate(chosen: List<Mii>) {
        if (chosen.isEmpty() || refuseWhileGameRuns()) return
        val mac = consoleMac() ?: return
        val message = if (chosen.size == 1) {
            activity.getString(R.string.miis_duplicated_one, chosen[0].name)
        } else {
            activity.getString(R.string.miis_duplicated_many, chosen.size)
        }
        change({ file -> chosen.forEach { MiiDatabase.add(file, stamped(it, mac)) } }) { toast(message) }
    }

    /**
     * The MAC address Miis made on this headset are stamped with (MiiDbService.AddToDatabase), or
     * null after telling the player to start the game once: the game creates the console's
     * identity on its first start.
     */
    private fun consoleMac(): ByteArray? {
        MiiIds.consoleMac(GameStorage.nandDirectory(activity))?.let { return it }
        AlertDialog.Builder(activity)
            .setTitle(R.string.miis_no_console_title)
            .setMessage(R.string.miis_no_console_message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
        return null
    }

    /** A new Mii of the database: valid, from [mac]'s console, with a fresh ID. */
    private fun stamped(mii: Mii, mac: ByteArray) = mii.copy(invalid = false, systemId = MiiIds.systemId(mac), miiId = MiiIds.newMiiId())

    /** MiiListPage.DeleteMii: never favourites, and only once confirmed. */
    private fun delete(chosen: List<Mii>) {
        if (chosen.isEmpty() || refuseWhileGameRuns()) return
        if (chosen.any { it.favorite }) {
            AlertDialog.Builder(activity)
                .setTitle(R.string.miis_delete_favorite_title)
                .setMessage(R.string.miis_delete_favorite_message)
                .setPositiveButton(android.R.string.ok, null)
                .show()
            return
        }
        val one = chosen.size == 1
        AlertDialog.Builder(activity)
            .setTitle(
                if (one) activity.getString(R.string.miis_delete_title_one, chosen[0].name)
                else activity.getString(R.string.miis_delete_title_many, chosen.size),
            )
            .setMessage(R.string.miis_delete_message)
            .setPositiveButton(R.string.miis_delete) { _, _ ->
                val done = if (one) activity.getString(R.string.miis_deleted_one, chosen[0].name)
                else activity.getString(R.string.miis_deleted_many, chosen.size)
                change({ file -> chosen.forEach { MiiDatabase.remove(file, it.miiId) } }) { toast(done) }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /**
     * The files the player picked with Import: .mii files, 74 bytes each. As on the PC, each is
     * added as a copy with a new ID, marked as made on another console. Unlike the PC, one bad
     * file does not stop the others.
     */
    fun importPicked(uris: List<Uri>) {
        if (uris.isEmpty()) return
        val resolver = activity.contentResolver
        val names = uris.map(::displayName)
        change({ file ->
            val problems = ArrayList<String>()
            for ((uri, name) in uris.zip(names)) {
                try {
                    val bytes = resolver.openInputStream(uri)?.use { it.readBytes() } ?: throw IOException("Cannot read the file.")
                    val mii = MiiData.parse(bytes).copy(invalid = false, systemId = MiiIds.systemId(MiiIds.IMPORT_MAC), miiId = MiiIds.newMiiId())
                    MiiDatabase.add(file, mii)
                } catch (e: IllegalArgumentException) {
                    problems += activity.getString(R.string.miis_import_problem, name, e.message ?: e.toString())
                } catch (e: IOException) {
                    problems += activity.getString(R.string.miis_import_problem, name, e.message ?: e.toString())
                }
            }
            problems
        }) { problems ->
            val added = uris.size - problems.size
            val summary = activity.resources.getQuantityString(R.plurals.miis_imported, added, added)
            if (problems.isEmpty()) {
                toast(summary)
            } else {
                AlertDialog.Builder(activity)
                    .setTitle(summary)
                    .setMessage(problems.joinToString("\n"))
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
        }
    }

    /** MiiListPage.ExportMultipleMiiFiles: each Mii as its 74 bytes in a .mii file. */
    private fun export(chosen: List<Mii>) {
        if (chosen.isEmpty()) return
        exporting = chosen
        pickExport(fileName(chosen[0]), chosen.size > 1)
    }

    /** Where the player chose to save one Mii (a new document) or several (a folder). */
    fun exportPicked(uri: Uri, folder: Boolean) {
        val chosen = exporting
        exporting = emptyList()
        if (chosen.isEmpty()) return
        val resolver = activity.contentResolver
        worker.execute {
            val result = runCatching {
                if (!folder) {
                    resolver.openOutputStream(uri, "wt")?.use { it.write(MiiData.serialize(chosen[0])) } ?: throw IOException("Cannot write the file.")
                    activity.getString(R.string.miis_exported_one, chosen[0].name)
                } else {
                    val parent = DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri))
                    for (mii in chosen) {
                        val document = DocumentsContract.createDocument(resolver, parent, MII_MIME, fileName(mii))
                            ?: throw IOException("Cannot create ${fileName(mii)}.")
                        resolver.openOutputStream(document, "wt")?.use { it.write(MiiData.serialize(mii)) }
                            ?: throw IOException("Cannot write ${fileName(mii)}.")
                    }
                    activity.getString(R.string.miis_exported_many, chosen.size)
                }
            }
            activity.runOnUiThread {
                if (activity.isDestroyed) return@runOnUiThread
                toast(result.getOrElse { activity.getString(R.string.miis_export_failed, it.message ?: it.toString()) })
            }
        }
    }

    /** CustomCharactersService.NormalizeToAscii and ReplaceInvalidFileNameChars, then .mii. */
    private fun fileName(mii: Mii): String {
        val ascii = java.text.Normalizer.normalize(mii.name, java.text.Normalizer.Form.NFD)
            .replace(Regex("\\p{M}+"), "")
            .filter { it.code in 0x20..0x7E }
            .split('/', '\\', ':', '*', '?', '"', '<', '>', '|')
            .filter { it.isNotEmpty() }
            .joinToString("_")
            .trim()
        return (ascii.ifEmpty { "Mii" }) + ".mii"
    }

    private fun displayName(uri: Uri): String {
        val queried = runCatching {
            activity.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0) else null
            }
        }.getOrNull()
        return queried?.takeIf { it.isNotBlank() } ?: uri.lastPathSegment?.substringAfterLast('/') ?: uri.toString()
    }

    /**
     * Changes the database off the main thread, unless the game runs (it reads the file), then
     * lists the Miis again and hands the change's result to [done]. A failure is shown instead.
     */
    private fun <T> change(work: (File) -> T, done: (T) -> Unit = {}) {
        if (refuseWhileGameRuns()) return
        val app = activity.applicationContext
        worker.execute {
            val result = runCatching { work(MiiDatabase.file(GameStorage.nandDirectory(app))) }
            activity.runOnUiThread {
                if (activity.isDestroyed) return@runOnUiThread
                result.onSuccess(done).onFailure { error ->
                    Log.w(TAG, "Mii database change failed", error)
                    AlertDialog.Builder(activity)
                        .setTitle(R.string.miis_change_failed)
                        .setMessage(error.message ?: error.toString())
                        .setPositiveButton(android.R.string.ok, null)
                        .show()
                }
                load()
            }
        }
    }

    private fun toast(message: String) = Toast.makeText(activity, message, Toast.LENGTH_LONG).show()

    private fun editorClosed() {
        list.visibility = View.VISIBLE
    }

    // --- The Mii parts ---

    /** The download offer: the parts and bodies, or only the bodies once the parts are here. */
    private fun showParts() {
        if (MiiRenderResource.complete(activity)) {
            parts.visibility = View.GONE
            return
        }
        parts.visibility = View.VISIBLE
        partsDownload.isEnabled = !MiiRenderResource.installing
        if (!MiiRenderResource.installing) {
            val message = if (MiiRenderResource.installed(activity)) R.string.miis_bodies_message else R.string.miis_parts_message
            partsText.text = activity.getString(message, Formatter.formatShortFileSize(activity, MiiRenderResource.downloadBytes(activity)))
        }
    }

    private fun downloadParts() {
        partsDownload.isEnabled = false
        val total = MiiRenderResource.downloadBytes(activity)
        partsText.text = activity.getString(
            if (MiiRenderResource.installed(activity)) R.string.miis_bodies_connecting else R.string.miis_parts_connecting,
        )
        MiiRenderResource.install(
            activity,
            progress = { bytes ->
                partsText.text = activity.getString(
                    R.string.miis_parts_downloading,
                    Formatter.formatShortFileSize(activity, bytes),
                    Formatter.formatShortFileSize(activity, total),
                )
            },
        ) { error ->
            if (activity.isDestroyed) return@install
            if (error != null) {
                partsDownload.isEnabled = true
                partsText.text = activity.getString(R.string.miis_parts_failed, error)
                return@install
            }
            MiiImages.clear()
            showParts()
            if (editor.isOpen) editor.refreshPicture() else load()
        }
    }

    private fun refuseWhileGameRuns(): Boolean {
        if (!gameRunning()) return false
        Toast.makeText(activity, R.string.miis_close_game, Toast.LENGTH_LONG).show()
        return true
    }

    /** The editor takes the list's place, as the PC opens its editor over the page. */
    private fun openEditor(mii: Mii, isNew: Boolean, onSave: (Mii) -> Unit) {
        list.visibility = View.GONE
        editor.open(mii, isNew, onSave)
    }

    private fun dp(value: Int): Int = PatchesWidgets.dp(activity, value)

    private companion object {
        const val TAG = "WiiCompiledLauncher"
        const val MII_MIME = "application/octet-stream"
        /** The list's pictures, at the 109 dp they are shown at: larger than the tile, which crops them. */
        const val TILE_PICTURE = 136

        /** One database change at a time, in order. */
        val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "MiiDatabase").apply { isDaemon = true } }
    }
}
