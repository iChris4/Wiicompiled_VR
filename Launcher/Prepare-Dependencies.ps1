# Populates Launcher/artifacts/dependencies: the pinned, unpatched upstream source trees (plus the
# prebuilt Dawn package and generated C++/WinRT headers) that Build-Installer.ps1 ships inside the
# installer so the user's build runs with FETCHCONTENT_FULLY_DISCONNECTED=ON. Every pin is the one
# its owning CMake file declares; the Pin fields below are asserted against those files so the two
# cannot drift apart silently. native_prebuilt is not fetched here - Prepare-NativePrebuilt.ps1
# compiles it from these trees.
[CmdletBinding()]
param(
    [string]$Destination,
    # Windows metadata source for cppwinrt.exe: 'local' (this machine's WinMetadata), 'sdk', or an
    # installed SDK version such as 10.0.26100.0.
    [string]$CppWinRtInput = 'local',
    # Output of Build-DawnVulkan.ps1. Rebuild native_prebuilt after changing Dawn.
    [string]$DawnVulkanPackage
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$auroraCMake = Join-Path $repoRoot 'aurora-main\CMakeLists.txt'
$auroraExtern = Join-Path $repoRoot 'aurora-main\extern\CMakeLists.txt'
$auroraDawn = Join-Path $repoRoot 'aurora-main\cmake\AuroraDawnProvider.cmake'
$auroraLibUsb = Join-Path $repoRoot 'aurora-main\cmake\AuroraLibUSB.cmake'
$auroraSdl = Join-Path $repoRoot 'aurora-main\cmake\AuroraSDL3Provider.cmake'
$runtimeCMake = Join-Path $repoRoot 'runtime\CMakeLists.txt'
$voiceCMake = Join-Path $repoRoot 'runtime\cmake\VoiceChat.cmake'
$mbedtlsPin = Join-Path $repoRoot 'runtime\cmake\MbedTLSPin.cmake'
if (-not (Test-Path -LiteralPath $auroraCMake -PathType Leaf)) {
    throw 'Aurora sources are missing: this fork vendors them in aurora-main\. Restore that directory first.'
}

# Name = directory under the destination; FetchContent maps it back through
# FETCHCONTENT_SOURCE_DIR_<UPPERCASE NAME> (NativeBuildFlags.ps1), so the names are the declared
# FetchContent names, not the upstream project names.
$packages = @(
    [pscustomobject]@{
        Name = 'vulkan_headers'; File = 'vulkan-headers-015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9.tar.gz'
        Sha256 = '666700236101d288dea8bb080c6f8039467cb1d29ea1e1aae6f93a819f4f9029'
        Uris = @('https://github.com/KhronosGroup/Vulkan-Headers/archive/015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9.tar.gz')
        Pins = @(@{ File = $runtimeCMake; Text = 'Vulkan-Headers/archive/015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'SDL'; File = 'SDL3-3.4.4.tar.gz'
        Sha256 = 'ee712dbe6a89bb140bbfc2ce72358fb5ee5cc2240abeabd54855012db30b3864'
        Uris = @('https://github.com/libsdl-org/SDL/releases/download/release-3.4.4/SDL3-3.4.4.tar.gz')
        Pins = @(@{ File = $auroraCMake; Text = 'set(AURORA_SDL3_VERSION "3.4.4"' },
                 @{ File = $auroraSdl; Text = 'releases/download/release-${AURORA_SDL3_VERSION}/SDL3-${AURORA_SDL3_VERSION}.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'abseil-cpp'; File = 'abseil-cpp-20240722.0.tar.gz'
        Sha256 = 'f50e5ac311a81382da7fa75b97310e4b9006474f9560ac46f54a9967f07d4ae3'
        Uris = @('https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'dawn_prebuilt'; File = 'dawn-v20260603.191052-windows-amd64.tar.gz'
        Sha256 = '13be9cff8b9b179c42dcd16aeabb6effcc8f0dfdcc14463eda2a5caeda225142'
        Uris = @('https://github.com/theofficialgman/dawn-build/releases/download/v20260603.191052/dawn-windows-amd64.tar.gz')
        Pins = @(@{ File = $auroraCMake; Text = 'set(AURORA_DAWN_VERSION "v20260603.191052"' },
                 @{ File = $auroraDawn; Text = 'SHA256=13be9cff8b9b179c42dcd16aeabb6effcc8f0dfdcc14463eda2a5caeda225142' })
    },
    [pscustomobject]@{
        Name = 'fmt'; File = 'fmt-11.1.4.tar.gz'
        Sha256 = 'ac366b7b4c2e9f0dde63a59b3feb5ee59b67974b14ee5dc9ea8ad78aa2c1ee1e'
        Uris = @('https://github.com/fmtlib/fmt/archive/refs/tags/11.1.4.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/fmtlib/fmt/archive/refs/tags/11.1.4.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'freetype'; File = 'freetype-2.14.3.tar.gz'
        Sha256 = 'e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1'
        Uris = @('https://files.twilitrealm.dev/freetype-2.14.3.tar.gz',
                 'https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.gz',
                 'https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://files.twilitrealm.dev/freetype-2.14.3.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'imgui'; File = 'imgui-1.91.9b-docking.tar.gz'
        Sha256 = '466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5'
        Uris = @('https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'libusb'; File = 'libusb-1.0.30.tar.bz2'
        Sha256 = 'fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf'
        Uris = @('https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2')
        Pins = @(@{ File = $auroraCMake; Text = 'set(AURORA_LIBUSB_VERSION "1.0.30"' },
                 @{ File = $auroraLibUsb; Text = 'releases/download/v${AURORA_LIBUSB_VERSION}/libusb-${AURORA_LIBUSB_VERSION}.tar.bz2' })
    },
    [pscustomobject]@{
        Name = 'openxr'; File = 'OpenXR-SDK-release-1.1.61.tar.gz'
        Sha256 = 'd5e2773d282642e6e250bd10266f316fc3ff3b8602c83db009a1f434205998e6'
        Uris = @('https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.61.tar.gz')
        Pins = @(@{ File = $runtimeCMake; Text = 'OpenXR-SDK/archive/refs/tags/release-1.1.61.tar.gz' },
                 @{ File = $runtimeCMake; Text = 'SHA256=d5e2773d282642e6e250bd10266f316fc3ff3b8602c83db009a1f434205998e6' })
    },
    [pscustomobject]@{
        Name = 'png'; File = 'libpng-1.6.58.tar.gz'
        Sha256 = 'a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf'
        Uris = @('https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'sqlite3'; File = 'sqlite-amalgamation-3510300.zip'
        Sha256 = 'acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4'
        Uris = @('https://sqlite.org/2026/sqlite-amalgamation-3510300.zip')
        Pins = @(@{ File = $auroraExtern; Text = 'https://sqlite.org/2026/sqlite-amalgamation-3510300.zip' })
    },
    [pscustomobject]@{
        Name = 'tracy'; File = 'tracy-a64b9a20294d59421a2f57aeca3c6383d8c48169.tar.gz'
        Sha256 = '24d342b5127d7f659dc3cf94f24347b348cc736f25b17a691fd7f69541937658'
        Uris = @('https://github.com/wolfpld/tracy/archive/a64b9a20294d59421a2f57aeca3c6383d8c48169.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/wolfpld/tracy/archive/a64b9a20294d59421a2f57aeca3c6383d8c48169.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'xxhash'; File = 'xxHash-0.8.4.tar.gz'
        Sha256 = '5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32'
        Uris = @('https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'zlib'; File = 'zlib-1.3.2.tar.gz'
        Sha256 = 'bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16'
        Uris = @('https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'zstd'; File = 'zstd-1.5.7.tar.gz'
        Sha256 = 'eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3'
        Uris = @('https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz' })
    },
    # Room voice chat (runtime/cmake/VoiceChat.cmake). mbed TLS is the pin the non-Windows network
    # HLE already uses; Windows needs it for voice chat's WebRTC transport only.
    [pscustomobject]@{
        Name = 'mkw_mbedtls_upstream'; File = 'mbedtls-3.6.7.tar.bz2'
        Sha256 = 'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6'
        Uris = @('https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2')
        Pins = @(@{ File = $mbedtlsPin; Text = 'set(MKW_MBEDTLS_VERSION "3.6.7")' },
                 @{ File = $mbedtlsPin; Text = 'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6' })
    },
    [pscustomobject]@{
        Name = 'mkw_opus'; File = 'opus-1.6.1.tar.gz'
        Sha256 = 'bf0b97ec7a65890b8db90ef94c4d6c18de12584c3085031953a10986f5917745'
        Uris = @('https://github.com/xiph/opus/archive/v1.6.1.tar.gz')
        Pins = @(@{ File = $voiceCMake; Text = 'SHA256=bf0b97ec7a65890b8db90ef94c4d6c18de12584c3085031953a10986f5917745' })
    },
    [pscustomobject]@{
        Name = 'mkw_miniaudio'; File = 'miniaudio-0.11.25.tar.gz'
        Sha256 = 'b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274'
        Uris = @('https://github.com/mackron/miniaudio/archive/0.11.25.tar.gz')
        Pins = @(@{ File = $voiceCMake; Text = 'SHA256=b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274' })
        Prune = @('examples', 'research', 'tests', 'website')
    },
    [pscustomobject]@{
        Name = 'mkw_rnnoise'; File = 'rnnoise-0.2.tar.gz'
        Sha256 = 'e4a17eb1493b7551f9f7781d2acc67b8847d6ed7c4b64db0fd62c017021ae1ec'
        Uris = @('https://github.com/xiph/rnnoise/archive/refs/tags/v0.2.tar.gz')
        Pins = @(@{ File = $voiceCMake; Text = 'SHA256=e4a17eb1493b7551f9f7781d2acc67b8847d6ed7c4b64db0fd62c017021ae1ec' })
    },
    [pscustomobject]@{
        Name = 'mkw_rnnoise_model'; File = 'rnnoise_data-0b50c45.tar.gz'
        Sha256 = '4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43'
        Uris = @('https://media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz')
        Pins = @(@{ File = $voiceCMake; Text = 'SHA256=4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43' })
        # Only src/rnnoise_data.[ch] is compiled: not the training checkpoint or the small model.
        Prune = @('models', 'src\rnnoise_data_little.c', 'src\rnnoise_data_little.h')
    },
    [pscustomobject]@{
        Name = 'mkw_plog'; File = 'plog-94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.tar.gz'
        Sha256 = '92a08bce559b5f28aa88d3fd9071567414b9f43a83fe2a05a6dd14f1da536072'
        Uris = @('https://github.com/SergiusTheBest/plog/archive/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.tar.gz')
        Pins = @(@{ File = $voiceCMake; Text = 'SHA256=92a08bce559b5f28aa88d3fd9071567414b9f43a83fe2a05a6dd14f1da536072' })
    },
    [pscustomobject]@{
        Name = 'mkw_usrsctp'; File = 'usrsctp-fec583d54493f879d2ae44a743423bf8a04371ab.tar.gz'
        Sha256 = 'e5c114afe73c9a0ec419fab5f5b3f63f3ce57b09b90d06ea85e63dca8aed3e7c'
        Uris = @('https://github.com/sctplab/usrsctp/archive/fec583d54493f879d2ae44a743423bf8a04371ab.tar.gz')
        Pins = @(@{ File = $voiceCMake; Text = 'SHA256=e5c114afe73c9a0ec419fab5f5b3f63f3ce57b09b90d06ea85e63dca8aed3e7c' })
    },
    [pscustomobject]@{
        Name = 'mkw_libjuice'; File = 'libjuice-1.7.2.tar.gz'
        Sha256 = '75159867c4a5a689a6559e11aa0d30c9eba12ce73a4ae3d898b521467e1f635d'
        Uris = @('https://github.com/paullouisageneau/libjuice/archive/refs/tags/v1.7.2.tar.gz')
        Pins = @(@{ File = $voiceCMake; Text = 'SHA256=75159867c4a5a689a6559e11aa0d30c9eba12ce73a4ae3d898b521467e1f635d' })
    },
    [pscustomobject]@{
        Name = 'mkw_libdatachannel'; File = 'libdatachannel-0.24.5.tar.gz'
        Sha256 = '454537c3cd526bed935d847bb2dff4046f266eef84d43b2a5f2f2f293c0026f4'
        Uris = @('https://github.com/paullouisageneau/libdatachannel/archive/refs/tags/v0.24.5.tar.gz')
        Pins = @(@{ File = $voiceCMake; Text = 'SHA256=454537c3cd526bed935d847bb2dff4046f266eef84d43b2a5f2f2f293c0026f4' })
        # 50 MB of browser examples; the library builds with NO_EXAMPLES and NO_TESTS.
        Prune = @('examples', 'pages', 'test')
    }
)

# The C++/WinRT compiler (NuGet package = zip) that generates the projection headers.
$cppWinRtTool = [pscustomobject]@{
    Name = 'cppwinrt-tool'; File = 'Microsoft.Windows.CppWinRT.3.0.260818.1.nupkg'
    Sha256 = 'a8993608ae9263a8288e1a1d9ee2684a5632f4acc59e1e89ddd86b3b889a7367'
    Uris = @('https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/3.0.260818.1')
    Pins = @()
}

if (-not $Destination) { $Destination = Join-Path $PSScriptRoot 'artifacts\dependencies' }
$Destination = [IO.Path]::GetFullPath($Destination)
$downloads = Join-Path $PSScriptRoot 'artifacts\downloads'
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path -LiteralPath $tar -PathType Leaf)) { throw "Windows archive tool is missing: $tar" }
[IO.Directory]::CreateDirectory($Destination) | Out-Null
[IO.Directory]::CreateDirectory($downloads) | Out-Null

function Assert-Pinned($Package) {
    foreach ($pin in $Package.Pins) {
        if (-not (Test-Path -LiteralPath $pin.File -PathType Leaf)) { throw "Pin source is missing: $($pin.File)" }
        $content = [IO.File]::ReadAllText($pin.File)
        if (-not $content.Contains($pin.Text)) {
            throw "$($Package.Name) is pinned to '$($pin.Text)' here but $($pin.File) no longer declares it; update both."
        }
    }
}

function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# Every archive this returns, cached or downloaded, matches its pinned Sha256.
function Get-Archive($Package) {
    $archive = Join-Path $downloads $Package.File
    if (Test-Path -LiteralPath $archive -PathType Leaf) {
        $actual = Get-Sha256 $archive
        if ($actual -eq $Package.Sha256) { return $archive }
        Write-Warning "$($Package.Name): cached $archive has SHA-256 $actual, expected $($Package.Sha256); discarding it and downloading again."
        Remove-Item -LiteralPath $archive -Force
    }
    $temporary = $archive + '.partial'
    $failures = @()
    $mismatch = $false
    foreach ($uri in $Package.Uris) {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        Write-Host "Downloading $($Package.Name) from $uri..."
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $temporary
        } catch {
            $failures += "$uri ($($_.Exception.Message))"
            continue
        }
        $actual = Get-Sha256 $temporary
        if ($actual -ne $Package.Sha256) {
            $failures += "$uri (SHA-256 $actual, expected $($Package.Sha256))"
            $mismatch = $true
            continue
        }
        Move-Item -LiteralPath $temporary -Destination $archive -Force
        return $archive
    }
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    $hint = ''
    if ($mismatch) {
        $hint = ' If upstream republished this archive, check the new file against an upstream-published checksum before updating its Sha256 in Prepare-Dependencies.ps1.'
    }
    throw "$($Package.Name) could not be fetched: $($failures -join '; ').$hint"
}

function Expand-Package([string]$Archive, [string]$Target) {
    # Release archives wrap everything in one versioned directory (SDL3-3.4.4/, tracy-<sha>/) while
    # dawn-build's package is flat; either way the tree lands directly under the target.
    $extract = Join-Path (Split-Path -Parent $Target) ('.extract-' + (Split-Path -Leaf $Target))
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    [IO.Directory]::CreateDirectory($extract) | Out-Null
    # Symbolic links need a privilege Windows tar usually lacks; the only ones in these archives
    # are zstd test aliases, and none are build inputs.
    $listing = & $tar -tvf $Archive
    if ($LASTEXITCODE -ne 0) { throw "Listing $Archive failed with exit code $LASTEXITCODE." }
    $excludes = @($listing | ForEach-Object { if ($_ -match '^l.*\s(\S+)\s->\s') { '--exclude=' + $Matches[1] } })
    & $tar -xf $Archive -C $extract @excludes
    if ($LASTEXITCODE -ne 0) { throw "Extracting $Archive failed with exit code $LASTEXITCODE." }
    $entries = @(Get-ChildItem -LiteralPath $extract -Force)
    $source = $extract
    if ($entries.Count -eq 1 -and $entries[0].PSIsContainer) { $source = $entries[0].FullName }
    if (Test-Path -LiteralPath $Target) { Remove-Item -LiteralPath $Target -Recurse -Force }
    Move-Item -LiteralPath $source -Destination $Target
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
}

# Records which archive a tree came from, so a pin change replaces a stale tree.
function Get-Stamp($Package) { "$($Package.File) $($Package.Sha256)" }

function Test-Stamp([string]$Target, [string]$Text) {
    $stamp = $Target + '.source'
    (Test-Path -LiteralPath $stamp -PathType Leaf) -and ([IO.File]::ReadAllText($stamp) -eq $Text)
}

function Install-Package($Package, [string]$Target, [string]$RequiredFile) {
    $ready = if ($RequiredFile) { Test-Path -LiteralPath $RequiredFile -PathType Leaf } else { Test-Path -LiteralPath $Target -PathType Container }
    if ($ready -and (Test-Stamp $Target (Get-Stamp $Package))) { return }
    Expand-Package (Get-Archive $Package) $Target
    # Parts of an archive no build reads (examples, tests, unused data), dropped from the tree the
    # installer ships.
    if ($Package.PSObject.Properties['Prune']) {
        foreach ($relative in $Package.Prune) {
            $path = Join-Path $Target $relative
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        }
    }
    [IO.File]::WriteAllText($Target + '.source', (Get-Stamp $Package))
    Write-Host "Prepared $($Package.Name)"
}

foreach ($package in $packages) {
    Assert-Pinned $package
    $target = Join-Path $Destination $package.Name
    if ($package.Name -eq 'dawn_prebuilt' -and $DawnVulkanPackage) {
        $custom = [IO.Path]::GetFullPath($DawnVulkanPackage)
        $manifest = Get-Content -LiteralPath (Join-Path $custom 'aurora-vulkan.json') -Raw | ConvertFrom-Json
        $dll = Join-Path $custom 'bin\webgpu_dawn.dll'
        if ($manifest.SourceRevision -ne '13abc3bc8ea2d3c2050f9e77a12d012108ceee24' -or
            $manifest.AuroraVulkanAbi -ne 1 -or
            (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest.DllSha256) {
            throw 'Custom Dawn package provenance or DLL hash does not match.'
        }
        if (Test-Path -LiteralPath $target) {
            throw "Use a fresh dependency destination for custom Dawn: $target already exists."
        }
        Copy-Item -LiteralPath $custom -Destination $target -Recurse
        Write-Host 'Prepared custom Dawn with Windows Vulkan OpenXR support'
        continue
    }
    Install-Package $package $target
}

$cppWinRt = Join-Path $Destination 'cppwinrt'
$cppWinRtCurrent = (Test-Path -LiteralPath (Join-Path $cppWinRt 'winrt\base.h') -PathType Leaf) -and
    (Test-Stamp $cppWinRt (Get-Stamp $cppWinRtTool))
if (-not $cppWinRtCurrent) {
    $toolRoot = Join-Path $PSScriptRoot 'artifacts\cppwinrt-tool'
    $compiler = Join-Path $toolRoot 'bin\cppwinrt.exe'
    Install-Package $cppWinRtTool $toolRoot $compiler
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "cppwinrt.exe is missing: $compiler" }
    if (Test-Path -LiteralPath $cppWinRt) { Remove-Item -LiteralPath $cppWinRt -Recurse -Force }
    [IO.Directory]::CreateDirectory($cppWinRt) | Out-Null
    Write-Host "Generating C++/WinRT headers (-input $CppWinRtInput)..."
    & $compiler -input $CppWinRtInput -output $cppWinRt
    if ($LASTEXITCODE -ne 0) { throw "cppwinrt.exe failed with exit code $LASTEXITCODE." }
    if (-not (Test-Path -LiteralPath (Join-Path $cppWinRt 'winrt\base.h') -PathType Leaf)) {
        throw "cppwinrt.exe produced no winrt\base.h under $cppWinRt"
    }
    Copy-Item -LiteralPath (Join-Path $toolRoot 'LICENSE') -Destination (Join-Path $cppWinRt 'LICENSE.txt')
    [IO.File]::WriteAllText($cppWinRt + '.source', (Get-Stamp $cppWinRtTool))
    Write-Host 'Prepared cppwinrt'
}

Write-Host "Pinned dependency sources ready: $Destination"
