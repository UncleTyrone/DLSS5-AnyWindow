param(
    [string]$Version,
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [switch]$SkipBuild,
    [long]$MaxZipBytes = 1900MB
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$sourceRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $sourceRoot

function Get-PackageVersion {
    param([string]$Requested)
    if ($Requested) { return $Requested.Trim() }
    $branch = (git rev-parse --abbrev-ref HEAD 2>$null)
    if ($branch -match '(?:v)?(\d+\.\d+\.\d+)') { return $Matches[1] }
    throw "Pass -Version, or use a branch name that contains a version such as dlss5-anywindow-1.10.0."
}

function Update-Changelog {
    param([string]$NotesPath, [string]$PackageVersion)
    if (!(Test-Path -LiteralPath $NotesPath)) { return }
    $previous = git tag --list "v*-dlss5-anywindow" | Sort-Object | Where-Object { $_ -ne "v$PackageVersion-dlss5-anywindow" } | Select-Object -Last 1
    $range = if ($previous) { "$previous..HEAD" } else { "HEAD" }
    $subjects = @(git log $range --pretty=format:"- %s") -join "`r`n"
    if (!$subjects) { $subjects = "- No commits since $previous." }
    $text = Get-Content -LiteralPath $NotesPath -Raw
    $block = "Commits since ``$previous``:`r`n`r`n$subjects`r`n"
    $updated = [regex]::Replace(
        $text,
        '(?s)<!-- changelog -->.*?<!-- /changelog -->',
        "<!-- changelog -->`r`n$block<!-- /changelog -->")
    if ($updated -eq $text) {
        Add-Content -LiteralPath $NotesPath -Value "`r`n<!-- changelog -->`r`n$block<!-- /changelog -->"
    } else {
        Set-Content -LiteralPath $NotesPath -Value $updated -NoNewline
    }
}

function Add-ZipGroup {
    param(
        [System.Collections.Generic.List[object]]$Groups,
        [string]$Name,
        [System.IO.FileInfo[]]$Files
    )
    $current = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    $bytes = 0L
    $part = 1
    foreach ($file in ($Files | Sort-Object FullName)) {
        if ($file.Length -gt $MaxZipBytes) {
            throw "$($file.FullName) is larger than the zip limit."
        }
        if ($bytes -gt 0 -and ($bytes + $file.Length) -gt $MaxZipBytes) {
            $Groups.Add([pscustomobject]@{ Name = "$Name-$part"; Files = @($current) })
            $current = New-Object System.Collections.Generic.List[System.IO.FileInfo]
            $bytes = 0
            $part++
        }
        $current.Add($file)
        $bytes += $file.Length
    }
    if ($current.Count -gt 0) {
        $suffix = if ($part -eq 1) { "" } else { "-$part" }
        $Groups.Add([pscustomobject]@{ Name = "$Name$suffix"; Files = @($current) })
    }
}

$running = Get-Process Magpie,MSBuild -ErrorAction SilentlyContinue
if ($running) {
    throw "Close Magpie and wait for MSBuild to finish before packaging."
}

$packageVersion = Get-PackageVersion $Version
$tag = "v$packageVersion-dlss5-anywindow"
$buildOutput = Join-Path $sourceRoot "bin\$Platform\$Configuration"
$outDir = Join-Path $sourceRoot "release\$tag"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if (!$SkipBuild) {
    $msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    if (!(Test-Path -LiteralPath $msbuild)) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    }
    if (!$msbuild) { throw "MSBuild was not found." }
    $vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    $project = Join-Path $sourceRoot "src\Magpie\Magpie.vcxproj"
    $solutionDir = $sourceRoot.TrimEnd('\') + '\'
    if (Test-Path -LiteralPath $vcvars) {
        cmd /c "`"$vcvars`" && `"$msbuild`" `"$project`" /p:Configuration=$Configuration /p:Platform=$Platform /p:SolutionDir=$solutionDir /m /v:minimal /nologo"
    } else {
        & $msbuild $project /p:Configuration=$Configuration /p:Platform=$Platform /p:SolutionDir=$solutionDir /m /v:minimal /nologo
    }
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
}

$notes = Join-Path $sourceRoot "docs\RELEASE_NOTES_v$packageVersion.md"
Update-Changelog $notes $packageVersion
if (Test-Path -LiteralPath $notes) {
    Copy-Item -LiteralPath $notes -Destination (Join-Path $outDir "RELEASE_NOTES.md") -Force
}

$hybridWeights = Join-Path $buildOutput "DLSSNR-Hybrid\weights.bin"
if (!(Test-Path -LiteralPath $hybridWeights)) {
    throw "DLSSNR-Hybrid\weights.bin is missing from the build output. Run tools\get_dlssnr_hybrid_assets.ps1 before packaging."
}
$licenseDir = Join-Path $sourceRoot "Licenses"
foreach ($notice in @(
    "janblade-OptiScaler-GPL-3.0.txt",
    "MFGAdaUnlock-RenoDx-MIT.txt"
)) {
    if (!(Test-Path -LiteralPath (Join-Path $licenseDir $notice))) {
        throw "Missing license notice Licenses\$notice."
    }
}

$remaining = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
Get-ChildItem -LiteralPath $buildOutput -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($buildOutput.Length).TrimStart('\')
    if ($rel -match '^(logs|cache)\\') { return }
    if ($_.Extension -in @(".pdb", ".lib", ".exp")) { return }
    $remaining.Add($_)
}

function Take-Matching([scriptblock]$Test) {
    $matched = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    $keep = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    foreach ($file in $remaining) {
        $rel = $file.FullName.Substring($buildOutput.Length).TrimStart('\')
        if (& $Test $file $rel) { $matched.Add($file) } else { $keep.Add($file) }
    }
    $script:remaining = $keep
    return @($matched)
}

$groups = New-Object System.Collections.Generic.List[object]
Add-ZipGroup $groups "tensorrt" (Take-Matching { param($f, $rel) $rel.StartsWith("FrameGuidance\TensorRT\", [StringComparison]::OrdinalIgnoreCase) })
Add-ZipGroup $groups "app" @($remaining)

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Get-ChildItem -LiteralPath $outDir -Filter "$tag-*.zip" -File -ErrorAction SilentlyContinue | Remove-Item -Force
$manifest = @()
foreach ($group in $groups) {
    $zipName = "$tag-$($group.Name).zip"
    $zipPath = Join-Path $outDir $zipName
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    $zip = [System.IO.Compression.ZipFile]::Open($zipPath, 'Create')
    try {
        foreach ($file in $group.Files) {
            $entryName = $file.FullName.Substring($buildOutput.Length).TrimStart('\').Replace('\', '/')
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $file.FullName, $entryName, [System.IO.Compression.CompressionLevel]::Optimal)
        }
        if ($group.Name -eq "app") {
            foreach ($notice in @(
                "janblade-OptiScaler-GPL-3.0.txt",
                "MFGAdaUnlock-RenoDx-MIT.txt"
            )) {
                $noticePath = Join-Path $licenseDir $notice
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $zip, $noticePath, "Licenses/$notice", [System.IO.Compression.CompressionLevel]::Optimal)
            }
            if (Test-Path -LiteralPath $notes) {
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $zip, $notes, "RELEASE_NOTES.md", [System.IO.Compression.CompressionLevel]::Optimal)
            }
        }
    } finally {
        $zip.Dispose()
    }
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    $manifest += [pscustomobject]@{
        file = $zipName
        bytes = (Get-Item -LiteralPath $zipPath).Length
        sha256 = $hash
    }
    Write-Host "$zipName  $hash"
}

$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outDir "SHA256SUMS.json") -Encoding utf8
Write-Host "Package written to $outDir"
