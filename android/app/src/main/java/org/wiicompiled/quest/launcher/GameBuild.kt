package org.wiicompiled.quest.launcher

import android.app.ActivityManager
import android.content.Context
import android.os.StatFs
import android.util.Log
import java.io.File
import java.io.IOException
import java.io.InterruptedIOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import java.util.zip.ZipInputStream
import org.json.JSONArray
import org.json.JSONObject
import org.wiicompiled.quest.BuildConfig
import org.wiicompiled.quest.GameLibrary
import org.wiicompiled.quest.GameProfile
import org.wiicompiled.quest.GameStorage

/**
 * Builds the game on the headset from the player's extracted disc, the way the PC launcher's
 * Build for Quest does (android/Build-QuestGame.ps1), for players without a PC:
 *
 *  1. unpack the toolchain (assets/quest_toolchain, android/Prepare-QuestToolchain.ps1) and the
 *     game kit (assets/game_kit) into private storage;
 *  2. download the Android NDK files the build needs from Google, checked against the pins the
 *     toolchain carries (ndk.json);
 *  3. translate main.dol and StaticR.rel: translate-recursive, generate-data-init, emit-build-shards
 *     (Retro Rewind adds translate-mod, with the Retro-WFC payload downloaded from rwfc.net);
 *  4. compile the generated sources with the kit's flags, several at a time;
 *  5. link them with the kit's objects and archives (kit.json link.lld);
 *  6. install libmain.so and its game.json like an imported game.
 *
 * About half an hour on a Quest 3. Work survives a cancelled or failed run: a translation whose
 * inputs have not changed is reused and finished objects are not compiled again. A successful
 * build removes everything it unpacked and generated.
 */
object GameBuild {

    enum class Step { Prepare, Download, Translate, Compile, Link, Install }

    /** [permille] of the whole build; [done] of [total] items in [step]. False stops the build. */
    fun interface Reporter {
        fun update(permille: Int, step: Step, done: Int, total: Int): Boolean
    }

    private const val TAG = "WiiCompiledLauncher"
    private const val KIT_SCHEMA = 3
    private const val TOOLCHAIN_ASSETS = "quest_toolchain"
    private const val KIT_ASSETS = "game_kit"
    private const val MARKER = ".complete"
    private const val REQUIRED_SPACE = 2_500_000_000L
    // Translation peaks near 3.1 GB of memory with the runtime's default heap; this limit keeps it
    // near 2.1 GB for the same output, at some cost in time.
    private const val MONO_GC_PARAMS = "soft-heap-limit=1200m"
    private const val TRANSLATOR_THREADS = 4
    private const val EXPECTED_TRANSLATION_SECONDS = 600.0
    private const val COMPILE_MEMORY_BYTES = 700L * 1024 * 1024
    // WiiCompiled Setup's fixed endpoint, size cap and staging layout
    // (Launcher/WiiCompiled.Setup.Common/RetroWfcPayload.cs), which validate-retro-wfc-payload expects.
    private const val RETRO_WFC_PAYLOAD_URL = "https://rwfc.net/api/wfc/payload?g=RMCPD00"
    private const val RETRO_WFC_PAYLOAD_MAX_BYTES = 16 * 1024 * 1024
    private const val RETRO_WFC_DIRECTORY = "retro-wfc"
    private const val RETRO_WFC_PAYLOAD_FILE = "binary/payload.RMCPD00.bin"

