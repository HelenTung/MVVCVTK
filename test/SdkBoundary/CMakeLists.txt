set(
    MVVCVTK_HOST_DEPENDENCY_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/host-source-dependencies"
)
set(
    MVVCVTK_SPI_DEPENDENCY_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/spi-source-dependencies"
)
set(
    MVVCVTK_CROP_DEPENDENCY_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/crop-source-dependencies"
)
set(
    MVVCVTK_GAP_DEPENDENCY_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/gap-source-dependencies"
)
set(
    MVVCVTK_PROBE_SOURCE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated-probes"
)
file(MAKE_DIRECTORY
    "${MVVCVTK_HOST_DEPENDENCY_DIR}"
    "${MVVCVTK_SPI_DEPENDENCY_DIR}"
    "${MVVCVTK_CROP_DEPENDENCY_DIR}"
    "${MVVCVTK_GAP_DEPENDENCY_DIR}"
    "${MVVCVTK_PROBE_SOURCE_DIR}"
)

# Probe 是构建边界的输入，不是需要长期维护的产品源码；配置时按清单生成。
function(BuildProbe outputName fileName sourceContent)
    set(probePath "${MVVCVTK_PROBE_SOURCE_DIR}/${fileName}")
    file(CONFIGURE
        OUTPUT "${probePath}"
        CONTENT "${sourceContent}"
        @ONLY
        NEWLINE_STYLE UNIX
    )
    set(${outputName} "${probePath}" PARENT_SCOPE)
endfunction()

function(BuildTypeProbe outputName fileName headerName typeName)
    string(CONCAT probeSource
        "#include \"${headerName}\"\n\n"
        "static_assert(sizeof(${typeName}) > 0);\n"
    )
    BuildProbe(${outputName} "${fileName}" "${probeSource}")
    set(${outputName} "${${outputName}}" PARENT_SCOPE)
endfunction()

function(BuildForbiddenProbe outputName fileName)
    set(probeSource)
    foreach(headerName IN LISTS ARGN)
        string(APPEND probeSource "#include \"${headerName}\"\n")
    endforeach()
    string(APPEND probeSource
        "\nint main()\n"
        "{\n"
        "    return 0;\n"
        "}\n"
    )
    BuildProbe(${outputName} "${fileName}" "${probeSource}")
    set(${outputName} "${${outputName}}" PARENT_SCOPE)
endfunction()

BuildTypeProbe(
    MVVCVTK_HOST_REQUEST_PROBE
    HostRequestTypesHeaderProbe.cpp
    Host/Types/HostRequestTypes.h
    HostViewSetRequest
)
BuildTypeProbe(
    MVVCVTK_HOST_SESSION_PROBE
    HostSessionHeaderProbe.cpp
    Host/VtkAppHostSession.h
    VtkAppHostSession
)
BuildProbe(
    MVVCVTK_FEATURE_SPI_PROBE
    FeatureSpiHeaderProbe.cpp
    [=[#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Contracts/RenderEffect.h"
#include "Render/Contracts/VisualStrategy.h"

static_assert(sizeof(FeatureViewService) > 0);
static_assert(sizeof(OverlayService) > 0);
static_assert(sizeof(RenderEffect) > 0);
static_assert(sizeof(AbstractVisualStrategy) > 0);
]=]
)
BuildTypeProbe(
    MVVCVTK_HOST_FEATURE_PROBE
    HostFeatureHeaderProbe.cpp
    Host/HostFeature.h
    HostFeature
)
BuildTypeProbe(
    MVVCVTK_CROP_HOST_PROBE
    CropHostFeatureHeaderProbe.cpp
    Host/CropHostFeature.h
    CropHostFeature
)
BuildTypeProbe(
    MVVCVTK_CROP_TYPES_PROBE
    CropTypesHeaderProbe.cpp
    OrthogonalCropTypes.h
    CropBuildResult
)
BuildTypeProbe(
    MVVCVTK_GAP_HOST_PROBE
    GapHostFeatureHeaderProbe.cpp
    Host/GapHostFeature.h
    GapHostFeature
)
BuildTypeProbe(
    MVVCVTK_GAP_TYPES_PROBE
    GapHostTypesHeaderProbe.cpp
    Host/GapHostTypes.h
    GapHostState
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_DATA_PROBE
    ForbiddenDataManagerProbe.cpp
    Host/Types/HostValueTypes.h
    Data/DataManager.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_HOST_PROBE
    ForbiddenHostRuntimeProbe.cpp
    Host/Types/HostValueTypes.h
    Host/HostViewRuntimeRegistry.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_SLICE_PROBE
    ForbiddenSliceStrategyProbe.cpp
    App/AppTypes.h
    Render/Strategies/SliceStrategy.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_SUPPORT_PROBE
    ForbiddenFeatureSupportProbe.cpp
    Render/Support/FeatureVisualStrategy.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_CROP_PROBE
    ForbiddenCropAlgorithmProbe.cpp
    Algorithms/CropAlgorithm.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_GAP_PROBE
    ForbiddenGapServiceProbe.cpp
    Services/GapAnalysisService.h
)

