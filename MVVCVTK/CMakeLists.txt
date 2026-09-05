set(MVVCVTK_HOST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(
    MVVCVTK_HOST_SOURCE_API_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-host-source-api-include"
)
set(
    MVVCVTK_HOST_SDK_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-sdk-host-include"
)
set(
    MVVCVTK_FEATURE_SPI_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-sdk-feature-spi-include"
)
set(
    MVVCVTK_FEATURE_SOURCE_SPI_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-feature-source-spi-include"
)
set(
    MVVCVTK_CROP_SDK_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/features/OrthogonalCrop/mvvcvtk-sdk-include"
)
set(
    MVVCVTK_CROP_SOURCE_API_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/features/OrthogonalCrop/mvvcvtk-source-api-include"
)
set(
    MVVCVTK_GAP_SDK_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/features/GapAnalysis/mvvcvtk-sdk-include"
)
set(
    MVVCVTK_GAP_SOURCE_API_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/features/GapAnalysis/mvvcvtk-source-api-include"
)
set(
    MVVCVTK_PART_SOURCE_API_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/features/PartSegmentation/mvvcvtk-source-api-include"
)
set(
    MVVCVTK_SDK_CLOSURE_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-sdk-closure-include"
)

# 源码头继续使用仓内既有路径；映射表只定义 SDK staging/install 中的物理路径。
function(SetSdkHeaderMap)
    set(sourceHeaders)
    set(sdkHeaders)
    set(sourceKeys)
    set(sdkKeys)
    foreach(headerEntry IN LISTS ARGN)
        if(NOT headerEntry MATCHES "^([^|]+)\\|([^|]+)$")
            message(FATAL_ERROR "Invalid SDK header mapping: ${headerEntry}")
        endif()
        set(sourceHeader "${CMAKE_MATCH_1}")
        set(sdkHeader "${CMAKE_MATCH_2}")
        cmake_path(NORMAL_PATH sourceHeader OUTPUT_VARIABLE normalizedSource)
        cmake_path(NORMAL_PATH sdkHeader OUTPUT_VARIABLE normalizedSdk)
        string(FIND "${sourceHeader}" "\\" sourceBackslashIndex)
        string(FIND "${sdkHeader}" "\\" sdkBackslashIndex)
        if(NOT normalizedSource STREQUAL sourceHeader
            OR NOT normalizedSdk STREQUAL sdkHeader
            OR NOT sourceBackslashIndex EQUAL -1
            OR NOT sdkBackslashIndex EQUAL -1
            OR NOT sourceHeader MATCHES "^include/.+\\.h$"
            OR NOT sdkHeader MATCHES "^MVVCVTK/(API|SPI)/.+\\.h$")
            message(FATAL_ERROR "Unsafe SDK header mapping: ${headerEntry}")
        endif()

        string(TOLOWER "${sourceHeader}" sourceKey)
        string(TOLOWER "${sdkHeader}" sdkKey)
        list(FIND sourceKeys "${sourceKey}" sourceIndex)
        list(FIND sdkKeys "${sdkKey}" sdkIndex)
        if(NOT sourceIndex EQUAL -1 OR NOT sdkIndex EQUAL -1)
            message(FATAL_ERROR
                "Duplicate SDK header mapping: ${headerEntry}"
            )
        endif()
        list(APPEND sourceHeaders "${sourceHeader}")
        list(APPEND sdkHeaders "${sdkHeader}")
        list(APPEND sourceKeys "${sourceKey}")
        list(APPEND sdkKeys "${sdkKey}")
    endforeach()
    set_property(GLOBAL PROPERTY MVVCVTK_SDK_MAP_SOURCES "${sourceHeaders}")
    set_property(GLOBAL PROPERTY MVVCVTK_SDK_MAP_TARGETS "${sdkHeaders}")
endfunction()

function(GetSdkHeaderPath outputName sourceHeader)
    get_property(sourceHeaders GLOBAL PROPERTY MVVCVTK_SDK_MAP_SOURCES)
    get_property(sdkHeaders GLOBAL PROPERTY MVVCVTK_SDK_MAP_TARGETS)
    list(FIND sourceHeaders "${sourceHeader}" headerIndex)
    if(headerIndex EQUAL -1)
        message(FATAL_ERROR
            "SDK header has no install mapping: ${sourceHeader}"
        )
    endif()
    list(GET sdkHeaders ${headerIndex} sdkHeader)
    set(${outputName} "${sdkHeader}" PARENT_SCOPE)
endfunction()

SetSdkHeaderMap(
    "include/Host/VtkAppHostSession.h|MVVCVTK/API/Host/VtkAppHostSession.h"
    "include/Host/Types/HostRequestTypes.h|MVVCVTK/API/Host/Types/HostRequestTypes.h"
    "include/Host/Types/HostSessionTypes.h|MVVCVTK/API/Host/Types/HostSessionTypes.h"
    "include/Host/Types/HostValueTypes.h|MVVCVTK/API/Host/Types/HostValueTypes.h"
    "include/Host/Types/HostViewTypes.h|MVVCVTK/API/Host/Types/HostViewTypes.h"
    "include/Data/DataVersion.h|MVVCVTK/API/Data/DataVersion.h"
    "include/Data/ImageReadTypes.h|MVVCVTK/API/Data/ImageReadTypes.h"
    "include/Host/HostFeature.h|MVVCVTK/SPI/Host/HostFeature.h"
    "include/App/Services/FeatureViewService.h|MVVCVTK/SPI/App/Services/FeatureViewService.h"
    "include/Render/Contracts/FeatureOverlay.h|MVVCVTK/SPI/Render/Contracts/FeatureOverlay.h"
    "include/Render/Contracts/OverlayService.h|MVVCVTK/SPI/Render/Contracts/OverlayService.h"
    "include/Render/Contracts/RenderEffect.h|MVVCVTK/SPI/Render/Contracts/RenderEffect.h"
    "include/App/FeatureTypes.h|MVVCVTK/SPI/App/FeatureTypes.h"
    "include/App/ViewTypes.h|MVVCVTK/SPI/App/ViewTypes.h"
    "include/Data/TrustedImageState.h|MVVCVTK/SPI/Data/TrustedImageState.h"
    "include/Interaction/InteractionTypes.h|MVVCVTK/SPI/Interaction/InteractionTypes.h"
    "include/Host/CropHostFeature.h|MVVCVTK/API/Features/OrthogonalCrop/Host/CropHostFeature.h"
    "include/OrthogonalCropTypes.h|MVVCVTK/API/Features/OrthogonalCrop/OrthogonalCropTypes.h"
    "include/Host/GapHostFeature.h|MVVCVTK/API/Features/GapAnalysis/Host/GapHostFeature.h"
    "include/Host/GapHostTypes.h|MVVCVTK/API/Features/GapAnalysis/Host/GapHostTypes.h"
)

# 源码目录保持原样；所有公开面先经过同一份声明与路径校验，再生成 staging、
# FILE_SET、安装清单、发布清单和边界测试输入。
function(SetHeaderSurface outputName sourceRoot)
    file(REAL_PATH "${sourceRoot}" sourceRootReal)
    string(TOLOWER "${sourceRootReal}" comparisonSourceRoot)
    set(surfaceHeaders)
    set(surfaceSdkHeaders)
    set(surfaceKeys)
    foreach(header IN LISTS ARGN)
        set(headerPath "${header}")
        cmake_path(IS_ABSOLUTE headerPath isAbsolute)
        cmake_path(NORMAL_PATH headerPath OUTPUT_VARIABLE normalizedHeader)
        string(FIND "${header}" "\\" backslashIndex)
        string(FIND "${header}" ":" colonIndex)
        if("${header}" STREQUAL ""
            OR isAbsolute
            OR NOT backslashIndex EQUAL -1
            OR NOT colonIndex EQUAL -1
            OR NOT normalizedHeader STREQUAL header
            OR NOT normalizedHeader MATCHES "^include/.+\\.h$")
            message(FATAL_ERROR "Unsafe SDK header path: ${header}")
        endif()

        string(REPLACE "/" ";" headerParts "${normalizedHeader}")
        foreach(headerPart IN LISTS headerParts)
            if(headerPart STREQUAL "" OR headerPart STREQUAL "."
                OR headerPart STREQUAL "..")
                message(FATAL_ERROR "Unsafe SDK header path: ${header}")
            endif()
        endforeach()

        string(TOLOWER "${normalizedHeader}" headerKey)
        list(FIND surfaceKeys "${headerKey}" duplicateIndex)
        if(NOT duplicateIndex EQUAL -1)
            message(FATAL_ERROR
                "Duplicate or case-conflicting SDK header path: ${header}"
            )
        endif()

        set(sourceHeader "${sourceRoot}/${normalizedHeader}")
        if(NOT EXISTS "${sourceHeader}" OR IS_DIRECTORY "${sourceHeader}")
            message(FATAL_ERROR "SDK header source is missing: ${sourceHeader}")
        endif()
        if(IS_SYMLINK "${sourceHeader}")
            message(FATAL_ERROR "SDK header source is a symlink: ${sourceHeader}")
        endif()
        file(REAL_PATH "${sourceHeader}" sourceHeaderReal)
        set(expectedSourceReal "${sourceRootReal}/${normalizedHeader}")
        cmake_path(NORMAL_PATH expectedSourceReal)
        string(TOLOWER "${sourceHeaderReal}" comparisonSourceHeader)
        string(TOLOWER "${expectedSourceReal}" comparisonExpectedSource)
        cmake_path(
            IS_PREFIX comparisonSourceRoot
            "${comparisonSourceHeader}"
            NORMALIZE
            isWithinSource
        )
        if(NOT isWithinSource
            OR NOT comparisonSourceHeader STREQUAL comparisonExpectedSource)
            message(FATAL_ERROR
                "SDK header source escapes its root or crosses a reparse point: "
                "${sourceHeader}"
            )
        endif()

        list(APPEND surfaceHeaders "${normalizedHeader}")
        GetSdkHeaderPath(sdkHeader "${normalizedHeader}")
        list(APPEND surfaceSdkHeaders "${sdkHeader}")
        list(APPEND surfaceKeys "${headerKey}")
    endforeach()
    set(${outputName} "${surfaceHeaders}" PARENT_SCOPE)
    set_property(GLOBAL PROPERTY "${outputName}" "${surfaceHeaders}")
    set_property(
        GLOBAL PROPERTY
        "${outputName}_SDK_PATHS"
        "${surfaceSdkHeaders}"
    )
endfunction()

function(GetSdkHeaderPaths outputName stagingRoot)
    set(stagedHeaders)
    set(stagedKeys)
    foreach(header IN LISTS ARGN)
        GetSdkHeaderPath(sdkHeader "${header}")
        string(TOLOWER "${sdkHeader}" stagedKey)
        list(FIND stagedKeys "${stagedKey}" duplicateIndex)
        if(NOT duplicateIndex EQUAL -1)
            message(FATAL_ERROR
                "Duplicate SDK staging destination: ${sdkHeader}"
            )
        endif()
        list(APPEND stagedHeaders "${stagingRoot}/${sdkHeader}")
        list(APPEND stagedKeys "${stagedKey}")
    endforeach()
    set(${outputName} "${stagedHeaders}" PARENT_SCOPE)
endfunction()

# SDK 副本只在构建树改写公开 include；仓内源码和 include 路径保持原样。
function(BuildSdkHeaders outputName sourceRoot stagingRoot)
    set(sourceHeaders ${ARGN})
    GetSdkHeaderPaths(stagedHeaders "${stagingRoot}" ${sourceHeaders})
    get_property(mapSources GLOBAL PROPERTY MVVCVTK_SDK_MAP_SOURCES)
    get_property(mapTargets GLOBAL PROPERTY MVVCVTK_SDK_MAP_TARGETS)
    foreach(sourceHeader stagedHeader IN ZIP_LISTS sourceHeaders stagedHeaders)
        cmake_path(GET stagedHeader PARENT_PATH stagedDirectory)
        file(MAKE_DIRECTORY "${stagedDirectory}")
        set(sourcePath "${sourceRoot}/${sourceHeader}")
        set_property(
            DIRECTORY APPEND PROPERTY
            CMAKE_CONFIGURE_DEPENDS "${sourcePath}"
        )
        file(READ "${sourcePath}" stagedContent)
        foreach(mapSource mapTarget IN ZIP_LISTS mapSources mapTargets)
            string(REGEX REPLACE "^include/" "" sourceInclude "${mapSource}")
            string(REPLACE
                "#include \"${sourceInclude}\""
                "#include \"${mapTarget}\""
                stagedContent
                "${stagedContent}"
            )
            string(REPLACE
                "#include <${sourceInclude}>"
                "#include <${mapTarget}>"
                stagedContent
                "${stagedContent}"
            )
        endforeach()
        file(WRITE "${stagedHeader}" "${stagedContent}")
    endforeach()
    set(${outputName} "${stagedHeaders}" PARENT_SCOPE)
endfunction()

# 仓内 Feature 继续按源码 include 名编译；该视图仅存在于 build tree。
function(BuildSourceHeaders outputName sourceRoot stagingRoot)
    set(stagedHeaders)
    foreach(sourceHeader IN LISTS ARGN)
        string(REGEX REPLACE "^include/" "" relativeHeader "${sourceHeader}")
        set(stagedHeader "${stagingRoot}/${relativeHeader}")
        cmake_path(GET stagedHeader PARENT_PATH stagedDirectory)
        file(MAKE_DIRECTORY "${stagedDirectory}")
        configure_file(
            "${sourceRoot}/${sourceHeader}"
            "${stagedHeader}"
            COPYONLY
        )
        list(APPEND stagedHeaders "${stagedHeader}")
    endforeach()
    set(${outputName} "${stagedHeaders}" PARENT_SCOPE)
endfunction()

function(ResetSdkStage stagingRoot)
    set(binaryRoot "${CMAKE_CURRENT_BINARY_DIR}")
    cmake_path(NORMAL_PATH binaryRoot)
    cmake_path(
        ABSOLUTE_PATH stagingRoot
        BASE_DIRECTORY "${binaryRoot}"
        NORMALIZE
        OUTPUT_VARIABLE normalizedStagingRoot
    )
    cmake_path(
        IS_PREFIX binaryRoot
        "${normalizedStagingRoot}"
        NORMALIZE
        isBuildSubdirectory
    )
    if(NOT isBuildSubdirectory OR normalizedStagingRoot STREQUAL binaryRoot)
        message(FATAL_ERROR
            "Refusing to reset SDK staging outside the MVVCVTK build tree: "
            "${normalizedStagingRoot}"
        )
    endif()

    file(REAL_PATH "${binaryRoot}" binaryRootReal)
    set(existingPath "${normalizedStagingRoot}")
    while(NOT EXISTS "${existingPath}")
        cmake_path(GET existingPath PARENT_PATH existingParent)
        if(existingParent STREQUAL existingPath)
            message(FATAL_ERROR
                "Cannot resolve an existing SDK staging ancestor: "
                "${normalizedStagingRoot}"
            )
        endif()
        set(existingPath "${existingParent}")
    endwhile()
    if(IS_SYMLINK "${existingPath}")
        message(FATAL_ERROR
            "SDK staging path contains a symlink: ${existingPath}"
        )
    endif()
    file(REAL_PATH "${existingPath}" existingPathReal)
    cmake_path(
        RELATIVE_PATH existingPath
        BASE_DIRECTORY "${binaryRoot}"
        OUTPUT_VARIABLE existingRelative
    )
    set(expectedExistingReal "${binaryRootReal}/${existingRelative}")
    cmake_path(NORMAL_PATH expectedExistingReal)
    string(REGEX REPLACE "/+$" "" existingPathReal "${existingPathReal}")
    string(REGEX REPLACE "/+$" "" expectedExistingReal "${expectedExistingReal}")
    string(TOLOWER "${existingPathReal}" comparisonExistingReal)
    string(TOLOWER "${expectedExistingReal}" comparisonExpectedReal)
    if(NOT comparisonExistingReal STREQUAL comparisonExpectedReal)
        message(FATAL_ERROR
            "SDK staging path contains a reparse point: ${existingPath}"
        )
    endif()

    file(REMOVE_RECURSE "${normalizedStagingRoot}")
    file(MAKE_DIRECTORY "${normalizedStagingRoot}")
    file(REAL_PATH "${normalizedStagingRoot}" createdStagingReal)
    cmake_path(
        RELATIVE_PATH normalizedStagingRoot
        BASE_DIRECTORY "${binaryRoot}"
        OUTPUT_VARIABLE stagingRelative
    )
    set(expectedStagingReal "${binaryRootReal}/${stagingRelative}")
    cmake_path(NORMAL_PATH expectedStagingReal)
    string(REGEX REPLACE "/+$" "" createdStagingReal "${createdStagingReal}")
    string(REGEX REPLACE "/+$" "" expectedStagingReal "${expectedStagingReal}")
    string(TOLOWER "${createdStagingReal}" comparisonCreatedReal)
    string(TOLOWER "${expectedStagingReal}" comparisonExpectedReal)
    if(NOT comparisonCreatedReal STREQUAL comparisonExpectedReal)
        message(FATAL_ERROR
            "SDK staging directory resolved through a reparse point: "
            "${normalizedStagingRoot}"
        )
    endif()
endfunction()

# closure support 从已改写的 SDK staging 安装，不读取或改动源码头。
function(ExportHeaders componentName stagingRoot)
    foreach(header IN LISTS ARGN)
        GetSdkHeaderPath(sdkHeader "${header}")
        cmake_path(GET sdkHeader PARENT_PATH sdkDirectory)
        install(
            FILES "${stagingRoot}/${sdkHeader}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${sdkDirectory}"
            COMPONENT ${componentName}
        )
    endforeach()
endfunction()

function(SetSurfaceLines outputName categoryName)
    set(surfaceLines)
    foreach(header IN LISTS ARGN)
        GetSdkHeaderPath(sdkHeader "${header}")
        list(APPEND surfaceLines "${categoryName}|${sdkHeader}")
    endforeach()
    set(${outputName} "${surfaceLines}" PARENT_SCOPE)
endfunction()

set(MVVCVTK_HOST_PRIVATE_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/App"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/App/Services"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/App/Tasks"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Data"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Geometry"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Host"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Interaction"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Render"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Contracts"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Strategies"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Support"
)
set_property(
    GLOBAL PROPERTY
    MVVCVTK_HOST_PRIVATE_INCLUDE_DIRS
    "${MVVCVTK_HOST_PRIVATE_INCLUDE_DIRS}"
)

set(MVVCVTK_HOST_SOURCES
    include/App/AppState.h
    include/App/AppStateEvents.h
    include/App/Services/AppPorts.h
    include/App/Services/AppServiceFactory.h
    include/App/Services/DataCommitTypes.h
    include/App/Tasks/AppDataExportTaskService.h
    include/App/Tasks/AppDataLoadTaskService.h
    include/Data/DataConverters.h
    include/Data/DataManager.h
    include/Data/DataService.h
    include/Data/ImageProcessor.h
    include/Data/VolumeTypes.h
    include/Geometry/InteractionComputeService.h
    include/Host/HostCommandRouter.h
    include/Host/HostCoreServices.h
    include/Host/HostFrameCoordinator.h
    include/Host/HostHotkeyRouter.h
    include/Host/HostInputRegistry.h
    include/Host/HostViewRuntimeRegistry.h
    include/Host/LoadCommitCoordinator.h
    include/Interaction/AbstractViewContext.h
    include/Interaction/DefaultNavigationPolicy.h
    include/Interaction/IInteractionHandler.h
    include/Interaction/InputCallbackHandler.h
    include/Interaction/InteractionPorts.h
    include/Interaction/InteractionRouter.h
    include/Interaction/TimeUpdateHandler.h
    include/Interaction/ViewContextFactory.h
    include/Interaction/Viewer2DHandler.h
    include/Interaction/Viewer3DHandler.h
    include/Platform/MemMappedFile.h
    include/Platform/Path.h
    include/Platform/TaskStopToken.h
    include/Render/Contracts/RenderBindPort.h
    include/Render/Contracts/RenderStrategyFactory.h
    include/Render/Internal/RenderResourceCoordinator.h
    include/Render/Internal/IsoSurfaceProductBuilder.h
    include/Render/Internal/VolumeLodProductBuilder.h
    include/Render/Strategies/ColoredPlanesStrategy.h
    include/Render/Strategies/CompositeStrategy.h
    include/Render/Strategies/IsoSurfaceStrategy.h
    include/Render/Strategies/SliceStrategy.h
    include/Render/Strategies/VolumeStrategy.h
    include/Render/Support/BaseVisualStrategy.h
    src/App/AppState.cpp
    include/App/VolumePresentationTypes.h
    src/App/Services/AppServiceFactory.cpp
    src/App/Tasks/AppDataExportTaskService.cpp
    src/App/Tasks/AppDataLoadTaskService.cpp
    src/Data/DataConverters.cpp
    src/Data/DataManager.cpp
    src/Data/ImageProcessor.cpp
    src/Data/VolumeTypes.cpp
    src/Host/HostTransferCodec.cpp
    include/Host/Internal/HostTransferCodec.h
    src/Host/HostCommandRouter.cpp
    src/Host/HostFrameCoordinator.cpp
    src/Host/HostHotkeyRouter.cpp
    src/Host/HostInputRegistry.cpp
    src/Host/HostViewRuntimeRegistry.cpp
    src/Host/LoadCommitCoordinator.cpp
    src/Host/VtkAppHostSession.cpp
    src/Interaction/AbstractViewContext.cpp
    src/Interaction/DefaultNavigationPolicy.cpp
    src/Interaction/InputCallbackHandler.cpp
    src/Interaction/InteractionRouter.cpp
    src/Interaction/TimeUpdateHandler.cpp
    src/Interaction/ViewContextFactory.cpp
    src/Interaction/Viewer2DHandler.cpp
    src/Interaction/Viewer3DHandler.cpp
    src/Platform/MemMappedFile.cpp
    src/Render/RenderStrategyFactory.cpp
    src/Render/Internal/RenderResourceCoordinator.cpp
    src/Render/Internal/IsoSurfaceProductBuilder.cpp
    src/Render/Internal/VolumeLodProductBuilder.cpp
    src/Render/Strategies/ColoredPlanesStrategy.cpp
    src/Render/Strategies/CompositeStrategy.cpp
    src/Render/Strategies/IsoLodController.cpp
    include/Render/Internal/IsoLodController.h
    src/Render/Strategies/IsoSurfaceStrategy.cpp
    src/Render/Strategies/SliceStrategy.cpp
    src/Render/Strategies/VolumeLodController.cpp
    include/Render/Internal/VolumeLodController.h
    src/Render/Strategies/VolumeStrategy.cpp
    src/Render/Support/BaseVisualStrategy.cpp
)
# 普通 Host consumer 被正式允许直接包含的稳定入口与请求定义。
SetHeaderSurface(
    MVVCVTK_HOST_PUBLIC_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    include/Host/VtkAppHostSession.h
    include/Host/Types/HostRequestTypes.h
)
# Host 公开入口递归编译所需，但不作为独立顶层 API 宣传的支持头。
SetHeaderSurface(
    MVVCVTK_HOST_CLOSURE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    include/Data/DataVersion.h
    include/Data/ImageReadTypes.h
    include/Host/Types/HostSessionTypes.h
    include/Host/Types/HostViewTypes.h
    include/Host/Types/HostValueTypes.h
)
# Feature 开发者可直接使用的扩展契约，不依赖 Host 实现库。
SetHeaderSurface(
    MVVCVTK_FEATURE_SPI_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    include/Host/HostFeature.h
    include/App/Services/FeatureViewService.h
    include/Render/Contracts/FeatureOverlay.h
    include/Render/Contracts/OverlayService.h
    include/Render/Contracts/RenderEffect.h
)
# Feature SPI 独立携带 HostFeature、交互、可信数据与渲染契约的完整物理闭包。
SetHeaderSurface(
    MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    include/App/FeatureTypes.h
    include/App/ViewTypes.h
    include/Data/DataVersion.h
    include/Data/ImageReadTypes.h
    include/Data/TrustedImageState.h
    include/Host/Types/HostViewTypes.h
    include/Interaction/InteractionTypes.h
)

set(
    _mvvcvtk_sdk_closure_headers
    ${MVVCVTK_HOST_CLOSURE_HEADERS}
    ${MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS}
)
list(REMOVE_DUPLICATES _mvvcvtk_sdk_closure_headers)
SetHeaderSurface(
    MVVCVTK_SDK_CLOSURE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${_mvvcvtk_sdk_closure_headers}
)
unset(_mvvcvtk_sdk_closure_headers)

# 每次配置都重建批准视图，避免已移出白名单的旧头残留在 staging 中。
ResetSdkStage("${MVVCVTK_HOST_SOURCE_API_INCLUDE_DIR}")
ResetSdkStage("${MVVCVTK_HOST_SDK_INCLUDE_DIR}")
ResetSdkStage("${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}")
ResetSdkStage("${MVVCVTK_FEATURE_SOURCE_SPI_INCLUDE_DIR}")
ResetSdkStage("${MVVCVTK_SDK_CLOSURE_INCLUDE_DIR}")

BuildSdkHeaders(
    MVVCVTK_SDK_STAGED_CLOSURE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_SDK_CLOSURE_INCLUDE_DIR}"
    ${MVVCVTK_SDK_CLOSURE_HEADERS}
)

