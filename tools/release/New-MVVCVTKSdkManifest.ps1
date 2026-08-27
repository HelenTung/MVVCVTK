[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Stage,

    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$PackageRevision,

    [Parameter(Mandatory = $true)]
    [string]$DirectoryVersion,

    [Parameter(Mandatory = $true)]
    [string]$DepsVersion,

    [Parameter(Mandatory = $true)]
    [string]$CMakeVersion,

    [Parameter(Mandatory = $true)]
    [string]$CMakeGenerator,

    [Parameter(Mandatory = $true)]
    [string]$CMakeGeneratorToolset,

    [Parameter(Mandatory = $true)]
    [string]$CompilerVersion,

    [Parameter(Mandatory = $true)]
    [string]$MsvcVersion,

    [Parameter(Mandatory = $true)]
    [string]$MsvcToolsVersion,

    [Parameter(Mandatory = $true)]
    [string]$WindowsSdkVersion,

    [Parameter(Mandatory = $true)]
    [string]$ReleaseInstructionSet,

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

$stageRoot = [IO.Path]::GetFullPath($Stage)
$repoPath = [IO.Path]::GetFullPath($RepoRoot)
$depsManifestPath = Join-Path $stageRoot 'deps\manifest.json'
if (-not [IO.Directory]::Exists($stageRoot)) {
    throw "SDK stage is missing: $stageRoot"
}
if (-not [IO.File]::Exists($depsManifestPath)) {
    throw "Packaged dependency manifest is missing: $depsManifestPath"
}
$null = @(Get-SafeTreeItems $stageRoot)

if ($DirectoryVersion -ne "$PackageRevision-win-x64" -or
    [IO.Path]::GetFileName($ArchiveName) -ne $ArchiveName -or
    $ArchiveName -ne "MVVCVTK-SDK-$DirectoryVersion.zip" -or
    $CMakeVersion -notmatch '^\d+\.\d+\.\d+$' -or
    [version]$CMakeVersion -lt [version]'4.2' -or
    $CMakeGenerator -ne 'Visual Studio 18 2026' -or
    $CMakeGeneratorToolset -ne 'v145,host=x64' -or
    $CompilerVersion -ne '19.51.36246.0' -or
    $MsvcVersion -ne '1951' -or
    $MsvcToolsVersion -ne '14.51.36231' -or
    $WindowsSdkVersion -ne '10.0.26100.0' -or
    $ReleaseInstructionSet -ne 'AVX2') {
    throw 'SDK build information does not match the fixed toolchain policy.'
}

$deps = Get-Content -LiteralPath $depsManifestPath -Raw |
    ConvertFrom-Json
$depComponents = @($deps.components)
if ($deps.schemaVersion -ne 1 -or
    $deps.packageId -ne 'MVVCVTK.Dependencies' -or
    $deps.packageVersion -ne $DepsVersion -or
    $deps.platform -ne 'windows' -or
    $deps.architecture -ne 'x64' -or
    $depComponents.Count -ne 4 -or
    @($depComponents.id | Sort-Object -Unique).Count -ne 4) {
    throw 'Dependency manifest does not match the SDK dependency policy.'
}

$gitHash = [string]::Join('', (& git -C $repoPath rev-parse HEAD))
if ($LASTEXITCODE -ne 0) {
    throw 'Cannot resolve the SDK source commit.'
}
$gitShort = [string]::Join(
    '',
    (& git -C $repoPath rev-parse --short=12 HEAD))
if ($LASTEXITCODE -ne 0) {
    throw 'Cannot resolve the SDK short commit.'
}
$gitDescribe = [string]::Join(
    '',
    (& git -C $repoPath describe --always --dirty))
$gitStatus = @(& git -C $repoPath status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw 'Cannot inspect the SDK source state.'
}
$isDirty = $gitStatus.Count -ne 0
$dateVersion = Get-Date -Format 'yyyy.MM.dd'
if ($isDirty -and -not [regex]::IsMatch(
        $PackageRevision,
        '^\d{4}\.\d{2}\.\d{2}-rev\.\d+$')) {
    throw 'Dirty SDK builds must use yyyy.MM.dd-rev.N.'
}
$cleanVersion = "$dateVersion-git.$gitShort"
if (-not $isDirty -and $PackageRevision -ne $cleanVersion) {
    throw "Clean SDK builds must use $cleanVersion."
}

$publicHeaders = @(
    'App/AppTypes.h'
    'App/ViewTypes.h'
    'App/Services/FeatureViewService.h'
    'Data/ImageReadTypes.h'
    'Data/TrustedImageState.h'
    'Host/CropHostFeature.h'
    'Host/GapHostFeature.h'
    'Host/GapHostTypes.h'
    'Host/HostFeature.h'
    'Host/Types/HostFeatureViewTypes.h'
    'Host/Types/HostInputTypes.h'
    'Host/Types/HostRequest.h'
    'Host/Types/HostRequestTypes.h'
    'Host/Types/HostSessionTypes.h'
    'Host/Types/HostValueTypes.h'
    'Host/VtkAppHostSession.h'
    'Interaction/InteractionTypes.h'
    'OrthogonalCropTypes.h'
    'Render/Contracts/OverlayService.h'
    'Render/Contracts/RenderEffect.h'
    'Render/Contracts/VisualStrategy.h'
)
$includeRoot = Join-Path $stageRoot 'include'
$actualHeaders = @(
    Get-ChildItem -LiteralPath $includeRoot -Recurse -File -Filter '*.h' |
        ForEach-Object {
            Get-RelativePath $includeRoot $_.FullName
        } |
        Sort-Object
)
if (@(Compare-Object ($publicHeaders | Sort-Object) $actualHeaders).Count -ne 0) {
    throw 'Installed public header closure is not the declared 21-header API.'
}

$libraryNames = @(
    'MVVCVTKHost.lib'
    'MVVCVTKOrthogonalCrop.lib'
    'MVVCVTKGapAnalysis.lib'
)
$configurations = [ordered]@{}
foreach ($configuration in @('Debug', 'Release')) {
    $libraries = @(
        foreach ($libraryName in $libraryNames) {
            "lib/$configuration/$libraryName"
        }
    )
    foreach ($library in $libraries) {
        if (-not [IO.File]::Exists((Join-Path $stageRoot $library))) {
            throw "SDK library is missing: $library"
        }
    }
    $configurations[$configuration] = [ordered]@{
        runtimeLibrary = if ($configuration -eq 'Debug') {
            '/MDd'
        }
        else {
            '/MD'
        }
        iteratorDebugLevel = if ($configuration -eq 'Debug') { 2 } else { 0 }
        wholeProgramOptimization = $false
        libraries = $libraries
    }
}

$revision = if ($isDirty) {
    [int]([regex]::Match(
        $PackageRevision,
        'rev\.(\d+)$').Groups[1].Value)
}
else {
    $null
}

$manifest = [ordered]@{
    schemaVersion = 1
    packageId = 'MVVCVTK.Sdk'
    packageKind = 'SelfContained'
    packageVersion = $PackageRevision
    directoryVersion = $DirectoryVersion
    versionSource = [ordered]@{
        kind = if ($isDirty) { 'revision' } else { 'git' }
        revision = $revision
        baseCommit = $gitHash
        gitDescribe = $gitDescribe
        isDirty = $isDirty
    }
    platform = 'windows'
    architecture = 'x64'
    libraryKind = 'static'
    languageStandard = 'c++17'
    featureTrust = 'trusted-in-process'
    abiPolicy = [ordered]@{
        compatibility = 'fixed-toolchain'
        stableBinaryAbi = $false
        platformToolset = 'v145'
        cmakeVersion = $CMakeVersion
        cmakeGenerator = $CMakeGenerator
        cmakeGeneratorToolset = $CMakeGeneratorToolset
        compilerId = 'MSVC'
        compilerVersion = $CompilerVersion
        msvcVersion = $MsvcVersion
        msvcToolsVersion = $MsvcToolsVersion
        windowsSdkVersion = $WindowsSdkVersion
        releaseInstructionSet = $ReleaseInstructionSet
        runtimeLibraryDebug = '/MDd'
        runtimeLibraryRelease = '/MD'
        iteratorDebugLevelDebug = 2
        iteratorDebugLevelRelease = 0
        wholeProgramOptimization = $false
    }
    configurations = $configurations
    modules = [ordered]@{
        Host = [ordered]@{
            target = 'MVVCVTK::Host'
            library = 'MVVCVTKHost.lib'
        }
        OrthogonalCrop = [ordered]@{
            target = 'MVVCVTK::OrthogonalCrop'
            library = 'MVVCVTKOrthogonalCrop.lib'
        }
        GapAnalysis = [ordered]@{
            target = 'MVVCVTK::GapAnalysis'
            library = 'MVVCVTKGapAnalysis.lib'
        }
    }
    cmakePackage = 'lib/cmake/MVVCVTK/MVVCVTKConfig.cmake'
    components = @(
        'HostAPI'
        'FeatureSPI'
        'Host'
        'OrthogonalCrop'
        'GapAnalysis'
        'Dependencies'
        'CMake'
    )
    dependencies = [ordered]@{
        packageVersion = $deps.packageVersion
        bundleMode = 'SelfContained'
        components = $deps.components
    }
    publicSurface = [ordered]@{
        entryHeaders = @(
            'Host/VtkAppHostSession.h'
            'Host/HostFeature.h'
            'Host/CropHostFeature.h'
            'Host/GapHostFeature.h'
        )
        headerClosure = $publicHeaders
        headerClosureCount = $publicHeaders.Count
    }
    validationPolicy = [ordered]@{
        configurations = @('Debug', 'Release')
        installedHeaderCompile = $true
        cmakeConsumer = $true
        qtCmakeConsumer = $true
        relocatableMetadata = $true
    }
    archive = [ordered]@{
        file = $ArchiveName
    }
}

$json = $manifest | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText(
    (Join-Path $stageRoot 'manifest.json'),
    $json + "`n",
    [Text.UTF8Encoding]::new($false))
