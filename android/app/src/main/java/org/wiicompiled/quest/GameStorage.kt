package org.wiicompiled.quest

import android.content.Context
import java.io.File
import org.wiicompiled.quest.launcher.TomlConfig

/**
 * Where the game's user files live on the headset. The launcher and the game
 * activity run in different processes and both resolve every path here, so
 * they cannot disagree about which Config.toml or DATA directory is meant.
 *
 * Everything sits under Android/data/<package>/files/WiiCompiledOpenXRVR, the
 * one directory a player can reach over adb: the extracted disc (DATA),
 * Config.toml, NAND saves and per-run logs.
 */
object GameStorage {

    // Must match kApplicationDirectoryName in runtime/include/runtime_config.h.
    const val APP_DIRECTORY = "WiiCompiledOpenXRVR"
    const val DISC_DIRECTORY = "DATA"
    /** The Retro Rewind pack the modded product reads (`[paths] retro_rewind_root`). */
    const val MOD_DIRECTORY = "RetroRewind6"
    /** `[vr] render_scale` until the player changes it; must match kVrRenderScaleDefault on Android. */
    const val DEFAULT_RENDER_SCALE = 0.8

    enum class DiscStatus { Missing, Incomplete, Ready }

    fun dataRoot(context: Context): File = context.getExternalFilesDir(null) ?: context.filesDir

    fun gameRoot(context: Context): File = File(dataRoot(context), APP_DIRECTORY)

    fun discDirectory(context: Context): File = File(gameRoot(context), DISC_DIRECTORY)

    fun modDirectory(context: Context): File = File(gameRoot(context), MOD_DIRECTORY)

    /** Mods imported on the Patches page, one folder each, as WheelWizard keeps them on a computer. */
    const val MODS_DIRECTORY = "Mods"

    /** The pack's own Code.pul, which is what a modded game and a mod translation both need. */
    fun modCodePul(context: Context): File = File(modDirectory(context), "Binaries/Code.pul")

    /** Whether the pack a profile needs is there. Always true for the unmodded game. */
    fun modContentReady(context: Context, profile: GameProfile): Boolean =
        !profile.modPack || modCodePul(context).isFile

    fun modsDirectory(context: Context): File = File(gameRoot(context), MODS_DIRECTORY)

    /** The pack's Patches folder, which its Riivolution XML maps onto the disc; refilled from Mods at each start. */
    fun patchesDirectory(context: Context): File = File(modDirectory(context), "Patches")

    fun configFile(context: Context): File = File(gameRoot(context), "Config.toml")

    /**
     * The runtime's NAND (ManagedNandRootPath in runtime/include/nand_path.h): `[paths] nand_root`
     * when Config.toml sets one, relative to the config's folder, otherwise NAND beside it.
     */
    fun nandDirectory(context: Context): File {
        val configured = runCatching { TomlConfig.parse(configFile(context).readText()).string("paths", "nand_root") }.getOrNull()
        if (configured.isNullOrBlank()) return File(gameRoot(context), "NAND")
        val path = File(configured)
        return if (path.isAbsolute) path else File(gameRoot(context), configured)
    }

    /**
     * Retro Rewind's save: the pack's Riivolution XML sends the game's saves to
     * riivolution/save/RetroWFC/<game ID><region> beside the pack, and the Quest only runs PAL.
     */
    fun retroRewindSave(context: Context): File = File(gameRoot(context), "riivolution/save/RetroWFC/RMCP/rksys.dat")

    /** Pulsar's record of the VR and BR Retro Rewind plays with, per profile. */
    fun retroRewindRatings(context: Context): File = File(nandDirectory(context), "shared2/Pulsar/RetroRewind6/RRRating.pul")

    fun logsDirectory(context: Context): File = File(gameRoot(context), "Logs")

    /** Game packages dropped here (adb push, Build-QuestGame.ps1 -Install) are imported when the launcher opens. */
    fun importDirectory(context: Context): File = File(gameRoot(context), "Import")

    /**
     * Whether DATA holds what the runtime's DVD layer accepts (IsDvdDataRoot in
     * runtime/src/hle/storage/dvd.cpp): the whole extracted partition, not just
     * its files/ folder. Incomplete also covers a tree adb left unreadable to
     * the app, which looks the same from here.
     */
    fun discStatus(context: Context): DiscStatus {
        val disc = discDirectory(context)
        if (!disc.isDirectory) {
            return DiscStatus.Missing
        }
        val usable = File(disc, "files").isDirectory && File(disc, "sys/fst.bin").isFile
        return if (usable) DiscStatus.Ready else DiscStatus.Incomplete
    }

    /**
     * Creates the game directory and a first Config.toml with VR on and the
     * disc path filled in. An existing file keeps every setting it has, and only
     * gains the Retro Rewind pack path when it predates it: the launcher, the
     * in-headset settings panel and the player all edit it in place.
     */
    fun prepare(context: Context): File {
        val gameRoot = gameRoot(context)
        gameRoot.mkdirs()
        // Created by the app so it owns it and can remove imported packages; adb only adds files.
        importDirectory(context).mkdirs()
        val config = configFile(context)
        if (!config.isFile) {
            // Written line by line: trimIndent runs after interpolation, so an interpolated line
            // would take the indent off every other one.
            val lines = mutableListOf(
                "# WiiCompiled Quest configuration. Edit with the launcher or the in-game panel. After an adb push, run chmod 664 on it or the app can no longer save settings.",
                "[paths]",
                "dvd_root = \"${discDirectory(context).absolutePath}\"",
            )
            // The modded product reads its pack from retro_rewind_root, which sits next to DATA
            // here. Harmless for the unmodded game, and one config serves both.
            lines += "retro_rewind_root = \"${modDirectory(context).absolutePath}\""
            lines += listOf(
                "",
                "[video]",
                "widescreen = true",
                "resolution_multiplier = 1.0",
                "",
                "[vr]",
                "enabled = true",
                "render_scale = $DEFAULT_RENDER_SCALE",
            )
            config.writeText(lines.joinToString("\n", postfix = "\n"))
        } else {
            // A config written before this app offered Retro Rewind names no pack, and the modded
            // game would find none. Only that one line is added; the rest is the player's.
            val toml = TomlConfig.parse(config.readText())
            if (toml.string("paths", "retro_rewind_root").isNullOrBlank()) {
                toml.setString("paths", "retro_rewind_root", modDirectory(context).absolutePath)
                config.writeText(toml.text())
            }
        }
        return gameRoot
    }
}