set(
    _mvvcvtk_host_stage_headers
    ${MVVCVTK_HOST_PUBLIC_HEADERS}
    ${MVVCVTK_HOST_CLOSURE_HEADERS}
)
BuildSourceHeaders(
    MVVCVTK_HOST_SOURCE_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_HOST_SOURCE_API_INCLUDE_DIR}"
    ${_mvvcvtk_host_stage_headers}
)
BuildSdkHeaders(
    MVVCVTK_HOST_STAGED_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
    ${_mvvcvtk_host_stage_headers}
)
GetSdkHeaderPaths(
    MVVCVTK_HOST_STAGED_PUBLIC_HEADERS
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
    ${MVVCVTK_HOST_PUBLIC_HEADERS}
)
unset(_mvvcvtk_host_stage_headers)

set(
    _mvvcvtk_spi_stage_headers
    ${MVVCVTK_FEATURE_SPI_HEADERS}
    ${MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS}
)
BuildSdkHeaders(
    MVVCVTK_FEATURE_STAGED_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
    ${_mvvcvtk_spi_stage_headers}
)
BuildSourceHeaders(
    MVVCVTK_FEATURE_SOURCE_SPI_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_FEATURE_SOURCE_SPI_INCLUDE_DIR}"
    ${_mvvcvtk_spi_stage_headers}
)
GetSdkHeaderPaths(
    MVVCVTK_FEATURE_STAGED_SPI_HEADERS
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
    ${MVVCVTK_FEATURE_SPI_HEADERS}
)
unset(_mvvcvtk_spi_stage_headers)