add_library(mvvcvtk_host_header_probes OBJECT
    "${MVVCVTK_HOST_REQUEST_PROBE}"
    "${MVVCVTK_HOST_SESSION_PROBE}"
)
SetTargetDefaults(mvvcvtk_host_header_probes)
target_link_libraries(
    mvvcvtk_host_header_probes
    PRIVATE MVVCVTK::HostAPI
)
target_compile_options(
    mvvcvtk_host_header_probes
    PRIVATE /sourceDependencies "${MVVCVTK_HOST_DEPENDENCY_DIR}"
)
set_target_properties(
    mvvcvtk_host_header_probes
    PROPERTIES
        PROJECT_LABEL MVVCVTKHostHeaderProbes
        FOLDER "MVVCVTK/Tests"
)

add_library(mvvcvtk_feature_spi_header_probe OBJECT
    "${MVVCVTK_FEATURE_SPI_PROBE}"
    "${MVVCVTK_HOST_FEATURE_PROBE}"
)
SetTargetDefaults(mvvcvtk_feature_spi_header_probe)
target_link_libraries(
    mvvcvtk_feature_spi_header_probe
    PRIVATE MVVCVTK::FeatureSPI
)
target_compile_options(
    mvvcvtk_feature_spi_header_probe
    PRIVATE /sourceDependencies "${MVVCVTK_SPI_DEPENDENCY_DIR}"
)
set_target_properties(
    mvvcvtk_feature_spi_header_probe
    PROPERTIES
        PROJECT_LABEL MVVCVTKFeatureSpiHeaderProbe
        FOLDER "MVVCVTK/Tests"
)

if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    add_library(mvvcvtk_crop_header_probes OBJECT
        "${MVVCVTK_CROP_HOST_PROBE}"
        "${MVVCVTK_CROP_TYPES_PROBE}"
    )
    SetTargetDefaults(mvvcvtk_crop_header_probes)
    target_link_libraries(
        mvvcvtk_crop_header_probes
        PRIVATE MVVCVTK::OrthogonalCrop
    )
    target_compile_options(
        mvvcvtk_crop_header_probes
        PRIVATE /sourceDependencies "${MVVCVTK_CROP_DEPENDENCY_DIR}"
    )
    set_target_properties(
        mvvcvtk_crop_header_probes
        PROPERTIES
            PROJECT_LABEL MVVCVTKCropHeaderProbes
            FOLDER "MVVCVTK/Tests"
    )
endif()

if(MVVCVTK_BUILD_GAP_ANALYSIS)
    add_library(mvvcvtk_gap_header_probes OBJECT
        "${MVVCVTK_GAP_HOST_PROBE}"
        "${MVVCVTK_GAP_TYPES_PROBE}"
    )
    SetTargetDefaults(mvvcvtk_gap_header_probes)
    target_link_libraries(
        mvvcvtk_gap_header_probes
        PRIVATE MVVCVTK::GapAnalysis
    )
    target_compile_options(
        mvvcvtk_gap_header_probes
        PRIVATE /sourceDependencies "${MVVCVTK_GAP_DEPENDENCY_DIR}"
    )
    set_target_properties(
        mvvcvtk_gap_header_probes
        PROPERTIES
            PROJECT_LABEL MVVCVTKGapHeaderProbes
            FOLDER "MVVCVTK/Tests"
    )
