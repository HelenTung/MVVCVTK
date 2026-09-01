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

function(GetSurfaceHeaders outputName propertyName)
    get_property(
        surfaceHeaders
        GLOBAL PROPERTY "${propertyName}_SDK_PATHS"
    )
    set(${outputName} "${surfaceHeaders}" PARENT_SCOPE)
endfunction()

GetSurfaceHeaders(MVVCVTK_HOST_ENTRY_HEADERS MVVCVTK_HOST_PUBLIC_HEADERS)
GetSurfaceHeaders(MVVCVTK_HOST_SUPPORT_HEADERS MVVCVTK_HOST_CLOSURE_HEADERS)
GetSurfaceHeaders(MVVCVTK_SPI_ENTRY_HEADERS MVVCVTK_FEATURE_SPI_HEADERS)
GetSurfaceHeaders(
    MVVCVTK_SPI_SUPPORT_HEADERS
    MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS
)
GetSurfaceHeaders(MVVCVTK_CROP_ENTRY_HEADERS MVVCVTK_CROP_API_HEADERS)
GetSurfaceHeaders(MVVCVTK_GAP_ENTRY_HEADERS MVVCVTK_GAP_API_HEADERS)
set(
    MVVCVTK_HOST_PROBE_HEADERS
    ${MVVCVTK_HOST_ENTRY_HEADERS}
    ${MVVCVTK_HOST_SUPPORT_HEADERS}
)
set(
    MVVCVTK_SPI_PROBE_HEADERS
    ${MVVCVTK_SPI_ENTRY_HEADERS}
    ${MVVCVTK_SPI_SUPPORT_HEADERS}
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

function(BuildHeaderProbes outputSources outputDependencies surfaceName dependencyDir)
    set(probeSources)
    set(dependencyFiles)
    foreach(headerName IN LISTS ARGN)
        get_filename_component(headerStem "${headerName}" NAME_WE)
        set(probeFile "${surfaceName}${headerStem}HeaderProbe.cpp")
        BuildProbe(
            generatedProbe
            "${probeFile}"
            "#include \"${headerName}\"\n"
        )
        list(APPEND probeSources "${generatedProbe}")
        list(APPEND dependencyFiles "${dependencyDir}/${probeFile}.json")
    endforeach()
    set(${outputSources} "${probeSources}" PARENT_SCOPE)
    set(${outputDependencies} "${dependencyFiles}" PARENT_SCOPE)
endfunction()

function(BuildAggregateProbe outputName fileName)
    set(probeSource)
    foreach(headerName IN LISTS ARGN)
        string(APPEND probeSource "#include \"${headerName}\"\n")
    endforeach()
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

BuildHeaderProbes(
    MVVCVTK_HOST_PROBES
    MVVCVTK_HOST_DEPENDENCY_FILES
    HostApi
    "${MVVCVTK_HOST_DEPENDENCY_DIR}"
    ${MVVCVTK_HOST_PROBE_HEADERS}
)
BuildHeaderProbes(
    MVVCVTK_SPI_PROBES
    MVVCVTK_SPI_DEPENDENCY_FILES
    FeatureSpi
    "${MVVCVTK_SPI_DEPENDENCY_DIR}"
    ${MVVCVTK_SPI_PROBE_HEADERS}
)
BuildHeaderProbes(
    MVVCVTK_CROP_PROBES
    MVVCVTK_CROP_DEPENDENCY_FILES
    CropApi
    "${MVVCVTK_CROP_DEPENDENCY_DIR}"
    ${MVVCVTK_CROP_ENTRY_HEADERS}
)
BuildHeaderProbes(
    MVVCVTK_GAP_PROBES
    MVVCVTK_GAP_DEPENDENCY_FILES
    GapApi
    "${MVVCVTK_GAP_DEPENDENCY_DIR}"
    ${MVVCVTK_GAP_ENTRY_HEADERS}
)
set(
    MVVCVTK_AGGREGATE_HEADERS
    ${MVVCVTK_HOST_ENTRY_HEADERS}
    ${MVVCVTK_SPI_ENTRY_HEADERS}
    ${MVVCVTK_CROP_ENTRY_HEADERS}
    ${MVVCVTK_GAP_ENTRY_HEADERS}
)
list(REMOVE_DUPLICATES MVVCVTK_AGGREGATE_HEADERS)
BuildAggregateProbe(
    MVVCVTK_AGGREGATE_PROBE
    SdkHeaderAggregateProbe.cpp
    ${MVVCVTK_AGGREGATE_HEADERS}
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_DATA_PROBE
    ForbiddenDataManagerProbe.cpp
    Data/DataManager.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_HOST_PROBE
    ForbiddenHostRuntimeProbe.cpp
    Host/HostViewRuntimeRegistry.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_APP_TYPES_PROBE
    ForbiddenAppTypesProbe.cpp
    App/AppTypes.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_SLICE_PROBE
    ForbiddenSliceStrategyProbe.cpp
    Render/Strategies/SliceStrategy.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_HOST_VALUE_PROBE
    ForbiddenHostValueTypesProbe.cpp
    MVVCVTK/API/Host/Types/HostValueTypes.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_SUPPORT_PROBE
    ForbiddenFeatureSupportProbe.cpp
    Render/Support/FeatureOverlayBase.h
)
BuildForbiddenProbe(
    MVVCVTK_FORBID_VISUAL_STRATEGY_PROBE
    ForbiddenVisualStrategyProbe.cpp
    Render/Contracts/VisualStrategy.h
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
    ${MVVCVTK_HOST_PROBES}
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

add_library(mvvcvtk_feature_spi_header_probes OBJECT
    ${MVVCVTK_SPI_PROBES}
)
SetTargetDefaults(mvvcvtk_feature_spi_header_probes)
target_link_libraries(
    mvvcvtk_feature_spi_header_probes
    PRIVATE MVVCVTK::FeatureSPI
)
target_compile_options(
    mvvcvtk_feature_spi_header_probes
    PRIVATE /sourceDependencies "${MVVCVTK_SPI_DEPENDENCY_DIR}"
)
set_target_properties(
    mvvcvtk_feature_spi_header_probes
    PROPERTIES
        PROJECT_LABEL MVVCVTKFeatureSpiHeaderProbes
        FOLDER "MVVCVTK/Tests"
)

if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    add_library(mvvcvtk_crop_header_probes OBJECT
        ${MVVCVTK_CROP_PROBES}
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
        ${MVVCVTK_GAP_PROBES}
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

add_library(mvvcvtk_sdk_header_aggregate OBJECT
    "${MVVCVTK_AGGREGATE_PROBE}"
)
SetTargetDefaults(mvvcvtk_sdk_header_aggregate)
target_link_libraries(
    mvvcvtk_sdk_header_aggregate
    PRIVATE
        MVVCVTK::HostAPI
        MVVCVTK::FeatureSPI
)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    target_link_libraries(
        mvvcvtk_sdk_header_aggregate
        PRIVATE MVVCVTK::OrthogonalCrop
    )
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    target_link_libraries(
        mvvcvtk_sdk_header_aggregate
        PRIVATE MVVCVTK::GapAnalysis
    )
endif()
set_target_properties(
    mvvcvtk_sdk_header_aggregate
    PROPERTIES
        PROJECT_LABEL MVVCVTKSdkHeaderAggregate
        FOLDER "MVVCVTK/Tests"
)

function(SetClosureTest testName dependencyFiles stagingRoot)
    set(expectedHeaders ${ARGN})
    list(JOIN dependencyFiles "," dependencyFileCsv)
    list(JOIN expectedHeaders "," expectedHeaderCsv)
    add_test(
        NAME ${testName}
        COMMAND "${CMAKE_COMMAND}"
            "-DDEPENDENCY_FILES=${dependencyFileCsv}"
            "-DSTAGING_ROOT=${stagingRoot}"
            "-DEXPECTED_HEADERS=${expectedHeaderCsv}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyHeaderClosure.cmake"
    )
endfunction()

set(
    MVVCVTK_EXPECTED_HOST_STAGE
    ${MVVCVTK_HOST_ENTRY_HEADERS}
    ${MVVCVTK_HOST_SUPPORT_HEADERS}
)
set(
    MVVCVTK_EXPECTED_SPI_STAGE
    ${MVVCVTK_SPI_ENTRY_HEADERS}
    ${MVVCVTK_SPI_SUPPORT_HEADERS}
)
set(MVVCVTK_EXPECTED_CROP_STAGE ${MVVCVTK_CROP_ENTRY_HEADERS})
set(MVVCVTK_EXPECTED_GAP_STAGE ${MVVCVTK_GAP_ENTRY_HEADERS})

SetClosureTest(
    SdkBoundary.HostApiClosure
    "${MVVCVTK_HOST_DEPENDENCY_FILES}"
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
    ${MVVCVTK_EXPECTED_HOST_STAGE}
)
SetClosureTest(
    SdkBoundary.FeatureSpiClosure
    "${MVVCVTK_SPI_DEPENDENCY_FILES}"
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
    ${MVVCVTK_EXPECTED_SPI_STAGE}
)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    SetClosureTest(
        SdkBoundary.CropApiClosure
        "${MVVCVTK_CROP_DEPENDENCY_FILES}"
        "${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
        ${MVVCVTK_EXPECTED_CROP_STAGE}
    )
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    SetClosureTest(
        SdkBoundary.GapApiClosure
        "${MVVCVTK_GAP_DEPENDENCY_FILES}"
        "${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
        ${MVVCVTK_EXPECTED_GAP_STAGE}
    )
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

function(SetHeaderGuard sourceFile headerPath usageName)
    set(includeRoots ${ARGN})
    foreach(includeRoot IN LISTS includeRoots)
        if(EXISTS "${includeRoot}/${headerPath}")
            message(FATAL_ERROR
                "Forbidden SDK header is staged for ${usageName}: ${headerPath}"
            )
        endif()
    endforeach()

    string(TOLOWER "${usageName}" usageId)
    set(usageTarget "mvvcvtk_${usageId}_negative_usage")
    if(NOT TARGET ${usageTarget})
        add_library(${usageTarget} INTERFACE IMPORTED)
        set_target_properties(
            ${usageTarget}
            PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${includeRoots}"
        )
    endif()
    get_filename_component(probeName "${sourceFile}" NAME_WE)
    try_compile(
        isForbiddenHeaderAccepted
        SOURCES "${sourceFile}"
        NO_CACHE
        CMAKE_FLAGS
            "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
        COMPILE_DEFINITIONS
            "/utf-8"
        LINK_LIBRARIES ${usageTarget}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        OUTPUT_VARIABLE forbiddenCompileOutput
    )
    if(isForbiddenHeaderAccepted)
        message(FATAL_ERROR
            "${probeName} unexpectedly compiled with ${usageName} usage requirements."
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
    HostApi
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
)
SetHeaderGuard(
    "${MVVCVTK_FORBID_HOST_PROBE}"
    Host/HostViewRuntimeRegistry.h
    HostApi
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
)
SetHeaderGuard(
    "${MVVCVTK_FORBID_APP_TYPES_PROBE}"
    App/AppTypes.h
    FeatureSpi
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
)
SetHeaderGuard(
    "${MVVCVTK_FORBID_SLICE_PROBE}"
    Render/Strategies/SliceStrategy.h
    FeatureSpi
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
)
SetHeaderGuard(
    "${MVVCVTK_FORBID_SUPPORT_PROBE}"
    Render/Support/FeatureOverlayBase.h
    FeatureSpi
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
)
SetHeaderGuard(
    "${MVVCVTK_FORBID_VISUAL_STRATEGY_PROBE}"
    Render/Contracts/VisualStrategy.h
    FeatureSpi
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
)
SetHeaderGuard(
    "${MVVCVTK_FORBID_HOST_VALUE_PROBE}"
    MVVCVTK/API/Host/Types/HostValueTypes.h
    FeatureSpi
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    SetHeaderGuard(
        "${MVVCVTK_FORBID_CROP_PROBE}"
        Algorithms/CropAlgorithm.h
        CropApi
        "${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
        "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
    )
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    SetHeaderGuard(
        "${MVVCVTK_FORBID_GAP_PROBE}"
        Services/GapAnalysisService.h
        GapApi
        "${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
        "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
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
unset(MVVCVTK_AGGREGATE_HEADERS)
unset(MVVCVTK_AGGREGATE_PROBE)
unset(MVVCVTK_CROP_DEPENDENCY_FILES)
unset(MVVCVTK_CROP_ENTRY_HEADERS)
unset(MVVCVTK_CROP_PROBES)
unset(MVVCVTK_FORBID_CROP_PROBE)
unset(MVVCVTK_FORBID_APP_TYPES_PROBE)
unset(MVVCVTK_FORBID_DATA_PROBE)
unset(MVVCVTK_FORBID_GAP_PROBE)
unset(MVVCVTK_FORBID_HOST_PROBE)
unset(MVVCVTK_FORBID_HOST_VALUE_PROBE)
unset(MVVCVTK_FORBID_SLICE_PROBE)
unset(MVVCVTK_FORBID_SUPPORT_PROBE)
unset(MVVCVTK_GAP_DEPENDENCY_FILES)
unset(MVVCVTK_GAP_ENTRY_HEADERS)
unset(MVVCVTK_GAP_PROBES)
unset(MVVCVTK_HOST_DEPENDENCY_FILES)
unset(MVVCVTK_HOST_ENTRY_HEADERS)
unset(MVVCVTK_HOST_PROBES)
unset(MVVCVTK_HOST_PROBE_HEADERS)
unset(MVVCVTK_HOST_SUPPORT_HEADERS)
unset(MVVCVTK_PROBE_SOURCE_DIR)
unset(MVVCVTK_SPI_DEPENDENCY_FILES)
unset(MVVCVTK_SPI_ENTRY_HEADERS)
unset(MVVCVTK_SPI_PROBES)
unset(MVVCVTK_SPI_PROBE_HEADERS)
unset(MVVCVTK_SPI_SUPPORT_HEADERS)
