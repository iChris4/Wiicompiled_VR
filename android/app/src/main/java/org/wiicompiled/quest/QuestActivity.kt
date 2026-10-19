package org.wiicompiled.quest

import android.app.AlertDialog
import android.content.Context
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.system.Os
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface

/**
 * The game: the immersive activity that runs the native runtime and the player's
 * own compiled game. The app opens on [org.wiicompiled.quest.launcher.LauncherActivity],
 * whose Play button starts this one.
 *
 * SDLActivity owns the surface, the Java-side event pump and the JNI plumbing
 * the OpenXR loader needs (the runtime fetches the JavaVM and this activity
 * through SDL_GetAndroidJNIEnv/SDL_GetAndroidActivity). What this subclass
 * adds is everything the native runtime cannot discover on its own:
 *
 *  - the data directory the player fills with the extracted disc
 *    (Android/data/<package>/files/WiiCompiledOpenXRVR/DATA) and where
 *    Config.toml, saves and logs live ([GameStorage]);
 *  - the bundled read-only runtime resources, unpacked from the APK once.
 *
 * The paths are handed over through the environment before SDLActivity's own
 * onCreate loads the native libraries, because the runtime resolves them from
 * static initialisers where SDL's JNI helpers are not yet usable.
 *
 * This activity lives in its own `:game` process, and that process ends with
 * it. Neither SDL nor the runtime can start a second time in one process:
 * SDLActivity calls System.exit on a re-created activity once SDL_main has run,
 * and guest memory, fibers and the OpenXR device are process-wide. A fresh
 * process per session also keeps the launcher alive when the game exits.
 */
class QuestActivity : SDLActivity() {

    /** The game the launcher's toggle last selected; read once, since a session plays one game. */
    private val profile: GameProfile by lazy { GameProfile.selected(this) }

    override fun getLibraries(): Array<String> = arrayOf("SDL3")

    /**
     * The game is not part of the APK: it is the libmain.so the player built from their own disc
     * ([GameLibrary]) for the selected profile, loaded from private storage. Its libSDL3.so,
     * libpng16.so and libc++_shared.so dependencies resolve against the ones this APK installed.
     */
    override fun loadLibraries() {
        super.loadLibraries()
        // SDLActivity reports a failed load in its own error dialog and never starts the game.
        storageFailure?.let { throw UnsatisfiedLinkError(getString(R.string.game_storage_failed, it)) }
        // A Retro Rewind game built from an earlier pack still runs (the launcher's Play anyway).
        if (!GameLibrary.loadable(GameLibrary.status(this, profile))) {
            throw UnsatisfiedLinkError(getString(R.string.game_not_installed))
        }
        System.load(GameLibrary.library(this, profile).absolutePath)
    }

    override fun getMainSharedObject(): String = GameLibrary.library(this, profile).absolutePath

    override fun createSDLSurface(context: Context): SDLSurface = QuestSurface(context)

    /** Why the game directory could not be prepared, reported through SDL's own error dialog. */
    private var storageFailure: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        try {
            GameStorage.prepare(this)
        } catch (e: Exception) {
            // Storage the app cannot write (a directory adb created first, a full disk) would
            // otherwise end the game with nothing on screen.
            Log.e(TAG, "Cannot prepare the game directory", e)
            storageFailure = e.message ?: e.toString()
        }
        val resources = unpackRuntimeResources()

        Os.setenv("MKW_ANDROID_DATA_DIR", GameStorage.dataRoot(this).absolutePath, true)
        Os.setenv("MKW_ANDROID_RESOURCES_DIR", resources.absolutePath, true)

        super.onCreate(savedInstanceState)

        // The launcher does not offer Play without DATA; this covers a direct
        // start, such as adb am start.
        if (GameStorage.discStatus(this) == GameStorage.DiscStatus.Missing && !mBrokenLibraries) {
            val disc = GameStorage.discDirectory(this)
            Log.w(TAG, "No extracted disc found under ${disc.parentFile?.absolutePath}")
            AlertDialog.Builder(this)
                .setTitle(getString(R.string.missing_game_data_title))
                .setMessage(getString(R.string.missing_game_data_message, disc.absolutePath))
                .setCancelable(false)
                .setPositiveButton(android.R.string.ok) { _, _ -> finish() }
                .show()
        }
    }

    override fun onDestroy() {
        // SDLActivity asks SDL_main to quit and waits up to a second for it.
        super.onDestroy()
        // Ended from the main looper so the activity manager has already
        // recorded this destruction and returns to the launcher.
        Log.i(TAG, "Game activity destroyed; ending the game process")
        Handler(Looper.getMainLooper()).post { Process.killProcess(Process.myPid()) }
    }

    /**
     * Copies runtime_resources/ from the APK assets into private storage the
     * first time this build runs. A stamp file keyed on the version and the
     * install time keeps every later launch to one existence check.
     */
    private fun unpackRuntimeResources(): File {
        val target = File(filesDir, "runtime_resources")
        val stamp = File(target, ".version")
        // The version fields stay the same across sideloaded builds, so the install time is what
        // keeps a rebuilt APK from reusing the previous build's bundled pipeline cache.
        @Suppress("DEPRECATION")
        val installedAt = packageManager.getPackageInfo(packageName, 0).lastUpdateTime
        val expected = "${BuildConfig.VERSION_CODE}:${BuildConfig.VERSION_NAME}:$installedAt"
        if (stamp.isFile && stamp.readText() == expected) {
            return target
        }
        target.deleteRecursively()
        target.mkdirs()
        copyAssetTree("runtime_resources", target)
        stamp.writeText(expected)
        Log.i(TAG, "Unpacked runtime resources into ${target.absolutePath}")
        return target
    }

    private fun copyAssetTree(assetPath: String, destination: File) {
        val entries = assets.list(assetPath) ?: emptyArray()
        if (entries.isEmpty()) {
            // A leaf: copy the file.
            assets.open(assetPath).use { input ->
                FileOutputStream(destination).use { output -> input.copyTo(output) }
            }
            return
        }
        destination.mkdirs()
        for (entry in entries) {
            copyAssetTree("$assetPath/$entry", File(destination, entry))
        }
    }

    private companion object {
        const val TAG = "WiiCompiledQuest"
    }
}
