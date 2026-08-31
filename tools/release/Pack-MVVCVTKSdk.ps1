[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Stage,

    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,

    [Parameter(Mandatory = $true)]
    [string]$ArchiveName
)

$ErrorActionPreference = 'Stop'

function Get-SafeAncestors([string]$path)
{
    $fullPath = [IO.Path]::GetFullPath($path)
    $rootPath = [IO.Path]::GetPathRoot($fullPath)
    $currentPath = $rootPath
    $segments = $fullPath.Substring($rootPath.Length).Split(
        [char[]]@([IO.Path]::DirectorySeparatorChar),
        [StringSplitOptions]::RemoveEmptyEntries)
    foreach ($segment in $segments) {
        $currentPath = Join-Path $currentPath $segment
        if (-not [IO.Directory]::Exists($currentPath) -and
            -not [IO.File]::Exists($currentPath)) {
            break
        }
        $item = Get-Item -LiteralPath $currentPath -Force
        if (($item.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Path contains a reparse point: $currentPath"
        }
    }
}

function Get-SafeTreeItems([string]$root)
{
    $rootPath = [IO.Path]::GetFullPath($root)
    if (-not [IO.Directory]::Exists($rootPath)) {
        throw "Directory is missing: $rootPath"
    }
    Get-SafeAncestors $rootPath
    $items = @(
        (Get-Item -LiteralPath $rootPath -Force)
        (Get-ChildItem -LiteralPath $rootPath -Recurse -Force)
    )
    $reparseItems = @($items | Where-Object {
            ($_.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0
        })
    if ($reparseItems.Count -ne 0) {
        throw "Directory tree contains a reparse point: $($reparseItems[0].FullName)"
    }
    return $items
}

function Test-SafeRelativePath([string]$path)
{
    if ([string]::IsNullOrWhiteSpace($path) -or
        $path.Contains('\') -or
        $path.Contains(':') -or
        $path.Contains([char]0)) {
        return $false
    }
    $unsafeSegments = @($path.Split('/') | Where-Object {
            $_ -eq '' -or $_ -eq '.' -or $_ -eq '..'
        })
    return $unsafeSegments.Count -eq 0
}

$stageRoot = [IO.Path]::GetFullPath($Stage)
$packagePath = [IO.Path]::GetFullPath($PackageRoot)
if (-not [IO.Directory]::Exists($stageRoot)) {
    throw "SDK stage is missing: $stageRoot"
}
$stageBase = Split-Path -Parent $stageRoot
$outRoot = Split-Path -Parent $stageBase
$expectedPackagePath = [IO.Path]::GetFullPath(
    (Join-Path $outRoot 'packages'))
if (-not $packagePath.Equals(
        $expectedPackagePath,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "SDK package directory must be: $expectedPackagePath"
}
$null = @(Get-SafeTreeItems $stageRoot)
if ([IO.Path]::GetFileName($ArchiveName) -ne $ArchiveName -or
    -not $ArchiveName.EndsWith(
        '.zip',
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe SDK archive name: $ArchiveName"
}

$expectedTopLevel = @('deps', 'include', 'lib')
$actualTopLevel = @(
    Get-ChildItem -LiteralPath $stageRoot -Force |
        ForEach-Object { $_.Name } |
        Sort-Object
)
if (@(Compare-Object `
        ($expectedTopLevel | Sort-Object) $actualTopLevel).Count -ne 0) {
    throw 'SDK stage contains an unexpected top-level entry.'
}

Get-SafeAncestors (Split-Path -Parent $packagePath)
[IO.Directory]::CreateDirectory($packagePath) | Out-Null
Get-SafeAncestors $packagePath
$archivePath = Join-Path $packagePath $ArchiveName
$retiredChecksumPath = "$archivePath.sha256"
Get-SafeAncestors $archivePath
if ([IO.File]::Exists($archivePath)) {
    [IO.File]::Delete($archivePath)
}
if ([IO.File]::Exists($retiredChecksumPath)) {
    [IO.File]::Delete($retiredChecksumPath)
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$topDirectory = Split-Path -Leaf $stageRoot
$expectedEntries = [Collections.Generic.Dictionary[string, long]]::new(
    [StringComparer]::Ordinal)
$expectedHashes = [Collections.Generic.Dictionary[string, string]]::new(
    [StringComparer]::Ordinal)
$stagePrefix = $stageRoot
if (-not $stagePrefix.EndsWith(
    [string][IO.Path]::DirectorySeparatorChar)) {
    $stagePrefix += [IO.Path]::DirectorySeparatorChar
}

$stream = [IO.File]::Open(
    $archivePath,
    [IO.FileMode]::CreateNew,
    [IO.FileAccess]::ReadWrite,
    [IO.FileShare]::None)
try {
    $archive = [IO.Compression.ZipArchive]::new(
        $stream,
        [IO.Compression.ZipArchiveMode]::Create,
        $false)
    try {
        foreach ($file in (
            Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
                Sort-Object FullName)) {
            if (-not $file.FullName.StartsWith(
                $stagePrefix,
                [StringComparison]::OrdinalIgnoreCase)) {
                throw "Archive input escapes the SDK stage: $($file.FullName)"
            }
            $relativePath = $file.FullName.Substring(
                $stagePrefix.Length).Replace('\', '/')
            if (-not (Test-SafeRelativePath $relativePath)) {
                throw "Unsafe SDK archive input path: $relativePath"
            }
            $entryName = "$topDirectory/$relativePath"
            if ($expectedEntries.ContainsKey($entryName)) {
                throw "Duplicate SDK archive entry: $entryName"
            }
            Get-SafeAncestors $file.FullName
            $inputStream = [IO.File]::Open(
                $file.FullName,
                [IO.FileMode]::Open,
                [IO.FileAccess]::Read,
                [IO.FileShare]::None)
            try {
                $sourceHasher = [Security.Cryptography.SHA256]::Create()
                try {
                    $sourceHash = [BitConverter]::ToString(
                        $sourceHasher.ComputeHash($inputStream)).Replace('-', '')
                }
                finally {
                    $sourceHasher.Dispose()
                }
                $inputStream.Position = 0
                $entry = $archive.CreateEntry(
                    $entryName,
                    [IO.Compression.CompressionLevel]::Optimal)
                $entryStream = $entry.Open()
                try {
                    $inputStream.CopyTo($entryStream)
                }
                finally {
                    $entryStream.Dispose()
                }
                $expectedEntries.Add($entryName, $inputStream.Length)
                $expectedHashes.Add($entryName, $sourceHash)
            }
            finally {
                $inputStream.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    $stream.Dispose()
}

$readStream = [IO.File]::Open(
    $archivePath,
    [IO.FileMode]::Open,
    [IO.FileAccess]::Read,
    [IO.FileShare]::None)
try {
    $readArchive = [IO.Compression.ZipArchive]::new(
        $readStream,
        [IO.Compression.ZipArchiveMode]::Read,
        $false)
    try {
        if ($readArchive.Entries.Count -ne $expectedEntries.Count) {
            throw 'SDK archive entry count does not match the staging tree.'
        }
        $seenEntries = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::Ordinal)
        foreach ($entry in $readArchive.Entries) {
            if (-not $seenEntries.Add($entry.FullName) -or
                -not $expectedEntries.ContainsKey($entry.FullName) -or
                $entry.Length -ne $expectedEntries[$entry.FullName]) {
                throw "Unexpected SDK archive entry: $($entry.FullName)"
            }
            $entryStream = $entry.Open()
            try {
                $entryHasher = [Security.Cryptography.SHA256]::Create()
                try {
                    $entryHash = [BitConverter]::ToString(
                        $entryHasher.ComputeHash($entryStream)).Replace('-', '')
                }
                finally {
                    $entryHasher.Dispose()
                }
            }
            finally {
                $entryStream.Dispose()
            }
            if ($entryHash -ne $expectedHashes[$entry.FullName]) {
                throw "SDK archive content mismatch: $($entry.FullName)"
            }
        }
    }
    finally {
        $readArchive.Dispose()
    }
}
finally {
    $readStream.Dispose()
}

Write-Host "MVVCVTK SDK archive: $archivePath"
Write-Host "MVVCVTK SDK archive entries: $($expectedEntries.Count)"
