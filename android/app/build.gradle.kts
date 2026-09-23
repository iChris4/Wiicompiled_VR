import javax.inject.Inject
import org.gradle.api.provider.Property
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Translator output for the disc the user owns (data_sections_init.cpp,
// RuntimeConfig.h, build_shards/shards.cmake). Never checked in.
val mkwGeneratedDir = providers.gradleProperty("mkwGeneratedDir").orNull
// Optional: a directory of already-fetched dependency sources (the installer's
// BuildWorkspace/Dependencies) so the native configure does not download them.
val mkwDependenciesDir = providers.gradleProperty("mkwDependenciesDir").orNull
// Optional: a directory holding a second SDL3 AAR/prefab is not needed; the
// AAR in app/libs is produced by Prepare-QuestDependencies.ps1.
val mkwRepoRoot = rootProject.file("..").canonicalFile

// The runtime's read-only assets travel inside the APK and are unpacked by
// QuestActivity into the app's private storage on first launch.
val runtimeResources = layout.buildDirectory.dir("generated/assets/runtimeResources")
val prepareRuntimeResources by tasks.registering(Copy::class) {
    val assets = File(mkwRepoRoot, "runtime/assets")
    from(File(assets, "wii")) { into("runtime_resources/wii_bootstrap") }
    from(File(assets, "dsp/dsp_coef.bin")) { into("runtime_resources") }
    from(File(assets, "pipeline/initial_pipeline_cache.db")) { into("runtime_resources") }
    into(runtimeResources)
}

// The disc the launcher accepts when it extracts the player's image: the game ID and
// the clean PAL main.dol / StaticR.rel hashes, read from the same recomp.yml pins the
// PC installer's payload manifest uses (Launcher/Build-Installer.ps1).
val discPins: Map<String, String> = run {
    val manifest = File(mkwRepoRoot, "projects/mkwii/recomp.yml").readText()
    fun pin(pattern: String): String =
        Regex(pattern, RegexOption.DOT_MATCHES_ALL).find(manifest)?.groupValues?.get(1)
            ?: throw GradleException("projects/mkwii/recomp.yml has no match for $pattern")
    mapOf(
        "DISC_GAME_ID" to pin("""\n\s*game_id:\s*(\w+)"""),
        "DISC_DOL_SHA256" to pin("""\n\s*dol:.*?sha256:\s*([0-9a-fA-F]{64})"""),
        "DISC_REL_SHA256" to pin("""\n\s*rel:.*?sha256:\s*([0-9a-fA-F]{64})"""),
    )
}

// Whether the translation this build is pointed at includes the Retro Rewind mod, which decides
// whether the kit can carry that product (emit-build-shards records it in shards.cmake).
val hasRetroRewindShards: Boolean = run {
    val shards = File(mkwGeneratedDir ?: return@run false, "build_shards/shards.cmake")
    shards.isFile && shards.readText().contains("set(MKW_HAVE_RETRO_REWIND_SHARDS ON)")
}

// android/nod-jni: nod, the disc image library the PC installer runs as nodtool,
// cross-compiled with cargo for the launcher's "Select disc image". Needs a Rust
// toolchain with the aarch64-linux-android target (see docs/quest-port.md).
val nodJniDir = rootProject.file("nod-jni")
val nodJniTargetDir = layout.buildDirectory.dir("nod-jni")
val nodJniLibs = layout.buildDirectory.dir("generated/jniLibs/nodJni")
val buildNodJni by tasks.registering(Exec::class) {
    inputs.dir(File(nodJniDir, "src"))
    inputs.files(File(nodJniDir, "Cargo.toml"), File(nodJniDir, "Cargo.lock"))
    outputs.file(nodJniTargetDir.map { it.file("aarch64-linux-android/release/libnod_jni.so") })
    workingDir = nodJniDir
    val windows = System.getProperty("os.name").startsWith("Windows")
    val cargoHome = System.getenv("CARGO_HOME")?.let(::File) ?: File(System.getProperty("user.home"), ".cargo")
    val cargo = providers.gradleProperty("cargo").orNull
        ?: File(cargoHome, if (windows) "bin/cargo.exe" else "bin/cargo").takeIf { it.isFile }?.path
        ?: "cargo"
    commandLine(cargo, "build", "--release", "--locked", "--target", "aarch64-linux-android")
    doFirst {
        val host = when {
            windows -> "windows-x86_64"
            System.getProperty("os.name").startsWith("Mac") -> "darwin-x86_64"
            else -> "linux-x86_64"
        }
        val bin = File(androidComponents.sdkComponents.ndkDirectory.get().asFile, "toolchains/llvm/prebuilt/$host/bin")
        val clang = File(bin, "aarch64-linux-android29-clang" + if (windows) ".cmd" else "").path
        environment("CARGO_TARGET_DIR", nodJniTargetDir.get().asFile.path)
        environment("CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER", clang)
        environment("CC_aarch64_linux_android", clang)
        environment("AR_aarch64_linux_android", File(bin, "llvm-ar" + if (windows) ".exe" else "").path)
    }
}
val stageNodJni by tasks.registering(Copy::class) {
    from(buildNodJni) { include("**/libnod_jni.so") }
    eachFile { path = "arm64-v8a/$name" }
    includeEmptyDirs = false
    into(nodJniLibs)
}
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("JniLibFolders") }.configureEach {
    dependsOn(stageNodJni)
}

