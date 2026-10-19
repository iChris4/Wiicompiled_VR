package org.wiicompiled.quest.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The launcher and the runtime edit the same Config.toml, so the line editor
 * must keep every line it does not edit, place keys where the runtime's former
 * line-based WriteSetting did, and read only what the runtime's toml11 lookups
 * accept.
 */
class TomlConfigTest {

    private val dvdRoot =
        "dvd_root = \"/storage/emulated/0/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/DATA\""

    private val sample = """
        # WiiCompiled Quest configuration.
        [paths]
        dvd_root = "/storage/emulated/0/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/DATA"

        [video]
        widescreen = true # set by the activity
        resolution_multiplier = 1.0

        [vr]
        enabled = true
        render_scale = 1.0
    """.trimIndent() + "\n"

    @Test
    fun untouchedFileRoundTrips() {
        assertEquals(sample, TomlConfig.parse(sample).text())
        val crlf = sample.replace("\n", "\r\n")
        assertEquals(sample, TomlConfig.parse(crlf).text())
    }

    @Test
    fun replacesAKeyInPlace() {
        val config = TomlConfig.parse(sample)
        config.setFloat("vr", "render_scale", 0.75)
        assertEquals(sample.replace("render_scale = 1.0", "render_scale = 0.75"), config.text())
    }

    @Test
    fun insertsAMissingKeyAboveTheBlankLineBeforeTheNextSection() {
        val config = TomlConfig.parse(sample)
        config.setBool("video", "skip_unready_pipelines", false)
        val expected = sample.replace(
            "resolution_multiplier = 1.0\n\n[vr]",
            "resolution_multiplier = 1.0\nskip_unready_pipelines = false\n\n[vr]",
        )
        assertEquals(expected, config.text())
    }

    @Test
    fun addsTheRetroRewindPackPathToAConfigThatPredatesIt() {
        // What GameStorage.prepare does to an install made before the app offered Retro Rewind:
        // the pack path joins [paths], and nothing else moves.
        val config = TomlConfig.parse(sample)
        assertNull(config.string("paths", "retro_rewind_root"))
        val pack = "/storage/emulated/0/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/RetroRewind6"
        config.setString("paths", "retro_rewind_root", pack)
        assertEquals(
            sample.replace("$dvdRoot\n\n[video]", "$dvdRoot\nretro_rewind_root = \"$pack\"\n\n[video]"),
            config.text(),
        )
        assertEquals(pack, TomlConfig.parse(config.text()).string("paths", "retro_rewind_root"))
    }

    @Test
    fun appendsAMissingSection() {
        val config = TomlConfig.parse(sample)
        config.setFloat("audio", "volume", 0.5)
        assertEquals("$sample\n[audio]\nvolume = 0.5\n", config.text())
    }

    @Test
    fun createsAFileFromNothing() {
        val config = TomlConfig.parse("")
        config.setString("vr", "controller_mode", "gamepad")
        assertEquals("[vr]\ncontroller_mode = \"gamepad\"\n", config.text())
    }

    @Test
    fun findsKeysOnlyInTheirOwnSection() {
        val config = TomlConfig.parse("[video]\nenabled = false\n\n[vr]\nenabled = true\n")
        assertEquals(false, config.bool("video", "enabled"))
        assertEquals(true, config.bool("vr", "enabled"))
        assertNull(config.bool("audio", "enabled"))
    }

    @Test
    fun ignoresCommentsButNotHashesInsideStrings() {
        val config = TomlConfig.parse(
            "[paths] # headers may carry comments\n" +
                "dvd_root = \"/data/#1/DATA\" # a comment\n" +
                "# enabled = true\n",
        )
        assertEquals("/data/#1/DATA", config.string("paths", "dvd_root"))
        assertNull(config.literal("paths", "enabled"))
    }

    @Test
    fun readsNumbersTheWayTheRuntimeDoes() {
        val config = TomlConfig.parse(
            "[vr]\n" +
                "render_scale = 1\n" +
                "hud_width_meters = 2.4\n" +
                "lean_back_degrees = -1_0.5\n" +
                "frame_interpolation_fps = 72\n" +
                "first_person_hidden_model = 72.0\n" +
                "world_units_per_meter = 1.0f\n" +
                "hud_distance_meters = \"2.0\"\n",
        )
        // FindConfigFloat accepts integers as well as floats.
        assertEquals(1.0, config.number("vr", "render_scale")!!, 0.0)
        assertEquals(2.4, config.number("vr", "hud_width_meters")!!, 0.0)
        assertEquals(-10.5, config.number("vr", "lean_back_degrees")!!, 0.0)
        // Integer keys reject floats; anything that is not a TOML number is absent.
        assertEquals(72L, config.integer("vr", "frame_interpolation_fps"))
        assertNull(config.integer("vr", "first_person_hidden_model"))
        assertNull(config.number("vr", "world_units_per_meter"))
        assertNull(config.number("vr", "hud_distance_meters"))
    }

    @Test
    fun formatsFloatsSoTomlReadsAFloat() {
        assertEquals("1.0", TomlConfig.formatFloat(1.0))
        assertEquals("0.05", TomlConfig.formatFloat(0.05))
        assertEquals("1.0", TomlConfig.formatFloat(0.25 + 15 * 0.05))
        assertEquals("-12.0", TomlConfig.formatFloat(-12.0))
        assertEquals("0.0", TomlConfig.formatFloat(-0.0))
    }

    @Test
    fun quotesAndUnquotesStrings() {
        val value = "C:\\Games\\\"Kart\"\ttab"
        assertEquals(value, TomlConfig.unquote(TomlConfig.quote(value)))
        assertEquals("yaw", TomlConfig.unquote("'yaw'"))
        assertEquals("é", TomlConfig.unquote("\"\\u00e9\""))
        assertNull(TomlConfig.unquote("yaw"))
        assertNull(TomlConfig.unquote("\"bad\\q\""))
    }
}