endif()

function(SetClosureTest testName dependencyFile stagingRoot)
    set(expectedHeaders ${ARGN})
    list(JOIN expectedHeaders "," expectedHeaderCsv)
    add_test(
        NAME ${testName}
        COMMAND "${CMAKE_COMMAND}"
            "-DDEPENDENCY_FILE=${dependencyFile}"
            "-DSTAGING_ROOT=${stagingRoot}"
            "-DEXPECTED_HEADERS=${expectedHeaderCsv}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyHeaderClosure.cmake"
    )
endfunction()

SetClosureTest(
    SdkBoundary.HostSessionClosure
    "${MVVCVTK_HOST_DEPENDENCY_DIR}/HostSessionHeaderProbe.cpp.json"
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
    Data/ImageReadTypes.h
    Host/Types/HostInputTypes.h
    Host/Types/HostRequest.h
    Host/Types/HostSessionTypes.h
    Host/Types/HostValueTypes.h
    Host/VtkAppHostSession.h
)
SetClosureTest(
    SdkBoundary.HostFeatureClosure
    "${MVVCVTK_SPI_DEPENDENCY_DIR}/HostFeatureHeaderProbe.cpp.json"
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
    App/ViewTypes.h
    Data/ImageReadTypes.h
    Data/TrustedImageState.h
    Host/HostFeature.h
    Host/Types/HostFeatureViewTypes.h
    Host/Types/HostValueTypes.h
    Interaction/InteractionTypes.h
)
SetClosureTest(
    SdkBoundary.HostRequestTypesClosure
    "${MVVCVTK_HOST_DEPENDENCY_DIR}/HostRequestTypesHeaderProbe.cpp.json"
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
    Host/Types/HostRequest.h
    Host/Types/HostRequestTypes.h
    Host/Types/HostValueTypes.h
)
SetClosureTest(
    SdkBoundary.FeatureSPIClosure
    "${MVVCVTK_SPI_DEPENDENCY_DIR}/FeatureSpiHeaderProbe.cpp.json"
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
    App/AppTypes.h
    App/ViewTypes.h
    App/Services/FeatureViewService.h
    Render/Contracts/OverlayService.h
    Render/Contracts/RenderEffect.h
    Render/Contracts/VisualStrategy.h
)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    SetClosureTest(
        SdkBoundary.CropHostClosure
        "${MVVCVTK_CROP_DEPENDENCY_DIR}/CropHostFeatureHeaderProbe.cpp.json"
        "${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
        Host/CropHostFeature.h
        OrthogonalCropTypes.h
    )
    SetClosureTest(
        SdkBoundary.CropTypesClosure
        "${MVVCVTK_CROP_DEPENDENCY_DIR}/CropTypesHeaderProbe.cpp.json"
        "${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
        OrthogonalCropTypes.h
    )
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    SetClosureTest(
        SdkBoundary.GapHostClosure
        "${MVVCVTK_GAP_DEPENDENCY_DIR}/GapHostFeatureHeaderProbe.cpp.json"
        "${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
        Host/GapHostFeature.h
        Host/GapHostTypes.h
    )
    SetClosureTest(
        SdkBoundary.GapHostTypesClosure
        "${MVVCVTK_GAP_DEPENDENCY_DIR}/GapHostTypesHeaderProbe.cpp.json"
        "${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
        Host/GapHostTypes.h
    )
endif()

set(MVVCVTK_EXPECTED_HOST_STAGE
    Data/ImageReadTypes.h
    Host/Types/HostInputTypes.h
    Host/Types/HostRequest.h
    Host/Types/HostRequestTypes.h
    Host/Types/HostSessionTypes.h
    Host/Types/HostValueTypes.h
    Host/VtkAppHostSession.h
)
set(MVVCVTK_EXPECTED_SPI_STAGE
    App/AppTypes.h
    App/ViewTypes.h
    App/Services/FeatureViewService.h
    Data/ImageReadTypes.h
    Data/TrustedImageState.h
    Host/HostFeature.h
    Host/Types/HostFeatureViewTypes.h
    Host/Types/HostInputTypes.h
    Host/Types/HostValueTypes.h
    Interaction/InteractionTypes.h
    Render/Contracts/OverlayService.h
    Render/Contracts/RenderEffect.h
    Render/Contracts/VisualStrategy.h
)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    set(MVVCVTK_EXPECTED_CROP_STAGE
        Host/CropHostFeature.h
        OrthogonalCropTypes.h
    )