android {
    namespace = "org.wiicompiled.quest"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "org.wiicompiled.quest"
        // Quest 2 ships Android 10 (API 29); AHardwareBuffer/Vulkan 1.1 need 26+.
        minSdk = 29
        targetSdk = 34
        versionCode = 4
        versionName = "0.4.0-quest"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        for ((name, value) in discPins) {
            buildConfigField("String", name, "\"$value\"")
        }
        // The APK carries game kits, not games: the player's libmain.so is built from their own
        // disc and loaded from private storage (see android/QuestGameKit.psm1). One app offers
        // both products, like the PC launcher, and the launcher's toggle picks which one to play.
        buildConfigField("boolean", "EXTERNAL_GAME", "true")
        buildConfigField("boolean", "ON_DEVICE_BUILD", "true")

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                // One probe per product the translation can build. Retro Rewind's exists only
                // once translate-mod and emit-build-shards have run over the mod.
                targets += "mkw_quest_kit_probe"
                if (hasRetroRewindShards) {
                    targets += "mkw_quest_kit_probe_retro"
                }
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-29",
                    "-DMKW_REPO_ROOT=${mkwRepoRoot.path.replace('\\', '/')}",
                )
                if (mkwGeneratedDir != null) {
                    arguments += "-DMKW_GENERATED_DIR=${File(mkwGeneratedDir).canonicalPath.replace('\\', '/')}"
                }
                if (mkwDependenciesDir != null) {
                    arguments += "-DMKW_DEPENDENCIES_DIR=${File(mkwDependenciesDir).canonicalPath.replace('\\', '/')}"
                }
            }
        }
    }

    flavorDimensions += "headset"
    productFlavors {
        create("modernQuest") {
            dimension = "headset"
            manifestPlaceholders["mkwQuestSupportedDevices"] = "quest2|quest3|quest3s|questpro"
            buildConfigField("boolean", "QUEST1_DIRECT_LAUNCH", "false")
            buildConfigField("String", "ANDROID_CPU", "\"cortex-a77\"")
            externalNativeBuild {
                cmake {
                    // Snapdragon XR2 and newer. Keep this as the default build target.
                    arguments += "-DMKW_ANDROID_CPU=cortex-a77"
                }
            }
        }
        create("quest1") {
            dimension = "headset"
            manifestPlaceholders["mkwQuestSupportedDevices"] = "quest|quest2"
            buildConfigField("boolean", "QUEST1_DIRECT_LAUNCH", "true")
            buildConfigField("String", "ANDROID_CPU", "\"kryo\"")
            externalNativeBuild {
                cmake {
                    // Snapdragon 835. cortex-a77 binaries terminate with SIGILL on Quest 1.
                    arguments += "-DMKW_ANDROID_CPU=kryo"
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            // aurora-main needs CMake 3.25+, newer than the SDK's bundled 3.22.1;
            // Build-Quest.ps1 points cmake.dir in local.properties at a system CMake.
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    buildFeatures {
        prefab = true
        buildConfig = true
    }

    sourceSets.named("main") {
        assets.srcDir(runtimeResources)
        jniLibs.srcDir(nodJniLibs)
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    packaging {
        jniLibs {
            useLegacyPackaging = false
            // Built only to produce each flavour's game kit; their objects travel as assets/game_kit.
            excludes += "**/libmkw_quest_kit_probe*.so"
        }
    }
    lint {
        disable += setOf("ChromeOsAbiSupport", "DiscouragedApi")
    }
}

tasks.named("preBuild") {
    dependsOn(prepareRuntimeResources)
}

/**
 * Exports the game kit (android/QuestGameKit.psm1) from the CMake tree this variant's native
 * build just produced, into the variant's assets as game_kit/. The tree is found through the
 * probe library: AGP gives the .cxx directory and the obj directory the same configuration hash.
 */
abstract class ExportQuestGameKit : DefaultTask() {
    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:Internal
    abstract val appDir: DirectoryProperty

    @get:Internal
    abstract val script: RegularFileProperty

    @get:Internal
    abstract val llvmStrip: RegularFileProperty

    @get:Input
    abstract val androidCpu: Property<String>

    @get:Inject
    abstract val execOperations: ExecOperations

    init {
        // The kit's inputs are CMake's outputs, which Gradle does not track; exporting takes seconds.
        outputs.upToDateWhen { false }
    }

    @TaskAction
    fun export() {
        val app = appDir.get().asFile
        data class Candidate(val probe: File, val binaryDir: File, val cpu: String?)
        fun cacheValue(cache: String, name: String): String? =
            Regex("(?m)^${Regex.escape(name)}(?::[^=\\r\\n]*)?=([^\\r\\n]*)$")
                .find(cache)?.groupValues?.get(1)?.trim()

        val candidates = File(app, "build/intermediates/cxx").walkTopDown()
            .filter { it.name == "libmkw_quest_kit_probe.so" && it.parentFile.name == "arm64-v8a" }
            .map { probe ->
                val configuration = probe.parentFile.parentFile.parentFile // <Variant>/<hash>
                val binaryDir = File(app, ".cxx/${configuration.parentFile.name}/${configuration.name}/arm64-v8a")
                val cacheFile = File(binaryDir, "CMakeCache.txt")
                val cache = cacheFile.takeIf { it.isFile }?.readText().orEmpty()
                Candidate(probe, binaryDir, cacheValue(cache, "MKW_ANDROID_CPU"))
            }
            .toList()
        val expectedCpu = androidCpu.get()
        val matching = candidates.filter {
            it.cpu == expectedCpu && File(it.binaryDir, "build.ninja").isFile
        }
        val chosen = matching.maxByOrNull { it.probe.lastModified() }
            ?: throw GradleException(
                "No CMake tree for Android CPU $expectedCpu. Found: " +
                    candidates.joinToString { "${it.binaryDir} (cpu=${it.cpu})" }
            )
        val binaryDir = chosen.binaryDir
        val kitDir = File(outputDir.get().asFile, "game_kit")
        execOperations.exec {
            commandLine(
                "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script.get().asFile.path,
                "-CMakeBinaryDir", binaryDir.path, "-OutputDir", kitDir.path, "-LlvmStrip", llvmStrip.get().asFile.path,
                "-AndroidCpu", expectedCpu,
            )
        }
    }
}

/**
 * The toolchain for building the game on the headset (android/Prepare-QuestToolchain.ps1): the
 * translator for Android, clang/lld, and the pin of the NDK files the headset downloads. Packaged
 * into the base variants' assets as quest_toolchain/.
 */
abstract class PrepareQuestToolchain : DefaultTask() {
    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val sources: ConfigurableFileCollection

    @get:Internal
    abstract val script: RegularFileProperty

    @get:Internal
    abstract val ndkLlvm: DirectoryProperty

    @get:Inject
    abstract val execOperations: ExecOperations

    @TaskAction
    fun prepare() {
        execOperations.exec {
            commandLine(
                "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script.get().asFile.path,
                "-OutputDir", File(outputDir.get().asFile, "quest_toolchain").path, "-NdkLlvm", ndkLlvm.get().asFile.path,
            )
        }
    }
}

androidComponents {
    onVariants { variant ->
        val host = if (System.getProperty("os.name").startsWith("Windows")) "windows-x86_64" else "linux-x86_64"
        val capitalized = variant.name.replaceFirstChar { it.uppercase() }
        // The APK ships kits instead of games. AGP packages every library in the CMake output
        // directory, so a game library left over from an earlier build would otherwise travel in it.
        variant.packaging.jniLibs.excludes.add("**/libmain*.so")
        val export = tasks.register<ExportQuestGameKit>("export${capitalized}QuestGameKit") {
            dependsOn("merge${capitalized}NativeLibs")
            appDir.set(layout.projectDirectory)
            script.set(rootProject.layout.projectDirectory.file("Export-QuestGameKit.ps1"))
            androidCpu.set(if (variant.name.startsWith("quest1", ignoreCase = true)) "kryo" else "cortex-a77")
            outputDir.set(layout.buildDirectory.dir("generated/assets/questGameKit/${variant.name}"))
            llvmStrip.set(sdkComponents.ndkDirectory.map {
                it.file("toolchains/llvm/prebuilt/$host/bin/llvm-strip" + if (host.startsWith("windows")) ".exe" else "")
            })
        }
        variant.sources.assets?.addGeneratedSourceDirectory(export, ExportQuestGameKit::outputDir)

        val prepareToolchain = tasks.register<PrepareQuestToolchain>("prepare${capitalized}QuestToolchain") {
            script.set(rootProject.layout.projectDirectory.file("Prepare-QuestToolchain.ps1"))
            sources.from(script, rootProject.layout.projectDirectory.dir("toolchain"))
            sources.from(fileTree(File(mkwRepoRoot, "translator/src")) { exclude("**/bin/**", "**/obj/**") })
            sources.from(fileTree(File(mkwRepoRoot, "Launcher/WiiCompiled.Setup.Common")) { exclude("**/bin/**", "**/obj/**") })
            ndkLlvm.set(sdkComponents.ndkDirectory.map { it.dir("toolchains/llvm/prebuilt/$host") })
        }
        variant.sources.assets?.addGeneratedSourceDirectory(prepareToolchain, PrepareQuestToolchain::outputDir)
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    // SDLActivity and libSDL3.so; the AAR is downloaded by Prepare-QuestDependencies.ps1.
    implementation(files("libs/SDL3-3.4.4.aar"))
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
}
