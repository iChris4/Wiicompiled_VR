package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.res.ColorStateList
import android.net.Uri
import android.os.Bundle
import android.text.format.Formatter
import android.text.method.ScrollingMovementMethod
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import org.wiicompiled.quest.BuildConfig
import org.wiicompiled.quest.GameLibrary
import org.wiicompiled.quest.GameProfile
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.QuestActivity
import org.wiicompiled.quest.R

/**
 * The app's entry point on the headset: a 2D panel modelled on the PC launcher (WheelWizard VR),
 * with a Home page that sets up and starts the game, a Patches page that imports mods for Retro
 * Rewind, and a Settings page that edits Config.toml.
 *
 * The APK carries no game code. Playing needs two things the player owns: the game files (DATA,
 * extracted from their disc image here or on a PC) and the game itself (libmain.so, built from
 * their disc on this headset, or on a PC and brought over with Import from computer). Home's main
 * button is always the next of those steps, and Play once both are there.
 *
 * The game runs as [QuestActivity], an immersive activity in its own `:game` process. SDL and the
 * runtime cannot start twice in one process, so that process ends with every game session, and
 * this panel stays behind to relaunch it.
 */
class LauncherActivity : Activity() {

    private enum class Page { Home, Patches, Settings }

    /** What Home's main and secondary buttons do. */
    private enum class Action { Play, Resume, SelectDisc, ImportGame, BuildGame, DownloadModPack, Reset }

    private lateinit var navHome: View
    private lateinit var navPatches: View
    private lateinit var navSettings: View
    private lateinit var homePage: View
    private lateinit var patchesView: View
    private lateinit var settingsView: View
    private lateinit var trails: WheelTrailsView
    private lateinit var playButton: View
    private lateinit var playIcon: ImageView
    private lateinit var playText: TextView
    private lateinit var progress: ProgressBar
    private lateinit var homeStatus: TextView
    private lateinit var secondary: TextView
    private lateinit var cancel: View
    private lateinit var dataBanner: View
    private lateinit var dataBannerIcon: ImageView
    private lateinit var dataBannerText: TextView
    private lateinit var homeTitle: TextView
    private lateinit var gameToggle: LinearLayout
    private lateinit var settings: SettingsPage
    private lateinit var patches: PatchesPage

    /** The games this APK carries a kit for, and the one the player picked. */
    private val profiles: List<GameProfile> by lazy { GameProfile.available(this) }
    private var profile = GameProfile.Base

    private var page = Page.Home
    private var launching = false
    /** Play is copying the enabled mods into Retro Rewind's Patches folder before starting it. */
    private var installingPatches = false
    private var trailsAway = true
    private var setupKind: Class<*>? = null
    private var mainAction = Action.Play
    private var secondaryAction: Action? = null

    private val setupListener: (GameSetup.State) -> Unit = { state ->
        when {
            page == Page.Home -> refreshHome()
            // Rows only change when a task starts or ends, not with every progress step.
            state.javaClass != setupKind -> settings.refresh()
        }
        setupKind = state.javaClass
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_launcher)
        try {
            GameStorage.prepare(this)
        } catch (e: Exception) {
            Log.w(TAG, "Cannot prepare the game directory", e)
        }

        navHome = findViewById(R.id.nav_home)
        navPatches = findViewById(R.id.nav_patches)
        navSettings = findViewById(R.id.nav_settings)
        homePage = findViewById(R.id.page_home)
        patchesView = findViewById(R.id.page_patches)
        settingsView = findViewById(R.id.page_settings)
        trails = findViewById(R.id.home_trails)
        playButton = findViewById(R.id.home_play)
        playIcon = findViewById(R.id.home_play_icon)
        playText = findViewById(R.id.home_play_text)
        progress = findViewById(R.id.home_progress)
        homeStatus = findViewById(R.id.home_status)
        secondary = findViewById(R.id.home_secondary)
        cancel = findViewById(R.id.home_cancel)
        dataBanner = findViewById(R.id.home_data_banner)
        dataBannerIcon = findViewById(R.id.home_data_banner_icon)
        dataBannerText = findViewById(R.id.home_data_banner_text)
        // The banner floats over the page, so a long message scrolls inside it rather than growing
        // over the buttons underneath.
        dataBannerText.movementMethod = ScrollingMovementMethod()

