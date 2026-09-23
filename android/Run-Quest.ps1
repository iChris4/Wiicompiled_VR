# Installs the built APK on the connected Quest, places the game data, launches
# the game activity directly (skipping the launcher panel) and collects
# diagnostics for one session.
#
#   powershell -ExecutionPolicy Bypass -File android/Run-Quest.ps1 [-Apk <path>] [-Data <extracted disc dir>]
#                         [-Headset modern|quest1] [-Configuration debug|release]
#                         [-Seconds 60] [-NoLaunch] [-SkipInstall]
#
# -Data names the extracted disc partition (the directory holding sys/, files/,
# disc/ ...). It is pushed once to a staging folder, then moved into the app's
# data directory; both steps are skipped when DATA is already in place.
# Diagnostics land in android/.runs/<stamp>/: logcat (UTF-8) and the runtime's
# own per-run log folder pulled from the device.
#
# Android storage rules this script works within:
#  * Never create directories under Android/data/<package> through adb before
#    the app has run: they would belong to the shell user and the app could
#    not write its Config.toml, NAND or logs there. The app creates its own
#    WiiCompiledOpenXRVR directory on first launch, so this script launches it
#    once before placing DATA.
#  * Files adb places stay owned by the shell user, so DATA is made
#    world-readable (chmod -R a+rX). The game only ever reads it.
#  * run-as cannot reach shared storage (SELinux), so it is not used here.
[CmdletBinding()]
param(
    [string]$Apk = '',
    [string]$Data = '',
    [ValidateSet('modern', 'quest1')] [string]$Headset = 'modern',
    [ValidateSet('debug', 'release')] [string]$Configuration = 'debug',
    [int]$Seconds = 60,
    [switch]$NoLaunch,
    [switch]$SkipInstall
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$adb = Join-Path $sdkRoot 'platform-tools\adb.exe'
if (-not (Test-Path $adb)) { throw "adb not found at $adb" }

$package = 'org.wiicompiled.quest'
$activity = "$package/.QuestActivity"
$appDir = "/sdcard/Android/data/$package/files/WiiCompiledOpenXRVR"
$stageDir = '/sdcard/Download/WiiCompiledQuestStaging'

function Invoke-Adb { param([string[]]$Arguments) & $adb @Arguments; if ($LASTEXITCODE -ne 0) { throw "adb $($Arguments -join ' ') failed ($LASTEXITCODE)" } }
# adb shell output carries a trailing CR, so trim before comparing.
function Test-DevicePath { param([string]$Path, [string]$Kind = 'd') ((& $adb shell "test -$Kind '$Path' && echo yes") | Out-String).Trim() -eq 'yes' }

$devices = (& $adb devices) -match "device$"
if (-not $devices) { throw 'No device in "device" state; check the Quest is connected and USB debugging is allowed.' }

if (-not $SkipInstall) {
    if (-not $Apk) {
        $flavour = if ($Headset -eq 'quest1') { 'quest1' } else { 'modernQuest' }
        $apkDir = Join-Path $root "app\build\outputs\apk\$flavour\$Configuration"
        $Apk = Get-ChildItem -Path $apkDir -Filter '*.apk' -ErrorAction SilentlyContinue |
            Select-Object -First 1 | ForEach-Object FullName
    }
    if (-not $Apk -or -not (Test-Path $Apk)) { throw 'No APK found; run Build-Quest.ps1 first or pass -Apk' }
    Write-Host "Installing $Apk"
    Invoke-Adb @('install', '-r', '-g', $Apk)
}

if (-not (Test-DevicePath "$appDir/DATA")) {
    if (-not (Test-DevicePath $appDir)) {
        Write-Host 'First launch so the app creates its own data directory'
        Invoke-Adb @('shell', 'am', 'start', '-W', '-n', $activity)
        for ($i = 0; $i -lt 30 -and -not (Test-DevicePath $appDir); ++$i) { Start-Sleep -Seconds 1 }
        & $adb shell am force-stop $package
        if (-not (Test-DevicePath $appDir)) { throw "The app did not create $appDir" }
    }
    if (-not (Test-DevicePath "$stageDir/DATA") -and $Data) {
        Write-Host "Pushing $Data to the staging folder (a few minutes)"
        Invoke-Adb @('shell', 'mkdir', '-p', $stageDir)
        Invoke-Adb @('push', $Data, "$stageDir/DATA")
    }
    if (Test-DevicePath "$stageDir/DATA") {
        Write-Host 'Moving staged DATA into the app directory'
        Invoke-Adb @('shell', "mv '$stageDir/DATA' '$appDir/DATA'")
    } else {
        Write-Warning 'No DATA on the device and -Data not given; the app will show its missing-data dialog.'
    }
}
if (Test-DevicePath "$appDir/DATA") {
    # Idempotent; also repairs a tree placed by hand.
    Invoke-Adb @('shell', "chmod -R a+rX '$appDir/DATA'")
}

if ($NoLaunch) { return }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = Join-Path $root ".runs\$stamp"
New-Item -ItemType Directory -Force $runDir | Out-Null

& $adb logcat -c
Write-Host "Launching $activity"
Invoke-Adb @('shell', 'am', 'start', '-n', $activity)
Start-Sleep -Seconds $Seconds
$logcat = Join-Path $runDir 'logcat.txt'
# Windows PowerShell's '>' writes UTF-16; keep the capture greppable.
& $adb logcat -d -v time | Out-File -FilePath $logcat -Encoding utf8
Write-Host "logcat: $logcat"

# The game runs in its own process; the package's main process is the launcher panel.
$running = ((& $adb shell pidof "${package}:game") | Out-String).Trim()
if ($running) { Write-Host "Game process still running (pid $running)" } else { Write-Warning 'Game process is not running' }

$latest = ((& $adb shell "ls -t '$appDir/Logs' 2>/dev/null | head -1") | Out-String).Trim()
if ($latest) {
    $pulled = Join-Path $runDir 'runtime-log'
    & $adb pull "$appDir/Logs/$latest" $pulled | Out-Null
    Write-Host "runtime log: $pulled"
}

Write-Host '--- app log (WiiCompiled / WiiCompiledQuest / SDL / crashes) ---'
Select-String -Path $logcat -Pattern '[VDIWEF]/(WiiCompiled|WiiCompiledQuest|SDL|AndroidRuntime|DEBUG|libc) *\(' |
    Select-Object -Last 120 | ForEach-Object { $_.Line }
