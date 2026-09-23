package org.wiicompiled.quest.launcher

import android.app.Activity
import android.content.res.ColorStateList
import android.graphics.drawable.Drawable
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import kotlin.math.roundToInt
import org.wiicompiled.quest.BuildConfig
import org.wiicompiled.quest.GameLibrary
import org.wiicompiled.quest.GameProfile
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.R

/**
 * The launcher's Settings page: tabs of setting rows, each bound to one or two
 * Config.toml keys.
 *
 * Every row reads its value the way runtime/include/runtime_config.h parses it,
 * with the same default and the same accepted range, so what is shown is what
 * the game will use. Nothing is written until the player changes a row, and
 * each change is one line edit of a fresh read of the file.
 */
class SettingsPage(
    private val activity: Activity,
    root: View,
    private val store: ConfigStore,
    private val gameRunning: () -> Boolean,
    private val selectDiscImage: () -> Unit,
    private val importGame: () -> Unit,
    private val buildGame: () -> Unit,
    private val downloadModPack: () -> Unit,
    private val resetInstallation: () -> Unit,
) {

    enum class Tab(val label: Int) {
        Vr(R.string.settings_tab_vr),
        Graphics(R.string.settings_tab_graphics),
        Controls(R.string.settings_tab_controls),
        Audio(R.string.settings_tab_audio),
        Other(R.string.settings_tab_other),
        About(R.string.settings_tab_about),
    }

    private class Dependent(val row: View, val control: View, val enabledIf: (TomlConfig) -> Boolean)

    private val tabStrip: LinearLayout = root.findViewById(R.id.settings_tabs)
    private val rows: LinearLayout = root.findViewById(R.id.settings_rows)
    private val scroll: ScrollView = root.findViewById(R.id.settings_scroll)
    private val dependents = mutableListOf<Dependent>()
    private val tabViews = mutableMapOf<Tab, TextView>()

    /** The file as read for the rows on screen. */
    private var current = TomlConfig.parse("")

    var tab: Tab = Tab.Vr
        private set

    init {
        for (entry in Tab.entries) {
            val view = TextView(activity, null, 0, R.style.Launcher_Tab).apply {
                setText(entry.label)
                setOnClickListener { select(entry) }
            }
            tabStrip.addView(view, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44)))
            tabViews[entry] = view
        }
        tabViews.getValue(tab).isSelected = true
    }

    fun select(entry: Tab) {
        if (entry != tab) {
            tabViews.getValue(tab).isSelected = false
            tab = entry
            tabViews.getValue(tab).isSelected = true
            scroll.scrollTo(0, 0)
        }
        refresh()
    }

    /** Rebuilds the open tab from the file, which the headset panel may have changed. */
    fun refresh() {
        current = store.load() ?: TomlConfig.parse("")
        rows.removeAllViews()
        dependents.clear()
        // About changes nothing, so the note about when changes take effect has no place there.
        if (tab != Tab.About) {
            if (gameRunning()) {
                banner(R.drawable.bg_banner_warning, R.string.settings_running_note)
            } else {
                banner(R.drawable.bg_banner_info, R.string.settings_apply_note)
            }
        }
        when (tab) {
            Tab.Vr -> buildVr()
            Tab.Graphics -> buildGraphics()
            Tab.Controls -> buildControls()
            Tab.Audio -> buildAudio()
            Tab.Other -> buildOther()
            Tab.About -> buildAbout()
        }
        updateDependents(current)
    }

    private fun buildVr() {
        // Flat Screen mode keeps races on the menu screen, which none of the race view rows reach.
        val immersive = { c: TomlConfig -> !(c.bool("vr", "flat_screen") ?: false) }
        val firstPerson = { c: TomlConfig -> immersive(c) && (c.bool("vr", "first_person") ?: false) }
        // The steering wheel and hand steering belong to the cockpit seat.
        val cockpit = { c: TomlConfig -> firstPerson(c) && stringIndex(c, "vr", "first_person_seat", SEATS) == 0 }
        section(R.string.section_vr_camera) {
            toggle(
                R.string.vr_flat_screen, R.string.vr_flat_screen_helper,
                read = { !immersive(it) },
                write = { c, value -> c.setBool("vr", "flat_screen", value) },
            )
            choice(
                R.string.vr_camera, R.string.vr_camera_helper,
                listOf(R.string.vr_camera_chase, R.string.vr_camera_first_person),
                read = { if (it.bool("vr", "first_person") == true) 1 else 0 },
                write = { c, index -> c.setBool("vr", "first_person", index == 1) },
                enabledIf = immersive,
            )
            choice(
                R.string.vr_rotation, R.string.vr_rotation_helper,
                listOf(R.string.vr_rotation_yaw, R.string.vr_rotation_yaw_pitch, R.string.vr_rotation_full),
                read = { stringIndex(it, "vr", "first_person_rotation", ROTATIONS, ROTATION_DEFAULT) },
                write = { c, index -> c.setString("vr", "first_person_rotation", ROTATIONS[index]) },
                enabledIf = firstPerson,
            )
            // One setting in two keys, presented as the in-headset panel's two
            // tick boxes are: nothing, the driver (model 0), or every model.
            choice(
                R.string.vr_hide, R.string.vr_hide_helper,
                listOf(R.string.vr_hide_nothing, R.string.vr_hide_driver, R.string.vr_hide_driver_and_kart),
                read = {
                    val hide = it.bool("vr", "first_person_hide_driver") ?: true
                    val model = it.integer("vr", "first_person_hidden_model")?.takeIf { m -> m in -1L..31L } ?: 0L
                    when {
                        !hide -> 0
                        model < 0 -> 2
                        else -> 1
                    }
                },
                write = { c, index ->
                    c.setBool("vr", "first_person_hide_driver", index != 0)
                    if (index != 0) {
                        c.setInteger("vr", "first_person_hidden_model", if (index == 1) 0L else -1L)
                    }
                },
                enabledIf = firstPerson,
            )
            choice(
                R.string.vr_seat, R.string.vr_seat_helper,
                listOf(R.string.vr_seat_cockpit, R.string.vr_seat_custom),
                read = { stringIndex(it, "vr", "first_person_seat", SEATS) },
                write = { c, index -> c.setString("vr", "first_person_seat", SEATS[index]) },
                enabledIf = firstPerson,
            )
            // heurazy's grab-and-turn wheel: runtime_config.h's kVrHandSteeringDefault is on.
            toggle(
                R.string.vr_hand_steering, R.string.vr_hand_steering_helper,
                read = { it.bool("vr", "hand_steering") ?: true },
                write = { c, value -> c.setBool("vr", "hand_steering", value) },
                enabledIf = cockpit,
            )
            slider(
                R.string.vr_lean_back, R.string.vr_lean_back_helper, -45.0, 45.0, 1.0,
                read = { number(it, "vr", "lean_back_degrees", -45.0, 45.0, 0.0) },
                format = { "%.0f°".format(it) },
                write = { c, value -> c.setFloat("vr", "lean_back_degrees", value) },
                enabledIf = immersive,
            )
        }
        section(R.string.section_vr_headset) {
            slider(
                R.string.vr_render_scale, R.string.vr_render_scale_helper, 0.25, 2.0, 0.05,
                read = { number(it, "vr", "render_scale", 0.25, 2.0, GameStorage.DEFAULT_RENDER_SCALE) },
                format = { "%.2f×".format(it) },
                write = { c, value -> c.setFloat("vr", "render_scale", value) },
            )
            choice(
                R.string.vr_performance_level, R.string.vr_performance_level_helper,
                listOf(
                    R.string.vr_performance_level_boost, R.string.vr_performance_level_sustained_high,
                    R.string.vr_performance_level_sustained_low, R.string.vr_performance_level_power_savings,
                    R.string.vr_performance_level_default,
                ),
                read = { stringIndex(it, "vr", "performance_level", PERFORMANCE_LEVELS) },
                write = { c, index -> c.setString("vr", "performance_level", PERFORMANCE_LEVELS[index]) },
            )
            choice(
                R.string.vr_interpolation, R.string.vr_interpolation_helper,
                listOf(activity.getString(R.string.vr_interpolation_off), activity.getString(R.string.vr_interpolation_auto), "72 FPS", "90 FPS", "120 FPS"),
                read = { INTERPOLATION_FPS.indexOf(vrInterpolationFps(it)) },
                write = { c, index -> c.setInteger("vr", "frame_interpolation_fps", INTERPOLATION_FPS[index]) },
            )
        }
        section(R.string.section_vr_screen) {
            toggle(
                R.string.vr_hud_screen, R.string.vr_hud_screen_helper,
                read = { it.bool("vr", "hud_virtual_screen") ?: true },
                write = { c, value -> c.setBool("vr", "hud_virtual_screen", value) },
                enabledIf = immersive,
            )
            slider(
                R.string.vr_hud_distance, R.string.vr_hud_distance_helper, 0.5, 5.0, 0.1,
                read = { number(it, "vr", "hud_distance_meters", 0.25, 10.0, 2.0) },
                format = { "%.1f m".format(it) },
                write = { c, value -> c.setFloat("vr", "hud_distance_meters", value) },
            )
            slider(
                R.string.vr_hud_width, R.string.vr_hud_width_helper, 1.0, 6.0, 0.1,
                read = { number(it, "vr", "hud_width_meters", 0.25, 20.0, 2.4) },
                format = { "%.1f m".format(it) },
                write = { c, value -> c.setFloat("vr", "hud_width_meters", value) },
            )
            toggle(
                R.string.vr_passthrough, R.string.vr_passthrough_helper,
                read = { it.bool("vr", "passthrough") ?: true },
                write = { c, value -> c.setBool("vr", "passthrough", value) },
            )
        }
    }

    private fun buildGraphics() {
        section(R.string.section_graphics) {
            choice(
                R.string.graphics_resolution, R.string.graphics_resolution_helper,
                RESOLUTIONS.map { if (it == 1.0) activity.getString(R.string.graphics_resolution_native) else "${TomlConfig.formatFloat(it).removeSuffix(".0")}x" },
                read = { RESOLUTIONS.indexOf(resolution(it)) },
                write = { c, index -> c.setFloat("video", "resolution_multiplier", RESOLUTIONS[index]) },
                custom = { if (resolution(it) == 0.0) "Auto" else "${TomlConfig.formatFloat(resolution(it)).removeSuffix(".0")}x" },
            )
            toggle(
                R.string.graphics_widescreen, R.string.graphics_widescreen_helper,
                read = { it.bool("video", "widescreen") ?: true },
                write = { c, value -> c.setBool("video", "widescreen", value) },
            )
            toggle(
                R.string.graphics_bloom, R.string.graphics_bloom_helper,
                read = { (disabledPostProcessing(it) and BLOOM_PATH) != 0L },
                write = { c, value -> c.setInteger("video", "disabled_post_processing_paths", if (value) BLOOM_PATH else 0L) },
            )
            toggle(
                R.string.graphics_skip_unready, R.string.graphics_skip_unready_helper,
                read = { it.bool("video", "skip_unready_pipelines") ?: true },
                write = { c, value -> c.setBool("video", "skip_unready_pipelines", value) },
            )
            toggle(
                R.string.graphics_gx_thread, R.string.graphics_gx_thread_helper,
                read = { it.bool("video", "gx_thread") ?: true },
                write = { c, value -> c.setBool("video", "gx_thread", value) },
            )
        }
    }

    private fun buildControls() {
        section(R.string.section_controls) {
            choice(
                R.string.controls_mode, R.string.controls_mode_helper,
                listOf(R.string.controls_mode_wii_remote, R.string.controls_mode_gamepad),
                read = { stringIndex(it, "vr", "controller_mode", CONTROLLER_MODES) },
                write = { c, index -> c.setString("vr", "controller_mode", CONTROLLER_MODES[index]) },
            )
            toggle(
                R.string.controls_rumble, R.string.controls_rumble_helper,
                read = { it.bool("controller", "rumble") ?: true },
                write = { c, value -> c.setBool("controller", "rumble", value) },
            )
        }
        section(R.string.section_controls_mapping) {
            info(R.string.controls_map_a, activity.getString(R.string.controls_map_a_value))
            info(R.string.controls_map_b, activity.getString(R.string.controls_map_b_value))
            info(R.string.controls_map_12, activity.getString(R.string.controls_map_12_value))
            info(R.string.controls_map_minus_plus, activity.getString(R.string.controls_map_minus_plus_value))
            info(R.string.controls_map_stick, activity.getString(R.string.controls_map_stick_value))
            info(R.string.controls_map_z, activity.getString(R.string.controls_map_z_value))
            info(R.string.controls_map_c, activity.getString(R.string.controls_map_c_value))
            info(R.string.controls_map_pointer, activity.getString(R.string.controls_map_pointer_value))
            info(R.string.controls_map_panel, activity.getString(R.string.controls_map_panel_value))
        }
    }

    private fun buildAudio() {
        section(R.string.section_audio) {
            for ((label, key) in VOLUMES) {
                slider(
                    label, null, 0.0, 100.0, 1.0,
                    read = { number(it, "audio", key, 0.0, 1.0, 1.0) * 100.0 },
                    format = { "%.0f%%".format(it) },
                    write = { c, value -> c.setFloat("audio", key, value / 100.0) },
                )
            }
            toggle(
                R.string.audio_muted, R.string.audio_muted_helper,
                read = { it.bool("audio", "muted") ?: false },
                write = { c, value -> c.setBool("audio", "muted", value) },
            )
        }
    }

    /** What the player's own files are and what makes them, as the PC launcher's Other tab does. */
    private fun buildOther() {
        section(R.string.section_about_storage) {
            val disc = GameStorage.discDirectory(activity).absolutePath
            val status = when (GameStorage.discStatus(activity)) {
                GameStorage.DiscStatus.Ready -> R.string.about_game_data_ready
                GameStorage.DiscStatus.Incomplete -> R.string.about_game_data_incomplete
                GameStorage.DiscStatus.Missing -> R.string.about_game_data_missing
            }
            info(R.string.about_game_data, activity.getString(status, disc), stacked = true)
            // Everything below replaces files the game reads, so nothing starts while it runs.
            val idle = !GameSetup.isRunning && !gameRunning()
            action(R.string.about_extract, R.string.about_extract_helper, R.string.home_select_disc, enabled = idle) {
                selectDiscImage()
            }
            info(R.string.about_disc_md5, activity.getString(R.string.disc_md5), stacked = true)
            // One row per game this app carries a kit for, so both are visible at once.
            for (profile in GameProfile.available(activity)) {
                val manifest = GameLibrary.manifest(activity, profile)
                val gameText = when (GameLibrary.status(activity, profile)) {
                    GameLibrary.Status.Ready -> manifest?.let {
                        activity.getString(R.string.about_game_ready, it.optString("builtBy"), it.optString("builtAt"))
                    } ?: activity.getString(R.string.about_game_missing)
                    GameLibrary.Status.Stale -> activity.getString(R.string.about_game_stale)
                    GameLibrary.Status.Missing -> activity.getString(R.string.about_game_missing)
                }
                info(profile.title, gameText, stacked = true)
            }
            if (BuildConfig.ON_DEVICE_BUILD) {
                action(
                    R.string.home_build, R.string.about_build_helper, R.string.home_build,
                    enabled = idle && GameStorage.discStatus(activity) == GameStorage.DiscStatus.Ready,
                ) {
                    buildGame()
                }
            }
            action(R.string.home_import, R.string.about_import_helper, R.string.home_import, enabled = idle) {
                importGame()
            }
            action(R.string.about_reset, R.string.about_reset_helper, R.string.home_reset, enabled = idle) {
                resetInstallation()
            }
            // Retro Rewind's pack is its own download, and the row says which version is installed.
            if (GameProfile.RetroRewind in GameProfile.available(activity)) {
                val installed = RetroRewindPack.installedVersion(activity)
                info(
                    R.string.about_mod_pack,
                    installed?.let { activity.getString(R.string.about_mod_pack_version, it) }
                        ?: activity.getString(R.string.about_mod_pack_missing),
                    stacked = true,
                )
                action(
                    R.string.about_mod_pack_action,
                    R.string.about_mod_pack_helper,
                    if (installed == null) R.string.home_download_mod_pack else R.string.about_mod_pack_update,
                    enabled = idle,
                ) {
                    downloadModPack()
                }
            }
            info(R.string.about_config, store.file.absolutePath, stacked = true)
            info(R.string.about_logs, GameStorage.logsDirectory(activity).absolutePath, stacked = true)
        }
        section(R.string.section_about_diagnostics) {
            toggle(
                R.string.about_openxr_logging, R.string.about_openxr_logging_helper,
                read = { it.bool("diagnostics", "openxr_logging") ?: false },
                write = { c, value -> c.setBool("diagnostics", "openxr_logging", value) },
            )
        }
    }

    /** Who made what, and what this app is built on. */
    private fun buildAbout() {
        section(R.string.section_about_this_app) {
            info(R.string.about_app_name, activity.getString(R.string.about_app_summary), stacked = true)
            info(R.string.about_version, BuildConfig.VERSION_NAME)
            // The fingerprint ties a built game to this APK, so it belongs where a player can read it out.
            GameLibrary.kitFingerprint(activity)?.let { info(R.string.about_kit, it, stacked = true) }
        }
        section(R.string.section_about_credits) {
            info(R.string.about_credit_title_vr, activity.getString(R.string.about_credit_vr), stacked = true)
            info(R.string.about_credit_title_hand_steering, activity.getString(R.string.about_credit_hand_steering), stacked = true)
            info(R.string.about_credit_title_wiicompiled, activity.getString(R.string.about_credit_wiicompiled), stacked = true)
            info(R.string.about_credit_title_retro_rewind, activity.getString(R.string.about_credit_retro_rewind), stacked = true)
            info(R.string.about_credit_title_wheel_wizard, activity.getString(R.string.about_credit_wheel_wizard), stacked = true)
            info(R.string.about_credit_title_graphics, activity.getString(R.string.about_credit_aurora), stacked = true)
            info(R.string.about_credit_title_platform, activity.getString(R.string.about_credit_openxr), stacked = true)
            info(R.string.about_credit_title_disc, activity.getString(R.string.about_credit_nod), stacked = true)
            info(R.string.about_credit_title_mod_browser, activity.getString(R.string.about_credit_mod_browser), stacked = true)
            if (BuildConfig.ON_DEVICE_BUILD) {
                info(R.string.about_credit_title_toolchain, activity.getString(R.string.about_credit_toolchain), stacked = true)
            }
            info(R.string.about_credit_title_dolphin, activity.getString(R.string.about_credit_dolphin), stacked = true)
            info(R.string.about_credit_title_logo, activity.getString(R.string.about_credit_logo), stacked = true)
        }
        section(R.string.section_about_licence) {
            info(R.string.about_licence_title, activity.getString(R.string.about_licence_text), stacked = true)
        }
    }

    // Row construction

    private inner class Section {
        private val config = current
        val views = mutableListOf<View>()

        fun toggle(
            title: Int,
            helper: Int?,
            read: (TomlConfig) -> Boolean,
            write: (TomlConfig, Boolean) -> Unit,
            enabledIf: ((TomlConfig) -> Boolean)? = null,
        ) {
            val control = Switch(activity).apply {
                isChecked = read(config)
                thumbTintList = checkedColors(R.color.neutral_50, R.color.neutral_300)
                trackTintList = checkedColors(R.color.primary_400, R.color.neutral_600)
                setOnCheckedChangeListener { _, checked -> commit { write(it, checked) } }
            }
            val row = row(title, helper, control)
            row.setOnClickListener { if (control.isEnabled) control.toggle() }
            add(row, control, enabledIf)
        }

        /** [read] returns the option index, or -1 when the file holds a value no option names. */
        fun choice(
            title: Int,
            helper: Int?,
            labels: List<Any>,
            read: (TomlConfig) -> Int,
            write: (TomlConfig, Int) -> Unit,
            enabledIf: ((TomlConfig) -> Boolean)? = null,
            custom: ((TomlConfig) -> String)? = null,
        ) {
            val names = labels.map { if (it is Int) activity.getString(it) else it.toString() }.toMutableList()
            var shown = read(config)
            if (shown < 0) {
                // A value only a hand edit can set: show it rather than pretend
                // it is one of the options, and keep it until the player picks one.
                names += activity.getString(R.string.settings_custom_value, custom?.invoke(config) ?: "?")
                shown = names.size - 1
            }
            val spinner = Spinner(activity, Spinner.MODE_DROPDOWN).apply {
                background = drawable(R.drawable.bg_dropdown)
                setPopupBackgroundDrawable(drawable(R.drawable.bg_dropdown_popup))
                dropDownVerticalOffset = dp(4)
                adapter = ArrayAdapter(activity, R.layout.item_dropdown, names).apply {
                    setDropDownViewResource(R.layout.item_dropdown_popup)
                }
                setSelection(shown, false)
                onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                    override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                        if (position == shown || position >= labels.size) {
                            return
                        }
                        shown = position
                        commit { write(it, position) }
                    }

                    override fun onNothingSelected(parent: AdapterView<*>?) = Unit
                }
                layoutParams = LinearLayout.LayoutParams(dp(210), LinearLayout.LayoutParams.WRAP_CONTENT)
            }
            add(row(title, helper, spinner), spinner, enabledIf)
        }

        fun slider(
            title: Int,
            helper: Int?,
            min: Double,
            max: Double,
            step: Double,
            read: (TomlConfig) -> Double,
            format: (Double) -> String,
            write: (TomlConfig, Double) -> Unit,
            enabledIf: ((TomlConfig) -> Boolean)? = null,
        ) {
            val initial = read(config)
            var committed = initial
            // A value the runtime accepts but the slider does not reach (a hand edit) widens the
            // slider to it, so the bar shows the real value and a nudge changes it by one step.
            val low = minOf(min, initial)
            val high = maxOf(max, initial)
            val valueText = TextView(activity).apply {
                text = format(initial)
                setTextColor(activity.getColor(R.color.neutral_100))
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
                gravity = Gravity.END or Gravity.CENTER_VERTICAL
                layoutParams = LinearLayout.LayoutParams(dp(64), LinearLayout.LayoutParams.WRAP_CONTENT)
            }
            val steps = ((high - low) / step).roundToInt()
            val seekBar = SeekBar(activity).apply {
                this.max = steps
                progress = ((initial.coerceIn(low, high) - low) / step).roundToInt()
                progressTintList = ColorStateList.valueOf(activity.getColor(R.color.primary_400))
                thumbTintList = ColorStateList.valueOf(activity.getColor(R.color.primary_300))
                progressBackgroundTintList = ColorStateList.valueOf(activity.getColor(R.color.neutral_600))
                layoutParams = LinearLayout.LayoutParams(dp(210), LinearLayout.LayoutParams.WRAP_CONTENT)
            }
            // Rounded so 0.25 + 15 * 0.05 is 1.0 again and an untouched value is not rewritten.
            fun valueAt(progress: Int) = (low + progress * step).let { Math.round(it * 1e6) / 1e6 }
            fun commitValue(value: Double) {
                if (value != committed) {
                    committed = value
                    commit { write(it, value) }
                }
            }
            seekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                private var tracking = false

                override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
                    if (!fromUser) return
                    valueText.text = format(valueAt(progress))
                    // Dragging writes once, on release; a key or controller step writes at once.
                    if (!tracking) commitValue(valueAt(progress))
                }

                override fun onStartTrackingTouch(bar: SeekBar) {
                    tracking = true
                }

                override fun onStopTrackingTouch(bar: SeekBar) {
                    tracking = false
                    commitValue(valueAt(bar.progress))
                }
            })
            val control = LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(seekBar)
                addView(valueText)
            }
            add(row(title, helper, control), seekBar, enabledIf)
        }

        fun action(title: Int, helper: Int?, button: Int, enabled: Boolean, onClick: () -> Unit) {
            val control = TextView(activity).apply {
                setText(button)
                setTextColor(activity.getColor(R.color.neutral_100))
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
                gravity = Gravity.CENTER
                background = drawable(R.drawable.bg_button_secondary)
                setPadding(dp(16), 0, dp(16), 0)
                minHeight = dp(40)
                isEnabled = enabled
                alpha = if (enabled) 1f else 0.45f
                setOnClickListener { onClick() }
            }
            add(row(title, helper, control), null, null)
        }

        /** A read-only line; [stacked] puts a long value such as a path under its title. */
        fun info(title: Int, value: String, stacked: Boolean = false) {
            val row: LinearLayout
            if (stacked) {
                row = row(title, null, null)
                val texts = row.getChildAt(0) as LinearLayout
                texts.addView(helperText(value, selectable = true))
            } else {
                val valueText = TextView(activity).apply {
                    text = value
                    setTextColor(activity.getColor(R.color.neutral_300))
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
                }
                row = row(title, null, valueText)
                row.minimumHeight = dp(44)
            }
            add(row, null, null)
        }

        private fun add(row: View, control: View?, enabledIf: ((TomlConfig) -> Boolean)?) {
            views += row
            if (control != null && enabledIf != null) {
                dependents += Dependent(row, control, enabledIf)
            }
        }
    }

    private fun section(label: Int, build: Section.() -> Unit) {
        rows.addView(
            TextView(activity, null, 0, R.style.Launcher_SectionLabel).apply {
                setText(label)
            },
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(18)
                bottomMargin = dp(6)
            },
        )
        val section = Section().apply(build)
        section.views.forEachIndexed { index, view ->
            val last = section.views.size - 1
            view.background = drawable(
                when {
                    last == 0 -> R.drawable.bg_row_single
                    index == 0 -> R.drawable.bg_row_top
                    index == last -> R.drawable.bg_row_bottom
                    else -> R.drawable.bg_row_middle
                },
            )
            rows.addView(
                view,
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    if (index > 0) topMargin = dp(3)
                },
            )
        }
    }

    private fun row(title: Int, helper: Int?, control: View?): LinearLayout {
        val texts = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            addView(
                TextView(activity).apply {
                    setText(title)
                    setTextColor(activity.getColor(R.color.neutral_100))
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
                },
            )
            if (helper != null) {
                addView(helperText(activity.getString(helper), selectable = false))
            }
        }
        return LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = dp(58)
            setPadding(dp(16), dp(10), dp(14), dp(10))
            addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply { marginEnd = dp(16) })
            if (control != null) {
                addView(control)
            }
        }
    }

    private fun helperText(text: String, selectable: Boolean) = TextView(activity).apply {
        this.text = text
        setTextColor(activity.getColor(R.color.neutral_400))
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f)
        setTextIsSelectable(selectable)
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(2)
        }
    }

    private fun banner(background: Int, text: Int) {
        val warning = background == R.drawable.bg_banner_warning
        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            this.background = drawable(background)
            setPadding(dp(12), dp(10), dp(14), dp(10))
            if (warning) {
                addView(
                    ImageView(activity).apply {
                        setImageResource(R.drawable.ic_warning)
                        imageTintList = ColorStateList.valueOf(activity.getColor(R.color.warning_500))
                    },
                    LinearLayout.LayoutParams(dp(20), dp(20)).apply { marginEnd = dp(12) },
                )
            }
            addView(
                TextView(activity).apply {
                    setText(text)
                    setTextColor(activity.getColor(R.color.neutral_300))
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                },
                LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f),
            )
        }
        rows.addView(content)
    }

    private fun commit(edit: (TomlConfig) -> Unit) {
        var edited: TomlConfig? = null
        val saved = store.update {
            edit(it)
            edited = it
        }
        if (!saved) {
            Toast.makeText(activity, R.string.settings_write_failed, Toast.LENGTH_LONG).show()
        }
        edited?.let(::updateDependents)
    }

    private fun updateDependents(config: TomlConfig) {
        for (dependent in dependents) {
            val enabled = dependent.enabledIf(config)
            dependent.control.isEnabled = enabled
            dependent.row.alpha = if (enabled) 1f else 0.45f
        }
    }

    private fun checkedColors(checked: Int, unchecked: Int) = ColorStateList(
        arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
        intArrayOf(activity.getColor(checked), activity.getColor(unchecked)),
    )

    private fun drawable(id: Int): Drawable = activity.getDrawable(id)!!

    private fun dp(value: Int): Int = (value * activity.resources.displayMetrics.density).roundToInt()

    private companion object {
        val ROTATIONS = listOf("yaw", "yaw_pitch", "full")

        /** runtime_config.h's kVrFirstPersonRotationDefault. */
        val ROTATION_DEFAULT = ROTATIONS.indexOf("yaw_pitch")
        // The runtime's default ("cockpit") first.
        val SEATS = listOf("cockpit", "custom")
        // The runtime's default ("boost") first: an absent key reads as index 0.
        val PERFORMANCE_LEVELS = listOf("boost", "sustained_high", "sustained_low", "power_savings", "default")
        val CONTROLLER_MODES = listOf("wii_remote", "gamepad")
        val INTERPOLATION_FPS = listOf(0L, 1L, 72L, 90L, 120L)
        val RESOLUTIONS = listOf(1.0, 1.5, 2.0, 3.0, 4.0)
        val SUPPORTED_RESOLUTIONS = listOf(0.0, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0)
        const val BLOOM_PATH = 0x10L
        const val UINT32_MAX = 0xFFFF_FFFFL
        val VOLUMES = listOf(
            R.string.audio_master to "volume",
            R.string.audio_music to "music_volume",
            R.string.audio_sound_effects to "sound_effects_volume",
            R.string.audio_voices to "voices_volume",
            R.string.audio_ui to "ui_volume",
        )

        /** A value outside what the runtime accepts reads as the runtime's default. */
        fun number(config: TomlConfig, section: String, key: String, min: Double, max: Double, default: Double): Double =
            config.number(section, key)?.takeIf { it in min..max } ?: default

        /** Unrecognised strings fall back to the first (default) option, as in the runtime. */
        fun stringIndex(config: TomlConfig, section: String, key: String, values: List<String>,
                        default: Int = 0): Int =
            values.indexOf(config.string(section, key)).takeIf { it >= 0 } ?: default

        fun resolution(config: TomlConfig): Double =
            config.number("video", "resolution_multiplier")?.takeIf { it in SUPPORTED_RESOLUTIONS } ?: 1.0

        /** RuntimeUserConfig's disabledPostProcessingPaths: only the bloom bit is accepted. */
        fun disabledPostProcessing(config: TomlConfig): Long =
            config.integer("video", "disabled_post_processing_paths")
                ?.takeIf { it in 0L..UINT32_MAX && (it and BLOOM_PATH.inv()) == 0L } ?: BLOOM_PATH

        /** vr.frame_interpolation_fps, with the legacy frame_interpolation switch and NormalizeFrameInterpolationFps. */
        fun vrInterpolationFps(config: TomlConfig): Long {
            val value = config.integer("vr", "frame_interpolation_fps")?.takeIf { it in 0L..UINT32_MAX }
                ?: config.bool("vr", "frame_interpolation")?.let { if (it) 1L else 0L }
                ?: 0L
            return if (value in INTERPOLATION_FPS) value else 0L
        }
    }
}