        homeTitle = findViewById(R.id.home_title)
        gameToggle = findViewById(R.id.home_game_toggle)
        buildGameToggle()
        findViewById<TextView>(R.id.launcher_version).text = getString(R.string.launcher_version, BuildConfig.VERSION_NAME)

        settings = SettingsPage(
            this,
            settingsView,
            ConfigStore(GameStorage.configFile(this)),
            gameRunning = ::isGameRunning,
            selectDiscImage = ::selectDiscImage,
            importGame = ::importGame,
            buildGame = ::buildGame,
            downloadModPack = ::downloadModPack,
            resetInstallation = ::resetInstallation,
        )
        patches = PatchesPage(this, patchesView) {
            openPicker(REQUEST_PATCH_FILES, multiple = true, noPicker = R.string.patches_no_picker)
        }
        savedInstanceState?.getString(KEY_TAB)?.let { name ->
            SettingsPage.Tab.entries.firstOrNull { it.name == name }?.let(settings::select)
        }

        navHome.setOnClickListener { showPage(Page.Home) }
        navPatches.setOnClickListener { showPage(Page.Patches) }
        navSettings.setOnClickListener { showPage(Page.Settings) }
        playButton.setOnClickListener { perform(mainAction) }
        secondary.setOnClickListener { secondaryAction?.let(::perform) }
        cancel.setOnClickListener { GameSetup.cancel() }

        val restored = savedInstanceState?.getString(KEY_PAGE)?.let { name -> Page.entries.firstOrNull { it.name == name } }
        showPage(restored ?: Page.Home)

