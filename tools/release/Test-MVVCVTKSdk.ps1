[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Stage,

    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$BuildRoot
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

function Get-RelativePath(
    [string]$root,
    [string]$path)
{
    $rootPath = [IO.Path]::GetFullPath($root)
    $filePath = [IO.Path]::GetFullPath($path)
    $prefix = $rootPath
    if (-not $prefix.EndsWith(
        [string][IO.Path]::DirectorySeparatorChar)) {
        $prefix += [IO.Path]::DirectorySeparatorChar
    }
    if (-not $filePath.StartsWith(
        $prefix,
        [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside its declared root: $filePath"
    }
    return $filePath.Substring($prefix.Length).Replace('\', '/')
}

function Get-SafePath(
    [string]$root,
    [string]$relativePath)
{
    if ([string]::IsNullOrWhiteSpace($relativePath) -or
        [IO.Path]::IsPathRooted($relativePath) -or
        $relativePath.Contains('\') -or
        $relativePath.Contains(':') -or
        $relativePath.Contains([char]0)) {
        throw "Unsafe SDK path: $relativePath"
    }
    $segments = @($relativePath.Split('/'))
    if (@($segments | Where-Object {
            $_ -eq '' -or $_ -eq '.' -or $_ -eq '..'
        }).Count -ne 0) {
        throw "Unsafe SDK path: $relativePath"
    }
    $rootPath = [IO.Path]::GetFullPath($root)
    $prefix = $rootPath
    if (-not $prefix.EndsWith(
        [string][IO.Path]::DirectorySeparatorChar)) {
        $prefix += [IO.Path]::DirectorySeparatorChar
    }
    $fullPath = [IO.Path]::GetFullPath(
        (Join-Path $rootPath $relativePath))
    if (-not $fullPath.StartsWith(
        $prefix,
        [StringComparison]::OrdinalIgnoreCase)) {
        throw "SDK path escapes its root: $relativePath"
    }
    return $fullPath
}

function Get-HeaderSurface(
    [string]$buildRoot,
    [string]$stageRoot)
{
    $surfacePath = Join-Path $buildRoot 'MVVCVTKHeaderSurface.txt'
    if (-not [IO.File]::Exists($surfacePath)) {
        throw "Build-tree header surface metadata is missing: $surfacePath"
    }
    Get-SafeAncestors $surfacePath
    $allowedCategories = @(
        'HostAPI', 'HostSupport',
        'FeatureSPI', 'FeatureSupport',
        'OrthogonalCrop', 'GapAnalysis'
    )
    $seenEntries = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $entries = @(
        foreach ($line in (Get-Content -LiteralPath $surfacePath)) {
            $match = [regex]::Match($line, '^([^|]+)\|([^|]+)$')
            if (-not $match.Success) {
                throw "Invalid SDK header surface entry: $line"
            }
            $category = $match.Groups[1].Value
            $headerPath = $match.Groups[2].Value
            $null = Get-SafePath $stageRoot "include/$headerPath"
            if ($category -notin $allowedCategories -or
                -not $headerPath.EndsWith('.h') -or
                -not $seenEntries.Add("$category|$headerPath")) {
                throw "Unsafe or duplicate SDK header surface entry: $line"
            }
            [pscustomobject]@{
                category = $category
                path = $headerPath
            }
        }
    )
    if ($entries.Count -eq 0) {
        throw 'SDK header surface metadata is empty.'
    }
    return $entries
}

$stageRoot = [IO.Path]::GetFullPath($Stage)
$null = @(Get-SafeTreeItems $stageRoot)

$buildInfoPath = Join-Path $BuildRoot 'MVVCVTKBuildInfo.json'
if (-not [IO.File]::Exists($buildInfoPath)) {
    throw "CMake build information is missing: $buildInfoPath"
}
$buildInfo = Get-Content -LiteralPath $buildInfoPath -Raw |
    ConvertFrom-Json
$toolsMatch = [regex]::Match(
    [string]$buildInfo.compilerPath,
    'Tools[/\\]MSVC[/\\]([^/\\]+)[/\\]bin')
if (-not $toolsMatch.Success -or
    [version]$buildInfo.cmakeVersion -lt [version]'4.2' -or
    $buildInfo.generator -ne 'Visual Studio 18 2026' -or
    $buildInfo.generatorToolset -ne 'v145,host=x64' -or
    $buildInfo.compilerId -ne 'MSVC' -or
    $buildInfo.msvcToolsetVersion -ne '145' -or
    $buildInfo.msvcVersion -notmatch '^195\d$' -or
    $buildInfo.compilerVersion -notmatch '^19\.5\d\.' -or
    $buildInfo.windowsSdkVersion -notmatch '^10\.0\.' -or
    $buildInfo.releaseInstructionSet -ne 'AVX2') {
    throw 'CMake build information does not match the fixed SDK toolchain.'
}

$headerSurface = @(Get-HeaderSurface $BuildRoot $stageRoot)
$expectedHeaders = @($headerSurface.path | Sort-Object -Unique)
$includeRoot = Join-Path $stageRoot 'include'
$actualHeaders = @(
    Get-ChildItem -LiteralPath $includeRoot -Recurse -File -Filter '*.h' |
        ForEach-Object { Get-RelativePath $includeRoot $_.FullName } |
        Sort-Object
)
$expectedHeaderDiff = @(Compare-Object `
        -ReferenceObject ($expectedHeaders | Sort-Object) `
        -DifferenceObject $actualHeaders)
if ($actualHeaders.Count -ne $expectedHeaders.Count -or
    $expectedHeaderDiff.Count -ne 0) {
    throw 'SDK public header closure mismatch.'
}

$expectedLibraries = @(
    'Debug/MVVCVTKGapAnalysis.lib'
    'Debug/MVVCVTKHost.lib'
    'Debug/MVVCVTKOrthogonalCrop.lib'
    'Release/MVVCVTKGapAnalysis.lib'
    'Release/MVVCVTKHost.lib'
    'Release/MVVCVTKOrthogonalCrop.lib'
)
$libraryRoot = Join-Path $stageRoot 'lib'
$actualLibraries = @(
    Get-ChildItem -LiteralPath $libraryRoot -Recurse -File -Filter '*.lib' |
        ForEach-Object { Get-RelativePath $libraryRoot $_.FullName } |
        Sort-Object
)
if (@(Compare-Object $expectedLibraries $actualLibraries).Count -ne 0) {
    throw 'SDK library closure mismatch.'
}

$requiredPaths = @(
    'lib/cmake/MVVCVTK/MVVCVTKConfig.cmake'
    'lib/cmake/MVVCVTK/MVVCVTKInternalDependencies.cmake'
    'lib/cmake/MVVCVTK/MVVCVTKTargets.cmake'
    'lib/cmake/MVVCVTK/MVVCVTKTargets-debug.cmake'
    'lib/cmake/MVVCVTK/MVVCVTKTargets-release.cmake'
)
foreach ($requiredPath in $requiredPaths) {
    if (-not [IO.File]::Exists((Get-SafePath $stageRoot $requiredPath))) {
        throw "SDK package metadata is missing: $requiredPath"
    }
}

$expectedTopLevel = @(
    'deps'
    'include'
    'lib'
)
$actualTopLevel = @(
    Get-ChildItem -LiteralPath $stageRoot -Force |
        ForEach-Object { $_.Name } |
        Sort-Object
)
if (@(Compare-Object ($expectedTopLevel | Sort-Object) $actualTopLevel).Count -ne 0) {
    throw 'SDK root contains an undeclared compatibility directory or file.'
}

$cmakeRoot = Join-Path $stageRoot 'lib\cmake\MVVCVTK'
$cmakeMetadata = @(Get-ChildItem -LiteralPath $cmakeRoot -File)
$expectedCMakeFiles = @(
    'MVVCVTKConfig.cmake'
    'MVVCVTKInternalDependencies.cmake'
    'MVVCVTKTargets.cmake'
    'MVVCVTKTargets-debug.cmake'
    'MVVCVTKTargets-release.cmake'
)
$actualCMakeFiles = @($cmakeMetadata.Name | Sort-Object)
if (@(Compare-Object `
        ($expectedCMakeFiles | Sort-Object) $actualCMakeFiles).Count -ne 0) {
    throw 'SDK CMake metadata closure mismatch.'
}
$forbiddenRoots = @(
    [IO.Path]::GetFullPath($RepoRoot)
    [IO.Path]::GetFullPath($BuildRoot)
)
foreach ($metadataFile in $cmakeMetadata) {
    $content = Get-Content -LiteralPath $metadataFile.FullName -Raw
    foreach ($forbiddenRoot in $forbiddenRoots) {
        if ($content.IndexOf(
                $forbiddenRoot,
                [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $content.IndexOf(
                $forbiddenRoot.Replace('\', '/'),
                [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "Installed CMake metadata contains a build path: $($metadataFile.FullName)"
        }
    }
    foreach ($retiredTerm in @(
            'MVVCVTK::FeatureAPI',
            'MVVCVTK::SDK',
            'MVVCVTKInternal::FeatureSupport',
            'MVVCVTKInternal::OpenCVWorld',
            'MVVCVTKInternal::UIPhantomCalib',
            'MVVCVTKInternal::UIReconstruct3D',
            'MVVCVTKDependencyPolicy',
            'MVVCVTKHeaderSurface')) {
        if ($content.IndexOf(
                $retiredTerm,
                [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "Installed CMake metadata exposes a retired contract: $retiredTerm"
        }
    }
}

$depsRoot = Join-Path $stageRoot 'deps'
$expectedDependencies = @('opencv', 'vtk')
$actualDependencies = @(
    Get-ChildItem -LiteralPath $depsRoot -Force |
        ForEach-Object { $_.Name } |
        Sort-Object
)
if (@(Compare-Object `
        ($expectedDependencies | Sort-Object) $actualDependencies).Count -ne 0) {
    throw 'SDK dependency directory closure mismatch.'
}

foreach ($requiredDependencyPath in @(
        'deps/vtk/lib/cmake/vtk-9.4/vtk-config.cmake',
        'deps/opencv/x64/vc16/lib/OpenCVConfig.cmake')) {
    if (-not [IO.File]::Exists(
            (Get-SafePath $stageRoot $requiredDependencyPath))) {
        throw "SDK dependency is missing: $requiredDependencyPath"
    }
}

Write-Host "MVVCVTK SDK validated: $stageRoot"
Write-Host "MVVCVTK SDK public headers: $($actualHeaders.Count)"
Write-Host "MVVCVTK SDK libraries: $($actualLibraries.Count)"
