[CmdletBinding()]
param([string]$Tag, [string]$SetupPath)
$ErrorActionPreference = 'Stop'
$version = ([xml](Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Directory.Build.props') -Raw)).Project.PropertyGroup.Version
if ($Tag -and $Tag -cne "v$version") { throw "Release tag must be v$version, got $Tag" }
if ($SetupPath) {
    $reported = (& $SetupPath --version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $reported -cne $version) { throw 'Setup executable version mismatch.' }
    $info = (& $SetupPath --info-json | Out-String) | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $info.productId -cne 'wiicompiled-openxr-vr' -or
        $info.version -cne $version -or $info.openxrD3D12 -ne $true) { throw 'Setup VR identity mismatch.' }
    # Inspect the appended payload as well: a correctly versioned host can still wrap stale tools.
    Add-Type -AssemblyName System.IO.Compression
    $stream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $SetupPath))
    $payload = [IO.MemoryStream]::new()
    try {
        if ($stream.Length -lt 24) { throw 'Missing payload footer.' }
        $stream.Position = $stream.Length - 24
        $reader = [IO.BinaryReader]::new($stream, [Text.Encoding]::ASCII, $true)
        try {
            $magic = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(8))
            $offset = $reader.ReadInt64()
            $length = $reader.ReadInt64()
        } finally { $reader.Dispose() }
        if ($magic -cne 'MKWCPAY1' -or $offset -lt 0 -or $length -le 0 -or $offset + $length -ne $stream.Length - 24) {
            throw 'Invalid payload footer.'
        }
        $stream.Position = $offset
        $buffer = [byte[]]::new(1048576)
        while ($length -gt 0) {
            $count = $stream.Read($buffer, 0, [int][Math]::Min($length, $buffer.Length))
            if ($count -eq 0) { throw 'Truncated payload.' }
            $payload.Write($buffer, 0, $count)
            $length -= $count
        }
        $payload.Position = 0
        $zip = [IO.Compression.ZipArchive]::new($payload, [IO.Compression.ZipArchiveMode]::Read, $true)
        try {
            $entry = $zip.GetEntry('payload-manifest.json')
            if (-not $entry) { throw 'Missing payload manifest.' }
            $textReader = [IO.StreamReader]::new($entry.Open())
            try { $manifest = $textReader.ReadToEnd() | ConvertFrom-Json } finally { $textReader.Dispose() }
            if ($manifest.ProductId -cne $info.productId -or $manifest.ProductVersion -cne $version) {
                throw 'Payload identity/version does not match the executable.'
            }
        } finally { $zip.Dispose() }
    } finally { $payload.Dispose(); $stream.Dispose() }
}
Write-Output "Verified WiiCompiled OpenXR VR $version"