# HostAPI 只承载稳定 Host 头及其物理闭包，不链接 Host 实现库。
add_library(mvvcvtk_host_api INTERFACE)
add_library(MVVCVTK::HostAPI ALIAS mvvcvtk_host_api)
set_target_properties(
    mvvcvtk_host_api
    PROPERTIES
        EXPORT_NAME HostAPI
        FOLDER "MVVCVTK/Host/API"
)
target_sources(
    mvvcvtk_host_api
    INTERFACE
        FILE_SET api_headers
        TYPE HEADERS
        BASE_DIRS "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
        FILES ${MVVCVTK_HOST_STAGED_PUBLIC_HEADERS}
)
target_include_directories(
    mvvcvtk_host_api
    INTERFACE
        "$<BUILD_INTERFACE:${MVVCVTK_HOST_SOURCE_API_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
SetApiDefaults(mvvcvtk_host_api)
target_link_libraries(
    mvvcvtk_host_api
    INTERFACE ${MVVCVTK_VTK_PUBLIC_TARGETS}
)
install(
    TARGETS mvvcvtk_host_api
    EXPORT MVVCVTKTargets
    FILE_SET api_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT Host
)

# FeatureSPI 只交付实现 Feature 所需的契约；复用实现留在内部 FeatureSupport。
add_library(mvvcvtk_feature_spi INTERFACE)
add_library(MVVCVTK::FeatureSPI ALIAS mvvcvtk_feature_spi)
set_target_properties(
    mvvcvtk_feature_spi
    PROPERTIES
        EXPORT_NAME FeatureSPI
        FOLDER "MVVCVTK/Host/SPI"
)
target_sources(
    mvvcvtk_feature_spi
    INTERFACE
        FILE_SET spi_headers
        TYPE HEADERS
        BASE_DIRS "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
        FILES ${MVVCVTK_FEATURE_STAGED_SPI_HEADERS}
)
target_include_directories(
    mvvcvtk_feature_spi
    INTERFACE
        "$<BUILD_INTERFACE:${MVVCVTK_FEATURE_SOURCE_SPI_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
SetApiDefaults(mvvcvtk_feature_spi)
target_link_libraries(
    mvvcvtk_feature_spi
    INTERFACE ${MVVCVTK_VTK_PUBLIC_TARGETS}
)
install(
    TARGETS mvvcvtk_feature_spi
    EXPORT MVVCVTKTargets
    FILE_SET spi_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT Host
)

ExportHeaders(
    Host
    "${MVVCVTK_SDK_CLOSURE_INCLUDE_DIR}"
    ${MVVCVTK_SDK_CLOSURE_HEADERS}
)
add_subdirectory(features/Support)

add_library(mvvcvtk_host STATIC)
target_sources(
    mvvcvtk_host
    PRIVATE
        ${MVVCVTK_HOST_SOURCES}
        ${MVVCVTK_HOST_PUBLIC_HEADERS}
        ${MVVCVTK_HOST_CLOSURE_HEADERS}
        ${MVVCVTK_FEATURE_SPI_HEADERS}
        ${MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS}
)
target_include_directories(
    mvvcvtk_host
    PRIVATE
        "${MVVCVTK_HOST_INCLUDE_DIR}"
        ${MVVCVTK_HOST_PRIVATE_INCLUDE_DIRS}
)
SetTargetDefaults(mvvcvtk_host)
add_library(MVVCVTK::Host ALIAS mvvcvtk_host)
set_target_properties(
    mvvcvtk_host
    PROPERTIES
        OUTPUT_NAME MVVCVTKHost
        EXPORT_NAME Host
        PROJECT_LABEL MVVCVTKHost
        FOLDER "MVVCVTK/Host"
)
set(
    _mvvcvtk_host_ide_files
    ${MVVCVTK_HOST_SOURCES}
    ${MVVCVTK_HOST_PUBLIC_HEADERS}
    ${MVVCVTK_HOST_CLOSURE_HEADERS}
    ${MVVCVTK_FEATURE_SPI_HEADERS}
    ${MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS}
)
list(
    TRANSFORM _mvvcvtk_host_ide_files
    PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
)
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${_mvvcvtk_host_ide_files}
)
unset(_mvvcvtk_host_ide_files)
SetApiDefaults(mvvcvtk_host)
target_link_libraries(
    mvvcvtk_host
    PUBLIC MVVCVTK::HostAPI
    PRIVATE
        MVVCVTK::FeatureSPI
        "$<BUILD_INTERFACE:MVVCVTKInternal::FeatureSupport>"
        ${MVVCVTK_VTK_TARGETS}
)

install(
    TARGETS mvvcvtk_host
    EXPORT MVVCVTKTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>"
        COMPONENT Host
)

add_subdirectory(features)

get_property(
    MVVCVTK_CROP_API_HEADERS
    GLOBAL PROPERTY MVVCVTK_CROP_API_HEADERS
)
get_property(
    MVVCVTK_GAP_API_HEADERS
    GLOBAL PROPERTY MVVCVTK_GAP_API_HEADERS
)

# 不同源码模块不得覆盖同一个安装路径；核心 API/SPI 中指向同一源头的闭包重复可合并。
set(
    _mvvcvtk_core_install_headers
    ${MVVCVTK_HOST_PUBLIC_HEADERS}
    ${MVVCVTK_HOST_CLOSURE_HEADERS}
    ${MVVCVTK_FEATURE_SPI_HEADERS}
    ${MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS}
)
list(REMOVE_DUPLICATES _mvvcvtk_core_install_headers)
set(_mvvcvtk_install_keys)
foreach(header IN LISTS
    _mvvcvtk_core_install_headers
    MVVCVTK_CROP_API_HEADERS
    MVVCVTK_GAP_API_HEADERS
)
    GetSdkHeaderPath(sdkHeader "${header}")
    string(TOLOWER "${sdkHeader}" installKey)
    list(FIND _mvvcvtk_install_keys "${installKey}" duplicateIndex)
    if(NOT duplicateIndex EQUAL -1)
        message(FATAL_ERROR
            "Multiple SDK sources target the same installed header: "
            "${sdkHeader}"
        )
    endif()
    list(APPEND _mvvcvtk_install_keys "${installKey}")
endforeach()
unset(_mvvcvtk_core_install_headers)
unset(_mvvcvtk_install_keys)

# 构建树中的机器可读清单由同一声明源生成，供发布验证使用但不进入 SDK。
SetSurfaceLines(
    _mvvcvtk_host_api_lines
    HostAPI
    ${MVVCVTK_HOST_PUBLIC_HEADERS}
)
SetSurfaceLines(
    _mvvcvtk_host_support_lines
    HostSupport
    ${MVVCVTK_HOST_CLOSURE_HEADERS}
)
SetSurfaceLines(
    _mvvcvtk_spi_lines
    FeatureSPI
    ${MVVCVTK_FEATURE_SPI_HEADERS}
)
SetSurfaceLines(
    _mvvcvtk_spi_support_lines
    FeatureSupport
    ${MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS}
)
SetSurfaceLines(
    _mvvcvtk_crop_lines
    OrthogonalCrop
    ${MVVCVTK_CROP_API_HEADERS}
)
SetSurfaceLines(
    _mvvcvtk_gap_lines
    GapAnalysis
    ${MVVCVTK_GAP_API_HEADERS}
)
set(
    _mvvcvtk_surface_lines
    ${_mvvcvtk_host_api_lines}
    ${_mvvcvtk_host_support_lines}
    ${_mvvcvtk_spi_lines}
    ${_mvvcvtk_spi_support_lines}
    ${_mvvcvtk_crop_lines}
    ${_mvvcvtk_gap_lines}
)
list(SORT _mvvcvtk_surface_lines)
list(JOIN _mvvcvtk_surface_lines "\n" _mvvcvtk_surface_content)
set(
    MVVCVTK_HEADER_SURFACE_FILE
    "${CMAKE_BINARY_DIR}/MVVCVTKHeaderSurface.txt"
)
file(GENERATE
    OUTPUT "${MVVCVTK_HEADER_SURFACE_FILE}"
    CONTENT "${_mvvcvtk_surface_content}\n"
    NEWLINE_STYLE UNIX
)
unset(_mvvcvtk_crop_lines)
unset(_mvvcvtk_gap_lines)
unset(_mvvcvtk_host_api_lines)
unset(_mvvcvtk_host_support_lines)
unset(_mvvcvtk_spi_lines)
unset(_mvvcvtk_spi_support_lines)
unset(_mvvcvtk_surface_content)
unset(_mvvcvtk_surface_lines)

# 测试目录由根 CMakeLists 后续加入，需要生成视图的绝对路径。
set(
    MVVCVTK_HOST_SDK_INCLUDE_DIR
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
    PARENT_SCOPE
)
set(
    MVVCVTK_FEATURE_SPI_INCLUDE_DIR
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
    PARENT_SCOPE
)
set(
    MVVCVTK_CROP_SDK_INCLUDE_DIR
    "${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
    PARENT_SCOPE
)
set(
    MVVCVTK_GAP_SDK_INCLUDE_DIR
    "${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
    PARENT_SCOPE
)

unset(MVVCVTK_FEATURE_SPI_HEADERS)
unset(MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS)
unset(MVVCVTK_FEATURE_SOURCE_SPI_HEADERS)
unset(MVVCVTK_FEATURE_STAGED_HEADERS)
unset(MVVCVTK_FEATURE_STAGED_SPI_HEADERS)
unset(MVVCVTK_SDK_CLOSURE_HEADERS)
unset(MVVCVTK_SDK_STAGED_CLOSURE_HEADERS)
unset(MVVCVTK_HOST_CLOSURE_HEADERS)
unset(MVVCVTK_HOST_PUBLIC_HEADERS)
unset(MVVCVTK_HOST_SOURCE_API_HEADERS)
unset(MVVCVTK_HOST_SOURCES)
unset(MVVCVTK_HOST_STAGED_HEADERS)
unset(MVVCVTK_HOST_STAGED_PUBLIC_HEADERS)
