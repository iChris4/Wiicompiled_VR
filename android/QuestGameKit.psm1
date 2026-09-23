# The Quest game kit: everything libmain.so links except the translated game, plus the recipe
# that compiles and links the player's own translation against it.
#
# The APK never contains translated Mario Kart code. Its assets carry this kit instead, and the
# player's libmain.so is built from their own disc, either on the headset (the launcher's
# on-device builder) or on a PC (Build-QuestGame.ps1, the PC launcher's "Build for Quest"), then
# loaded from the app's private storage. Both builders replay one recipe, kit.json, which is
# exported from CMake's own build graph so its flags and link order cannot drift from a normal
# build:
#
#  * the link line of the flavour's probe (runtime/cmake/PublicProducts.cmake), which is that
#    product without any translated code, becomes kit.json's link inputs, with {game:*} markers
#    where the game's objects sat in the product's own link;
#  * the compile commands CMake recorded for one source of each generated kind (translated shard,
#    registration shard, disc-generated runtime source, blob assembly) become kit.json's
#    compile flags, with include paths reduced to the kit's copy of runtime/include;
#  * kit.json's "sources" says which of the translator's build_shards lists fill each slot, so a
#    builder never has to know which flavour it is building.
#
# Placeholders in kit.json: {kit} the extracted kit, {sysroot} the NDK sysroot the compiler
# uses, {workspace} the directory holding the translator's generated/ tree.
#
# Windows PowerShell 5.1 compatible.

Set-StrictMode -Version Latest

$script:KitSchema = 3
$script:GameSchema = 1

# What each app flavour's kit is made of. Slots name the game's objects in the probe's link line,
# and Sources says which of the translator's build_shards lists ("@NAME") or generated files fill
# each of them, in link order. Retro Rewind adds a slot: the mod's own shards and the
# profile-sensitive base shards it replaces the base flavour's with, linked as objects rather
# than through the base archive.
$script:KitProfiles = @{
    base = @{
        Probe = 'libmkw_quest_kit_probe'
        ProductObjectPattern = '/base_product\.cpp\.o$'
        RegistrationPattern = '/build_shards/base_registration/[^/]+\.cpp$'
        Slots = @('runtime', 'product', 'translated')
        Sources = [ordered]@{
            runtime = @('{workspace}/generated/data_sections_init.cpp', '{workspace}/generated/guest_symbol_table.cpp',
                '{workspace}/generated/data_sections_init_blobs.S')
            product = @('@MKW_BASE_REGISTRATION_SOURCES')
            translated = @('@MKW_BASE_COMMON_SHARDS', '@MKW_BASE_PORTABLE_SENSITIVE_SHARDS')
        }
    }
    retro_rewind = @{
        Probe = 'libmkw_quest_kit_probe_retro'
        ProductObjectPattern = '/retro_rewind_product\.cpp\.o$'
        RegistrationPattern = '/build_shards/retro_rewind_registration/[^/]+\.cpp$'
        Slots = @('runtime', 'product', 'mod', 'translated')
        Sources = [ordered]@{
            runtime = @('{workspace}/generated/data_sections_init.cpp', '{workspace}/generated/guest_symbol_table.cpp',
                '{workspace}/generated/data_sections_init_blobs.S')
            product = @('@MKW_RETRO_REGISTRATION_SOURCES')
            mod = @('@MKW_RETRO_PORTABLE_SENSITIVE_SHARDS', '@MKW_RETRO_MOD_SHARDS', '@MKW_RETRO_EXTRA_SOURCES')
            translated = @('@MKW_BASE_COMMON_SHARDS')
        }
    }
}

function ConvertTo-ForwardPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).Replace('\', '/').TrimEnd('/')
}

function ConvertFrom-NinjaPath([string]$Text) {
    return $Text.Replace('$:', ':').Replace('$ ', ' ').Replace('$$', '$')
}

# Splits a CMake compile command. CMake quotes only arguments that need it and escapes inner
# quotes as \" (for example -DIMGUI_USER_CONFIG=\"aurora/imgui_config.h\").
function Split-CommandLine([string]$Command) {
    $arguments = New-Object System.Collections.Generic.List[string]
    $current = New-Object System.Text.StringBuilder
    $inQuotes = $false
    $pending = $false
    for ($i = 0; $i -lt $Command.Length; $i++) {
        $c = $Command[$i]
        if ($c -eq '\' -and $i + 1 -lt $Command.Length -and $Command[$i + 1] -eq '"') {
            [void]$current.Append('"'); $i++; $pending = $true; continue
        }
        if ($c -eq '"') { $inQuotes = -not $inQuotes; $pending = $true; continue }
        if (-not $inQuotes -and [char]::IsWhiteSpace($c)) {
            if ($pending) { $arguments.Add($current.ToString()); [void]$current.Clear(); $pending = $false }
            continue
        }
        [void]$current.Append($c); $pending = $true
    }
    if ($pending) { $arguments.Add($current.ToString()) }
    return ,$arguments.ToArray()
}