else()
    set(MVVCVTK_EXPECTED_CROP_STAGE)
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    set(MVVCVTK_EXPECTED_GAP_STAGE
        Host/GapHostFeature.h
        Host/GapHostTypes.h
    )
else()
    set(MVVCVTK_EXPECTED_GAP_STAGE)
endif()
list(JOIN MVVCVTK_EXPECTED_HOST_STAGE "," MVVCVTK_EXPECTED_HOST_STAGE_CSV)
list(JOIN MVVCVTK_EXPECTED_SPI_STAGE "," MVVCVTK_EXPECTED_SPI_STAGE_CSV)
list(JOIN MVVCVTK_EXPECTED_CROP_STAGE "," MVVCVTK_EXPECTED_CROP_STAGE_CSV)
list(JOIN MVVCVTK_EXPECTED_GAP_STAGE "," MVVCVTK_EXPECTED_GAP_STAGE_CSV)
add_test(
    NAME SdkBoundary.StagingInventory
    COMMAND "${CMAKE_COMMAND}"
        "-DHOST_STAGING_ROOT=${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
        "-DSPI_STAGING_ROOT=${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
        "-DCROP_STAGING_ROOT=${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
        "-DGAP_STAGING_ROOT=${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
        "-DEXPECTED_HOST_HEADERS=${MVVCVTK_EXPECTED_HOST_STAGE_CSV}"
        "-DEXPECTED_SPI_HEADERS=${MVVCVTK_EXPECTED_SPI_STAGE_CSV}"
        "-DEXPECTED_CROP_HEADERS=${MVVCVTK_EXPECTED_CROP_STAGE_CSV}"
        "-DEXPECTED_GAP_HEADERS=${MVVCVTK_EXPECTED_GAP_STAGE_CSV}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifySdkStage.cmake"
)

function(SetSourceRootGuard targetName propertyName sourceRoot)
    get_target_property(targetDirectories ${targetName} ${propertyName})
    if(NOT targetDirectories)
        return()
    endif()
    foreach(targetDirectory IN LISTS targetDirectories)
        string(
            FIND
            "${targetDirectory}"
            "${sourceRoot}"
            sourceRootIndex
        )
        if(NOT sourceRootIndex EQUAL -1)
            message(FATAL_ERROR
                "${targetName} exposes a source include root through "
                "${propertyName}: ${targetDirectory}"
            )
        endif()
    endforeach()
endfunction()

function(SetHostBinaryGuard targetName)
    get_target_property(targetLinks ${targetName} INTERFACE_LINK_LIBRARIES)
    if(NOT targetLinks)
        return()
    endif()
    list(FIND targetLinks "MVVCVTK::Host" hostAliasIndex)
    list(FIND targetLinks "mvvcvtk_host" hostTargetIndex)
    if(NOT hostAliasIndex EQUAL -1 OR NOT hostTargetIndex EQUAL -1)
        message(FATAL_ERROR
            "${targetName} must not depend on the Host implementation target."
        )
    endif()
endfunction()

function(SetHeaderGuard sourceFile headerPath stagingRoot)
    if(EXISTS "${stagingRoot}/${headerPath}")
        message(FATAL_ERROR
            "Forbidden SDK header is staged: ${headerPath}"
        )
    endif()
    get_filename_component(probeName "${sourceFile}" NAME_WE)
    try_compile(
        isForbiddenHeaderAccepted
        SOURCES "${sourceFile}"
        CMAKE_FLAGS
            "-DCMAKE_CXX_STANDARD=17"
            "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
        COMPILE_DEFINITIONS
            "/I${stagingRoot}"
            "/utf-8"
        OUTPUT_VARIABLE forbiddenCompileOutput
    )
    if(isForbiddenHeaderAccepted)
        message(FATAL_ERROR
            "${probeName} unexpectedly compiled against the SDK staging root."
        )
    endif()
    string(FIND "${forbiddenCompileOutput}" "C1083" missingHeaderIndex)
    if(missingHeaderIndex EQUAL -1)
        message(FATAL_ERROR
            "${probeName} failed for an unrelated reason:\n"
            "${forbiddenCompileOutput}"
        )
    endif()
