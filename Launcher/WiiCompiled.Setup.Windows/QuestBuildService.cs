using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace WiiCompiled.Setup.Windows;

/// <summary>
/// <c>--build-quest</c>: builds the player's game for the WiiCompiled Meta Quest app from this
/// installation's own translation, so the Quest app, like this installer, never ships game code.
///
/// The Quest app's APK carries a game kit (its prebuilt runtime plus <c>kit.json</c>, the recipe; see
/// android/QuestGameKit.psm1). This service extracts that kit, provides the Android NDK compiler it
/// was built with (downloaded from Google once, never redistributed), and runs the workspace's
/// <c>android\Build-QuestGame.ps1</c>, which compiles the translation against the kit and writes a
/// <c>.wcgame</c> the headset imports. With game files included, the package also carries the
/// extracted disc, so the headset needs nothing else.
/// </summary>
internal sealed class QuestBuildService
{
    /// <summary>The NDK the game kit is built with (android/app/build.gradle.kts, ndkVersion).</summary>
    internal static class Ndk
    {
        public const string Revision = "29.0.14206865";
        public const string ArchiveName = "android-ndk-r29-windows.zip";
        public const string Url = "https://dl.google.com/android/repository/" + ArchiveName;
        // Google's repository manifest (repository2-3.xml) publishes size and SHA-1 only.
        public const long Size = 833850862;
        public const string Sha1 = "ab3bb30fbb9e6903666d60c55d11e78b04e07472";
        public const string LlvmPrefix = "android-ndk-r29/toolchains/llvm/prebuilt/windows-x86_64/";
        public const string LicenseUrl = "https://developer.android.com/studio/terms";
    }

    /// <summary>Developers point this at an installed NDK's toolchains\llvm\prebuilt\windows-x86_64.</summary>
    public const string ToolchainOverrideVariable = "WIICOMPILED_QUEST_NDK_TOOLCHAIN";

    private const string QuestDirectoryName = "QuestBuild";
    private static readonly Regex NinjaProgress = new(@"^\[(\d+)/(\d+)\]", RegexOptions.Compiled);
    private static readonly Regex OtherArchitectureInclude = new(
        @"^sysroot/usr/include/(arm-linux-androideabi|i686-linux-android|x86_64-linux-android|riscv64-linux-android)/",
        RegexOptions.Compiled);

    private readonly IInstallReporter _reporter;

    public QuestBuildService(IInstallReporter reporter) => _reporter = reporter;

    internal sealed record Result(string PackagePath, string KitFingerprint, bool IncludesGameFiles, long SizeBytes);

    internal sealed record Toolchain(string ClangCxx, string ClangC, string Sysroot);