# One argument for a response file or a Windows command line (CommandLineToArgvW rules, which
# clang's GNU response-file tokenizer reads the same way for these arguments).
function ConvertTo-QuotedArgument([string]$Argument) {
    if ($Argument -notmatch '[\s"]') { return $Argument }
    return '"' + $Argument.Replace('"', '\"') + '"'
}

function Get-Sha256Hex([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try { $bytes = $sha.ComputeHash($stream) } finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Get-StringSha256Hex([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)) } finally { $sha.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

# Identity of a runtime/include tree. Translated code compiles against these headers, so a game is
# only built from a translation whose runtime headers are the kit's own.
function Get-RuntimeIncludeFingerprint([string]$Directory) {
    $lines = Get-RelativeFiles $Directory | Sort-Object FullName | ForEach-Object {
        $_.Relative + ' ' + (Get-Sha256Hex $_.FullName)
    }
    return Get-StringSha256Hex ($lines -join "`n")
}

# The files below a directory with their paths relative to it. .NET enumeration keeps the root
# exactly as given; Resolve-Path and Get-ChildItem can disagree about an 8.3 short name (TEMP),
# which would shift every relative path.
function Get-RelativeFiles([string]$Directory) {
    $root = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/')
    foreach ($path in [IO.Directory]::EnumerateFiles($root, '*', [IO.SearchOption]::AllDirectories)) {
        [pscustomobject]@{ FullName = $path; Relative = $path.Substring($root.Length + 1).Replace('\', '/') }
    }
}

# compile_commands.json as {file, command} objects. Windows PowerShell's ConvertFrom-Json refuses
# documents over 2 MB, so 5.1 uses the serializer underneath it with the limit lifted.
function Read-CompileCommands([string]$Path) {
    $text = [IO.File]::ReadAllText($Path)
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        return @($text | ConvertFrom-Json | ForEach-Object { [pscustomobject]@{ file = $_.file; command = $_.command } })
    }
    Add-Type -AssemblyName System.Web.Extensions
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = [int]::MaxValue
    return @($serializer.DeserializeObject($text) | ForEach-Object { [pscustomobject]@{ file = $_['file']; command = $_['command'] } })
}

# Compile flags of one recorded command, reduced to what a generated source needs anywhere.
function ConvertTo-KitCompileFlags {
    param([string[]]$Arguments, [string]$RuntimeInclude, [string]$Workspace)
    $flags = New-Object System.Collections.Generic.List[string]
    for ($i = 1; $i -lt $Arguments.Length; $i++) {
        $a = $Arguments[$i]
        switch -Regex -CaseSensitive ($a) {
            '^(-o|-c|-MT|-MF)$' { $i++; continue }
            '^(-MD|-MMD|-g|-Winvalid-pch)$' { continue }
            '^-isystem$' { $i++; continue }
            '^--sysroot=' { $flags.Add('--sysroot={sysroot}'); continue }
            '^-Xclang$' {
                if ($i + 3 -lt $Arguments.Length -and $Arguments[$i + 1] -eq '-include-pch') { $i += 3; continue }
                if ($i + 3 -lt $Arguments.Length -and $Arguments[$i + 1] -eq '-include') {
                    # CMake's cmake_pch.hxx only includes the precompiled header; builders parse it
                    # directly (a PCH is only valid for the compiler that wrote it).
                    $i += 3; $flags.Add('-include'); $flags.Add('mkw_pch.h'); continue
                }
                $flags.Add($a); continue
            }
            '^-I' {
                $path = ConvertTo-ForwardPath $a.Substring(2)
                if ($path -eq $RuntimeInclude) { $flags.Add('-I{kit}/include') }
                elseif ($path -eq $Workspace) { $flags.Add('-I{workspace}') }
                continue
            }
            default { $flags.Add($a) }
        }
    }
    return ,$flags.ToArray()
}

# The arguments of a `clang -###` command line: double-quoted, with backslash escapes.
function Split-DriverCommand([string]$Line) {
    $arguments = New-Object System.Collections.Generic.List[string]
    $current = New-Object System.Text.StringBuilder
    $inQuotes = $false
    for ($i = 0; $i -lt $Line.Length; $i++) {
        $c = $Line[$i]
        if ($inQuotes) {
            if ($c -eq '\' -and $i + 1 -lt $Line.Length) { [void]$current.Append($Line[++$i]) }
            elseif ($c -eq '"') { $arguments.Add($current.ToString()); [void]$current.Clear(); $inQuotes = $false }
            else { [void]$current.Append($c) }
        } elseif ($c -eq '"') { $inQuotes = $true }
    }
    return , $arguments.ToArray()
}

# The kit's link as a raw ld.lld command, for the headset. Android lets the app start its tools
# only through the system linker, so clang there cannot start lld itself and the builder runs lld
# directly. The NDK's own driver expands the link here, with the kit and marker objects standing in
# as real files. Placeholders: {kit}, {ndk} (the NDK's prebuilt toolchain directory, which holds
# sysroot/ and lib/clang/), {output}, and {game:runtime}, {game:product}, {game:translated}, each
# replaced by its object files (the translated ones already sit inside --start-lib/--end-lib).
function Get-KitLldArguments {
    param([string]$ClangCxx, [string]$KitDir, [string[]]$Flags, [string[]]$Inputs, [string[]]$Slots)
    $kit = ConvertTo-ForwardPath $KitDir
    $ndk = ConvertTo-ForwardPath (Join-Path (Split-Path -Parent $ClangCxx) '..')
    # Forward slashes throughout: clang reads response files with GNU quoting, where '\' escapes.
    $probe = ConvertTo-ForwardPath (Join-Path ([IO.Path]::GetTempPath()) ('mkw-kit-link-' + [Guid]::NewGuid().ToString('N')))
    New-Item -ItemType Directory $probe | Out-Null
    try {
        $markers = [ordered]@{}
        foreach ($slot in $Slots) {
            $markers[$slot] = ConvertTo-ForwardPath (Join-Path $probe "game_$slot.o")
            [IO.File]::WriteAllBytes($markers[$slot], [byte[]]@())
        }
        $real = { param([string]$s) $s.Replace('{kit}', $kit).Replace('{sysroot}', "$ndk/sysroot") }
        $arguments = New-Object System.Collections.Generic.List[string]
        foreach ($flag in $Flags) { $arguments.Add((& $real $flag)) }
        $arguments.Add('-o'); $arguments.Add("$probe/libmain.so")
        foreach ($linkInput in $Inputs) {
            $slot = [regex]::Match($linkInput, '^\{game:(\w+)\}$').Groups[1].Value
            if (-not $slot) { $arguments.Add((& $real $linkInput)); continue }
            # Only the base archive's slot keeps archive semantics; the rest are plain objects.
            if ($slot -eq 'translated') { $arguments.Add('-Wl,--start-lib') }
            $arguments.Add($markers[$slot])
            if ($slot -eq 'translated') { $arguments.Add('-Wl,--end-lib') }
        }
        $rsp = Join-Path $probe 'link.rsp'
        [IO.File]::WriteAllText($rsp, (($arguments | ForEach-Object { ConvertTo-QuotedArgument $_ }) -join "`n"), (New-Object Text.UTF8Encoding $false))

        # -### prints the command on stderr; Windows PowerShell turns redirected native stderr into errors.
        $start = New-Object Diagnostics.ProcessStartInfo $ClangCxx, "-### `"@$rsp`""
        $start.UseShellExecute = $false
        $start.RedirectStandardError = $true
        $start.RedirectStandardOutput = $true
        $process = [Diagnostics.Process]::Start($start)
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        [void]$stdout.Result
        if ($process.ExitCode -ne 0) { throw "clang++ -### failed for the kit link:`n$stderr" }
        $line = @($stderr -split "`r?`n" | Where-Object { $_ -match '^\s*"[^"]*ld(\.lld)?(\.exe)?"' }) | Select-Object -Last 1
        if (-not $line) { throw "clang++ -### printed no linker command:`n$stderr" }

        $lld = New-Object System.Collections.Generic.List[string]
        $driverArguments = Split-DriverCommand $line
        for ($i = 1; $i -lt $driverArguments.Length; $i++) {
            $argument = $driverArguments[$i].Replace('\', '/')
            if ($argument -eq "$probe/libmain.so") { $lld.Add('{output}'); continue }
            $slot = @($markers.Keys | Where-Object { $markers[$_] -eq $argument })
            if ($slot.Count -eq 1) { $lld.Add("{game:$($slot[0])}"); continue }
            $argument = [regex]::Replace($argument, [regex]::Escape($kit), '{kit}', 'IgnoreCase')
            $argument = [regex]::Replace($argument, [regex]::Escape($ndk), '{ndk}', 'IgnoreCase')
            if ($argument -match '[A-Za-z]:/') { throw "The kit link names a path outside the kit and the NDK: $argument" }
            $lld.Add($argument)
        }
        foreach ($required in @('{output}') + @($Slots | ForEach-Object { "{game:$_}" })) {
            if (-not $lld.Contains($required)) { throw "The expanded kit link lacks $required" }
        }
        return , $lld.ToArray()
    } finally {
        Remove-Item -Recurse -Force $probe
    }
}

function Export-QuestGameKit {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]$CMakeBinaryDir,
        [Parameter(Mandatory)] [string]$OutputDir,
        [Parameter(Mandatory)] [string]$RepoRoot,
        [Parameter(Mandatory)] [string]$LlvmStrip,
        [Parameter(Mandatory)] [string]$AndroidCpu
    )
    $ErrorActionPreference = 'Stop'
    $binary = ConvertTo-ForwardPath $CMakeBinaryDir
    $cache = [IO.File]::ReadAllText("$binary/CMakeCache.txt")
    $configuredCpu = [regex]::Match($cache, '(?m)^MKW_ANDROID_CPU(?::[^=\r\n]*)?=([^\r\n]+)\r?$').Groups[1].Value.Trim()
    if ($configuredCpu -ne $AndroidCpu) {
        throw "CMake tree $binary targets Android CPU '$configuredCpu', expected '$AndroidCpu'"
    }
    $runtimeInclude = ConvertTo-ForwardPath (Join-Path $RepoRoot 'runtime/include')
    $ninja = [IO.File]::ReadAllText("$binary/build.ninja")
    $commands = Read-CompileCommands "$binary/compile_commands.json"
    $rules = if (Test-Path "$binary/CMakeFiles/rules.ninja") { [IO.File]::ReadAllText("$binary/CMakeFiles/rules.ninja") } else { '' }

    function Find-Command([string]$Pattern) {
        $entry = @($commands | Where-Object { $_.file.Replace('\', '/') -match $Pattern })
        if ($entry.Count -eq 0) { throw "compile_commands.json has no source matching $Pattern" }
        return $entry[0]
    }
    $translatedEntry = Find-Command '/build_shards/base_common/[^/]+\.cpp$'
    $workspace = ConvertTo-ForwardPath ($translatedEntry.file.Replace('\', '/') -replace '/generated/build_shards/.*$', '')
    $flagsFor = {
        param($Entry)
        ConvertTo-KitCompileFlags -Arguments (Split-CommandLine $Entry.command) -RuntimeInclude $runtimeInclude -Workspace $workspace
    }

    if (Test-Path $OutputDir) { Remove-Item -Recurse -Force $OutputDir }
    New-Item -ItemType Directory -Force (Join-Path $OutputDir 'objects'), (Join-Path $OutputDir 'libs') | Out-Null
    Copy-Item -Recurse (Join-Path $RepoRoot 'runtime/include') (Join-Path $OutputDir 'include')

    # One copy of every object and archive, however many products link it: the two probes differ
    # only in their own product object.
    $copied = @{}
    $script:kitIndex = 0
    $copy = {
        param([string]$Source, [string]$Directory)
        $full = ConvertTo-ForwardPath $(if ([IO.Path]::IsPathRooted($Source)) { $Source } else { "$binary/$Source" })
        if ($copied.ContainsKey($full)) { return $copied[$full] }
        $script:kitIndex++
        $name = '{0:D3}_{1}' -f $script:kitIndex, [IO.Path]::GetFileName($full)
        $destination = Join-Path (Join-Path $OutputDir $Directory) $name
        & $LlvmStrip --strip-debug -o $destination $full
        if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed on $full" }
        $copied[$full] = "{kit}/$Directory/$name"
        return $copied[$full]
    }

    # One recipe per product this CMake tree built a probe for. The base one must be there; Retro
    # Rewind's exists only once translate-mod and emit-build-shards have run.
    $products = [ordered]@{}
    foreach ($product in 'base', 'retro_rewind') {
        $kitProfile = $script:KitProfiles[$product]
        $edge = [regex]::Match($ninja, "(?m)^build (\S*$([regex]::Escape($kitProfile.Probe))\.so): (\S+) ([^\r\n]*)\r?\n((?:  [^\r\n]*\r?\n)+)")
        if (-not $edge.Success) {
            if ($product -eq 'base') { throw "No $($kitProfile.Probe) link edge in $binary/build.ninja" }
            Write-Host "No $($kitProfile.Probe) in this build; the kit carries the base game only."
            continue
        }
        $explicit = ($edge.Groups[3].Value.Replace('$ ', '<ninja-space>') -split ' \|')[0]
        $objects = @($explicit -split ' ' | Where-Object { $_ } | ForEach-Object { ConvertFrom-NinjaPath $_.Replace('<ninja-space>', '$ ') })
        $vars = @{}
        foreach ($line in ($edge.Groups[4].Value -split "\r?\n")) {
            if ($line -match '^  (\w+) = (.*)$') { $vars[$Matches[1]] = $Matches[2] }
        }
        $rule = [regex]::Match($ninja, "(?ms)^rule $([regex]::Escape($edge.Groups[2].Value))\r?\n.*?command = ([^\r\n]*)")
        if (-not $rule.Success) {
            $rule = [regex]::Match($rules, "(?ms)^rule $([regex]::Escape($edge.Groups[2].Value))\r?\n.*?command = ([^\r\n]*)")
        }
        $target = [regex]::Match($rule.Groups[1].Value, '--target=(\S+)').Groups[1].Value
        if (-not $target) { throw "Cannot read the link target triple from rule $($edge.Groups[2].Value)" }

        # Link inputs in the probe's order, with the game's slots marked where the product has them.
        $inputs = New-Object System.Collections.Generic.List[string]
        $lastRuntime = -1
        for ($i = 0; $i -lt $objects.Count; $i++) {
            if ($objects[$i] -match '/mkw_runtime_common\.dir/') { $lastRuntime = $i }
        }
        for ($i = 0; $i -lt $objects.Count; $i++) {
            $inputs.Add((& $copy $objects[$i] 'objects'))
            if ($objects[$i] -match $kitProfile.ProductObjectPattern) { $inputs.Add('{game:product}') }
            if ($i -eq $lastRuntime) {
                $inputs.Add('{game:runtime}')
                # Retro Rewind links the mod's own objects after the runtime's, as its CMake target does.
                if ($kitProfile.Slots -contains 'mod') { $inputs.Add('{game:mod}') }
            }
        }
        foreach ($slot in $kitProfile.Slots) {
            if ($slot -ne 'translated' -and -not $inputs.Contains("{game:$slot}")) {
                throw "The $product probe link line lacks the objects the {game:$slot} slot follows"
            }
        }
        foreach ($token in ((ConvertFrom-NinjaPath $vars['LINK_LIBRARIES']) -split ' ' | Where-Object { $_ })) {
            if ($token.StartsWith('-')) { $inputs.Add($token); continue }
            $path = $token.Replace('\', '/')
            if ($path -match '/sysroot/(.+)$') { $inputs.Add("{sysroot}/$($Matches[1])") }
            else { $inputs.Add((& $copy $token 'libs')) }
            if ($path -match '/libmkw_platform\.a$') { $inputs.Add('{game:translated}') }
        }
        if (-not $inputs.Contains('{game:translated}')) { throw "The $product probe link line lacks libmkw_platform.a" }

        $linkFlags = New-Object System.Collections.Generic.List[string]
        $linkFlags.Add("--target=$target"); $linkFlags.Add('--sysroot={sysroot}'); $linkFlags.Add('-fPIC')
        foreach ($name in 'LANGUAGE_COMPILE_FLAGS', 'ARCH_FLAGS', 'LINK_FLAGS') {
            if (-not $vars.ContainsKey($name)) { continue }
            $tokens = Split-CommandLine (ConvertFrom-NinjaPath $vars[$name])
            for ($i = 0; $i -lt $tokens.Length; $i++) {
                if ($tokens[$i] -eq '-g' -or $tokens[$i] -eq '-Wl,--unresolved-symbols=ignore-all') { continue }
                if ($tokens[$i] -eq '-Xlinker' -and $i + 1 -lt $tokens.Length -and $tokens[$i + 1].StartsWith('--dependency-file')) { $i++; continue }
                $linkFlags.Add($tokens[$i])
            }
        }
        $linkFlags.Add('-Wl,-soname,libmain.so')

        $products[$product] = [ordered]@{
            output = 'libmain.so'
            target = $target
            compile = [ordered]@{
                translated = & $flagsFor $translatedEntry
                product = & $flagsFor (Find-Command $kitProfile.RegistrationPattern)
                runtime = & $flagsFor (Find-Command '/generated/data_sections_init\.cpp$')
                asm = & $flagsFor (Find-Command 'data_sections_init_blobs[^/]*\.S$')
            }
            sources = $kitProfile.Sources
            link = [ordered]@{
                flags = $linkFlags.ToArray()
                inputs = $inputs.ToArray()
                lld = Get-KitLldArguments -ClangCxx (Join-Path (Split-Path -Parent $LlvmStrip) 'clang++.exe') -KitDir $OutputDir `
                    -Flags $linkFlags.ToArray() -Inputs $inputs.ToArray() -Slots $kitProfile.Slots
            }
        }
    }

    # What the headset's translator needs besides the disc: the game manifest and symbol map, and
    # the runtime sources it indexes for native registrations, which must be the ones compiled into
    # this kit.
    $translation = Join-Path $OutputDir 'translation'
    New-Item -ItemType Directory -Force (Join-Path $translation 'projects/mkwii'), (Join-Path $translation 'runtime') | Out-Null
    Copy-Item (Join-Path $RepoRoot 'projects/mkwii/recomp.yml'), (Join-Path $RepoRoot 'projects/mkwii/MAP.txt') (Join-Path $translation 'projects/mkwii')
    Copy-Item -Recurse (Join-Path $RepoRoot 'runtime/src') (Join-Path $translation 'runtime/src')

    $recipe = [ordered]@{
        schema = $script:KitSchema
        androidCpu = $AndroidCpu
        runtimeIncludeFingerprint = Get-RuntimeIncludeFingerprint (Join-Path $OutputDir 'include')
        products = $products
    }
    $recipeJson = $recipe | ConvertTo-Json -Depth 8 -Compress
    $files = Get-RelativeFiles $OutputDir | Sort-Object FullName | ForEach-Object {
        "/$($_.Relative) $(Get-Sha256Hex $_.FullName)"
    }
    $recipe.fingerprint = Get-StringSha256Hex ($recipeJson + "`n" + ($files -join "`n"))
    [IO.File]::WriteAllText((Join-Path $OutputDir 'kit.json'), ($recipe | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))
    return [pscustomobject]@{ Fingerprint = $recipe.fingerprint; Products = @($products.Keys) }
}

# The generated source lists emit-build-shards wrote into shards.cmake.
function Read-ShardList {
    param([string]$ShardsCmake, [string]$Name)
    $text = [IO.File]::ReadAllText($ShardsCmake)
    $match = [regex]::Match($text, "(?s)set\($Name\s*(.*?)\)")
    if (-not $match.Success) { return @() }
    return @([regex]::Matches($match.Groups[1].Value, '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value.Replace('\', '/') })
}

function Invoke-QuestGameBuild {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]$KitDir,
        [Parameter(Mandatory)] [string]$GeneratedDir,
        [Parameter(Mandatory)] [string]$BuildDir,
        [Parameter(Mandatory)] [string]$ClangCxx,
        [Parameter(Mandatory)] [string]$ClangC,
        [Parameter(Mandatory)] [string]$Sysroot,
        [Parameter(Mandatory)] [string]$Ninja,
        [Parameter(Mandatory)] [int]$TranslatedJobs,
        [ValidateSet('base', 'retro_rewind')] [string]$Product = 'base'
    )
    $ErrorActionPreference = 'Stop'
    $kit = ConvertTo-ForwardPath $KitDir
    $generated = ConvertTo-ForwardPath $GeneratedDir
    $workspace = ConvertTo-ForwardPath (Split-Path -Parent $generated)
    $recipe = [IO.File]::ReadAllText("$kit/kit.json") | ConvertFrom-Json
    if ($recipe.schema -ne $script:KitSchema) { throw "Unsupported game kit schema $($recipe.schema)" }
    if ([string]::IsNullOrWhiteSpace($recipe.androidCpu)) { throw 'The game kit does not name its Android CPU target' }
    if (-not $recipe.products.PSObject.Properties.Name.Contains($Product)) {
        throw "This Quest app's game kit cannot build $Product (it has: $($recipe.products.PSObject.Properties.Name -join ', '))"
    }
    $productRecipe = $recipe.products.$Product
    foreach ($kind in 'translated', 'product', 'runtime', 'asm') {
        if (@($productRecipe.compile.$kind) -notcontains "-mcpu=$($recipe.androidCpu)") {
            throw "The game kit says CPU '$($recipe.androidCpu)' but its $kind compile flags do not match"
        }
    }
    # The translation must come from the same release as the kit: its code is compiled against the
    # kit's runtime headers and linked with the kit's runtime objects.
    $workspaceInclude = Join-Path $workspace 'runtime/include'
    if (Test-Path $workspaceInclude) {
        if ((Get-RuntimeIncludeFingerprint $workspaceInclude) -ne $recipe.runtimeIncludeFingerprint) {
            throw ('The Quest app and the translation on this PC come from different WiiCompiled releases ' +
                '(their runtime headers differ). Update both to the same release, then build again.')
        }
    }
    New-Item -ItemType Directory -Force $BuildDir | Out-Null
    $build = ConvertTo-ForwardPath $BuildDir
    # Ninja sees the response-file path in each command, not changes to that file's contents. A
    # different kit can therefore otherwise reuse objects compiled with another flavour's -mcpu.
    # Keep an explicit identity next to the build and discard every reusable native output when it
    # changes. WiiCompiled Setup deliberately reuses one BuildDir across APK selections.
    $identity = "$($recipe.fingerprint) $Product"
    $identityFile = "$build/kit-identity.txt"
    $previousIdentity = if (Test-Path $identityFile) { [IO.File]::ReadAllText($identityFile).Trim() } else { '' }
    if ($previousIdentity -ne $identity) {
        foreach ($stale in 'obj', 'libmain.so', '.ninja_deps', '.ninja_log') {
            $path = "$build/$stale"
            if (Test-Path $path) { Remove-Item -Recurse -Force $path }
        }
    }
    [IO.File]::WriteAllText($identityFile, $identity, (New-Object Text.UTF8Encoding $false))
    $expand = { param([string]$s) $s.Replace('{kit}', $kit).Replace('{sysroot}', (ConvertTo-ForwardPath $Sysroot)).Replace('{workspace}', $workspace) }

    foreach ($kind in 'translated', 'product', 'runtime', 'asm') {
        $lines = @($productRecipe.compile.$kind | ForEach-Object { ConvertTo-QuotedArgument (& $expand $_) })
        [IO.File]::WriteAllText("$build/$kind.rsp", ($lines -join "`n"), (New-Object Text.UTF8Encoding $false))
    }
    $linkLines = @($productRecipe.link.flags | ForEach-Object { ConvertTo-QuotedArgument (& $expand $_) })
    [IO.File]::WriteAllText("$build/link.rsp", ($linkLines -join "`n"), (New-Object Text.UTF8Encoding $false))

    # The Windows installer generates blob assembly for PE/COFF; every one of them is rewritten for
    # ELF exactly as runtime/cmake/PublicProducts.cmake does, into the build directory.
    $toElf = {
        param([string]$Source)
        $text = [IO.File]::ReadAllText($Source)
        $elf = $text.Replace('.section .rdata,"dr"', '.section .rodata,"a",@progbits')
        if ($elf -ne $text) { $elf += "`n.section .note.GNU-stack,`"`",@progbits`n" }
        $rewritten = "$build/$([IO.Path]::GetFileNameWithoutExtension($Source))_android.S"
        [IO.File]::WriteAllText($rewritten, $elf, (New-Object Text.UTF8Encoding $false))
        return $rewritten
    }

    # Each slot's sources, as the recipe names them: a build_shards list ("@NAME") or one generated
    # file.
    $shards = "$generated/build_shards/shards.cmake"
    $sources = [ordered]@{}
    foreach ($slot in $productRecipe.sources.PSObject.Properties.Name) {
        $slotSources = New-Object System.Collections.Generic.List[string]
        foreach ($entry in $productRecipe.sources.$slot) {
            if ($entry.StartsWith('@')) {
                Read-ShardList $shards $entry.Substring(1) | ForEach-Object { $slotSources.Add($_) }
            } else {
                $slotSources.Add((& $expand $entry))
            }
        }
        $sources[$slot] = @($slotSources | ForEach-Object { if ($_.EndsWith('.S')) { & $toElf $_ } else { $_ } })
    }
    if ($sources.translated.Count -eq 0) { throw "No translated shards in $shards" }
    # Online play needs the Retro-WFC payload translated into the mod (translate-mod
    # --retro-wfc-payload). Without it the mod downloads the payload at run time and jumps into
    # code that was never translated: the game crashes on entering Retro Rewind WFC.
    if ($Product -eq 'retro_rewind') {
        $dataPatches = @($sources.Values | ForEach-Object { $_ } |
            Where-Object { [IO.Path]::GetFileName($_) -eq 'mod_data_patches.cpp' })
        if ($dataPatches.Count -ne 1 -or
            -not (Select-String -LiteralPath $dataPatches[0] -Pattern 'kRetroWfcInitializerAddress' -SimpleMatch -Quiet)) {
            throw ('This Retro Rewind translation has no Retro-WFC payload, so online play would crash. ' +
                'Translate the mod again with its payload (translate-mod --retro-wfc-payload, or repair ' +
                'Retro Rewind in WiiCompiled with the payload download on), then build again.')
        }
    }
    foreach ($slot in $sources.Keys) {
        if ($sources[$slot].Count -eq 0) { throw "The $slot sources of a $Product game are missing from $shards" }
    }

    $ninjaText = New-Object System.Text.StringBuilder
    $escape = { param([string]$p) $p.Replace('$', '$$').Replace(':', '$:').Replace(' ', '$ ') }
    [void]$ninjaText.AppendLine('ninja_required_version = 1.10')
    [void]$ninjaText.AppendLine("pool translated`n  depth = $TranslatedJobs")
    # Depfiles, so a changed runtime header recompiles the shards that include it instead of
    # leaving objects built against the previous kit.
    [void]$ninjaText.AppendLine("rule cxx`n  command = $(ConvertTo-QuotedArgument $ClangCxx) @`$flags -MD -MF `$out.d -c `$in -o `$out`n  depfile = `$out.d`n  deps = gcc`n  description = Compiling `$in")
    [void]$ninjaText.AppendLine("rule asm`n  command = $(ConvertTo-QuotedArgument $ClangC) @`$flags -c `$in -o `$out`n  description = Assembling `$in")
    [void]$ninjaText.AppendLine("rule link`n  command = $(ConvertTo-QuotedArgument $ClangCxx) @`$flags -o `$out @`$out.rsp`n  rspfile = `$out.rsp`n  rspfile_content = `$inputs`n  description = Linking `$out")
    $objectsOf = @{}
    $seen = @{}
    foreach ($slot in $sources.Keys) {
        $objectsOf[$slot] = @()
        foreach ($source in $sources[$slot]) {
            $stem = [IO.Path]::GetFileNameWithoutExtension($source)
            $object = "obj/$stem.o"
            if ($seen.ContainsKey($object)) { throw "Two generated sources share the object name $object" }
            $seen[$object] = $true
            # Assembly is assembled; a mod shard compiles with the same flags as a translated one.
            $assembly = $source.EndsWith('.S')
            $rule = if ($assembly) { 'asm' } else { 'cxx' }
            $flags = if ($assembly) { 'asm.rsp' } elseif ($slot -eq 'mod') { 'translated.rsp' } else { "$slot.rsp" }
            [void]$ninjaText.AppendLine("build $(& $escape $object): $rule $(& $escape $source)`n  flags = $(& $escape "$build/$flags")")
            if ($slot -eq 'translated' -or $slot -eq 'mod') { [void]$ninjaText.AppendLine('  pool = translated') }
            $objectsOf[$slot] += "$build/$object"
        }
    }
    $linkInputs = New-Object System.Collections.Generic.List[string]
    foreach ($linkInput in $productRecipe.link.inputs) {
        $slot = [regex]::Match($linkInput, '^\{game:(\w+)\}$').Groups[1].Value
        if (-not $slot) { $linkInputs.Add((& $expand $linkInput)); continue }
        if (-not $objectsOf.ContainsKey($slot)) { throw "The kit recipe links a {game:$slot} slot it names no sources for" }
        # The base shards keep the archive semantics libmkw_base_shared.a has in a CMake build;
        # every other slot is linked as plain objects, as the product's own CMake target does.
        if ($slot -eq 'translated') { $linkInputs.Add('-Wl,--start-lib') }
        $objectsOf[$slot] | ForEach-Object { $linkInputs.Add($_) }
        if ($slot -eq 'translated') { $linkInputs.Add('-Wl,--end-lib') }
    }
    $allObjects = @($sources.Keys | ForEach-Object { $objectsOf[$_] } | ForEach-Object { & $escape $_.Substring($build.Length + 1) })
    $rspInputs = ($linkInputs | ForEach-Object { ConvertTo-QuotedArgument $_ }) -join ' '
    # The kit's objects and archives keep their names from one kit export to the next, so they are
    # implicit inputs of the link: a game built against an earlier kit is relinked, not reused.
    $kitInputs = @($linkInputs | Where-Object { $_.StartsWith($kit) } | ForEach-Object { & $escape $_ })
    [void]$ninjaText.AppendLine("build libmain.so: link $($allObjects -join ' ') | $($kitInputs -join ' ')`n  flags = $(& $escape "$build/link.rsp")`n  inputs = $($rspInputs.Replace('$', '$$'))")
    [IO.File]::WriteAllText("$build/build.ninja", $ninjaText.ToString(), (New-Object Text.UTF8Encoding $false))

    # Ninja's progress goes to the console, not into this function's return value; its [n/N]
    # lines are what WiiCompiled Setup turns into build progress.
    Write-Host 'MKWCBUILD:STEP:quest-compile Compiling the game for Quest'
    & $Ninja -C $build | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Game build failed (ninja exit $LASTEXITCODE)" }
    return "$build/libmain.so"
}

# A .wcgame: the built library and what it was built from, for the headset to import. It can also
# carry, written without compression, the extracted disc (-DataDir, as DATA/...) and the Retro
# Rewind pack (-ModDir, as MOD/...), so a player who builds on a PC copies one file to the headset
# and needs nothing else there.
function New-QuestGamePackage {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]$Library,
        [string]$DataDir = '',
        [string]$ModDir = '',
        [Parameter(Mandatory)] [string]$KitDir,
        [Parameter(Mandatory)] [string]$GameId,
        [Parameter(Mandatory)] [string]$DolSha256,
        [Parameter(Mandatory)] [string]$RelSha256,
        [Parameter(Mandatory)] [string]$BuiltBy,
        [Parameter(Mandatory)] [string]$OutputPath,
        [ValidateSet('base', 'retro_rewind')] [string]$Product = 'base'
    )
    $ErrorActionPreference = 'Stop'
    $recipe = [IO.File]::ReadAllText((Join-Path $KitDir 'kit.json')) | ConvertFrom-Json
    if ($ModDir -and $Product -ne 'retro_rewind') { throw 'Only a Retro Rewind game carries the mod pack.' }
    $game = [ordered]@{
        schema = $script:GameSchema
        profile = $Product
        gameId = $GameId
        dolSha256 = $DolSha256
        relSha256 = $RelSha256
        kitFingerprint = $recipe.fingerprint
        androidCpu = $recipe.androidCpu
        library = $recipe.products.$Product.output
        librarySha256 = Get-Sha256Hex $Library
        includesData = [bool]$DataDir
        includesMod = [bool]$ModDir
        builtBy = $BuiltBy
        builtAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    if (Test-Path $OutputPath) { Remove-Item -Force $OutputPath }
    New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($OutputPath))) | Out-Null
    $zip = [IO.Compression.ZipFile]::Open($OutputPath, 'Create')
    try {
        $entry = $zip.CreateEntry('game.json')
        $writer = New-Object IO.StreamWriter($entry.Open(), (New-Object Text.UTF8Encoding $false))
        try { $writer.Write(($game | ConvertTo-Json)) } finally { $writer.Dispose() }
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $Library, $game.library, 'Optimal')
        foreach ($tree in @{ Prefix = 'DATA'; Directory = $DataDir }, @{ Prefix = 'MOD'; Directory = $ModDir }) {
            if (-not $tree.Directory) { continue }
            foreach ($file in Get-RelativeFiles $tree.Directory) {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $zip, $file.FullName, "$($tree.Prefix)/$($file.Relative)", 'NoCompression')
            }
        }
    } finally { $zip.Dispose() }
    return $game
}

Export-ModuleMember -Function Export-QuestGameKit, Invoke-QuestGameBuild, New-QuestGamePackage
