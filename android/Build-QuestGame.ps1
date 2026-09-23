# Builds the player's Quest game on a PC: compiles the translator's output for their own disc
# against the Quest game kit (see QuestGameKit.psm1) and packages libmain.so as a .wcgame the
# headset imports ("Import from computer" in the launcher).
#
# Two callers. A developer runs it from the checkout, where the toolchain defaults to the Android
# SDK's NDK and ninja:
#
#   powershell -ExecutionPolicy Bypass -File android/Build-QuestGame.ps1 [-Generated <dir>] [-Kit <dir>]
#                                    [-Headset modern|quest1] [-Configuration debug|release]
#                                    [-Data <extracted disc dir>] [-Output <file.wcgame>] [-Install]
#
# WiiCompiled Setup's --build-quest runs the copy staged in an installation's BuildWorkspace\android,
# naming everything explicitly: the kit it extracted from the Quest app, the NDK toolchain it
# downloaded, the toolkit's ninja and the workspace's recomp.yml.
#
# -Generated is the translator output (the dev workspace's generated/ by default). -Kit is the game
# kit the Quest app carries; it defaults to the one the last APK build exported. -Product picks the
# game: base, or retro_rewind (whose translation must include the mod). -Data adds the extracted
# disc (the directory holding sys/ and files/) and -Mod the RetroRewind6 pack, so the headset needs
# nothing else.
# -Install pushes the package into the app's Import folder over adb, where the launcher picks it
# up the next time it opens.
[CmdletBinding()]
param(
    [string]$Generated = '',
    [string]$Kit = '',
    [string]$Output = '',
    [string]$Data = '',
    [string]$BuildDir = '',
    [string]$Manifest = '',
    [string]$ClangCxx = '',
    [string]$ClangC = '',
    [string]$Sysroot = '',
    [string]$Ninja = '',
    [string]$BuiltBy = 'android/Build-QuestGame.ps1',
    [ValidateSet('modern', 'quest1')] [string]$Headset = 'modern',
    [ValidateSet('debug', 'release')] [string]$Configuration = 'debug',
    [ValidateSet('base', 'retro_rewind')] [string]$Product = 'base',
    [string]$Mod = '',
    [int]$TranslatedJobs = 0,
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'QuestGameKit.psm1') -Force

$root = $PSScriptRoot
$workspace = (Resolve-Path (Join-Path $root '..')).Path
if (-not $Generated) { $Generated = Join-Path $workspace '.scratch\vr-build-workspace\BuildWorkspace\generated' }
if (-not $Manifest) { $Manifest = Join-Path $workspace 'projects\mkwii\recomp.yml' }
$variant = if ($Headset -eq 'quest1') { 'quest1' } else { 'modernQuest' }
$variant += (Get-Culture).TextInfo.ToTitleCase($Configuration)
if (-not $BuildDir) { $BuildDir = Join-Path $root "app\build\questGame\$variant\$Product" }
if (-not $Kit) {
    # Select the requested variant explicitly. Modification time is unsafe now that flavours use
    # different -mcpu targets and an up-to-date native probe is not necessarily the newest one.
    $Kit = Join-Path $root "app\build\generated\assets\questGameKit\$variant\game_kit"
}
if (-not $Kit -or -not (Test-Path (Join-Path $Kit 'kit.json'))) {
    throw 'No game kit; run Build-Quest.ps1 first, or pass -Kit'
}
if (-not (Test-Path (Join-Path $Generated 'build_shards\shards.cmake'))) { throw "No translated graph at $Generated" }

$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if (-not $ClangCxx -or -not $ClangC -or -not $Sysroot) {
    $llvm = Join-Path $sdkRoot 'ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64'
    if (-not $ClangCxx) { $ClangCxx = Join-Path $llvm 'bin\clang++.exe' }
    if (-not $ClangC) { $ClangC = Join-Path $llvm 'bin\clang.exe' }
    if (-not $Sysroot) { $Sysroot = Join-Path $llvm 'sysroot' }
}
foreach ($tool in $ClangCxx, $ClangC) { if (-not (Test-Path $tool)) { throw "Compiler not found: $tool" } }
if (-not $Ninja) {
    $Ninja = Get-ChildItem -Path (Join-Path $sdkRoot 'cmake') -Filter ninja.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    if (-not $Ninja) { $Ninja = (Get-Command ninja.exe -ErrorAction SilentlyContinue).Source }
}
if (-not $Ninja -or -not (Test-Path $Ninja)) { throw 'ninja.exe not found' }

# Translated shards need roughly 1.5 GB each at -O2; leave a margin for the rest of the machine.
if ($TranslatedJobs -le 0) {
    $memoryGb = [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    $TranslatedJobs = [math]::Max(1, [math]::Min([Environment]::ProcessorCount, [math]::Floor(($memoryGb - 4) / 1.5)))
}

# The disc this translation was made from: recomp.yml's pins, which the translator enforced.
$pins = [IO.File]::ReadAllText($Manifest)
$gameId = [regex]::Match($pins, '\n\s*game_id:\s*(\w+)').Groups[1].Value
$dolSha = [regex]::Match($pins, '(?s)\n\s*dol:.*?sha256:\s*([0-9a-fA-F]{64})').Groups[1].Value
$relSha = [regex]::Match($pins, '(?s)\n\s*rel:.*?sha256:\s*([0-9a-fA-F]{64})').Groups[1].Value
if (-not $gameId -or -not $dolSha -or -not $relSha) { throw "Cannot read the disc pins from $Manifest" }

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$library = Invoke-QuestGameBuild -KitDir $Kit -GeneratedDir $Generated -BuildDir $BuildDir `
    -ClangCxx $ClangCxx -ClangC $ClangC -Sysroot $Sysroot -Ninja $Ninja -TranslatedJobs $TranslatedJobs -Product $Product
Write-Host ("Built {0} in {1:N1} min" -f $library, $stopwatch.Elapsed.TotalMinutes)

if (-not $Output) { $Output = Join-Path $BuildDir 'MarioKartWii.wcgame' }
Write-Host 'MKWCBUILD:STEP:quest-package Packaging the game for Quest'
$game = New-QuestGamePackage -Library $library -DataDir $Data -ModDir $Mod -KitDir $Kit -GameId $gameId -DolSha256 $dolSha -RelSha256 $relSha `
    -BuiltBy $BuiltBy -OutputPath $Output -Product $Product
Write-Host "Game package: $Output (kit $($game.kitFingerprint))"

if ($Install) {
    $adb = Join-Path $sdkRoot 'platform-tools\adb.exe'
    $import = '/sdcard/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/Import'
    # The launcher creates Import itself and must own it to remove imported packages (see
    # Run-Quest.ps1 on adb-created directories), so only push into one that exists.
    $exists = ((& $adb shell "test -d '$import' && echo yes") | Out-String).Trim()
    if ($exists -ne 'yes') { throw 'The launcher has not created its Import folder yet; open the app once on the headset first.' }
    $remote = "$import/$([IO.Path]::GetFileName($Output))"
    & $adb push $Output $remote
    if ($LASTEXITCODE -ne 0) { throw "adb push failed ($LASTEXITCODE)" }
    # The folder belongs to the app; only the pushed file is the shell user's to open up.
    & $adb shell "chmod a+r '$remote'"
    Write-Host 'Pushed; the launcher imports it the next time it opens.'
}