    public async Task<Result> BuildAsync(Installation installation, string apkPath, string outputPath,
        bool includeGameFiles, CancellationToken cancellationToken, string product = "base",
        string? modContentDirectory = null)
    {
        _reporter.Progress(InstallStages.Validate, "Checking the installation and the Quest app...", 1);
        var workspace = installation.WorkspaceDirectory;
        var generated = Path.Combine(workspace, "generated");
        var script = Path.Combine(workspace, "android", "Build-QuestGame.ps1");
        var manifest = Path.Combine(workspace, "projects", "mkwii", "recomp.yml");
        var ninja = Path.Combine(installation.ToolkitDirectory, "Ninja", "ninja.exe");
        if (!File.Exists(Path.Combine(generated, "build_shards", "shards.cmake")))
            throw new InvalidOperationException(
                "This installation has no translated game to build from. Repair WiiCompiled, then try again.");
        if (!File.Exists(script) || !File.Exists(manifest) || !File.Exists(ninja))
            throw new InvalidOperationException(
                "This version of WiiCompiled cannot build for Quest. Update it to the release that matches the Quest app.");
        if (!File.Exists(apkPath))
            throw new FileNotFoundException("The Quest app (APK) was not found.", apkPath);
        if (includeGameFiles && !IsGameData(installation.GameDataDirectory))
            throw new InvalidOperationException(
                "This installation's game files are missing or incomplete. Repair WiiCompiled, or build without game files.");

        var questRoot = Path.Combine(installation.Root, QuestDirectoryName);
        var kitDirectory = Path.Combine(questRoot, "kit");
        _reporter.Progress(InstallStages.QuestKit, "Reading the game kit from the Quest app...", 2);
        var kit = ExtractKit(apkPath, kitDirectory, cancellationToken);
        _reporter.Diagnostic($"Quest game kit {kit.Fingerprint} [{string.Join(", ", kit.Products)}, CPU {kit.AndroidCpu}]");
        if (!kit.Products.Contains(product))
        {
            throw new InvalidOperationException(
                $"The Quest app cannot play {product}: its game kit carries {string.Join(", ", kit.Products)}.");
        }
        // Retro Rewind needs this installation's own Retro Rewind translation, which only exists
        // once the mod has been built here at least once.
        if (product == "retro_rewind" && !HasRetroRewindShards(generated))
        {
            throw new InvalidOperationException(
                "This installation has no Retro Rewind translation. Install or repair Retro Rewind, " +
                "then build for Quest again.");
        }
        if (modContentDirectory is not null && !File.Exists(Path.Combine(modContentDirectory, "Binaries", "Code.pul")))
        {
            throw new InvalidOperationException(
                "The Retro Rewind content to put in the game file is missing its Binaries/Code.pul.");
        }

        var toolchain = await EnsureToolchainAsync(questRoot, cancellationToken);

        var temporaryOutput = outputPath + ".partial";
        if (File.Exists(temporaryOutput)) File.Delete(temporaryOutput);
        Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
        var arguments = new List<string>
        {
            "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
            "-Generated", generated, "-Kit", kitDirectory, "-Manifest", manifest,
            "-BuildDir", Path.Combine(questRoot, "build"), "-Output", temporaryOutput,
            "-ClangCxx", toolchain.ClangCxx, "-ClangC", toolchain.ClangC, "-Sysroot", toolchain.Sysroot,
            "-Ninja", ninja, "-BuiltBy", $"WiiCompiled Setup {ProductInfo.Version}", "-Product", product
        };
        if (includeGameFiles)
        {
            arguments.Add("-Data");
            arguments.Add(installation.GameDataDirectory);
        }
        if (modContentDirectory is not null)
        {
            arguments.Add("-Mod");
            arguments.Add(modContentDirectory);
        }

        _reporter.Progress(InstallStages.QuestBuild, "Preparing the Quest build...", 40);
        var powershell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
            "WindowsPowerShell", "v1.0", "powershell.exe");
        try
        {
            var run = await ProcessRunner.RunAsync(powershell, arguments, ObserveBuild, cancellationToken,
                start => start.WorkingDirectory = workspace, capture: false,
                ex => _reporter.Diagnostic("The cancelled Quest build could not be stopped immediately: " + ex.Message));
            if (run.ExitCode != 0)
                throw new InvalidOperationException(
                    "Building the game for Quest failed. The setup log has the compiler output.");
            if (!File.Exists(temporaryOutput))
                throw new InvalidDataException("The Quest build finished without producing a game package.");

            File.Move(temporaryOutput, outputPath, overwrite: true);
        }
        catch
        {
            // A package with game files is several GB; a failed or cancelled one must not stay behind.
            TryDelete(temporaryOutput);
            throw;
        }
        return new Result(outputPath, kit.Fingerprint, includeGameFiles, new FileInfo(outputPath).Length);
    }

    private void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path)) File.Delete(path);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            _reporter.Diagnostic($"Could not remove {path}: {ex.Message}");
        }
    }

    private void ObserveBuild(string line)
    {
        if (string.IsNullOrWhiteSpace(line)) return;
        _reporter.Diagnostic(line);
        var match = NinjaProgress.Match(line);
        if (match.Success && int.TryParse(match.Groups[1].Value, out var done) &&
            int.TryParse(match.Groups[2].Value, out var total) && total > 0)
        {
            _reporter.Progress(InstallStages.QuestBuild, "Compiling the game for Quest...", 40 + 54 * done / total);
        }
        else if (line.Contains("MKWCBUILD:STEP:quest-package", StringComparison.Ordinal))
        {
            _reporter.Progress(InstallStages.QuestPackage, "Packaging the game for Quest...", 95);
        }
    }

    /// <summary>Whether emit-build-shards recorded a Retro Rewind translation in this workspace.</summary>
    private static bool HasRetroRewindShards(string generated)
    {
        var shards = Path.Combine(generated, "build_shards", "shards.cmake");
        return File.Exists(shards) &&
               File.ReadAllText(shards).Contains("set(MKW_HAVE_RETRO_REWIND_SHARDS ON)", StringComparison.Ordinal);
    }

    private static bool IsGameData(string directory) =>
        Directory.Exists(Path.Combine(directory, "files")) &&
        File.Exists(Path.Combine(directory, "sys", "fst.bin")) &&
        File.Exists(Path.Combine(directory, "sys", "main.dol"));

    /// <summary>What the extracted kit says it builds: its fingerprint and the products it carries.</summary>
    internal sealed record Kit(string Fingerprint, string AndroidCpu, IReadOnlyCollection<string> Products);

    /// <summary>Extracts <c>assets/game_kit</c> from the Quest app's APK and reads what it is for.</summary>
    internal static Kit ExtractKit(string apkPath, string destination, CancellationToken cancellationToken)
    {
        const string prefix = "assets/game_kit/";
        if (Directory.Exists(destination)) Directory.Delete(destination, recursive: true);
        Directory.CreateDirectory(destination);
        var root = Path.GetFullPath(destination) + Path.DirectorySeparatorChar;
        using (var apk = ZipFile.OpenRead(apkPath))
        {
            foreach (var entry in apk.Entries)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (!entry.FullName.StartsWith(prefix, StringComparison.Ordinal) || entry.FullName.EndsWith('/'))
                    continue;
                var target = Path.GetFullPath(Path.Combine(destination, entry.FullName[prefix.Length..]));
                if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"The Quest app contains an invalid game kit path: {entry.FullName}");
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                entry.ExtractToFile(target, overwrite: true);
            }
        }

        var kitJson = Path.Combine(destination, "kit.json");
        if (!File.Exists(kitJson))
            throw new InvalidDataException(
                "This APK is not a WiiCompiled Quest app with a game kit. Choose the Quest app installed on the headset.");
        using var document = JsonDocument.Parse(File.ReadAllText(kitJson));
        if (!document.RootElement.TryGetProperty("fingerprint", out var fingerprint) ||
            fingerprint.ValueKind != JsonValueKind.String)
        {
            throw new InvalidDataException("The Quest app's game kit has no fingerprint.");
        }
        if (!document.RootElement.TryGetProperty("androidCpu", out var androidCpu) ||
            androidCpu.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(androidCpu.GetString()))
        {
            throw new InvalidDataException("The Quest app's game kit has no Android CPU target.");
        }
        var products = document.RootElement.TryGetProperty("products", out var value) &&
                       value.ValueKind == JsonValueKind.Object
            ? value.EnumerateObject().Select(property => property.Name).ToArray()
            : throw new InvalidDataException("The Quest app's game kit lists no games it can build.");
        return new Kit(fingerprint.GetString()!, androidCpu.GetString()!, products);
    }

    private async Task<Toolchain> EnsureToolchainAsync(string questRoot, CancellationToken cancellationToken)
    {
        var configured = Environment.GetEnvironmentVariable(ToolchainOverrideVariable);
        if (!string.IsNullOrWhiteSpace(configured))
        {
            _reporter.Diagnostic($"Using the NDK toolchain at {configured} ({ToolchainOverrideVariable})");
            return ToolchainIn(configured);
        }

        var llvm = Path.Combine(questRoot, "android-ndk-" + Ndk.Revision);
        var marker = Path.Combine(llvm, ".complete");
        if (File.Exists(marker) && File.ReadAllText(marker).Trim() == Ndk.Sha1)
            return ToolchainIn(llvm);

        var downloads = Path.Combine(questRoot, "downloads");
        Directory.CreateDirectory(downloads);
        var archive = Path.Combine(downloads, Ndk.ArchiveName);
        if (!File.Exists(archive) || new FileInfo(archive).Length != Ndk.Size || Sha1Of(archive) != Ndk.Sha1)
            await DownloadNdkAsync(archive, cancellationToken);

        _reporter.Progress(InstallStages.QuestToolchain, "Unpacking the Android compiler...", 34);
        var staging = llvm + ".extracting";
        if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
        using (var zip = ZipFile.OpenRead(archive))
        {
            foreach (var entry in zip.Entries)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var relative = SelectToolchainEntry(entry.FullName);
                if (relative is null) continue;
                var target = Path.Combine(staging, relative.Replace('/', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                entry.ExtractToFile(target, overwrite: true);
            }
        }
        _ = ToolchainIn(staging);
        if (Directory.Exists(llvm)) Directory.Delete(llvm, recursive: true);
        Directory.Move(staging, llvm);
        File.WriteAllText(marker, Ndk.Sha1);
        File.Delete(archive);
        return ToolchainIn(llvm);
    }

    private async Task DownloadNdkAsync(string archive, CancellationToken cancellationToken)
    {
        _reporter.Progress(InstallStages.QuestToolchain,
            $"Downloading the Android NDK compiler from Google (about {Ndk.Size / 1_000_000} MB, Android SDK License: {Ndk.LicenseUrl})...", 5);
        var partial = archive + ".partial";
        using (var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan })
        using (var response = await http.GetAsync(Ndk.Url, HttpCompletionOption.ResponseHeadersRead, cancellationToken))
        {
            response.EnsureSuccessStatusCode();
            await using var source = await response.Content.ReadAsStreamAsync(cancellationToken);
            await using var destination = File.Create(partial);
            using var sha1 = IncrementalHash.CreateHash(HashAlgorithmName.SHA1);
            var buffer = new byte[1 << 20];
            long received = 0;
            var lastPercent = -1;
            while (true)
            {
                var read = await source.ReadAsync(buffer, cancellationToken);
                if (read == 0) break;
                await destination.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
                sha1.AppendData(buffer, 0, read);
                received += read;
                var percent = 5 + (int)(28 * Math.Min(received, Ndk.Size) / Ndk.Size);
                if (percent != lastPercent)
                {
                    lastPercent = percent;
                    _reporter.Progress(InstallStages.QuestToolchain,
                        $"Downloading the Android NDK compiler ({received / 1_000_000} of {Ndk.Size / 1_000_000} MB)...", percent);
                }
            }
            var digest = Convert.ToHexString(sha1.GetHashAndReset()).ToLowerInvariant();
            if (received != Ndk.Size || digest != Ndk.Sha1)
                throw new InvalidDataException(
                    $"The Android NDK download is damaged (size {received}, SHA-1 {digest}). Try again.");
        }
        File.Move(partial, archive, overwrite: true);
    }

    /// <summary>
    /// The files of the NDK's Windows toolchain a game build needs, as their path below the LLVM
    /// directory, or null: clang and lld with their DLLs, clang's own headers and the aarch64
    /// runtime pieces, and the aarch64 half of the sysroot. About 230 MB of the 2.5 GB NDK.
    /// </summary>
    internal static string? SelectToolchainEntry(string entryName)
    {
        if (!entryName.StartsWith(Ndk.LlvmPrefix, StringComparison.Ordinal)) return null;
        var relative = entryName[Ndk.LlvmPrefix.Length..];
        if (relative.Length == 0 || relative.EndsWith('/')) return null;
        var keep = relative is "bin/clang.exe" or "bin/clang++.exe" or "bin/ld.lld.exe"
                       or "bin/libwinpthread-1.dll" or "bin/libxml2.dll" ||
                   Regex.IsMatch(relative, @"^lib/clang/[^/]+/include/") ||
                   Regex.IsMatch(relative, @"^lib/clang/[^/]+/lib/linux/libclang_rt\.builtins-aarch64-android\.a$") ||
                   Regex.IsMatch(relative, @"^lib/clang/[^/]+/lib/linux/aarch64/(libunwind|libatomic)\.a$") ||
                   (relative.StartsWith("sysroot/usr/include/", StringComparison.Ordinal) &&
                    !OtherArchitectureInclude.IsMatch(relative)) ||
                   Regex.IsMatch(relative, @"^sysroot/usr/lib/aarch64-linux-android/([^/]+|29/.+)$");
        return keep ? relative : null;
    }

    private static Toolchain ToolchainIn(string llvmDirectory)
    {
        var toolchain = new Toolchain(
            Path.Combine(llvmDirectory, "bin", "clang++.exe"),
            Path.Combine(llvmDirectory, "bin", "clang.exe"),
            Path.Combine(llvmDirectory, "sysroot"));
        if (!File.Exists(toolchain.ClangCxx) || !File.Exists(toolchain.ClangC) || !Directory.Exists(toolchain.Sysroot))
            throw new InvalidDataException($"The Android NDK toolchain at {llvmDirectory} is incomplete.");
        return toolchain;
    }

    private static string Sha1Of(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA1.HashData(stream)).ToLowerInvariant();
    }
}
