# Downloads the verified NVFP4 hybrid bundle and extracts it beside Magpie.
# The files are not committed. Re-run this if bin\x64\Release\DLSSNR-Hybrid is missing.
$ErrorActionPreference = 'Stop'
$expectedHash = '3e735f825b85872d20c35c0d75404c1a28b7b6e11ca28ef06243a18225191037'
$root = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $root 'bin\x64\Release\DLSSNR-Hybrid'
$archive = Join-Path ([IO.Path]::GetTempPath()) 'OptiScaler-DLSSNR-v0.7.5-nr-fixes.zip'
if (-not (Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/releases/download/v0.7.5-nr-fixes/OptiScaler-DLSSNR-v0.7.5-nr-fixes.zip' -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw 'Hybrid source archive checksum mismatch.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $prefix = 'OptiScaler/nvfp4/hybrid/'
    $count = 0
    foreach ($entry in $zip.Entries) {
        if (-not $entry.FullName.StartsWith($prefix) -or -not $entry.Name) { continue }
        $relative = $entry.FullName.Substring($prefix.Length)
        $target = [IO.Path]::GetFullPath((Join-Path $destination ($relative -replace '/','\')))
        $assetRoot = [IO.Path]::GetFullPath($destination)
        if (-not $target.StartsWith($assetRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid hybrid asset path.' }
        New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($target)) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        $count++
    }
    if ($count -ne 15) { throw "Incomplete hybrid source archive ($count files)." }
    Write-Host "Extracted $count hybrid assets to $destination"
}
finally { $zip.Dispose() }