        // Unattended headset tests (docs/quest-port.md): debug builds start a build from adb.
        if (BuildConfig.DEBUG && savedInstanceState == null && intent.getBooleanExtra(EXTRA_DEBUG_BUILD_GAME, false) &&
            !GameSetup.isRunning && GameStorage.discStatus(this) == GameStorage.DiscStatus.Ready
        ) {
            Log.i(TAG, "Starting a game build requested over adb")
            GameSetupService.startBuild(this, GameProfile.selected(this))
        }
    }

    override fun onResume() {
        super.onResume()
        launching = false
        // An import can install the other game and select it, so the toggle follows the file.
        profile = GameProfile.selected(this)
        importDroppedPackage()
        setupKind = GameSetup.state.javaClass
        refresh()
        GameSetup.addListener(setupListener)
        // The game's process can take a moment to go away after its activity
        // closes, which would still read as running.
        val runningAtResume = isGameRunning()
        window.decorView.postDelayed({
            if (!isFinishing && isGameRunning() != runningAtResume) refresh()
        }, PROCESS_EXIT_GRACE_MS)
    }

    override fun onPause() {
        GameSetup.removeListener(setupListener)
        super.onPause()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(KEY_PAGE, page.name)
        outState.putString(KEY_TAB, settings.tab.name)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data == null) return
        if (requestCode == REQUEST_PATCH_FILES) {
            // Several files arrive as clip data, a single one as the data URI.
            val clip = data.clipData
            val uris = if (clip != null) (0 until clip.itemCount).map { clip.getItemAt(it).uri } else listOfNotNull(data.data)
            showPage(Page.Patches)
            patches.importPicked(uris)
            return
        }
        val uri = data.data ?: return
        val task = when (requestCode) {
            REQUEST_DISC_IMAGE -> GameSetup.Task.ExtractDisc
            REQUEST_GAME_PACKAGE -> GameSetup.Task.ImportPackage
            else -> return
        }
        GameSetupService.start(this, task, uri)
        // Progress and the outcome are shown on Home, wherever the picker was opened from.
        showPage(Page.Home)
    }

    private fun showPage(target: Page) {
        page = target
        navHome.isSelected = target == Page.Home
        navPatches.isSelected = target == Page.Patches
        navSettings.isSelected = target == Page.Settings
        homePage.visibility = if (target == Page.Home) View.VISIBLE else View.GONE
        patchesView.visibility = if (target == Page.Patches) View.VISIBLE else View.GONE
        settingsView.visibility = if (target == Page.Settings) View.VISIBLE else View.GONE
        if (target == Page.Home) {
            trailsAway = true
        }
        refresh()
    }

    private fun refresh() {
        when (page) {
            Page.Home -> refreshHome()
            Page.Patches -> patches.refresh()
            Page.Settings -> settings.refresh()
        }
    }

    private fun refreshHome() {
        val disc = GameStorage.discDirectory(this).absolutePath
        val discStatus = GameStorage.discStatus(this)
        val gameStatus = GameLibrary.status(this, profile)
        homeTitle.setText(profile.title)
        for ((index, entry) in profiles.withIndex()) {
            gameToggle.getChildAt(index).isSelected = entry == profile
        }
        val running = isGameRunning()
        val setup = GameSetup.state
        val settingUp = GameSetup.isRunning
        val task = setupTask(setup)
        val importing = task == GameSetup.Task.ImportPackage
        val building = task == GameSetup.Task.BuildGame
        val downloadingPack = task == GameSetup.Task.DownloadModPack
        val resetting = task == GameSetup.Task.Reset

        // Without anything yet, a player with only a headset starts from their disc image, and
        // builds the game once its files are there; a PC-built game can always be imported instead.
        // Retro Rewind also needs its own pack, which neither building nor playing can do without.
        // Game files that are there but unusable, or an attempt to set them up that failed, lead
        // with a reset: what the app put on the headset is removed and set up again.
        val gameFilesTask = task == GameSetup.Task.ExtractDisc || task == GameSetup.Task.ImportPackage || resetting
        val filesBroken = discStatus == GameStorage.DiscStatus.Incomplete ||
            (setup is GameSetup.State.Failed && gameFilesTask && discStatus != GameStorage.DiscStatus.Ready)
        mainAction = when {
            running -> Action.Resume
            filesBroken -> Action.Reset
            discStatus != GameStorage.DiscStatus.Ready -> Action.SelectDisc
            !GameStorage.modContentReady(this, profile) -> Action.DownloadModPack
            gameStatus != GameLibrary.Status.Ready -> Action.BuildGame
            else -> Action.Play
        }
        secondaryAction = when {
            settingUp || running -> null
            mainAction == Action.Reset -> Action.SelectDisc
            setup is GameSetup.State.Failed && gameFilesTask -> Action.Reset
            mainAction == Action.ImportGame -> null
            gameStatus != GameLibrary.Status.Ready || mainAction == Action.SelectDisc -> Action.ImportGame
            else -> null
        }

        playButton.isEnabled = !settingUp
        playIcon.setImageResource(if (!settingUp && (mainAction == Action.Play || mainAction == Action.Resume)) R.drawable.ic_play else R.drawable.ic_disc)
        playText.text = when {
            setup is GameSetup.State.Checking -> getString(
                when {
                    building || resetting -> R.string.home_build_preparing
                    importing -> R.string.home_checking_package
                    else -> R.string.home_checking
                },
            )
            setup is GameSetup.State.Working -> getString(
                when {
                    building -> R.string.home_building
                    importing -> R.string.home_importing
                    downloadingPack -> R.string.home_mod_pack_downloading
                    resetting -> R.string.home_resetting
                    else -> R.string.home_extracting
                },
                percent(setup),
            )
            setup is GameSetup.State.Finishing -> getString(R.string.home_finishing)
            else -> getString(label(mainAction))
        }
        if (installingPatches) {
            playButton.isEnabled = false
            playText.setText(R.string.home_installing_patches)
        }
        secondary.visibility = if (secondaryAction != null) View.VISIBLE else View.GONE
        secondaryAction?.let { secondary.setText(label(it)) }

        progress.visibility = if (settingUp || installingPatches) View.VISIBLE else View.GONE
        progress.isIndeterminate = setup !is GameSetup.State.Working
        if (setup is GameSetup.State.Working && setup.total > 0) {
            progress.progress = (setup.done * progress.max / setup.total).toInt()
        }
        cancel.visibility = if (settingUp && setup !is GameSetup.State.Finishing) View.VISIBLE else View.GONE

        homeStatus.text = when {
            setup is GameSetup.State.Working && building -> buildStatus(setup)
            setup is GameSetup.State.Working && resetting -> getString(R.string.home_reset_progress, setup.done, setup.total)
            setup is GameSetup.State.Checking && resetting -> getString(R.string.home_reset_status)
            setup is GameSetup.State.Done && resetting -> getString(R.string.home_reset_done)
            setup is GameSetup.State.Working -> getString(
                R.string.home_extract_progress,
                Formatter.formatShortFileSize(this, setup.done),
                Formatter.formatShortFileSize(this, setup.total),
            )
            setup is GameSetup.State.Checking -> getString(
                when {
                    building -> R.string.build_step_prepare
                    importing -> R.string.home_checking_package_status
                    else -> R.string.home_checking_status
                },
            )
            settingUp || setup is GameSetup.State.Failed -> ""
            setup is GameSetup.State.Cancelled -> getString(
                when {
                    building -> R.string.home_build_cancelled
                    resetting -> R.string.home_reset_cancelled
                    else -> R.string.home_setup_cancelled
                },
            )
            running -> getString(R.string.home_running)
            mainAction == Action.Play && setup is GameSetup.State.Done -> getString(if (building) R.string.home_build_done else R.string.home_setup_done)
            mainAction == Action.Play -> getString(R.string.home_put_on_headset)
            else -> ""
        }

        when {
            settingUp && building -> showBanner(getString(R.string.home_build_running), warning = false)
            settingUp && downloadingPack -> showBanner(getString(R.string.home_mod_pack_running), warning = false)
            settingUp && resetting -> showBanner(getString(R.string.home_reset_running), warning = false)
            settingUp -> showBanner(null)
            setup is GameSetup.State.Failed && building -> showBanner(getString(R.string.home_build_failed, setup.message), warning = true)
            setup is GameSetup.State.Failed && downloadingPack -> showBanner(getString(R.string.home_mod_pack_failed, setup.message), warning = true)
            setup is GameSetup.State.Failed && resetting -> showBanner(getString(R.string.home_reset_failed, setup.message), warning = true)
            setup is GameSetup.State.Failed -> showBanner(getString(R.string.home_setup_failed, setup.message), warning = true)
            discStatus == GameStorage.DiscStatus.Incomplete -> showBanner(getString(R.string.home_data_incomplete, disc), warning = true)
            // Only once the disc files are there is the pack the next thing missing; before that a
            // first-time player is still reading how to get those.
            discStatus == GameStorage.DiscStatus.Ready && !GameStorage.modContentReady(this, profile) ->
                showBanner(getString(R.string.home_mod_needed_message), warning = true)
            gameStatus == GameLibrary.Status.Stale -> showBanner(getString(R.string.home_game_stale), warning = true)
            gameStatus == GameLibrary.Status.Missing && discStatus == GameStorage.DiscStatus.Missing ->
                showBanner(getString(R.string.home_setup_intro) + discMd5Note(), warning = false)
            gameStatus == GameLibrary.Status.Missing -> showBanner(getString(R.string.home_game_missing), warning = false)
            discStatus == GameStorage.DiscStatus.Missing ->
                showBanner(getString(R.string.home_data_missing, disc) + discMd5Note(), warning = false)
            else -> showBanner(null)
        }

        if (trailsAway && !launching) {
            trailsAway = false
            trails.enter()
        }
    }

    /**
     * Home's game switch, shown only when this APK carries more than one kit. Each game keeps its
     * own built library, so switching is immediate; only the panel's state changes.
     */
    private fun buildGameToggle() {
        for (entry in profiles) {
            val button = TextView(this).apply {
                setText(entry.title)
                textSize = 14f
                setTextColor(getColorStateList(R.color.game_toggle_text))
                setBackgroundResource(R.drawable.bg_game_toggle)
                gravity = android.view.Gravity.CENTER
                // One line: the pill has a fixed height, so a wrapped "Retro Rewind" loses half of itself.
                isSingleLine = true
                setPadding(10.dp(), 0, 10.dp(), 0)
                isClickable = true
                isFocusable = true
                layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, 38.dp())
                setOnClickListener { selectProfile(entry) }
            }
            gameToggle.addView(button)
        }
        gameToggle.visibility = if (profiles.size > 1) View.VISIBLE else View.GONE
    }

    private fun Int.dp(): Int = (this * resources.displayMetrics.density).toInt()

    private fun selectProfile(target: GameProfile) {
        if (target == profile || GameSetup.isRunning) return
        profile = target
        GameProfile.select(this, target)
        refresh()
    }

    private fun label(action: Action): Int = when (action) {
        Action.Play -> R.string.home_play
        Action.Resume -> R.string.home_resume
        Action.SelectDisc -> R.string.home_select_disc
        Action.ImportGame -> R.string.home_import
        Action.BuildGame -> R.string.home_build
        Action.DownloadModPack -> R.string.home_download_mod_pack
        Action.Reset -> R.string.home_reset
    }

    private fun perform(action: Action) {
        if (GameSetup.isRunning) return
        when (action) {
            Action.Play, Action.Resume -> play()
            Action.SelectDisc -> selectDiscImage()
            Action.ImportGame -> importGame()
            Action.BuildGame -> buildGame()
            Action.DownloadModPack -> downloadModPack()
            Action.Reset -> resetInstallation()
        }
    }

    /** The game reads its files while it runs, so nothing that replaces them starts meanwhile. */
    private fun refuseWhileGameRuns(): Boolean {
        if (!isGameRunning()) return false
        Toast.makeText(this, R.string.home_close_game_first, Toast.LENGTH_LONG).show()
        return true
    }

    /**
     * Removes what the app put on the headset so the game can be set up again from scratch: the
     * game files, and on request the built games and the Retro Rewind pack. Saves and settings are
     * never touched. The choice is a small form rather than a list dialog, which would hide the
     * explanation.
     */
    private fun resetInstallation() {
        if (GameSetup.isRunning || refuseWhileGameRuns()) return
        val form = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(20.dp(), 12.dp(), 20.dp(), 0)
        }
        form.addView(
            TextView(this).apply {
                setText(R.string.home_reset_message)
                setTextColor(getColor(R.color.neutral_300))
                textSize = 14f
            },
        )
        val choices = listOf(
            R.string.home_reset_option_game_files to true,
            R.string.home_reset_option_games to false,
            R.string.home_reset_option_mod_pack to false,
        ).map { (label, checked) ->
            CheckBox(this).apply {
                setText(label)
                setTextColor(getColor(R.color.neutral_100))
                isChecked = checked
                setPadding(8.dp(), 10.dp(), 0, 10.dp())
                form.addView(this)
            }
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.home_reset_title)
            .setView(form)
            .setPositiveButton(R.string.home_reset) { _, _ ->
                val options = InstallReset.Options(choices[0].isChecked, choices[1].isChecked, choices[2].isChecked)
                if (!options.anything || GameSetup.isRunning || refuseWhileGameRuns()) return@setPositiveButton
                GameSetupService.startReset(this, options)
                showPage(Page.Home)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /**
     * Fetches Retro Rewind's pack from Retro Rewind's own server, as the computer launcher does,
     * after saying where it comes from and how big it is.
     */
    private fun downloadModPack() {
        if (GameSetup.isRunning || refuseWhileGameRuns()) return
        val installed = RetroRewindPack.installedVersion(this)
        val message = if (installed == null) {
            getString(R.string.home_mod_pack_message)
        } else {
            getString(R.string.home_mod_pack_update_message, installed)
        }
        confirm(R.string.home_mod_pack_title, message, R.string.home_download_mod_pack) {
            GameSetupService.startModPackDownload(this)
            showPage(Page.Home)
        }
    }

    private fun buildStatus(state: GameSetup.State.Working): String = when (state.step) {
        GameBuild.Step.Download -> getString(R.string.build_step_download, state.stepDone, state.stepTotal)
        GameBuild.Step.Translate -> getString(R.string.build_step_translate, state.stepDone, state.stepTotal)
        GameBuild.Step.Compile -> getString(R.string.build_step_compile, state.stepDone, state.stepTotal)
        GameBuild.Step.Link -> getString(R.string.build_step_link)
        GameBuild.Step.Install -> getString(R.string.build_step_install)
        GameBuild.Step.Prepare, null -> getString(R.string.build_step_prepare)
    }

    /** Builds the selected game on this headset from DATA, after saying what that takes. */
    private fun buildGame() {
        if (GameSetup.isRunning || GameStorage.discStatus(this) != GameStorage.DiscStatus.Ready || refuseWhileGameRuns()) return
        if (!GameStorage.modContentReady(this, profile)) {
            confirm(R.string.home_mod_needed_title, R.string.home_mod_needed_message, R.string.home_download_mod_pack) { downloadModPack() }
            return
        }
        val message = if (GameLibrary.status(this, profile) == GameLibrary.Status.Ready) {
            R.string.home_build_replace_message
        } else {
            R.string.home_build_message
        }
        confirm(R.string.home_build_title, message, R.string.home_build) {
            GameSetupService.startBuild(this, profile)
            showPage(Page.Home)
        }
    }

    /** The clean disc's .iso hash, for the banners that ask for a disc image. */
    private fun discMd5Note(): String = "\n\n" + getString(R.string.disc_md5_note, getString(R.string.disc_md5))

    private fun showBanner(text: String?, warning: Boolean = false) {
        if (text == null) {
            dataBanner.visibility = View.GONE
            return
        }
        dataBannerText.text = text
        dataBannerText.scrollTo(0, 0)
        dataBanner.setBackgroundResource(if (warning) R.drawable.bg_banner_warning else R.drawable.bg_banner_info)
        dataBannerIcon.setImageResource(if (warning) R.drawable.ic_warning else R.drawable.ic_disc)
        dataBannerIcon.imageTintList = ColorStateList.valueOf(getColor(if (warning) R.color.warning_500 else R.color.primary_400))
        dataBanner.visibility = View.VISIBLE
    }

    /** Opens the document picker for a disc image, asking first when it would replace DATA. */
    private fun selectDiscImage() {
        if (GameSetup.isRunning || refuseWhileGameRuns()) return
        if (GameStorage.discStatus(this) == GameStorage.DiscStatus.Missing) {
            openPicker(REQUEST_DISC_IMAGE)
            return
        }
        confirm(R.string.home_replace_title, R.string.home_replace_message, R.string.home_select_disc) {
            openPicker(REQUEST_DISC_IMAGE)
        }
    }

    /** Opens the document picker for a .wcgame, asking first when it would replace the game. */
    private fun importGame() {
        if (GameSetup.isRunning || refuseWhileGameRuns()) return
        if (GameLibrary.status(this, profile) != GameLibrary.Status.Ready) {
            openPicker(REQUEST_GAME_PACKAGE)
            return
        }
        confirm(R.string.home_replace_game_title, R.string.home_replace_game_message, R.string.home_import) {
            openPicker(REQUEST_GAME_PACKAGE)
        }
    }

    private fun confirm(title: Int, message: Int, positive: Int, onConfirm: () -> Unit) =
        confirm(title, getString(message), positive, onConfirm)

    private fun confirm(title: Int, message: CharSequence, positive: Int, onConfirm: () -> Unit) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(positive) { _, _ -> onConfirm() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun openPicker(requestCode: Int, multiple: Boolean = false, noPicker: Int = R.string.home_no_picker) {
        // Disc images, game files and mod files have no reliable MIME type, so every file is
        // offered and the task checks the name and contents.
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*")
            .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple)
        try {
            @Suppress("DEPRECATION")
            startActivityForResult(intent, requestCode)
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "No document picker", e)
            Toast.makeText(this, noPicker, Toast.LENGTH_LONG).show()
        }
    }

    /**
     * Imports the newest .wcgame in the Import folder, where Build-QuestGame.ps1 -Install and adb
     * put them. Each package is tried once, whether it imports or not, until it changes: a file
     * adb pushed belongs to the shell user, so the app cannot always delete it afterwards.
     */
    private fun importDroppedPackage() {
        // Not while the game reads its files; the package is still there at the next resume.
        if (GameSetup.isRunning || isGameRunning()) return
        val dropped = GameStorage.importDirectory(this)
            .listFiles { file -> file.isFile && file.name.endsWith(".wcgame", ignoreCase = true) }
            ?.maxByOrNull { it.lastModified() }
            ?: return
        val identity = "${dropped.absolutePath}:${dropped.length()}:${dropped.lastModified()}"
        val preferences = getSharedPreferences(PREFERENCES, MODE_PRIVATE)
        if (preferences.getString(KEY_LAST_DROPPED_IMPORT, null) == identity) return
        preferences.edit().putString(KEY_LAST_DROPPED_IMPORT, identity).apply()
        Log.i(TAG, "Importing dropped game package ${dropped.absolutePath}")
        GameSetupService.start(this, GameSetup.Task.ImportPackage, Uri.fromFile(dropped), deleteSource = true)
    }

    private fun play() {
        if (BuildConfig.QUEST1_DIRECT_LAUNCH) {
            Toast.makeText(this, R.string.home_quest1_launch_from_library, Toast.LENGTH_LONG).show()
            return
        }
        if (launching) {
            return
        }
        // Resume only brings the running game back, and its files must not change under it.
        if (profile.modPack && !isGameRunning()) {
            preparePatches()
            return
        }
        startGame()
    }

    /**
     * Retro Rewind reads its pack's Patches folder, so the enabled mods are copied into it before
     * every start, as the PC launcher does (ModsLaunchService.PrepareModsForLaunch). With none
     * enabled, a folder that still holds files is only cleared if the player says so.
     */
    private fun preparePatches() {
        val mods = ModLibrary.load(GameStorage.modsDirectory(this))
        when {
            ModLibrary.shouldAskToClear(mods, GameStorage.patchesDirectory(this)) ->
                AlertDialog.Builder(this)
                    .setTitle(R.string.patches_clear_title)
                    .setMessage(R.string.patches_clear_message)
                    .setPositiveButton(R.string.patches_delete) { _, _ -> installPatches(mods, clear = true) }
                    .setNegativeButton(R.string.patches_keep) { _, _ -> startGame() }
                    .show()
            mods.any { it.enabled } -> installPatches(mods, clear = false)
            else -> startGame()
        }
    }

    private fun installPatches(mods: List<ModLibrary.Mod>, clear: Boolean) {
        if (launching) return
        launching = true
        installingPatches = true
        refreshHome()
        val modsDir = GameStorage.modsDirectory(this)
        val patchesDir = GameStorage.patchesDirectory(this)
        ModLibrary.background({ ModLibrary.prepareForLaunch(modsDir, patchesDir, mods, clear) }) { result ->
            installingPatches = false
            launching = false
            if (isDestroyed) return@background
            val error = result.getOrElse { it.message ?: it.toString() }
            if (error != null) {
                Log.w(TAG, "Mods could not be installed: $error")
                refreshHome()
                AlertDialog.Builder(this)
                    .setTitle(R.string.home_patches_failed)
                    .setMessage(error)
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
                return@background
            }
            Log.i(TAG, "Patches folder ready: ${mods.count { it.enabled }} of ${mods.size} mods enabled")
            refreshHome()
            startGame()
        }
    }

    private fun startGame() {
        if (launching) {
            return
        }
        launching = true
        trailsAway = true
        trails.leave {
            try {
                startActivity(Intent(this, QuestActivity::class.java))
            } catch (e: ActivityNotFoundException) {
                Log.e(TAG, "Cannot start the game activity", e)
                Toast.makeText(this, R.string.home_launch_failed, Toast.LENGTH_LONG).show()
                launching = false
                refreshHome()
            }
        }
    }

    private fun isGameRunning(): Boolean {
        val manager = getSystemService(ActivityManager::class.java) ?: return false
        val gameProcess = "$packageName$GAME_PROCESS_SUFFIX"
        return manager.runningAppProcesses.orEmpty().any { it.processName == gameProcess }
    }

    private fun setupTask(state: GameSetup.State): GameSetup.Task? = when (state) {
        is GameSetup.State.Checking -> state.task
        is GameSetup.State.Working -> state.task
        is GameSetup.State.Finishing -> state.task
        is GameSetup.State.Done -> state.task
        is GameSetup.State.Failed -> state.task
        is GameSetup.State.Cancelled -> state.task
        GameSetup.State.Idle -> null
    }

    private fun percent(state: GameSetup.State.Working): Int =
        if (state.total > 0) (state.done * 100 / state.total).toInt() else 0

    private companion object {
        const val TAG = "WiiCompiledLauncher"
        // Must match QuestActivity's android:process in AndroidManifest.xml.
        const val GAME_PROCESS_SUFFIX = ":game"
        const val KEY_PAGE = "page"
        const val KEY_TAB = "settingsTab"
        const val PROCESS_EXIT_GRACE_MS = 1000L
        const val REQUEST_DISC_IMAGE = 1
        const val REQUEST_GAME_PACKAGE = 2
        const val REQUEST_PATCH_FILES = 3
        const val PREFERENCES = "launcher"
        const val KEY_LAST_DROPPED_IMPORT = "lastDroppedImport"
        const val EXTRA_DEBUG_BUILD_GAME = "org.wiicompiled.quest.debug.BUILD_GAME"
    }
}
