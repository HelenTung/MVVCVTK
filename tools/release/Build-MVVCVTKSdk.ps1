[CmdletBinding()]
param(
    [string]$Preset = 'vs2026-x64',
    [string]$PackageRevision,
    [switch]$SkipTests,
    [switch]$SkipCleanRoom
)

$ErrorActionPreference = 'Stop'

function Start-Command(
    [string]$command,
    [string[]]$arguments)
{
    & $command @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$command failed with exit code $LASTEXITCODE."
    }
}

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

function Clear-SafeDirectory(
    [string]$base,
    [string]$target)
{
    $basePath = [IO.Path]::GetFullPath($base)
    $targetPath = [IO.Path]::GetFullPath($target)
    $prefix = $basePath
    if (-not $prefix.EndsWith(
        [string][IO.Path]::DirectorySeparatorChar)) {
        $prefix += [IO.Path]::DirectorySeparatorChar
    }
    if ($targetPath -eq $basePath -or
        -not $targetPath.StartsWith(
            $prefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clear unsafe directory: $targetPath"
    }
    Get-SafeAncestors $basePath
    Get-SafeAncestors $targetPath
    if (-not [IO.Directory]::Exists($targetPath)) {
        return
    }
    $null = @(Get-SafeTreeItems $targetPath)
    Get-SafeAncestors $targetPath
    Remove-Item -LiteralPath $targetPath -Recurse -Force
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

function Build-DependencyBundle(
    [string]$source,
    [string]$destination)
{
    if (-not [IO.Directory]::Exists($source)) {
        throw "Dependency bundle is missing: $source"
    }
    $null = @(Get-SafeTreeItems $source)
    Get-SafeAncestors (Split-Path -Parent $destination)
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    Get-SafeAncestors $destination
    & robocopy.exe $source $destination /E /COPY:DAT /DCOPY:DAT /XJ /R:2 /W:1 /NFL /NDL /NJH /NJS /NP
    $robocopyCode = $LASTEXITCODE
    if ($robocopyCode -gt 7) {
        throw "robocopy failed with exit code $robocopyCode."
    }
    $global:LASTEXITCODE = 0
    $null = @(Get-SafeTreeItems $destination)
}

function Set-ConsumerSource(
    [string]$source,
    [string]$destination)
{
    $null = @(Get-SafeTreeItems $source)
    Get-SafeAncestors (Split-Path -Parent $destination)
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    Get-SafeAncestors $destination
    foreach ($item in (Get-ChildItem -LiteralPath $source -Force)) {
        Copy-Item -LiteralPath $item.FullName -Destination $destination -Recurse
    }
    $null = @(Get-SafeTreeItems $destination)
}

function Get-PackageRevision([string]$repoRoot)
{
    $gitStatus = @(& git -C $repoRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot inspect the SDK source state.'
    }
    $dateVersion = Get-Date -Format 'yyyy.MM.dd'
    if ($gitStatus.Count -eq 0) {
        $gitShort = [string]::Join(
            '',
            (& git -C $repoRoot rev-parse --short=12 HEAD))
        if ($LASTEXITCODE -ne 0) {
            throw 'Cannot resolve the SDK source commit.'
        }
        return "$dateVersion-git.$gitShort"
    }
    return "$dateVersion-rev.$(Get-Date -Format 'HHmmss')"
}

function Get-BuildInfo([string]$buildRoot)
{
    $buildInfoPath = Join-Path $buildRoot 'MVVCVTKBuildInfo.json'
    if (-not [IO.File]::Exists($buildInfoPath)) {
        throw "Cannot locate CMake build information: $buildInfoPath"
    }
    $buildInfo = Get-Content -LiteralPath $buildInfoPath -Raw |
        ConvertFrom-Json
    if ([version]$buildInfo.cmakeVersion -lt [version]'4.2' -or
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
    $match = [regex]::Match(
        [string]$buildInfo.compilerPath,
        'Tools[/\\]MSVC[/\\]([^/\\]+)[/\\]bin')
    if (-not $match.Success) {
        throw 'Cannot resolve the MSVC tools version from the configured compiler.'
    }
    Add-Member -InputObject $buildInfo -NotePropertyName msvcToolsVersion `
        -NotePropertyValue $match.Groups[1].Value
    return $buildInfo
}

function Clear-ConsumerEnv()
{
    $exactNames = @(
        'CC', 'CXX', 'RC',
        'CL', '_CL_', 'LINK', '_LINK_', 'INCLUDE', 'LIB', 'LIBPATH',
        'CMAKE_PREFIX_PATH', 'CMAKE_TOOLCHAIN_FILE', 'CMAKE_GENERATOR',
        'CMAKE_GENERATOR_INSTANCE', 'CMAKE_GENERATOR_PLATFORM',
        'CMAKE_GENERATOR_TOOLSET', 'CMAKE_BUILD_TYPE',
        'DirectoryBuildPropsPath', 'DirectoryBuildTargetsPath',
        'ImportDirectoryBuildProps', 'ImportDirectoryBuildTargets',
        'MSBuildProjectExtensionsPath', 'VCTargetsPath',
        'VSINSTALLDIR', 'VCINSTALLDIR', 'VCToolsInstallDir',
        'VCToolsVersion', 'WindowsSdkDir', 'WindowsSDKVersion',
        'UniversalCRTSdkDir', 'UCRTVersion', 'VSCMD_ARG_TGT_ARCH',
        'QTDIR', 'QT_PLUGIN_PATH'
    )
    foreach ($name in $exactNames) {
        Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
    }
    foreach ($item in (Get-ChildItem Env:)) {
        if ($item.Name -match
            '^(CMAKE_PROJECT_|CMAKE_FIND_|MVVCVTK_|QMAKE_|VCPKG_)') {
            Remove-Item -LiteralPath "Env:$($item.Name)" `
                -ErrorAction SilentlyContinue
        }
    }
}

function Get-RuntimeDirs([string]$depsRoot)
{
    return @(
        (Join-Path $depsRoot 'ui\bin')
        (Join-Path $depsRoot 'opencv\x64\vc16\bin')
        (Join-Path $depsRoot 'vtk\bin')
        (Join-Path $depsRoot 'qt\bin')
    )
}

function Start-RuntimeExecutable(
    [string]$executable,
    [string]$depsRoot,
    [bool]$isQt)
{
    $oldPath = $env:PATH
    $oldPluginPath = $env:QT_PLUGIN_PATH
    try {
        $systemPath = @(
            (Join-Path $env:SystemRoot 'System32')
            $env:SystemRoot
        )
        $env:PATH = (@(Get-RuntimeDirs $depsRoot) + $systemPath) -join ';'
        if ($isQt) {
            $env:QT_PLUGIN_PATH = Join-Path $depsRoot 'qt\plugins'
        }
        Start-Command $executable @()
    }
    finally {
        $env:PATH = $oldPath
        $env:QT_PLUGIN_PATH = $oldPluginPath
    }
}

function Start-CMakeConsumer(
    [string]$verifyBase,
    [string]$stage,
    [string]$repoRoot)
{
    $consumerRoot = Join-Path $verifyBase 'cmake'
    Clear-SafeDirectory $verifyBase $consumerRoot
    $sourceRoot = Join-Path $consumerRoot 'src'
    $buildRoot = Join-Path $consumerRoot 'build'
    Set-ConsumerSource (Join-Path $repoRoot 'tools\release\consumer') `
        $sourceRoot
    Start-Command $script:cmakePath @(
        '-S', $sourceRoot,
        '-B', $buildRoot,
        '-G', 'Visual Studio 18 2026',
        '-A', 'x64',
        '-T', 'v145,host=x64',
        "-DMVVCVTK_DIR=$(Join-Path $stage 'lib\cmake\MVVCVTK')"
    )
    foreach ($configuration in @('Debug', 'Release')) {
        Start-Command $script:cmakePath @(
            '--build', $buildRoot,
            '--config', $configuration,
            '--parallel'
        )
        Start-RuntimeExecutable (
            Join-Path $buildRoot "$configuration\mvvcvtk_sdk_consumer.exe") (
            Join-Path $stage 'deps') $false
    }
}

function Start-QtConsumer(
    [string]$verifyBase,
    [string]$stage,
    [string]$repoRoot)
{
    $consumerRoot = Join-Path $verifyBase 'qt-cmake'
    Clear-SafeDirectory $verifyBase $consumerRoot
    $sourceRoot = Join-Path $consumerRoot 'src'
    $buildRoot = Join-Path $consumerRoot 'build'
    Set-ConsumerSource (Join-Path $repoRoot 'tools\release\qt-consumer') `
        $sourceRoot
    $depsRoot = Join-Path $stage 'deps'
    Start-Command $script:cmakePath @(
        '-S', $sourceRoot,
        '-B', $buildRoot,
        '-G', 'Visual Studio 18 2026',
        '-A', 'x64',
        '-T', 'v145,host=x64',
        "-DMVVCVTK_DIR=$(Join-Path $stage 'lib\cmake\MVVCVTK')",
        "-DQt5_DIR=$(Join-Path $depsRoot 'qt\lib\cmake\Qt5')",
        "-DVTK_DIR=$(Join-Path $depsRoot 'vtk\lib\cmake\vtk-9.4')"
    )
    foreach ($configuration in @('Debug', 'Release')) {
        Start-Command $script:cmakePath @(
            '--build', $buildRoot,
            '--config', $configuration,
            '--parallel'
        )
        Start-RuntimeExecutable (
            Join-Path $buildRoot "$configuration\MVVCVTKQtCleanRoom.exe") (
            $depsRoot) $true
    }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$buildRoot = Join-Path $repoRoot "out\build\$Preset"
$stageBase = Join-Path $repoRoot 'out\stage'
$verifyBase = Join-Path (
    Split-Path -Parent $repoRoot) 'MVVCVTK-sdk-verify'
$repoPrefix = [IO.Path]::GetFullPath($repoRoot)
if (-not $repoPrefix.EndsWith(
    [string][IO.Path]::DirectorySeparatorChar)) {
    $repoPrefix += [IO.Path]::DirectorySeparatorChar
}
if ([IO.Path]::GetFullPath($verifyBase).StartsWith(
    $repoPrefix,
    [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Clean-room verification must run outside the source repository.'
}
$packageRoot = Join-Path $repoRoot 'out\packages'
$depsVersion = '2026.08.21-deps.1'
$depsRoot = Join-Path $repoRoot "deps\$depsVersion-win-x64"
$script:cmakePath = (Get-Command cmake.exe -ErrorAction Stop).Source
$ctestPath = Join-Path (
    Split-Path -Parent $script:cmakePath) 'ctest.exe'
$powerShellPath = (Get-Command powershell.exe -ErrorAction Stop).Source

if ([string]::IsNullOrWhiteSpace($PackageRevision)) {
    $PackageRevision = Get-PackageRevision $repoRoot
}
if (-not [regex]::IsMatch(
        $PackageRevision,
        '^\d{4}\.\d{2}\.\d{2}-(git\.[0-9a-fA-F]{7,40}|rev\.\d+)$')) {
    throw "Unsafe package revision: $PackageRevision"
}
$directoryVersion = "$PackageRevision-win-x64"
$stage = Join-Path $stageBase $directoryVersion
$archiveName = "MVVCVTK-SDK-$directoryVersion.zip"

Clear-ConsumerEnv
Start-Command $script:cmakePath @('--preset', $Preset)
foreach ($configuration in @('Debug', 'Release')) {
    if ($SkipTests) {
        Start-Command $script:cmakePath @(
            '--build', $buildRoot,
            '--config', $configuration,
            '--target',
            'mvvcvtk_host',
            'mvvcvtk_orthogonal_crop',
            'mvvcvtk_gap_analysis',
            '--parallel'
        )
    }
    else {
        Start-Command $script:cmakePath @(
            '--build', $buildRoot,
            '--config', $configuration,
            '--parallel'
        )
    }
    if (-not $SkipTests) {
        Start-Command $ctestPath @(
            '--test-dir', $buildRoot,
            '-C', $configuration,
            '--output-on-failure'
        )
    }
}

Get-SafeAncestors (Split-Path -Parent $stageBase)
[IO.Directory]::CreateDirectory($stageBase) | Out-Null
Get-SafeAncestors $stageBase
Clear-SafeDirectory $stageBase $stage
foreach ($configuration in @('Debug', 'Release')) {
    Start-Command $script:cmakePath @(
        '--install', $buildRoot,
        '--config', $configuration,
        '--prefix', $stage
    )
}
Build-DependencyBundle $depsRoot (Join-Path $stage 'deps')

$buildInfo = Get-BuildInfo $buildRoot
Start-Command $powerShellPath @(
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $repoRoot 'tools\release\New-MVVCVTKSdkManifest.ps1'),
    '-Stage', $stage,
    '-RepoRoot', $repoRoot,
    '-PackageRevision', $PackageRevision,
    '-DirectoryVersion', $directoryVersion,
    '-DepsVersion', $depsVersion,
    '-CMakeVersion', $buildInfo.cmakeVersion,
    '-CMakeGenerator', $buildInfo.generator,
    '-CMakeGeneratorToolset', $buildInfo.generatorToolset,
    '-CompilerVersion', $buildInfo.compilerVersion,
    '-MsvcVersion', $buildInfo.msvcVersion,
    '-MsvcToolsVersion', $buildInfo.msvcToolsVersion,
    '-WindowsSdkVersion', $buildInfo.windowsSdkVersion,
    '-ReleaseInstructionSet', $buildInfo.releaseInstructionSet,
    '-ArchiveName', $archiveName
)
Start-Command $powerShellPath @(
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $repoRoot 'tools\release\Test-MVVCVTKSdk.ps1'),
    '-Stage', $stage,
    '-RepoRoot', $repoRoot,
    '-BuildRoot', $buildRoot,
    '-PackageRevision', $PackageRevision,
    '-DirectoryVersion', $directoryVersion,
    '-DepsVersion', $depsVersion
)

if (-not $SkipCleanRoom) {
    Get-SafeAncestors (Split-Path -Parent $verifyBase)
    [IO.Directory]::CreateDirectory($verifyBase) | Out-Null
    Get-SafeAncestors $verifyBase
    Clear-ConsumerEnv
    Start-CMakeConsumer $verifyBase $stage $repoRoot
    Start-QtConsumer $verifyBase $stage $repoRoot
}

Start-Command $powerShellPath @(
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $repoRoot 'tools\release\Pack-MVVCVTKSdk.ps1'),
    '-Stage', $stage,
    '-PackageRoot', $packageRoot,
    '-ArchiveName', $archiveName
)

Write-Host "MVVCVTK SDK stage: $stage"
Write-Host "MVVCVTK SDK package directory: $packageRoot"