endfunction()

SetSourceRootGuard(
    mvvcvtk_host_api
    INTERFACE_INCLUDE_DIRECTORIES
    "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
)
SetSourceRootGuard(
    mvvcvtk_feature_spi
    INTERFACE_INCLUDE_DIRECTORIES
    "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
)
SetSourceRootGuard(
    mvvcvtk_host
    INTERFACE_INCLUDE_DIRECTORIES
    "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
)
SetHostBinaryGuard(mvvcvtk_host_api)
SetHostBinaryGuard(mvvcvtk_feature_spi)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    SetSourceRootGuard(
        mvvcvtk_orthogonal_crop
        INTERFACE_INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include"
    )
    SetSourceRootGuard(
        mvvcvtk_orthogonal_crop
        INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
    )
    SetHostBinaryGuard(mvvcvtk_orthogonal_crop)
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    SetSourceRootGuard(
        mvvcvtk_gap_analysis
        INTERFACE_INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include"
    )
    SetSourceRootGuard(
        mvvcvtk_gap_analysis
        INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
    )
    SetHostBinaryGuard(mvvcvtk_gap_analysis)
endif()
SetHeaderGuard(
    "${MVVCVTK_FORBID_DATA_PROBE}"
    Data/DataManager.h
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
)
SetHeaderGuard(
    "${MVVCVTK_FORBID_HOST_PROBE}"
    Host/HostViewRuntimeRegistry.h
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
)
SetHeaderGuard(
    "${MVVCVTK_FORBID_SLICE_PROBE}"
    Render/Strategies/SliceStrategy.h
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
)
SetHeaderGuard(
    "${MVVCVTK_FORBID_SUPPORT_PROBE}"
    Render/Support/FeatureVisualStrategy.h
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    SetHeaderGuard(
        "${MVVCVTK_FORBID_CROP_PROBE}"
        Algorithms/CropAlgorithm.h
        "${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
    )
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    SetHeaderGuard(
        "${MVVCVTK_FORBID_GAP_PROBE}"
        Services/GapAnalysisService.h
        "${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
    )
endif()

unset(MVVCVTK_EXPECTED_CROP_STAGE)
unset(MVVCVTK_EXPECTED_CROP_STAGE_CSV)
unset(MVVCVTK_EXPECTED_SPI_STAGE)
unset(MVVCVTK_EXPECTED_SPI_STAGE_CSV)
unset(MVVCVTK_EXPECTED_GAP_STAGE)
unset(MVVCVTK_EXPECTED_GAP_STAGE_CSV)
unset(MVVCVTK_EXPECTED_HOST_STAGE)
unset(MVVCVTK_EXPECTED_HOST_STAGE_CSV)
unset(MVVCVTK_CROP_HOST_PROBE)
unset(MVVCVTK_CROP_TYPES_PROBE)
unset(MVVCVTK_FEATURE_SPI_PROBE)
unset(MVVCVTK_FORBID_CROP_PROBE)
unset(MVVCVTK_FORBID_DATA_PROBE)
unset(MVVCVTK_FORBID_GAP_PROBE)
unset(MVVCVTK_FORBID_HOST_PROBE)
unset(MVVCVTK_FORBID_SLICE_PROBE)
unset(MVVCVTK_FORBID_SUPPORT_PROBE)
unset(MVVCVTK_GAP_HOST_PROBE)
unset(MVVCVTK_GAP_TYPES_PROBE)
unset(MVVCVTK_HOST_FEATURE_PROBE)
unset(MVVCVTK_HOST_REQUEST_PROBE)
unset(MVVCVTK_HOST_SESSION_PROBE)
unset(MVVCVTK_PROBE_SOURCE_DIR)
