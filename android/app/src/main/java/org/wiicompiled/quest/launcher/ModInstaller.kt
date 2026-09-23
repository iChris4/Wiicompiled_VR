package org.wiicompiled.quest.launcher

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.io.IOException
import java.util.concurrent.Executors
import org.wiicompiled.quest.GameStorage

/**
 * The mod browser's Download and Install (ModContent.DownloadAndInstallCurrentModAsync): one of a
 * mod's files is downloaded from GameBanana into the app's cache, checked, and installed as a new
 * mod carrying its author and GameBanana id, which is how the browser knows later that it is
 * installed. The download has its own thread, so Play never waits behind it; the install then takes
 * its turn on ModLibrary's thread like any import. One runs at a time, holding ModLibrary's import
 * slot from start to end, so the Patches page's Import and this wait for each other.
 */
object ModInstaller {

    sealed class State {
        data object Idle : State()

        /** [done] of [total] bytes fetched for the GameBanana mod [modId]. */
        data class Downloading(val modId: Int, val done: Long, val total: Long) : State()

        /** [done] of [total] bytes unpacked into the Mods folder; 0 of 0 before it knows. */
        data class Installing(val modId: Int, val done: Long, val total: Long) : State()
    }

    private const val TAG = "WiiCompiledLauncher"
    private const val DOWNLOAD_DIRECTORY = "mod-download"
    private const val PROGRESS_INTERVAL_MS = 100L

    /** Changed and read on the main thread. */
    var state: State = State.Idle
        private set

    val busy: Boolean get() = state != State.Idle

    private val listeners = mutableListOf<(State) -> Unit>()
    private val main by lazy { Handler(Looper.getMainLooper()) }
    private val downloader = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "ModDownload").apply { isDaemon = true } }

    @Volatile
    private var cancelRequested = false

    @Volatile
    private var lastReport = 0L

    fun addListener(listener: (State) -> Unit) {
        listeners += listener
    }

    fun removeListener(listener: (State) -> Unit) {
        listeners -= listener
    }

    /** Asks the running download or install to stop; its folder in Mods is never left half-made. */
    fun cancel() {
        if (busy) cancelRequested = true
    }

    /**
     * Downloads [file] of [mod] and installs it as the mod [title]. [done] gets, on the main
     * thread, the new mod, the failure, or null when the player cancelled. Returns false without
     * starting when an import or another install is already running.
     */
    fun start(context: Context, mod: GameBanana.Details, file: GameBanana.ModFile, title: String, done: (Result<ModLibrary.Mod>?) -> Unit): Boolean {
        if (busy || ModLibrary.importing) return false
        ModLibrary.importing = true
        cancelRequested = false
        val modsDir = GameStorage.modsDirectory(context)
        // WheelWizard's temporary mods folder: emptied before each download and after each install.
        val folder = File(context.cacheDir, DOWNLOAD_DIRECTORY)
        val author = mod.author.ifBlank { ModLibrary.NO_ID }
        Log.i(TAG, "Installing GameBanana mod ${mod.id} (${file.name}, ${file.size} bytes) as $title")
        update(State.Downloading(mod.id, 0, file.size))
        downloader.execute {
            val started = SystemClock.elapsedRealtime()
            val archive = try {
                folder.deleteRecursively()
                if (!folder.mkdirs() && !folder.isDirectory) throw IOException("Could not create ${folder.absolutePath}")
                File(folder, safeFileName(file.name)).also { target ->
                    val retried: (Int, IOException) -> Unit = { attempt, failure ->
                        Log.w(TAG, "Download attempt $attempt of ${file.name} failed", failure)
                    }
                    GameBanana.download(file, target, retried) { bytes, total ->
                        report(State.Downloading(mod.id, bytes, total))
                        !cancelRequested
                    }
                }
            } catch (e: Exception) {
                main.post { finish(folder, Result.failure(e), done) }
                return@execute
            }
            Log.i(TAG, "Downloaded ${file.name} in ${SystemClock.elapsedRealtime() - started} ms")
            main.post { if (busy) update(State.Installing(mod.id, 0, 0)) }
            ModLibrary.background({
                if (cancelRequested) throw GameBanana.Cancelled()
                val unpacking = SystemClock.elapsedRealtime()
                val source = ModLibrary.Source(file.name, archive) { archive.inputStream() }
                val progress = ModArchive.Listener { bytes, total ->
                    report(State.Installing(mod.id, bytes, total))
                    !cancelRequested
                }
                ModLibrary.import(modsDir, title, listOf(source), ModLibrary.load(modsDir), author, mod.id, progress).also {
                    Log.i(TAG, "Unpacked ${file.name} into Mods in ${SystemClock.elapsedRealtime() - unpacking} ms")
                }
            }) { result -> finish(folder, result, done) }
        }
        return true
    }

    /** On the main thread: the download folder goes, and the outcome is handed over. */
    private fun finish(folder: File, result: Result<ModLibrary.Mod>, done: (Result<ModLibrary.Mod>?) -> Unit) {
        folder.deleteRecursively()
        ModLibrary.importing = false
        val cancelled = result.isFailure && cancelRequested
        cancelRequested = false
        result.onSuccess { Log.i(TAG, "Installed mod ${it.title} (GameBanana ${it.modId})") }
        result.onFailure { if (!cancelled) Log.w(TAG, "Mod install failed", it) else Log.i(TAG, "Mod install cancelled") }
        update(State.Idle)
        done(if (cancelled) null else result)
    }

    /** Progress from a worker thread, passed on a few times a second at most. */
    private fun report(next: State) {
        val now = SystemClock.uptimeMillis()
        if (now - lastReport < PROGRESS_INTERVAL_MS) return
        lastReport = now
        main.post { if (busy) update(next) }
    }

    private fun update(next: State) {
        state = next
        listeners.toList().forEach { it(next) }
    }

    /** GameBanana's name for the file, kept to one path segment. */
    private fun safeFileName(name: String): String =
        name.substringAfterLast('/').substringAfterLast('\\').takeIf { it.isNotBlank() && it != "." && it != ".." } ?: "mod"
}
