[CmdletBinding()]
param(
    [string]$Preset = 'vs2026-x64',
    [string]$PackageRevision,
    [string]$DepsRoot,
    [string]$DefXRoot,
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

function Get-ProcessEnv()
{
    $state = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($item in (Get-ChildItem Env:)) {
        $state[$item.Name] = [string]$item.Value
    }
    return ,$state
}

function Reset-ProcessEnv(
    [Collections.Generic.Dictionary[string, string]]$state)
{
    foreach ($item in @(Get-ChildItem Env:)) {
        if (-not $state.ContainsKey($item.Name)) {
            Remove-Item -LiteralPath "Env:$($item.Name)" `
                -ErrorAction SilentlyContinue
        }
    }
    foreach ($entry in $state.GetEnumerator()) {
        $currentItem = Get-Item -LiteralPath "Env:$($entry.Key)" `
            -ErrorAction SilentlyContinue
        if ($null -ne $currentItem -and
            ([string]$currentItem.Value -ceq $entry.Value)) {
            continue
        }
        [Environment]::SetEnvironmentVariable(
            $entry.Key,
            $entry.Value,
            'Process')
    }
}

function Get-IsPathWithin(
    [string]$base,
    [string]$candidate)
{
    $basePath = [IO.Path]::GetFullPath($base).TrimEnd(
        [IO.Path]::DirectorySeparatorChar)
    $candidatePath = [IO.Path]::GetFullPath($candidate)
    if ($candidatePath.Equals(
            $basePath,
            [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    $prefix = $basePath + [IO.Path]::DirectorySeparatorChar
    return $candidatePath.StartsWith(
        $prefix,
        [StringComparison]::OrdinalIgnoreCase)
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

function Build-SdkDependencies(
    [string]$source,
    [string]$destination)
{
    if (-not [IO.Directory]::Exists($source)) {
        throw "Dependency bundle is missing: $source"
    }
    Get-SafeAncestors (Split-Path -Parent $destination)
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    Get-SafeAncestors $destination

    foreach ($dependencyName in @('vtk', 'opencv')) {
        $sourcePath = Join-Path $source $dependencyName
        $destinationPath = Join-Path $destination $dependencyName
        if (-not [IO.Directory]::Exists($sourcePath)) {
            throw "Required SDK dependency is missing: $sourcePath"
        }
        $null = @(Get-SafeTreeItems $sourcePath)
        [IO.Directory]::CreateDirectory($destinationPath) | Out-Null
        Get-SafeAncestors $destinationPath
        & $script:robocopyPath $sourcePath $destinationPath /E /COPY:DAT /DCOPY:DAT /XJ /R:2 /W:1 /NFL /NDL /NJH /NJS /NP
        $robocopyCode = $LASTEXITCODE
        if ($robocopyCode -gt 7) {
            throw "robocopy failed with exit code $robocopyCode."
        }
        $global:LASTEXITCODE = 0
        $null = @(Get-SafeTreeItems $destinationPath)
    }
}

function Set-Utf8File(
    [string]$path,
    [string]$content)
{
    $parent = Split-Path -Parent $path
    Get-SafeAncestors $parent
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    Get-SafeAncestors $parent
    $normalized = $content.Replace("`r`n", "`n").Replace("`r", "`n")
    if (-not $normalized.EndsWith("`n")) {
        $normalized += "`n"
    }
    [IO.File]::WriteAllText(
        $path,
        $normalized,
        [Text.UTF8Encoding]::new($false))
}

function Build-CleanRoomSource([string]$sourceRoot)
{
    Get-SafeAncestors (Split-Path -Parent $sourceRoot)
    [IO.Directory]::CreateDirectory($sourceRoot) | Out-Null
    Get-SafeAncestors $sourceRoot

    $cmakeContent = @'
cmake_minimum_required(VERSION 4.2)

project(MVVCVTKCleanRoomProbe LANGUAGES CXX)

option(MVVCVTK_PROBE_CROP "Link the OrthogonalCrop SDK module." ON)
option(MVVCVTK_PROBE_GAP "Link the GapAnalysis SDK module." ON)
set(
    MVVCVTK_VERIFY_SURFACE_FILE
    ""
    CACHE FILEPATH
    "Build-tree MVVCVTK header surface used only by release verification."
)
if(NOT EXISTS "${MVVCVTK_VERIFY_SURFACE_FILE}")
    message(FATAL_ERROR
        "MVVCVTK verification surface is missing: "
        "${MVVCVTK_VERIFY_SURFACE_FILE}"
    )
endif()

set(_mvvcvtk_components Host)
set(_mvvcvtk_links MVVCVTK::Host)
set(_mvvcvtk_surface_categories HostAPI HostSupport)
if(MVVCVTK_PROBE_CROP OR MVVCVTK_PROBE_GAP)
    list(APPEND
        _mvvcvtk_surface_categories
        FeatureSPI
        FeatureSupport
    )
endif()
if(MVVCVTK_PROBE_CROP)
    list(APPEND _mvvcvtk_components OrthogonalCrop)
    list(APPEND _mvvcvtk_links MVVCVTK::OrthogonalCrop)
    list(APPEND _mvvcvtk_surface_categories OrthogonalCrop)
endif()
if(MVVCVTK_PROBE_GAP)
    list(APPEND _mvvcvtk_components GapAnalysis)
    list(APPEND _mvvcvtk_links MVVCVTK::GapAnalysis)
    list(APPEND _mvvcvtk_surface_categories GapAnalysis)
endif()

find_package(
    MVVCVTK
    CONFIG REQUIRED
    COMPONENTS ${_mvvcvtk_components}
)
if(NOT TARGET opencv_world)
    message(FATAL_ERROR "The staged OpenCV package has no opencv_world target.")
endif()
get_filename_component(
    _mvvcvtk_stage_include
    "${MVVCVTK_DIR}/../../../include"
    ABSOLUTE
)

add_executable(mvvcvtk_clean_room_probe main.cpp)
target_compile_features(mvvcvtk_clean_room_probe PRIVATE cxx_std_17)
target_link_libraries(
    mvvcvtk_clean_room_probe
    PRIVATE ${_mvvcvtk_links} opencv_world
)
if(MVVCVTK_PROBE_CROP)
    target_compile_definitions(
        mvvcvtk_clean_room_probe
        PRIVATE MVVCVTK_PROBE_CROP=1
    )
else()
    target_compile_definitions(
        mvvcvtk_clean_room_probe
        PRIVATE MVVCVTK_PROBE_CROP=0
    )
endif()
if(MVVCVTK_PROBE_GAP)
    target_compile_definitions(
        mvvcvtk_clean_room_probe
        PRIVATE MVVCVTK_PROBE_GAP=1
    )
else()
    target_compile_definitions(
        mvvcvtk_clean_room_probe
        PRIVATE MVVCVTK_PROBE_GAP=0
    )
endif()

file(STRINGS
    "${MVVCVTK_VERIFY_SURFACE_FILE}"
    _mvvcvtk_surface_entries
    ENCODING UTF-8
)
set(_mvvcvtk_entry_headers)
set(_mvvcvtk_entry_keys)
foreach(_mvvcvtk_surface_entry IN LISTS _mvvcvtk_surface_entries)
    if(NOT _mvvcvtk_surface_entry MATCHES "^([^|]+)\\|([^|]+)$")
        message(FATAL_ERROR
            "Invalid MVVCVTK header surface entry: ${_mvvcvtk_surface_entry}"
        )
    endif()
    set(_mvvcvtk_surface_category "${CMAKE_MATCH_1}")
    set(_mvvcvtk_surface_header "${CMAKE_MATCH_2}")
    if(NOT _mvvcvtk_surface_header MATCHES
        "^MVVCVTK/(API|SPI)/.+\\.h$")
        message(FATAL_ERROR
            "Unpartitioned MVVCVTK SDK header: ${_mvvcvtk_surface_header}"
        )
    endif()
    if(_mvvcvtk_surface_category IN_LIST _mvvcvtk_surface_categories)
        string(TOLOWER "${_mvvcvtk_surface_header}" _mvvcvtk_surface_key)
        list(FIND
            _mvvcvtk_entry_keys
            "${_mvvcvtk_surface_key}"
            _mvvcvtk_key_index
        )
        if(_mvvcvtk_key_index EQUAL -1)
            list(APPEND _mvvcvtk_entry_headers "${_mvvcvtk_surface_header}")
            list(APPEND _mvvcvtk_entry_keys "${_mvvcvtk_surface_key}")
        endif()
    endif()
endforeach()
if(NOT _mvvcvtk_entry_headers)
    message(FATAL_ERROR "MVVCVTK probe selected no public headers.")
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/headers")
set(_mvvcvtk_header_sources)
foreach(_mvvcvtk_header IN LISTS _mvvcvtk_entry_headers)
    string(MAKE_C_IDENTIFIER "${_mvvcvtk_header}" _mvvcvtk_source_name)
    set(
        _mvvcvtk_source
        "${CMAKE_CURRENT_BINARY_DIR}/headers/${_mvvcvtk_source_name}.cpp"
    )
    file(CONFIGURE
        OUTPUT "${_mvvcvtk_source}"
        CONTENT "#include <${_mvvcvtk_header}>\n"
        NEWLINE_STYLE UNIX
    )
    list(APPEND _mvvcvtk_header_sources "${_mvvcvtk_source}")
endforeach()
add_library(
    mvvcvtk_installed_headers
    OBJECT
    ${_mvvcvtk_header_sources}
)
target_link_libraries(
    mvvcvtk_installed_headers
    PRIVATE ${_mvvcvtk_links}
)
add_dependencies(mvvcvtk_clean_room_probe mvvcvtk_installed_headers)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/forbidden")
function(SetForbiddenHeader headerName)
    if(EXISTS "${_mvvcvtk_stage_include}/${headerName}")
        message(FATAL_ERROR
            "Installed SDK contains a private header: ${headerName}"
        )
    endif()
    string(MAKE_C_IDENTIFIER "${headerName}" headerId)
    set(sourceFile
        "${CMAKE_CURRENT_BINARY_DIR}/forbidden/${headerId}.cpp"
    )
    file(CONFIGURE
        OUTPUT "${sourceFile}"
        CONTENT "#include <${headerName}>\nint main() { return 0; }\n"
        NEWLINE_STYLE UNIX
    )
    try_compile(
        isForbiddenHeaderAccepted
        SOURCES "${sourceFile}"
        NO_CACHE
        CMAKE_FLAGS
            "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
        COMPILE_DEFINITIONS
            "/utf-8"
        LINK_LIBRARIES ${_mvvcvtk_links}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        OUTPUT_VARIABLE forbiddenCompileOutput
    )
    if(isForbiddenHeaderAccepted)
        message(FATAL_ERROR
            "Installed SDK exposes a private header: ${headerName}"
        )
    endif()
    string(FIND "${forbiddenCompileOutput}" "C1083" missingHeaderIndex)
    if(missingHeaderIndex EQUAL -1)
        message(FATAL_ERROR
            "Private-header probe failed for an unrelated reason:\n"
            "${forbiddenCompileOutput}"
        )
    endif()
endfunction()
SetForbiddenHeader(Data/DataManager.h)
SetForbiddenHeader(Host/HostViewRuntimeRegistry.h)
SetForbiddenHeader(Render/Strategies/SliceStrategy.h)
SetForbiddenHeader(Render/Support/FeatureOverlayBase.h)
if(MVVCVTK_PROBE_CROP)
    SetForbiddenHeader(Algorithms/CropAlgorithm.h)
endif()
if(MVVCVTK_PROBE_GAP)
    SetForbiddenHeader(Services/GapAnalysisService.h)
endif()
'@

    $mainContent = @'
#include "MVVCVTK/API/Host/VtkAppHostSession.h"
#include "MVVCVTK/API/Host/Types/HostRequestTypes.h"
#include <opencv2/core/utility.hpp>

#if MVVCVTK_PROBE_CROP
#include "MVVCVTK/API/Features/OrthogonalCrop/Host/CropHostFeature.h"
#include "MVVCVTK/SPI/Host/HostFeature.h"
#endif
#if MVVCVTK_PROBE_GAP
#include "MVVCVTK/API/Features/GapAnalysis/Host/GapHostFeature.h"
#include "MVVCVTK/SPI/Host/HostFeature.h"
#endif

#include <memory>
#include <string_view>

#if MVVCVTK_PROBE_CROP || MVVCVTK_PROBE_GAP
class CleanRoomFeature final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override
    {
        return "CleanRoomFeature";
    }
    bool AttachHost(const HostFeatureContext&) override { return true; }
    bool DetachHost() override { return true; }
    bool OnHostTick() override { return true; }
};
#endif

int main()
{
    if (cv::getBuildInformation().empty()) {
        return 5;
    }

    VtkAppHostSession session{ HostSessionConfig{} };
    if (!session.GetIsStopped()) {
        return 1;
    }

    const HostViewTarget sceneTarget{};
    if (session.GetSceneViewState(sceneTarget)
        || !session.GetSceneViewStates().empty()) {
        return 6;
    }

    HostViewSetRequest viewRequest{};
    HostVolumeTransferFunction volumeTransferFunction{};
    volumeTransferFunction.colorNodes = {
        { -1000.0, 0.0, 0.0, 0.0 },
        { 3000.0, 1.0, 1.0, 1.0 }
    };
    volumeTransferFunction.opacityNodes = {
        { -1000.0, 0.0 },
        { 3000.0, 1.0 }
    };
    viewRequest.volumeTransferFunction = volumeTransferFunction;
    viewRequest.volumeQuality = HostVolumeQuality::High;
    bool isFeatureContractValid = true;
#if MVVCVTK_PROBE_CROP || MVVCVTK_PROBE_GAP
    isFeatureContractValid = CleanRoomFeature{}.GetFeatureId()
        == std::string_view{ "CleanRoomFeature" };
#endif
    if (!viewRequest.volumeTransferFunction
        || !viewRequest.volumeQuality
        || *viewRequest.volumeQuality != HostVolumeQuality::High
        || viewRequest.volumeTransferFunction->colorNodes.size() != 2
        || !isFeatureContractValid) {
        return 4;
    }

#if MVVCVTK_PROBE_CROP
    const std::shared_ptr<HostFeature> crop =
        std::make_shared<CropHostFeature>(CropHostConfig{});
    if (!crop || crop->GetFeatureId()
            != std::string_view{ "OrthogonalCrop" }) {
        return 2;
    }
#endif

#if MVVCVTK_PROBE_GAP
    const std::shared_ptr<HostFeature> gap =
        std::make_shared<GapHostFeature>(GapHostConfig{});
    if (!gap || gap->GetFeatureId()
            != std::string_view{ "GapAnalysis" }) {
        return 3;
    }
#endif

    return 0;
}
'@

    Set-Utf8File (Join-Path $sourceRoot 'CMakeLists.txt') $cmakeContent
    Set-Utf8File (Join-Path $sourceRoot 'main.cpp') $mainContent
    $null = @(Get-SafeTreeItems $sourceRoot)
}

function Get-PackageRevision([string]$repoRoot)
{
    $gitStatus = @(& $script:gitPath -C $repoRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot inspect the SDK source state.'
    }
    $dateVersion = Get-Date -Format 'yyyy.MM.dd'
    if ($gitStatus.Count -eq 0) {
        $gitShort = [string]::Join(
            '',
            (& $script:gitPath -C $repoRoot rev-parse --short=12 HEAD))
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

function Clear-BuildEnv()
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
        $item = Get-Item -LiteralPath "Env:$name" `
            -ErrorAction SilentlyContinue
        if ($null -ne $item -and
            -not [string]::IsNullOrEmpty([string]$item.Value)) {
            Remove-Item -LiteralPath "Env:$name"
        }
    }
    foreach ($item in (Get-ChildItem Env:)) {
        if ($item.Name -match
            '^(CMAKE_PROJECT_|CMAKE_FIND_|MVVCVTK_|QMAKE_|VCPKG_)' -and
            -not [string]::IsNullOrEmpty([string]$item.Value)) {
            Remove-Item -LiteralPath "Env:$($item.Name)" `
                -ErrorAction SilentlyContinue
        }
    }
}

function Get-RuntimeDirs([string]$depsRoot)
{
    return @(
        (Join-Path $depsRoot 'official\opencv\x64\vc16\bin')
        (Join-Path $depsRoot 'official\vtk\bin')
    )
}

function Start-RuntimeExecutable(
    [string]$executable,
    [string]$depsRoot,
    [string]$configuration,
    [bool]$isGapEnabled)
{
    $oldPath = $env:PATH
    $hasPluginPath = Test-Path -LiteralPath Env:QT_PLUGIN_PATH
    $oldPluginPath = $env:QT_PLUGIN_PATH
    $hasGapRuntime = Test-Path -LiteralPath Env:MVVCVTK_GAP_RUNTIME_DIR
    $oldGapRuntime = $env:MVVCVTK_GAP_RUNTIME_DIR
    $oldLocation = (Get-Location).Path
    try {
        $systemPath = @(
            (Join-Path $env:SystemRoot 'System32')
            $env:SystemRoot
        )
        $env:PATH = (@(Get-RuntimeDirs $depsRoot) +
                $systemPath) -join ';'
        if ($isGapEnabled) {
            $gapRuntime = [IO.Path]::GetFullPath(
                (Join-Path $depsRoot `
                    "third_party\defx\bin\$configuration"))
            if (-not [IO.Directory]::Exists($gapRuntime)) {
                throw "Gap runtime directory is missing: $gapRuntime"
            }
            $env:MVVCVTK_GAP_RUNTIME_DIR = $gapRuntime
        }
        else {
            Remove-Item -LiteralPath Env:MVVCVTK_GAP_RUNTIME_DIR `
                -ErrorAction SilentlyContinue
        }
        if (-not [string]::IsNullOrEmpty($oldPluginPath)) {
            Remove-Item -LiteralPath Env:QT_PLUGIN_PATH
        }
        Set-Location -LiteralPath (Split-Path -Parent $executable)
        Start-Command $executable @()
    }
    finally {
        try {
            $env:PATH = $oldPath
            if (-not $hasPluginPath) {
                Remove-Item -LiteralPath Env:QT_PLUGIN_PATH `
                    -ErrorAction SilentlyContinue
            }
            elseif (-not [string]::IsNullOrEmpty($oldPluginPath)) {
                $env:QT_PLUGIN_PATH = $oldPluginPath
            }
            if (-not $hasGapRuntime) {
                Remove-Item -LiteralPath Env:MVVCVTK_GAP_RUNTIME_DIR `
                    -ErrorAction SilentlyContinue
            }
            else {
                $env:MVVCVTK_GAP_RUNTIME_DIR = $oldGapRuntime
            }
        }
        finally {
            Set-Location -LiteralPath $oldLocation
        }
    }
}

function Start-CleanRoomProbe(
    [string]$verifyBase,
    [string]$stage,
    [string]$productBuildRoot)
{
    $probeRoot = Join-Path $verifyBase 'cmake'
    Clear-SafeDirectory $verifyBase $probeRoot
    $sourceRoot = Join-Path $probeRoot 'src'
    Build-CleanRoomSource $sourceRoot
    $probeCases = @(
        [ordered]@{ name = 'host'; crop = 'OFF'; gap = 'OFF' }
        [ordered]@{ name = 'host-crop'; crop = 'ON'; gap = 'OFF' }
        [ordered]@{ name = 'host-gap'; crop = 'OFF'; gap = 'ON' }
        [ordered]@{ name = 'host-crop-gap'; crop = 'ON'; gap = 'ON' }
    )
    foreach ($probeCase in $probeCases) {
        $caseBuildRoot = Join-Path $probeRoot "build-$($probeCase.name)"
        Start-Command $script:cmakePath @(
            '-S', $sourceRoot,
            '-B', $caseBuildRoot,
            '-G', 'Visual Studio 18 2026',
            '-A', 'x64',
            '-T', 'v145,host=x64',
            "-DCMAKE_PREFIX_PATH=$stage",
            "-DMVVCVTK_VERIFY_SURFACE_FILE=$(Join-Path $productBuildRoot 'MVVCVTKHeaderSurface.txt')",
            "-DMVVCVTK_PROBE_CROP=$($probeCase.crop)",
            "-DMVVCVTK_PROBE_GAP=$($probeCase.gap)"
        )
        foreach ($configuration in @('Debug', 'Release')) {
            Start-Command $script:cmakePath @(
                '--build', $caseBuildRoot,
                '--config', $configuration,
                '--parallel'
            )
            Start-RuntimeExecutable (
                Join-Path $caseBuildRoot `
                    "$configuration\mvvcvtk_clean_room_probe.exe") (
                Join-Path $stage 'deps') $configuration (
                $probeCase.gap -eq 'ON')
        }
    }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not [regex]::IsMatch($Preset, '^[A-Za-z0-9][A-Za-z0-9._-]*$')) {
    throw "Unsafe CMake preset name: $Preset"
}
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
if ([string]::IsNullOrWhiteSpace($DepsRoot)) {
    $DepsRoot = Join-Path $repoRoot 'deps\official'
}
$depsRootPath = [IO.Path]::GetFullPath($DepsRoot)
if (-not [IO.Directory]::Exists($depsRootPath)) {
    throw "Dependency bundle is missing: $depsRootPath"
}
Get-SafeAncestors $depsRootPath
if ([string]::IsNullOrWhiteSpace($DefXRoot)) {
    $DefXRoot = Join-Path $repoRoot 'deps\third_party\defx'
}
$defxRootPath = [IO.Path]::GetFullPath($DefXRoot)
foreach ($defxArtifact in @(
        'include\DefXAnalysisService.h',
        'include\DefXTypes.h',
        'bin\Debug\DefXAnalysis.dll',
        'lib\Debug\DefXAnalysis.lib',
        'bin\Release\DefXAnalysis.dll',
        'lib\Release\DefXAnalysis.lib')) {
    $artifactPath = Join-Path $defxRootPath $defxArtifact
    if (-not [IO.File]::Exists($artifactPath)) {
        throw "DefX artifact is missing: $artifactPath"
    }
    Get-SafeAncestors $artifactPath
}
$script:cmakePath = (Get-Command cmake.exe -CommandType Application `
        -ErrorAction Stop).Source
$script:gitPath = (Get-Command git.exe -CommandType Application `
        -ErrorAction Stop).Source
$script:robocopyPath = (Get-Command robocopy.exe -CommandType Application `
        -ErrorAction Stop).Source
$ctestPath = Join-Path (
    Split-Path -Parent $script:cmakePath) 'ctest.exe'
$powerShellPath = (Get-Command powershell.exe -CommandType Application `
        -ErrorAction Stop).Source

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

$processEnv = Get-ProcessEnv
$originalLocation = (Get-Location).Path
if ((Get-IsPathWithin $stage $originalLocation) -or
    (Get-IsPathWithin (Join-Path $verifyBase 'cmake') $originalLocation)) {
    throw "The caller location would be removed by this SDK build: $originalLocation"
}
try {
    Set-Location -LiteralPath $repoRoot
    Clear-BuildEnv
    Start-Command $script:cmakePath @(
        '--preset', $Preset,
        '-B', $buildRoot,
        "-DMVVCVTK_DEPS_ROOT=$depsRootPath",
        "-DMVVCVTK_DEFX_ROOT=$defxRootPath"
    )
    $null = Get-BuildInfo $buildRoot
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
    Build-SdkDependencies $depsRootPath (Join-Path $stage 'deps\official')

    Start-Command $powerShellPath @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $repoRoot 'tools\release\Test-MVVCVTKSdk.ps1'),
        '-Stage', $stage,
        '-RepoRoot', $repoRoot,
        '-BuildRoot', $buildRoot
    )

    if (-not $SkipCleanRoom) {
        Get-SafeAncestors (Split-Path -Parent $verifyBase)
        [IO.Directory]::CreateDirectory($verifyBase) | Out-Null
        Get-SafeAncestors $verifyBase
        Clear-BuildEnv
        Start-CleanRoomProbe $verifyBase $stage $buildRoot
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
}
finally {
    try {
        Reset-ProcessEnv $processEnv
    }
    finally {
        Set-Location -LiteralPath $originalLocation
    }
}