    /** Builds [profile]'s game. Null on success, otherwise the message to show. */
    fun run(context: Context, profile: GameProfile, reporter: Reporter, cancelled: () -> Boolean, finishing: () -> Unit): String? {
        if (!BuildConfig.ON_DEVICE_BUILD) {
            return "This app cannot build its game on the headset. Build it on a computer and use Import from computer."
        }
        val disc = GameStorage.discDirectory(context)
        if (GameStorage.discStatus(context) != GameStorage.DiscStatus.Ready) {
            return "The game files (DATA) are needed to build the game. Select your disc image first."
        }
        if (!GameStorage.modContentReady(context, profile)) {
            return "Retro Rewind needs its pack before it can be built. Import a Retro Rewind game file that " +
                "carries it, or copy the RetroRewind6 folder to ${GameStorage.modDirectory(context).absolutePath}."
        }
        GameFiles.validateData(disc)?.let { return it }
        val root = File(context.filesDir, "build")
        root.mkdirs()
        val available = StatFs(root.absolutePath).availableBytes
        if (available < REQUIRED_SPACE) {
            return "Not enough free space: building needs ${GameFiles.gigabytes(REQUIRED_SPACE)}, and ${GameFiles.gigabytes(available)} is free."
        }

        val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val log = BuildLog(File(GameStorage.logsDirectory(context), "build_$stamp.log"))
        try {
            return Build(context, profile, root, disc, reporter, cancelled, finishing, log).run()
        } catch (e: InterruptedIOException) {
            throw e
        } catch (e: Exception) {
            Log.e(TAG, "Game build failed", e)
            log.line("Build failed: $e")
            return "${e.message ?: e.toString()}\n\nThe build log is ${log.file.absolutePath}"
        } finally {
            log.close()
        }
    }

