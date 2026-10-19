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
 *
 * Retro Rewind's code is translated from its pack's Code.pul, so a game whose game.json names a
 * different Code.pul than the pack now holds is PackChanged: it still loads, but the new pack may
 * expect code it does not have, and Retro WFC is likely to refuse it online. Games built before
 * game.json recorded the Code.pul cannot be checked and count as Ready.
 */
object GameLibrary {

    enum class Status { Missing, Stale, PackChanged, Ready }

    const val LIBRARY_NAME = "libmain.so"
    const val MANIFEST_NAME = "game.json"
    /** game.json's SHA-256 of the Code.pul a Retro Rewind game was translated from. */
    const val MOD_CODE_PUL_KEY = "modCodePulSha256"

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
        if (manifest.optString("kitFingerprint") != kitFingerprint(context) ||
            manifest.optString("androidCpu") != BuildConfig.ANDROID_CPU
        ) return Status.Stale
        if (profile.modPack &&
            packChanged(manifest.optString(MOD_CODE_PUL_KEY), GameStorage.modCodePulSha256(context))
        ) return Status.PackChanged
        return Status.Ready
    }

    /** Whether the game's library can be loaded at all: a changed pack still runs, another kit does not. */
    fun loadable(status: Status): Boolean = status == Status.Ready || status == Status.PackChanged

    /**
     * Whether a game translated from the Code.pul hashed [recorded] mismatches the pack's [current]
     * one. Unknown on either side (an older game.json, no pack) is not a mismatch.
     */
    fun packChanged(recorded: String?, current: String?): Boolean =
        !recorded.isNullOrBlank() && !current.isNullOrBlank() && !recorded.equals(current, ignoreCase = true)

    /** Whether game.json records which Code.pul the game was translated from. */
    fun recordsCodePul(manifest: JSONObject?): Boolean = !manifest?.optString(MOD_CODE_PUL_KEY).isNullOrBlank()
}
