[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Stage,

    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$BuildRoot,

    [Parameter(Mandatory = $true)]
    [string]$PackageRevision,

    [Parameter(Mandatory = $true)]
    [string]$DirectoryVersion,

    [Parameter(Mandatory = $true)]
    [string]$DepsVersion
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

function Get-HeaderSurface([string]$stageRoot)
{
    $surfacePath = Get-SafePath $stageRoot `
        'lib/cmake/MVVCVTK/MVVCVTKHeaderSurface.txt'
    if (-not [IO.File]::Exists($surfacePath)) {
        throw "SDK header surface metadata is missing: $surfacePath"
    }
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
$manifestPath = Join-Path $stageRoot 'manifest.json'
if (-not [IO.File]::Exists($manifestPath)) {
    throw "SDK manifest is missing: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw |
    ConvertFrom-Json
if ($manifest.schemaVersion -isnot [int] -or
    $manifest.schemaVersion -ne 1 -or
    $manifest.packageId -ne 'MVVCVTK.Sdk' -or
    $manifest.packageKind -ne 'SelfContained' -or
    $manifest.packageVersion -ne $PackageRevision -or
    $manifest.directoryVersion -ne $DirectoryVersion) {
    throw 'Unsupported SDK manifest or package version.'
}
if ($manifest.platform -ne 'windows' -or
    $manifest.architecture -ne 'x64' -or
    $manifest.libraryKind -ne 'static' -or
    $manifest.languageStandard -ne 'c++17' -or
    $manifest.featureTrust -ne 'trusted-in-process') {
    throw 'SDK platform, language, library, or trust policy mismatch.'
}
$expectedConsumerMatrix = @(
    'Host'
    'Host+OrthogonalCrop'
    'Host+GapAnalysis'
    'Host+OrthogonalCrop+GapAnalysis'
)
$actualConsumerMatrix = @($manifest.validationPolicy.cmakeConsumerMatrix)
$consumerMatrixDiff = @(Compare-Object `
        -ReferenceObject $expectedConsumerMatrix `
        -DifferenceObject $actualConsumerMatrix)
if (-not $manifest.validationPolicy.installedHeaderCompile -or
    -not $manifest.validationPolicy.cmakeConsumer -or
    -not $manifest.validationPolicy.qtCmakeConsumer -or
    -not $manifest.validationPolicy.relocatableMetadata -or
    $actualConsumerMatrix.Count -ne $expectedConsumerMatrix.Count -or
    @($actualConsumerMatrix | Sort-Object -Unique).Count -ne
        $actualConsumerMatrix.Count -or
    $consumerMatrixDiff.Count -ne 0) {
    throw 'SDK validation policy or consumer matrix is incomplete.'
}
if ($manifest.abiPolicy.compatibility -ne 'fixed-toolchain' -or
    $manifest.abiPolicy.platformToolset -ne 'v145' -or
    $manifest.abiPolicy.stableBinaryAbi -isnot [bool] -or
    $manifest.abiPolicy.stableBinaryAbi -or
    $manifest.abiPolicy.compilerId -ne 'MSVC' -or
    $manifest.abiPolicy.compilerVersion -ne '19.51.36246.0' -or
    [string]$manifest.abiPolicy.msvcVersion -ne '1951' -or
    $manifest.abiPolicy.msvcToolsVersion -ne '14.51.36231' -or
    $manifest.abiPolicy.windowsSdkVersion -ne '10.0.26100.0' -or
    $manifest.abiPolicy.releaseInstructionSet -ne 'AVX2' -or
    [version]$manifest.abiPolicy.cmakeVersion -lt [version]'4.2' -or
    $manifest.abiPolicy.cmakeGenerator -ne 'Visual Studio 18 2026' -or
    $manifest.abiPolicy.cmakeGeneratorToolset -ne 'v145,host=x64' -or
    $manifest.abiPolicy.runtimeLibraryDebug -ne '/MDd' -or
    $manifest.abiPolicy.runtimeLibraryRelease -ne '/MD' -or
    $manifest.abiPolicy.iteratorDebugLevelDebug -ne 2 -or
    $manifest.abiPolicy.iteratorDebugLevelRelease -ne 0 -or
    $manifest.abiPolicy.wholeProgramOptimization) {
    throw 'SDK ABI policy is incomplete.'
}

$expectedModules = @('GapAnalysis', 'Host', 'OrthogonalCrop')
$actualModules = @($manifest.modules.PSObject.Properties.Name | Sort-Object)
if (@(Compare-Object $expectedModules $actualModules).Count -ne 0 -or
    $manifest.modules.Host.target -ne 'MVVCVTK::Host' -or
    $manifest.modules.OrthogonalCrop.target -ne 'MVVCVTK::OrthogonalCrop' -or
    $manifest.modules.GapAnalysis.target -ne 'MVVCVTK::GapAnalysis' -or
    $manifest.configurations.Debug.runtimeLibrary -ne '/MDd' -or
    $manifest.configurations.Debug.iteratorDebugLevel -ne 2 -or
    $manifest.configurations.Release.runtimeLibrary -ne '/MD' -or
    $manifest.configurations.Release.iteratorDebugLevel -ne 0) {
    throw 'SDK configuration or module contract mismatch.'
}
if ($null -ne $manifest.manifestIdentity -or
    $null -ne $manifest.artifactClosureIdentity -or
    $null -ne $manifest.dependencyClosureIdentity -or
    $null -ne $manifest.artifacts -or
    $null -ne $manifest.archive.checksumFile -or
    $null -ne $manifest.dependencies.sourceManifestSha256 -or
    $null -ne $manifest.dependencies.artifacts) {
    throw 'Retired SDK hash or artifact identity metadata remains.'
}

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
    $manifest.abiPolicy.cmakeVersion -ne $buildInfo.cmakeVersion -or
    $manifest.abiPolicy.cmakeGenerator -ne $buildInfo.generator -or
    $manifest.abiPolicy.cmakeGeneratorToolset -ne
        $buildInfo.generatorToolset -or
    $manifest.abiPolicy.compilerVersion -ne $buildInfo.compilerVersion -or
    [string]$manifest.abiPolicy.msvcVersion -ne
        [string]$buildInfo.msvcVersion -or
    $manifest.abiPolicy.msvcToolsVersion -ne
        $toolsMatch.Groups[1].Value -or
    $manifest.abiPolicy.windowsSdkVersion -ne
        $buildInfo.windowsSdkVersion -or
    $manifest.abiPolicy.releaseInstructionSet -ne
        $buildInfo.releaseInstructionSet) {
    throw 'SDK manifest toolchain does not match the configured build tree.'
}

$headerSurface = @(Get-HeaderSurface $stageRoot)
$expectedHeaders = @($headerSurface.path | Sort-Object -Unique)
$entryCategories = @('HostAPI', 'FeatureSPI', 'OrthogonalCrop', 'GapAnalysis')
$expectedEntryHeaders = @(
    $headerSurface |
        Where-Object { $_.category -in $entryCategories } |
        ForEach-Object { $_.path } |
        Sort-Object -Unique
)
$includeRoot = Join-Path $stageRoot 'include'
$actualHeaders = @(
    Get-ChildItem -LiteralPath $includeRoot -Recurse -File -Filter '*.h' |
        ForEach-Object { Get-RelativePath $includeRoot $_.FullName } |
        Sort-Object
)
$declaredHeaders = @($manifest.publicSurface.headerClosure | Sort-Object)
$declaredEntryHeaders = @($manifest.publicSurface.entryHeaders | Sort-Object)
$expectedHeaderDiff = @(Compare-Object `
        -ReferenceObject ($expectedHeaders | Sort-Object) `
        -DifferenceObject $actualHeaders)
$declaredHeaderDiff = @(Compare-Object `
        -ReferenceObject $declaredHeaders `
        -DifferenceObject $actualHeaders)
$entryHeaderDiff = @(Compare-Object `
        -ReferenceObject $expectedEntryHeaders `
        -DifferenceObject $declaredEntryHeaders)
if ($actualHeaders.Count -ne $expectedHeaders.Count -or
    $manifest.publicSurface.headerClosureCount -ne $expectedHeaders.Count -or
    $declaredHeaders.Count -ne $actualHeaders.Count -or
    @($declaredHeaders | Sort-Object -Unique).Count -ne $declaredHeaders.Count -or
    $declaredEntryHeaders.Count -ne $expectedEntryHeaders.Count -or
    @($declaredEntryHeaders | Sort-Object -Unique).Count -ne
        $declaredEntryHeaders.Count -or
    $expectedHeaderDiff.Count -ne 0 -or
    $declaredHeaderDiff.Count -ne 0 -or
    $entryHeaderDiff.Count -ne 0) {
    throw 'SDK public header closure mismatch.'
}
foreach ($entryHeader in $manifest.publicSurface.entryHeaders) {
    if ($actualHeaders -notcontains $entryHeader) {
        throw "SDK entry header is outside the closure: $entryHeader"
    }
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
    'lib/cmake/MVVCVTK/MVVCVTKConfigVersion.cmake'
    'lib/cmake/MVVCVTK/MVVCVTKInternalDependencies.cmake'
    'lib/cmake/MVVCVTK/MVVCVTKDependencyPolicy.cmake'
    'lib/cmake/MVVCVTK/MVVCVTKHeaderSurface.txt'
    'lib/cmake/MVVCVTK/MVVCVTKTargets.cmake'
    'lib/cmake/MVVCVTK/MVVCVTKTargets-debug.cmake'
    'lib/cmake/MVVCVTK/MVVCVTKTargets-release.cmake'
    'deps/manifest.json'
    'README.md'
    'NOTICE'
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
    'manifest.json'
    'NOTICE'
    'README.md'
)
$actualTopLevel = @(
    Get-ChildItem -LiteralPath $stageRoot -Force |
        ForEach-Object { $_.Name } |
        Sort-Object
)
if (@(Compare-Object ($expectedTopLevel | Sort-Object) $actualTopLevel).Count -ne 0) {
    throw 'SDK root contains an undeclared compatibility directory or file.'
}

$cmakeMetadata = @(
    Get-ChildItem -LiteralPath (
        Join-Path $stageRoot 'lib\cmake\MVVCVTK') -File
)
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
            'MVVCVTKInternal::FeatureSupport')) {
        if ($content.IndexOf(
                $retiredTerm,
                [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "Installed CMake metadata exposes a retired contract: $retiredTerm"
        }
    }
}

if ($manifest.dependencies.packageVersion -ne $DepsVersion -or
    $manifest.dependencies.bundleMode -ne 'SelfContained') {
    throw 'Packaged dependency policy mismatch.'
}
$depsManifestPath = Join-Path $stageRoot 'deps\manifest.json'
$deps = Get-Content -LiteralPath $depsManifestPath -Raw |
    ConvertFrom-Json
$expectedComponents = @{
    vtk = @('9.4.2', 'msvc-x64')
    opencv = @('4.12.0', 'vc16-x64')
    qt = @('5.14.2', 'msvc2017_64')
    ui = @('ct-1209-sha256', 'qt5-msvc-x64')
}
$depConfigs = @($deps.configurations)
$depComponents = @($deps.components)
$declaredComponents = @($manifest.dependencies.components)
if ($deps.schemaVersion -ne 1 -or
    $deps.packageId -ne 'MVVCVTK.Dependencies' -or
    $deps.packageVersion -ne $DepsVersion -or
    $deps.platform -ne 'windows' -or
    $deps.architecture -ne 'x64' -or
    $deps.validatedConsumer.platformToolset -ne 'v145' -or
    $deps.validatedConsumer.windowsTargetPlatformVersion -ne '10.0' -or
    $depConfigs.Count -ne 2 -or
    'Debug' -notin $depConfigs -or
    'Release' -notin $depConfigs -or
    $depComponents.Count -ne 4) {
    throw 'Packaged dependency manifest contract mismatch.'
}
$depComponentValues = @($depComponents | Sort-Object id |
        ForEach-Object { "$($_.id)|$($_.version)|$($_.abi)" })
$declaredComponentValues = @($declaredComponents | Sort-Object id |
        ForEach-Object { "$($_.id)|$($_.version)|$($_.abi)" })
if (@(Compare-Object $depComponentValues $declaredComponentValues).Count -ne 0) {
    throw 'Packaged dependency component declaration mismatch.'
}
foreach ($component in $depComponents) {
    if (-not $expectedComponents.ContainsKey($component.id)) {
        throw "Unknown packaged dependency component: $($component.id)"
    }
    $expected = $expectedComponents[$component.id]
    if ($component.version -ne $expected[0] -or
        $component.abi -ne $expected[1]) {
        throw "Packaged dependency component mismatch: $($component.id)"
    }
}
