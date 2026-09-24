# Builds the pinned Dawn for the Quest (Android arm64) with Aurora's patches (aurora-main/patches/dawn).
# The stock prebuilt package has no fragment density maps, which the headset's foveated rendering
# attaches to the eye render passes. The result is an install tree in the stock package's layout;
# Build-Quest.ps1 hands it to the native build through -PmkwQuestDawnDir.
#
#   powershell -ExecutionPolicy Bypass -File android/Build-QuestDawn.ps1 [-WorkDirectory <dir>] [-Force]
#
# The package is cached. Its aurora-dawn.json records the source revision, the patch files' hash, the
# NDK and the flags it was built with, and a later run with the same inputs reuses it. That matters
# beyond build time: every Quest game kit fingerprints the Dawn archive, so a rebuilt archive would
# make players rebuild their games.
#
# Prerequisites: Git, Python 3.12+, CMake 3.25+, and the Android SDK with NDK 29.0.14206865 and CMake
# 3.22.1 (for Ninja). The first build compiles all of Dawn and Tint and takes a while.
[CmdletBinding()]
param(
    [string]$WorkDirectory = '',
    [string]$Python = 'python',
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $root '..')).Path
if (-not $WorkDirectory) { $WorkDirectory = Join-Path $repo '.scratch\quest-dawn' }
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
[IO.Directory]::CreateDirectory($WorkDirectory) | Out-Null

# The revision the stock package and the Windows Vulkan DLL (Launcher/Build-DawnVulkan.ps1) are
# built from, and the dawn-build CI's Android configuration.
$revision = '13abc3bc8ea2d3c2050f9e77a12d012108ceee24'
$archiveHash = '713bea5b92d4f6c5175752fd7cbf1c3c5ce36598ff5dd98685d8a1216614ebba'
$ndkVersion = '29.0.14206865'
$platform = 'android-28'
$flags = @(
    '-DCMAKE_BUILD_TYPE=Release',
    '-DANDROID_ABI=arm64-v8a',
    "-DANDROID_PLATFORM=$platform",
    '-DANDROID_STL=c++_shared',
    '-DDAWN_FETCH_DEPENDENCIES=ON',
    '-DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC',
    '-DBUILD_SHARED_LIBS=OFF',
    '-DDAWN_ENABLE_INSTALL=ON',
    '-DDAWN_BUILD_SAMPLES=OFF',
    '-DDAWN_BUILD_TESTS=OFF',
    '-DDAWN_BUILD_BENCHMARKS=OFF',
    '-DDAWN_USE_GLFW=OFF',
    '-DTINT_BUILD_TESTS=OFF',
    '-DTINT_BUILD_CMD_TOOLS=OFF',
    '-DTINT_BUILD_IR_BINARY=OFF',
    # Only the IR binary format needs protobuf, and building it would run the protoc it cross-compiled
    # for Android (the CI hands it a host protoc instead).
    '-DDAWN_BUILD_PROTOBUF=OFF'
)

$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$ndk = Join-Path $sdkRoot "ndk\$ndkVersion"
$toolchainFile = Join-Path $ndk 'build\cmake\android.toolchain.cmake'
if (-not (Test-Path $toolchainFile)) { throw "NDK $ndkVersion not found under $sdkRoot (set ANDROID_HOME)" }
$llvmBin = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$ninja = Get-ChildItem -Path (Join-Path $sdkRoot 'cmake') -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'bin\ninja.exe' } |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ninja) { throw "No Ninja under $sdkRoot\cmake; install the SDK's CMake 3.22.1" }
$pythonPath = (Get-Command $Python -ErrorAction Stop).Source

function Get-TextSha256([string]$text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))
        return -join ($bytes | ForEach-Object { $_.ToString('x2') })
    } finally { $sha.Dispose() }
}

