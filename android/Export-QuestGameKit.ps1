# Exports the Quest game kit (see QuestGameKit.psm1) from a configured and built Android CMake
# tree. android/app/build.gradle.kts runs this after the native build and packages the result as
# the APK's game_kit assets. One kit carries a recipe per product the tree built a probe for:
# the base game always, and Retro Rewind when the translation includes the mod.
#
#   powershell -ExecutionPolicy Bypass -File android/Export-QuestGameKit.ps1 -CMakeBinaryDir <dir> -OutputDir <dir> -LlvmStrip <llvm-strip>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$CMakeBinaryDir,
    [Parameter(Mandatory)] [string]$OutputDir,
    [Parameter(Mandatory)] [string]$LlvmStrip,
    [Parameter(Mandatory)] [string]$AndroidCpu
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'QuestGameKit.psm1') -Force
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$kit = Export-QuestGameKit -CMakeBinaryDir $CMakeBinaryDir -OutputDir $OutputDir -RepoRoot $repo -LlvmStrip $LlvmStrip -AndroidCpu $AndroidCpu
Write-Host "Quest game kit $($kit.Fingerprint) [$($kit.Products -join ', '), CPU $AndroidCpu] -> $OutputDir"
