# Builds the standalone Meta Quest APK on a Windows host.
#
#   powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 [-Generated <dir>]
#                                [-Headset modern|quest1] [-Configuration debug|release] [-Install]
#
# One app offers both games: the APK carries a kit for the base game, and for Retro Rewind too
# when -Generated holds a translation that includes the mod.
#
# -Generated names the translator output for the disc you own: the directory
# holding data_sections_init.cpp, RuntimeConfig.h and build_shards/shards.cmake.
# It defaults to the installer's build workspace next to this checkout. The
# Windows-generated tree is used as-is; runtime/cmake/PublicProducts.cmake
# rewrites its blob assembly for ELF at configure time.
#
# Prerequisites (see docs/quest-port.md): JDK 17, the Android SDK with NDK
# 29.0.14206865 and CMake 3.22.1, and android/Prepare-QuestDependencies.ps1
# having staged the SDL3 AAR.
[CmdletBinding()]
param(
    [string]$Generated = '',
    [string]$Dependencies = '',
    [string]$CMakeDir = '',
    [ValidateSet('modern', 'quest1')] [string]$Headset = 'modern',
    [ValidateSet('debug', 'release')] [string]$Configuration = 'debug',
    [switch]$Install
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $root '..')).Path

if (-not $Generated) {
    $Generated = Join-Path $repo '.scratch\vr-build-workspace\BuildWorkspace\generated'
}
if (-not (Test-Path (Join-Path $Generated 'build_shards\shards.cmake'))) {
    throw "No translated graph at $Generated (expected build_shards\shards.cmake). Run the translator first; see docs/quest-port.md."
}
if (-not (Test-Path (Join-Path $root 'app\libs\SDL3-3.4.4.aar'))) {
    throw 'SDL3 AAR missing; run android/Prepare-QuestDependencies.ps1 first.'
}

$javaHome = if ($env:JAVA_HOME) { $env:JAVA_HOME } else { 'C:\Program Files\Java\jdk-17' }
$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if (-not (Test-Path (Join-Path $javaHome 'bin\java.exe'))) { throw "JDK 17 not found at $javaHome (set JAVA_HOME)" }
if (-not (Test-Path $sdkRoot)) { throw "Android SDK not found at $sdkRoot (set ANDROID_HOME)" }

$env:JAVA_HOME = $javaHome
$env:ANDROID_HOME = $sdkRoot
$env:ANDROID_SDK_ROOT = $sdkRoot

# aurora-main requires CMake 3.25+, newer than the SDK's bundled 3.22.1. Point
# the Android Gradle plugin at a system CMake through local.properties and put
# the SDK's Ninja on PATH, which a non-bundled CMake needs.
if (-not $CMakeDir) {
    $cmakeExe = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmakeExe) { $CMakeDir = Split-Path -Parent (Split-Path -Parent $cmakeExe.Source) }
}
if (-not $CMakeDir -or -not (Test-Path (Join-Path $CMakeDir 'bin\cmake.exe'))) {
    throw 'No CMake 3.25+ found; install one or pass -CMakeDir <install root>'
}
$sdkCMake = Get-ChildItem -Path (Join-Path $sdkRoot 'cmake') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if ($sdkCMake) { $env:PATH = (Join-Path $sdkCMake.FullName 'bin') + ';' + $env:PATH }
# Parenthesised: PowerShell's comma binds tighter than +, which would join both
# properties into a single line.
$localProperties = @(
    ('sdk.dir=' + $sdkRoot.Replace('\', '\\')),
    ('cmake.dir=' + $CMakeDir.Replace('\', '\\'))
)
Set-Content -Path (Join-Path $root 'local.properties') -Value $localProperties -Encoding ascii

if (-not $Dependencies) {
    $candidate = Join-Path $repo '.scratch\vr-build-workspace\BuildWorkspace\Dependencies'
    if (Test-Path $candidate) { $Dependencies = $candidate }
}

$variant = (Get-Culture).TextInfo.ToTitleCase($Configuration)
$flavour = if ($Headset -eq 'quest1') { 'Quest1' } else { 'ModernQuest' }
$expectedCpu = if ($Headset -eq 'quest1') { 'kryo' } else { 'cortex-a77' }
$task = "app:assemble$flavour$variant"
$gradleArgs = @(
    '--project-dir', $root,
    "-PmkwGeneratedDir=$Generated"
)
if ($Dependencies) { $gradleArgs += "-PmkwDependenciesDir=$Dependencies" }
$gradleArgs += $task
Write-Host "gradlew $($gradleArgs -join ' ')"
& (Join-Path $root 'gradlew.bat') @gradleArgs
if ($LASTEXITCODE -ne 0) { throw "Gradle failed ($LASTEXITCODE)" }

$flavourDir = if ($Headset -eq 'quest1') { 'quest1' } else { 'modernQuest' }
$apkDir = Join-Path $root "app\build\outputs\apk\$flavourDir\$Configuration"
$apk = Get-ChildItem -Path $apkDir -Filter '*.apk' | Select-Object -First 1
if (-not $apk) { throw "No APK under $apkDir" }
Write-Host "APK: $($apk.FullName)"

# The APK must be distributable: no translated game inside, only the game kit and the toolchain
# that builds one on the headset.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($apk.FullName)
try {
    $gameLibraries = @($archive.Entries | Where-Object { $_.FullName -match '^lib/[^/]+/libmain[^/]*\.so$' })
    $kitEntry = @($archive.Entries | Where-Object { $_.FullName -eq 'assets/game_kit/kit.json' }) | Select-Object -First 1
    $kitCpu = if ($kitEntry) {
        $reader = New-Object IO.StreamReader($kitEntry.Open())
        try { ($reader.ReadToEnd() | ConvertFrom-Json).androidCpu } finally { $reader.Dispose() }
    } else { '' }
    $hasToolchain = @($archive.Entries | Where-Object { $_.FullName -eq 'assets/quest_toolchain/files.zip' }).Count -gt 0
} finally { $archive.Dispose() }
if ($gameLibraries.Count -gt 0) { throw "The APK contains a translated game library: $($gameLibraries.FullName -join ', ')" }
if (-not $kitEntry) { throw 'The APK has no game kit (assets/game_kit/kit.json)' }
if ($kitCpu -ne $expectedCpu) { throw "The $Headset APK contains a game kit for CPU '$kitCpu', expected '$expectedCpu'" }
if (-not $hasToolchain) { throw 'The APK has no build toolchain (assets/quest_toolchain/files.zip)' }

if ($Install) {
    $adb = Join-Path $sdkRoot 'platform-tools\adb.exe'
    & $adb install -r $apk.FullName
    if ($LASTEXITCODE -ne 0) { throw "adb install failed ($LASTEXITCODE)" }
    if ($Headset -eq 'quest1') {
        Write-Host 'Installed. Use WiiCompiled Settings for setup, then launch WiiCompiled VR directly from the library.'
    } else {
        Write-Host 'Installed. Build a game with Build on this Quest in the launcher, or on this PC with android/Build-QuestGame.ps1 -Install.'
    }
}
