package org.wiicompiled.quest

import android.content.Context
import java.io.File
import org.json.JSONObject

/**
 * A player's compiled game: libmain.so built from their own disc against the game kit this APK
 * carries (assets/game_kit, see android/QuestGameKit.psm1), plus the game.json it came with. One
 * per [GameProfile], so the unmodded game and Retro Rewind can both be installed.
 *
 * It lives in internal private storage, the one place Android lets an app load native code it
 * did not install (execution from shared or external storage is blocked). A library built for
 * another kit, meaning another app version's runtime, is Stale and must be rebuilt: it would link
 * against runtime objects this APK no longer has.
 */
object GameLibrary {

    enum class Status { Missing, Stale, Ready }

    const val LIBRARY_NAME = "libmain.so"
    const val MANIFEST_NAME = "game.json"

    @Volatile
    private var cachedKitFingerprint: String? = null

    fun directory(context: Context, profile: GameProfile = GameProfile.selected(context)): File =
        File(context.filesDir, "game/${profile.id}")

    fun library(context: Context, profile: GameProfile = GameProfile.selected(context)): File =
        File(directory(context, profile), LIBRARY_NAME)

    fun manifest(context: Context, profile: GameProfile = GameProfile.selected(context)): JSONObject? =
        File(directory(context, profile), MANIFEST_NAME)
            .takeIf { it.isFile }
            ?.let { runCatching { JSONObject(it.readText()) }.getOrNull() }

    /** The fingerprint of the kit in this APK's assets, which every game must be built against. */
    fun kitFingerprint(context: Context): String? {
        cachedKitFingerprint?.let { return it }
        val fingerprint = runCatching {
            context.assets.open("game_kit/kit.json").use { JSONObject(it.reader().readText()).getString("fingerprint") }
        }.getOrNull()
        cachedKitFingerprint = fingerprint
        return fingerprint
    }

    fun status(context: Context, profile: GameProfile = GameProfile.selected(context)): Status {
        val manifest = manifest(context, profile)
        if (manifest == null || !library(context, profile).isFile) {
            return Status.Missing
        }
        return if (manifest.optString("kitFingerprint") == kitFingerprint(context) &&
            manifest.optString("androidCpu") == BuildConfig.ANDROID_CPU
        ) Status.Ready else Status.Stale
    }
}
