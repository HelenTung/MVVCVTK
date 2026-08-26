[CmdletBinding()]
param(
    [string]$Uri = $env:MVVCVTK_DEPS_URI,
    [string]$ArchivePath,
    [string]$RepoRoot = (Join-Path $PSScriptRoot '..\..'),
    [switch]$VerifyOnly,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$lockPath = Join-Path $PSScriptRoot 'MVVCVTK.Dependencies.lock.psd1'
$lock = Import-PowerShellDataFile -LiteralPath $lockPath
if (($lock.SchemaVersion -ne 1) -or
    ($lock.PackageId -ne 'MVVCVTK.Dependencies')) {
    throw 'Unsupported dependency lock.'
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Get-BundleTree([string]$Root) {
    $rootPath = [IO.Path]::GetFullPath($Root)
    $files = @(Get-ChildItem -LiteralPath $rootPath -Recurse -File)
    $paths = @($files | ForEach-Object {
        $_.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
    })
    [Array]::Sort($paths, [StringComparer]::Ordinal)

    $records = [Text.StringBuilder]::new()
    [Int64]$totalBytes = 0
    foreach ($relativePath in $paths) {
        $fullPath = Join-Path $rootPath $relativePath.Replace('/', '\')
        $file = Get-Item -LiteralPath $fullPath
        $fileHash = Get-Sha256 $fullPath
        [void]$records.Append($relativePath)
        [void]$records.Append([char]0)
        [void]$records.Append(
            $file.Length.ToString([Globalization.CultureInfo]::InvariantCulture))
        [void]$records.Append([char]0)
        [void]$records.Append($fileHash)
        [void]$records.Append("`n")
        $totalBytes += $file.Length
    }

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($records.ToString())
        $treeHash = [BitConverter]::ToString(
            $sha.ComputeHash($bytes)).Replace('-', '')
    }
    finally {
        $sha.Dispose()
    }
    return [pscustomobject]@{
        FileCount = $paths.Count
        TotalBytes = $totalBytes
        TreeSha256 = $treeHash
    }
}

function Assert-NoReparse([string]$Root) {
    $rootItem = Get-Item -LiteralPath $Root -Force
    if (-not $rootItem.PSIsContainer -or
        (($rootItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "Dependency root must be a plain directory: '$Root'."
    }
    $reparse = @(Get-ChildItem -LiteralPath $Root -Recurse -Force |
        Where-Object {
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        })
    if ($reparse.Count -ne 0) {
        throw 'Dependency bundle cannot contain reparse points.'
    }
}

function Assert-Bundle([string]$Root) {
    # 在读取 manifest 或递归计算内容树前先拒绝根目录及子项中的 junction/symlink。
    Assert-NoReparse $Root
    $manifestPath = Join-Path $Root 'manifest.json'
    if (-not [IO.File]::Exists($manifestPath)) {
        throw "Dependency manifest is missing at '$Root'."
    }
    if ((Get-Sha256 $manifestPath) -ne $lock.ManifestSha256) {
        throw 'Dependency manifest hash mismatch.'
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw |
        ConvertFrom-Json
    if (($manifest.packageId -ne $lock.PackageId) -or
        ($manifest.packageVersion -ne $lock.PackageVersion) -or
        ($manifest.platform -ne 'windows') -or
        ($manifest.architecture -ne 'x64')) {
        throw 'Dependency manifest identity or platform mismatch.'
    }
    $tree = Get-BundleTree $Root
    if (($tree.FileCount -ne $lock.FileCount) -or
        ($tree.TotalBytes -ne $lock.TotalBytes) -or
        ($tree.TreeSha256 -ne $lock.TreeSha256)) {
        throw 'Dependency content tree does not match the checked-in lock.'
    }
}

$repoPath = [IO.Path]::GetFullPath($RepoRoot)
$depsBase = [IO.Path]::GetFullPath((Join-Path $repoPath 'sdk\deps'))
$repoPrefix = $repoPath.TrimEnd('\') + '\'
if (-not ($depsBase + '\').StartsWith(
        $repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Dependency destination escapes the repository.'
}
$destination = [IO.Path]::GetFullPath(
    (Join-Path $depsBase $lock.DirectoryName))
$depsPrefix = $depsBase.TrimEnd('\') + '\'
if (-not $destination.StartsWith(
        $depsPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Dependency version destination is unsafe.'
}

if ([IO.Directory]::Exists($destination)) {
    try {
        Assert-Bundle $destination
        Write-Host "Dependency bundle verified: $destination"
        return
    }
    catch {
        if ($VerifyOnly -or -not $Force) { throw }
    }
}
elseif ($VerifyOnly) {
    throw "Dependency bundle is not installed at '$destination'."
}

if ([string]::IsNullOrWhiteSpace($Uri) -eq
    [string]::IsNullOrWhiteSpace($ArchivePath)) {
    throw 'Specify exactly one of -Uri/MVVCVTK_DEPS_URI or -ArchivePath.'
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'MVVCVTK-deps-' + [Guid]::NewGuid().ToString('N'))
$extractRoot = Join-Path $tempRoot 'extract'
$archive = Join-Path $tempRoot 'dependencies.zip'
try {
    [void](New-Item -ItemType Directory -Path $extractRoot -Force)
    if (-not [string]::IsNullOrWhiteSpace($ArchivePath)) {
        $archive = [IO.Path]::GetFullPath($ArchivePath)
        if (-not [IO.File]::Exists($archive)) {
            throw "Dependency archive is missing: '$archive'."
        }
    }
    else {
        Invoke-WebRequest -Uri $Uri -OutFile $archive -UseBasicParsing
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $extractRoot

    $manifestCandidates = @(
        Get-ChildItem -LiteralPath $extractRoot -Recurse -File `
            -Filter 'manifest.json' | Where-Object {
            try {
                $value = Get-Content -LiteralPath $_.FullName -Raw |
                    ConvertFrom-Json
                $value.packageId -eq $lock.PackageId
            }
            catch { $false }
        })
    if ($manifestCandidates.Count -ne 1) {
        throw 'Archive must contain exactly one MVVCVTK dependency manifest.'
    }
    $bundleRoot = $manifestCandidates[0].Directory.FullName
    Assert-Bundle $bundleRoot

    [void](New-Item -ItemType Directory -Path $depsBase -Force)
    if ([IO.Directory]::Exists($destination)) {
        # 强制替换前重新核验实体目录，避免校验与删除之间把目标换成 junction。
        Assert-NoReparse $destination
        Remove-Item -LiteralPath $destination -Recurse -Force
    }
    Move-Item -LiteralPath $bundleRoot -Destination $destination
    Assert-Bundle $destination
    Write-Host "Dependency bundle installed: $destination"
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