$patchDir = Join-Path $repo 'aurora-main\patches\dawn'
$patchFiles = @(
    (Join-Path $patchDir 'apply.py'),
    (Join-Path $patchDir 'aurora_vulkan_hooks.h'),
    (Join-Path $patchDir 'aurora_vulkan_interop.inc'),
    (Join-Path $patchDir 'aurora_fdm.h'),
    (Join-Path $patchDir 'aurora_fdm.inc'),
    (Join-Path $repo 'aurora-main\include\aurora\dawn_vulkan_abi.h'),
    (Join-Path $repo 'aurora-main\include\aurora\dawn_fdm_abi.h')
)
# Line endings vary between checkouts; hash the patch text with LF only.
$patchHash = Get-TextSha256 (($patchFiles | ForEach-Object {
    "$([IO.Path]::GetFileName($_))`n$(([IO.File]::ReadAllText($_)).Replace("`r`n", "`n"))"
}) -join "`n")
$cacheKey = Get-TextSha256 "$revision|$archiveHash|$patchHash|$ndkVersion|$($flags -join ' ')"

$package = Join-Path $WorkDirectory 'package'
$manifestPath = Join-Path $package 'aurora-dawn.json'
$library = Join-Path $package 'lib\libwebgpu_dawn.a'
if (-not $Force -and (Test-Path $manifestPath) -and (Test-Path $library)) {
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $libraryHash = (Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($manifest.CacheKey -eq $cacheKey -and $manifest.ArchiveSha256 -eq $libraryHash) {
        Write-Host "Patched Dawn for the Quest is up to date: $package"
        return
    }
}

$archive = Join-Path $WorkDirectory 'dawn-source.tar.gz'
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest "https://github.com/google/dawn/archive/$revision.tar.gz" -OutFile $archive -UseBasicParsing
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $archiveHash) {
    throw 'Dawn source archive does not match the pinned SHA-256.'
}

# The patches are applied to pristine sources: src/ is extracted again whenever the patches changed,
# while third_party/, which DAWN_FETCH_DEPENDENCIES fills, is kept. A tree already patched with these
# patches is reused, so a flag change does not recompile all of Dawn.
$source = Join-Path $WorkDirectory "dawn-$revision"
$patchMarker = Join-Path $source 'aurora-patches.sha256'
$patched = (Test-Path -LiteralPath $patchMarker) -and ([IO.File]::ReadAllText($patchMarker).Trim() -eq $patchHash)
$extract = @'
import sys, tarfile
archive, destination, only_src = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
with tarfile.open(archive) as tar:
    members = tar.getmembers()
    if only_src:
        members = [m for m in members if m.name.split("/", 2)[1:2] == ["src"]]
    tar.extractall(destination, members=members, filter="data")
'@
if (-not $patched) {
    $extractScript = Join-Path $WorkDirectory 'extract.py'
    [IO.File]::WriteAllText($extractScript, $extract)
    $onlySource = Test-Path -LiteralPath $source
    if ($onlySource) { Remove-Item -LiteralPath (Join-Path $source 'src') -Recurse -Force -ErrorAction SilentlyContinue }
    & $pythonPath $extractScript $archive $WorkDirectory $(if ($onlySource) { '1' } else { '0' })
    if ($LASTEXITCODE -ne 0) { throw 'Dawn source extraction failed (Python 3.12+ required).' }
    & $pythonPath (Join-Path $patchDir 'apply.py') $source
    if ($LASTEXITCODE -ne 0) { throw 'Applying the Aurora Dawn patches failed.' }
    [IO.File]::WriteAllText($patchMarker, $patchHash)
}

$build = Join-Path $WorkDirectory 'build'
& cmake -S $source -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile" `
    "-DPython3_EXECUTABLE=$pythonPath" @flags "-DCMAKE_INSTALL_PREFIX=$package"
if ($LASTEXITCODE -ne 0) { throw 'Dawn configuration failed.' }
& cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Dawn build failed.' }
if (Test-Path -LiteralPath $package) { Remove-Item -LiteralPath $package -Recurse -Force }
& cmake --install $build
if ($LASTEXITCODE -ne 0) { throw 'Dawn installation failed.' }
if (-not (Test-Path -LiteralPath $library)) { throw "Dawn archive missing: $library" }
# As the stock package: debug info would multiply the archive the APK carries in its game kit.
& (Join-Path $llvmBin 'llvm-strip.exe') --strip-debug $library
if ($LASTEXITCODE -ne 0) { throw 'Stripping the Dawn archive failed.' }

$manifestJson = [ordered]@{
    SourceRevision = $revision
    SourceSha256 = $archiveHash
    PatchSha256 = $patchHash
    AuroraVulkanAbi = 1
    AuroraFdmAbi = 1
    Ndk = $ndkVersion
    Flags = ($flags -join ' ')
    CacheKey = $cacheKey
    ArchiveSha256 = (Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json
# No byte order mark: CMake's string(JSON) reads this file.
[IO.File]::WriteAllText($manifestPath, $manifestJson, (New-Object Text.UTF8Encoding $false))
Write-Host "Patched Dawn for the Quest ready: $package"