    private class Build(
        val context: Context,
        val profile: GameProfile,
        val root: File,
        val disc: File,
        val reporter: Reporter,
        val cancelled: () -> Boolean,
        val finishing: () -> Unit,
        val log: BuildLog,
    ) {
        val toolchain = File(root, "toolchain")
        val ndk = File(root, "ndk")
        val kit = File(root, "kit")
        val work = File(root, profile.id)
        val workspace = File(work, "ws")
        val objects = File(work, "obj")
        val temporary = File(root, "tmp")
        val failure = AtomicReference<String?>(null)

        fun run(): String? {
            log.line("WiiCompiled Quest ${BuildConfig.VERSION_NAME} building ${profile.id} on this headset")
            report(0, Step.Prepare)
            val toolchainManifest = JSONObject(context.assets.open("$TOOLCHAIN_ASSETS/toolchain.json").reader().use { it.readText() })
            unpackToolchain(toolchainManifest)
            report(30, Step.Prepare)
            val kitFingerprint = GameLibrary.kitFingerprint(context) ?: return "This app carries no game kit."
            unpackKit(kitFingerprint)
            val kitJson = JSONObject(File(kit, "kit.json").readText())
            if (kitJson.optInt("schema") != KIT_SCHEMA) {
                return "This app's game kit is version ${kitJson.optInt("schema")}, which this builder does not know."
            }
            if (kitJson.optString("androidCpu") != BuildConfig.ANDROID_CPU) {
                return "This app contains a game kit for ${kitJson.optString("androidCpu")}, but this build requires ${BuildConfig.ANDROID_CPU}."
            }
            val recipe = kitJson.getJSONObject("products").optJSONObject(profile.id)
                ?: return "This app's game kit cannot build ${profile.id}."
            report(40, Step.Prepare)
            downloadNdk(JSONObject(context.assets.open("$TOOLCHAIN_ASSETS/${toolchainManifest.getString("ndk")}").reader().use { it.readText() }))

            temporary.mkdirs()
            val tools = ToolProcess(
                mapOf(
                    "LD_LIBRARY_PATH" to File(toolchain, "llvm/lib").absolutePath,
                    "HOME" to temporary.absolutePath,
                    "TMPDIR" to temporary.absolutePath,
                    "MONO_GC_PARAMS" to MONO_GC_PARAMS,
                ),
                log,
                cancelled = { cancelled() || failure.get() != null },
            )
            val buildIdentity = "$kitFingerprint ${toolchainManifest.getString("fingerprint")}"
            translate(tools, buildIdentity)?.let { return it }
            val objectsBySlot = compile(tools, recipe, buildIdentity)
            failure.get()?.let { return it }
            val library = link(tools, recipe, objectsBySlot) ?: return failure.get() ?: "Linking the game failed."

            finishing()
            report(990, Step.Install)
            install(library, kitFingerprint)?.let { return it }
            log.line("Installed the game built on this headset")
            // Everything above is only needed again after an app update, which changes the kit.
            if (!root.deleteRecursively()) Log.w(TAG, "Could not remove all of ${root.absolutePath}")
            return null
        }

        fun report(permille: Int, step: Step, done: Int = 0, total: Int = 0) {
            if (!reporter.update(permille, step, done, total) || cancelled()) throw InterruptedIOException("Build cancelled")
        }

        /** Unpacks files.zip, which holds exactly the files toolchain.json lists. */
        fun unpackToolchain(manifest: JSONObject) {
            val fingerprint = manifest.getString("fingerprint")
            if (File(toolchain, MARKER).takeIf { it.isFile }?.readText() == fingerprint) return
            toolchain.deleteRecursively()
            val listed = manifest.getJSONArray("files")
            val sizes = HashMap<String, Long>()
            for (i in 0 until listed.length()) {
                val file = listed.getJSONObject(i)
                sizes[file.getString("path")] = file.getLong("size")
            }
            var unpacked = 0
            ZipInputStream(context.assets.open("$TOOLCHAIN_ASSETS/${manifest.getString("archive")}").buffered(1 shl 20)).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (entry.isDirectory) continue
                    val expected = sizes[entry.name] ?: throw IOException("The app's toolchain holds an unlisted file ${entry.name}.")
                    val target = File(toolchain, entry.name)
                    target.parentFile?.mkdirs()
                    target.outputStream().use { zip.copyTo(it, 1 shl 20) }
                    if (target.length() != expected) throw IOException("The app's toolchain file ${entry.name} is damaged.")
                    unpacked++
                    report(30 * unpacked / sizes.size, Step.Prepare)
                }
            }
            if (unpacked != sizes.size) throw IOException("The app's toolchain is incomplete.")
            File(toolchain, MARKER).writeText(fingerprint)
        }

        fun unpackKit(fingerprint: String) {
            if (File(kit, MARKER).takeIf { it.isFile }?.readText() == fingerprint) return
            kit.deleteRecursively()
            copyAssetTree(KIT_ASSETS, kit)
            File(kit, MARKER).writeText(fingerprint)
        }

        fun copyAssetTree(asset: String, target: File) {
            val children = context.assets.list(asset).orEmpty()
            if (children.isEmpty()) {
                copyAsset(asset, target)
                return
            }
            for (child in children) copyAssetTree("$asset/$child", File(target, child))
        }

        fun copyAsset(asset: String, target: File) {
            target.parentFile?.mkdirs()
            context.assets.open(asset).use { input -> target.outputStream().use { input.copyTo(it, 1 shl 20) } }
        }

        /** The NDK subset ndk.json lists, from Google's zip, each file checked against its SHA-256. */
        fun downloadNdk(subset: JSONObject) {
            val digest = subset.getString("digest")
            if (File(ndk, MARKER).takeIf { it.isFile }?.readText() == digest) return
            report(40, Step.Download)
            ndk.deleteRecursively()
            val url = subset.getString("url")
            val prefix = subset.getString("prefix")
            val expected = HashMap<String, String>()
            val listed: JSONArray = subset.getJSONArray("files")
            for (i in 0 until listed.length()) {
                val file = listed.getJSONObject(i)
                expected[prefix + file.getString("path")] = file.getString("sha256")
            }
            log.line("Downloading ${expected.size} Android NDK files from $url")
            val zip = RemoteZip(subset.getLong("size")) { start, end -> fetchRange(url, start, end) }
            val wanted = zip.entries().filter { it.name in expected }
            if (wanted.size != expected.size) throw IOException("Google's Android NDK download no longer has the expected files.")
            val totalBytes = wanted.sumOf { it.end - it.offset }
            var doneBytes = 0L
            val actual = HashMap<String, String>()
            zip.read(wanted) { entry, bytes ->
                val sha256 = BuildRecipe.hex(MessageDigest.getInstance("SHA-256").digest(bytes))
                if (sha256 != expected[entry.name]) throw IOException("The downloaded Android NDK file ${entry.name} is damaged. Try again.")
                val path = entry.name.removePrefix(prefix)
                File(ndk, path).apply { parentFile?.mkdirs() }.writeBytes(bytes)
                actual[path] = sha256
                doneBytes += entry.end - entry.offset
                report(40 + (10 * doneBytes / totalBytes).toInt(), Step.Download, (doneBytes shr 20).toInt(), (totalBytes shr 20).toInt())
            }
            if (BuildRecipe.listDigest(actual) != digest) throw IOException("The downloaded Android NDK files do not match this app's pins.")
            File(ndk, MARKER).writeText(digest)
        }

        fun fetchRange(url: String, start: Long, end: Long): ByteArray {
            if (cancelled()) throw InterruptedIOException("Build cancelled")
            val connection = URL(url).openConnection() as HttpURLConnection
            try {
                connection.connectTimeout = 30_000
                connection.readTimeout = 60_000
                connection.setRequestProperty("Range", "bytes=$start-$end")
                // Android asks for gzip by default, and Google's server then serves a gzip-encoded
                // zip whose ranges are not the file's (HTTP 416 near the end).
                connection.setRequestProperty("Accept-Encoding", "identity")
                val code = try {
                    connection.responseCode
                } catch (e: IOException) {
                    throw IOException("The Android NDK files could not be downloaded from Google. Check the headset's internet connection.", e)
                }
                if (code != HttpURLConnection.HTTP_PARTIAL) throw IOException("Google's server answered $code to the Android NDK download.")
                val bytes = connection.inputStream.use { it.readBytes() }
                if (bytes.size.toLong() != end - start + 1) throw IOException("The Android NDK download was cut short. Try again.")
                return bytes
            } finally {
                connection.disconnect()
            }
        }

        /**
         * The Retro-WFC payload Retro Rewind's online play runs. translate-mod lowers it into the
         * mod; without it the mod downloads the payload while connecting and jumps into code that
         * was never translated. Retried once, like Setup's download.
         */
        fun downloadRetroWfcPayload(): File {
            val file = File(workspace, "$RETRO_WFC_DIRECTORY/$RETRO_WFC_PAYLOAD_FILE")
            file.parentFile?.mkdirs()
            log.line("Downloading the Retro-WFC payload from $RETRO_WFC_PAYLOAD_URL")
            var failure: IOException? = null
            for (attempt in 1..2) {
                if (cancelled()) throw InterruptedIOException("Build cancelled")
                try {
                    file.writeBytes(fetchRetroWfcPayload())
                    log.line("Retro-WFC payload: ${file.length()} bytes, sha256 ${BuildRecipe.hex(sha256(file))}")
                    return file
                } catch (e: IOException) {
                    // A socket timeout is an InterruptedIOException too, which run() takes for a cancel.
                    failure = e
                    log.line("Retro-WFC payload download attempt $attempt failed: $e")
                    if (attempt == 1) Thread.sleep(1_000)
                }
            }
            if (cancelled()) throw InterruptedIOException("Build cancelled")
            throw IOException(
                "Retro Rewind's online play needs the Retro-WFC payload from rwfc.net, which could not be " +
                    "downloaded (${failure?.message}). Check the headset's internet connection, then build again.",
            )
        }

        fun fetchRetroWfcPayload(): ByteArray {
            val connection = URL(RETRO_WFC_PAYLOAD_URL).openConnection() as HttpURLConnection
            try {
                connection.connectTimeout = 30_000
                connection.readTimeout = 30_000
                // A redirect would fetch from a target other than the fixed endpoint; Setup refuses it too.
                connection.instanceFollowRedirects = false
                connection.setRequestProperty("Accept-Encoding", "identity")
                val code = connection.responseCode
                if (code != HttpURLConnection.HTTP_OK) throw IOException("rwfc.net answered $code")
                connection.inputStream.use { input ->
                    val bytes = java.io.ByteArrayOutputStream()
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        bytes.write(buffer, 0, read)
                        if (bytes.size() > RETRO_WFC_PAYLOAD_MAX_BYTES) throw IOException("the payload is unexpectedly large")
                    }
                    return bytes.toByteArray()
                }
            } finally {
                connection.disconnect()
            }
        }

        /** Translates the disc unless this workspace already holds a translation of the same inputs. */
        fun translate(tools: ToolProcess, identity: String): String? {
            val generated = File(workspace, "generated")
            val provenance = File(generated, "translation-provenance.txt")
            // Fetched before anything else, so an unreachable server fails the build before the
            // long base translation rather than after it.
            val payload = if (profile.modPack) downloadRetroWfcPayload() else null
            if (payload != null) {
                translator(
                    tools, "checking the Retro-WFC payload",
                    "validate-retro-wfc-payload", "--directory", File(workspace, RETRO_WFC_DIRECTORY).absolutePath,
                )?.let { return it }
            }
            // A modded game needs a base translation that knows this Code.pul, so the pack's own
            // identity (and the payload's) is part of what the stored translation is reused for.
            val modIdentity = if (payload != null) {
                "${BuildRecipe.hex(sha256(GameStorage.modCodePul(context)))} ${BuildRecipe.hex(sha256(payload))}"
            } else {
                ""
            }
            val expected = "$identity ${profile.id} ${BuildConfig.DISC_DOL_SHA256} ${BuildConfig.DISC_REL_SHA256} $modIdentity"
            val shards = File(generated, "build_shards/shards.cmake")
            if (provenance.isFile && provenance.readText() == expected && shards.isFile) {
                log.line("Reusing the translation of an earlier build")
                report(400, Step.Translate)
                return null
            }
            provenance.delete()
            val project = File(workspace, "projects/mkwii")
            File(kit, "translation/projects/mkwii").copyRecursively(project, overwrite = true)
            File(workspace, "runtime").deleteRecursively()
            File(kit, "translation/runtime").copyRecursively(File(workspace, "runtime"), overwrite = true)
            File(disc, DiscChecks.DOL_PATH).copyTo(File(workspace, "Assets/main.dol"), overwrite = true)
            File(disc, DiscChecks.REL_PATH).copyTo(File(workspace, "Assets/StaticR.rel"), overwrite = true)
            val manifest = "projects/mkwii/recomp.yml"
            val manifestText = File(workspace, manifest).readText()
            val entryPoint = BuildRecipe.entryPoint(manifestText)
            // The base translation only knows a mod's patches when the profile's Code.pul sits
            // where recomp.yml's mod_root says, so the pack's copy is staged there first.
            val steps = if (profile.modPack) 4 else 3
            if (profile.modPack) {
                val modRoot = File(workspace, BuildRecipe.modRoot(manifestText))
                GameStorage.modCodePul(context).copyTo(File(modRoot, "Binaries/Code.pul"), overwrite = true)
            }

            report(50, Step.Translate, 1, steps)
            val timing = Regex("""\(t=([0-9.]+)s\)""")
            translator(
                tools, "translating the game",
                "translate-recursive", entryPoint, "--project", manifest, "--outdir", "generated/functions",
                "--output-metadata", "generated/base_translation_output.json",
                "--production-source-bundle", "generated/base_translation_sources.bin",
                "--no-function-files", "--prune-stale", "--threads", TRANSLATOR_THREADS.toString(),
            ) { line ->
                timing.find(line)?.groupValues?.get(1)?.toDoubleOrNull()?.let { seconds ->
                    val fraction = minOf(seconds / EXPECTED_TRANSLATION_SECONDS, 0.97)
                    reporter.update(50 + (300 * fraction).toInt(), Step.Translate, 1, steps)
                }
            }?.let { return it }

            // Retro Rewind's own code: its Code.pul translated against the base translation, as
            // Launcher/LocalBuild.ps1 does on a PC, with the Retro-WFC payload for online play.
            val modOutput = "build/mods/retro_rewind_full_cpp"
            if (profile.modPack) {
                report(350, Step.Translate, 2, steps)
                translator(
                    tools, "creating the base manifest",
                    "emit-base-manifest", "--project", manifest, "--out", "build/base",
                    "--functions-dir", "generated/functions",
                    "--translation-output-metadata", "generated/base_translation_output.json", "--region", "P",
                )?.let { return it }
                translator(
                    tools, "translating Retro Rewind",
                    "translate-mod", "--project", manifest, "--profile", "retro-rewind",
                    "--base-manifest", "build/base/mkwii_base_manifest.json",
                    "--base-translation-output-metadata", "generated/base_translation_output.json",
                    "--code-pul", GameStorage.modCodePul(context).absolutePath,
                    "--mod-root", GameStorage.modDirectory(context).absolutePath,
                    "--mod-name", "Retro Rewind", "--region", "P", "--out", modOutput,
                    "--prefer-cached-inputs", "--emit-cpp", "--retro-wfc-payload", payload!!.absolutePath,
                    "--threads", TRANSLATOR_THREADS.toString(),
                )?.let { return it }
            }

            report(370, Step.Translate, steps - 1, steps)
            translator(tools, "generating the game data", "generate-data-init", "--project", manifest, "--target-os", "android")?.let { return it }
            report(385, Step.Translate, steps, steps)
            val shardArguments = mutableListOf(
                "emit-build-shards", "--project", manifest, "--base-metadata", "generated/base_translation_output.json",
                "--base-functions-dir", "generated/functions", "--native-source-dir", "runtime/src", "--out", "generated/build_shards",
            )
            if (profile.modPack) {
                shardArguments += listOf(
                    "--resolved-profile", "$modOutput/resolved_dispatch_profile.json",
                    "--retro-cpp-dir", "$modOutput/cpp",
                )
            }
            translator(tools, "preparing the build", *shardArguments.toTypedArray())?.let { return it }
            provenance.writeText(expected)
            report(400, Step.Translate, steps, steps)
            return null
        }

        fun sha256(file: File): ByteArray {
            val digest = MessageDigest.getInstance("SHA-256")
            file.inputStream().use { input ->
                val buffer = ByteArray(1 shl 20)
                while (true) {
                    val read = input.read(buffer)
                    if (read < 0) break
                    digest.update(buffer, 0, read)
                }
            }
            return digest.digest()
        }

        fun translator(tools: ToolProcess, what: String, vararg arguments: String, onLine: (String) -> Unit = {}): String? {
            log.line("== translator ${arguments.first()}")
            val result = tools.run(
                workspace,
                File(toolchain, "translator/translator_host"),
                listOf(File(toolchain, "translator").absolutePath, "Translator.Cli") + arguments,
                onLine,
            )
            if (result.exitCode == 0) return null
            return if (result.killed) {
                "The headset ran out of memory while $what. Close other apps, then build again."
            } else {
                "The translator failed while $what (exit ${result.exitCode}).\n${result.tail.takeLast(6).joinToString("\n")}"
            }
        }

        /** Compiles every generated source; returns their objects by link slot (runtime, product, translated). */
        // The identity is the kit's fingerprint and the toolchain's: the kit is fingerprinted as a
        // whole, and each game already compiles in its own work directory.
        fun compile(tools: ToolProcess, recipe: JSONObject, identity: String): Map<String, List<String>> {
            val generated = File(workspace, "generated")
            val shards = File(generated, "build_shards/shards.cmake").readText()
            val stamp = File(objects, ".stamp")
            if (stamp.takeIf { it.isFile }?.readText() != identity) {
                objects.deleteRecursively()
                objects.mkdirs()
                stamp.writeText(identity)
            }

            // Blob assembly generated on Windows is rewritten for ELF, as PublicProducts.cmake does.
            fun elfAssembly(source: File): File {
                val rewritten = File(work, source.nameWithoutExtension + "_android.S")
                val text = BuildRecipe.elfBlobAssembly(source.readText())
                if (!rewritten.isFile || rewritten.readText() != text) rewritten.writeText(text)
                return rewritten
            }

            // Which sources fill each link slot is the recipe's to say, so this builds either flavour.
            data class Job(val kind: String, val source: File, val slot: String)
            val jobs = ArrayList<Job>()
            val sources = recipe.getJSONObject("sources")
            for (slot in sources.keys()) {
                val entries = sources.getJSONArray(slot)
                val slotSources = ArrayList<File>()
                for (i in 0 until entries.length()) {
                    val entry = entries.getString(i)
                    when {
                        entry.startsWith("@") -> BuildRecipe.sourceList(shards, entry.substring(1)).forEach { slotSources += File(it) }
                        else -> slotSources += File(BuildRecipe.expand(entry, mapOf("workspace" to workspace.absolutePath)))
                    }
                }
                if (slotSources.isEmpty()) {
                    throw IOException("The translation has no $slot sources for a ${recipe.getString("product")} game.")
                }
                // Assembly is assembled; a mod shard compiles with the same flags as a translated one.
                slotSources.forEach { source ->
                    val assembly = source.name.endsWith(".S")
                    val kind = when {
                        assembly -> "asm"
                        slot == "mod" -> "translated"
                        else -> slot
                    }
                    jobs += Job(kind, if (assembly) elfAssembly(source) else source, slot)
                }
            }
            if (jobs.map { BuildRecipe.objectName(it.source.path) }.toSet().size != jobs.size) {
                throw IOException("Two generated sources share an object name.")
            }

            val values = mapOf(
                "kit" to kit.absolutePath,
                "sysroot" to File(ndk, "sysroot").absolutePath,
                "workspace" to workspace.absolutePath,
            )
            val compileFlags = recipe.getJSONObject("compile")
            for (kind in jobs.map { it.kind }.distinct()) {
                val flags = compileFlags.getJSONArray(kind).let { array -> (0 until array.length()).map { BuildRecipe.expand(array.getString(it), values) } }
                File(work, "$kind.rsp").writeText(BuildRecipe.responseFile(flags))
            }

            val clang = File(toolchain, "llvm/bin/clang-21")
            val resourceDir = File(toolchain, "llvm/lib/clang/21").absolutePath
            val pending = jobs.filter { job ->
                val obj = File(objects, BuildRecipe.objectName(job.source.path))
                !(obj.isFile && obj.length() > 0 && obj.lastModified() >= job.source.lastModified())
            }
            val finished = AtomicInteger(jobs.size - pending.size)
            log.line("== compiling ${pending.size} of ${jobs.size} sources")
            val pool = Executors.newFixedThreadPool(compileJobs())
            try {
                val futures: List<Future<*>> = pending.map { job ->
                    pool.submit {
                        if (failure.get() != null || cancelled()) return@submit
                        val obj = File(objects, BuildRecipe.objectName(job.source.path))
                        val partial = File(objects, obj.name + ".partial")
                        // The blob assembly needs no preprocessing, and preprocessing would make clang
                        // start itself again, which Android does not allow here.
                        val language = if (job.kind == "asm") listOf("--driver-mode=gcc", "-x", "assembler") else listOf("--driver-mode=g++")
                        val result = tools.run(
                            work, clang,
                            language + listOf(
                                "-resource-dir", resourceDir, "@" + File(work, "${job.kind}.rsp").absolutePath,
                                "-c", job.source.absolutePath, "-o", partial.absolutePath,
                            ),
                        )
                        if (result.exitCode != 0) {
                            partial.delete()
                            failure.compareAndSet(
                                null,
                                if (result.killed) "The headset ran out of memory while compiling the game. Close other apps, then build again."
                                else "Compiling ${job.source.name} failed.\n${result.tail.takeLast(6).joinToString("\n")}",
                            )
                            return@submit
                        }
                        if (!partial.renameTo(obj)) failure.compareAndSet(null, "Could not store ${obj.name}.")
                        finished.incrementAndGet()
                    }
                }
                while (futures.any { !it.isDone }) {
                    report(400 + 570 * finished.get() / jobs.size, Step.Compile, finished.get(), jobs.size)
                    TimeUnit.MILLISECONDS.sleep(500)
                }
                futures.forEach { future ->
                    try {
                        future.get()
                    } catch (e: java.util.concurrent.ExecutionException) {
                        val cause = e.cause
                        // A failed compile stops the others, which is not the player cancelling.
                        if (cause is InterruptedIOException && failure.get() == null) throw cause
                        failure.compareAndSet(null, cause?.message ?: cause.toString())
                    }
                }
            } finally {
                pool.shutdownNow()
            }
            if (failure.get() == null) report(970, Step.Compile, jobs.size, jobs.size)
            return jobs.groupBy({ it.slot }, { File(objects, BuildRecipe.objectName(it.source.path)).absolutePath })
        }

        fun compileJobs(): Int {
            val memory = ActivityManager.MemoryInfo()
            context.getSystemService(ActivityManager::class.java)?.getMemoryInfo(memory)
            val byMemory = (memory.availMem / COMPILE_MEMORY_BYTES).toInt()
            return minOf(4, Runtime.getRuntime().availableProcessors(), byMemory).coerceAtLeast(1)
        }

        fun link(tools: ToolProcess, recipe: JSONObject, objectsBySlot: Map<String, List<String>>): File? {
            report(970, Step.Link)
            val library = File(work, recipe.getString("output"))
            library.delete()
            val template = recipe.getJSONObject("link").getJSONArray("lld").let { array -> (0 until array.length()).map { array.getString(it) } }
            val arguments = BuildRecipe.linkArguments(
                template,
                mapOf("kit" to kit.absolutePath, "ndk" to ndk.absolutePath, "output" to library.absolutePath),
                objectsBySlot,
            )
            val rsp = File(work, "link.rsp").apply { writeText(BuildRecipe.responseFile(arguments)) }
            log.line("== linking")
            val result = tools.run(work, File(toolchain, "llvm/bin/ld.lld"), listOf("@" + rsp.absolutePath))
            if (result.exitCode != 0 || !library.isFile) {
                failure.compareAndSet(null, "Linking the game failed.\n${result.tail.takeLast(6).joinToString("\n")}")
                return null
            }
            return library
        }

        fun install(library: File, kitFingerprint: String): String? {
            val destination = GameLibrary.directory(context, profile)
            val staging = File(destination.parentFile, "${destination.name}.building")
            staging.deleteRecursively()
            staging.mkdirs()
            try {
                val installed = File(staging, GameLibrary.LIBRARY_NAME)
                library.copyTo(installed)
                val digest = MessageDigest.getInstance("SHA-256")
                installed.inputStream().use { input ->
                    val buffer = ByteArray(1 shl 20)
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        digest.update(buffer, 0, read)
                    }
                }
                val builtAt = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date())
                val manifest = JSONObject()
                    .put("schema", 1)
                    .put("profile", profile.id)
                    .put("gameId", BuildConfig.DISC_GAME_ID)
                    .put("dolSha256", BuildConfig.DISC_DOL_SHA256)
                    .put("relSha256", BuildConfig.DISC_REL_SHA256)
                    .put("kitFingerprint", kitFingerprint)
                    .put("androidCpu", BuildConfig.ANDROID_CPU)
                    .put("library", GameLibrary.LIBRARY_NAME)
                    .put("librarySha256", BuildRecipe.hex(digest.digest()))
                    .put("includesData", false)
                    .put("builtBy", "this headset (WiiCompiled Quest ${BuildConfig.VERSION_NAME})")
                    .put("builtAt", builtAt)
                File(staging, GameLibrary.MANIFEST_NAME).writeText(manifest.toString(2))
                destination.parentFile?.mkdirs()
                return GameFiles.replace(destination, staging)
            } finally {
                staging.deleteRecursively()
            }
        }
    }
}
