set(MVVCVTK_HOST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(
    MVVCVTK_HOST_SDK_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-sdk-host-include"
)
set(
    MVVCVTK_FEATURE_SPI_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-sdk-feature-spi-include"
)
set(
    MVVCVTK_CROP_SDK_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/features/OrthogonalCrop/mvvcvtk-sdk-include"
)
set(
    MVVCVTK_GAP_SDK_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/features/GapAnalysis/mvvcvtk-sdk-include"
)

# 源码目录保持原样；build-tree consumer 只看到批准的头文件视图。
# configure_file 会把输入登记为配置依赖，源头变化时由 CMake 刷新副本。
function(BuildSdkHeaders outputName sourceRoot stagingRoot)
    set(stagedHeaders)
    foreach(header IN LISTS ARGN)
        string(REGEX REPLACE "^include/" "" relativeHeader "${header}")
        set(sourceHeader "${sourceRoot}/${header}")
        set(stagedHeader "${stagingRoot}/${relativeHeader}")
        cmake_path(GET stagedHeader PARENT_PATH stagedDirectory)
        file(MAKE_DIRECTORY "${stagedDirectory}")
        configure_file("${sourceHeader}" "${stagedHeader}" COPYONLY)
        list(APPEND stagedHeaders "${stagedHeader}")
    endforeach()
    set(${outputName} "${stagedHeaders}" PARENT_SCOPE)
endfunction()

function(ResetSdkStage stagingRoot)
    cmake_path(
        ABSOLUTE_PATH stagingRoot
        NORMALIZE
        OUTPUT_VARIABLE normalizedStagingRoot
    )
    cmake_path(
        IS_PREFIX CMAKE_CURRENT_BINARY_DIR
        "${normalizedStagingRoot}"
        NORMALIZE
        isBuildSubdirectory
    )
    if(NOT isBuildSubdirectory
        OR normalizedStagingRoot STREQUAL CMAKE_CURRENT_BINARY_DIR)
        message(FATAL_ERROR
            "Refusing to reset SDK staging outside the MVVCVTK build tree: "
            "${normalizedStagingRoot}"
        )
    endif()
    file(REMOVE_RECURSE "${normalizedStagingRoot}")
    file(MAKE_DIRECTORY "${normalizedStagingRoot}")
endfunction()

# closure support 随安装包交付，但不进入公开 FILE_SET 元数据。
function(ExportHeaders componentName)
    foreach(header IN LISTS ARGN)
        string(REGEX REPLACE "^include/" "" relativeHeader "${header}")
        cmake_path(GET relativeHeader PARENT_PATH relativeDirectory)
        install(
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/${header}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${relativeDirectory}"
            COMPONENT ${componentName}
        )
    endforeach()
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
    include/Host/HostHotkeyRouter.h
    include/Host/HostViewRuntimeRegistry.h
    include/Host/LoadCommitCoordinator.h
    include/Interaction/AbstractViewContext.h
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
    include/Render/Strategies/ColoredPlanesStrategy.h
    include/Render/Strategies/CompositeStrategy.h
    include/Render/Strategies/IsoSurfaceStrategy.h
    include/Render/Strategies/SliceStrategy.h
    include/Render/Strategies/VolumeStrategy.h
    include/Render/Support/BaseVisualStrategy.h
    src/App/AppState.cpp
    src/App/Services/AppServiceFactory.cpp
    src/App/Tasks/AppDataExportTaskService.cpp
    src/App/Tasks/AppDataLoadTaskService.cpp
    src/Data/DataConverters.cpp
    src/Data/DataManager.cpp
    src/Data/ImageProcessor.cpp
    src/Data/VolumeTypes.cpp
    src/Host/HostCommandRouter.cpp
    src/Host/HostHotkeyRouter.cpp
    src/Host/HostViewRuntimeRegistry.cpp
    src/Host/LoadCommitCoordinator.cpp
    src/Host/VtkAppHostSession.cpp
    src/Interaction/AbstractViewContext.cpp
    src/Interaction/InputCallbackHandler.cpp
    src/Interaction/InteractionRouter.cpp
    src/Interaction/TimeUpdateHandler.cpp
    src/Interaction/ViewContextFactory.cpp
    src/Interaction/Viewer2DHandler.cpp
    src/Interaction/Viewer3DHandler.cpp
    src/Platform/MemMappedFile.cpp
    src/Render/RenderStrategyFactory.cpp
    src/Render/Strategies/ColoredPlanesStrategy.cpp
    src/Render/Strategies/CompositeStrategy.cpp
    src/Render/Strategies/IsoSurfaceStrategy.cpp
    src/Render/Strategies/SliceStrategy.cpp
    src/Render/Strategies/VolumeStrategy.cpp
)
# 普通 Host consumer 被正式允许直接包含的稳定入口与请求定义。
set(MVVCVTK_HOST_PUBLIC_HEADERS
    include/Host/VtkAppHostSession.h
    include/Host/Types/HostRequestTypes.h
)
# Host 公开入口递归编译所需，但不作为独立顶层 API 宣传的支持头。
set(MVVCVTK_HOST_CLOSURE_HEADERS
    include/Data/ImageReadTypes.h
    include/Host/Types/HostInputTypes.h
    include/Host/Types/HostRequest.h
    include/Host/Types/HostSessionTypes.h
    include/Host/Types/HostValueTypes.h
)
# Feature 开发者可直接使用的扩展契约，不依赖 Host 实现库。
set(MVVCVTK_FEATURE_SPI_HEADERS
    include/Host/HostFeature.h
    include/App/Services/FeatureViewService.h
    include/Render/Contracts/OverlayService.h
    include/Render/Contracts/RenderEffect.h
    include/Render/Contracts/VisualStrategy.h
)
# Feature SPI 独立携带 HostFeature、交互、可信数据与渲染契约的完整物理闭包。
set(MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS
    include/App/AppTypes.h
    include/App/ViewTypes.h
    include/Data/ImageReadTypes.h
    include/Data/TrustedImageState.h
    include/Host/Types/HostFeatureViewTypes.h
    include/Host/Types/HostInputTypes.h
    include/Host/Types/HostValueTypes.h
    include/Interaction/InteractionTypes.h
)

set(
    MVVCVTK_SDK_CLOSURE_HEADERS
    ${MVVCVTK_HOST_CLOSURE_HEADERS}
    ${MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS}
)
list(REMOVE_DUPLICATES MVVCVTK_SDK_CLOSURE_HEADERS)

# 每次配置都重建批准视图，避免已移出白名单的旧头残留在 staging 中。
ResetSdkStage("${MVVCVTK_HOST_SDK_INCLUDE_DIR}")
ResetSdkStage("${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}")

BuildSdkHeaders(
    MVVCVTK_HOST_STAGED_PUBLIC_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
    ${MVVCVTK_HOST_PUBLIC_HEADERS}
)
BuildSdkHeaders(
    MVVCVTK_HOST_STAGED_CLOSURE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_HOST_SDK_INCLUDE_DIR}"
    ${MVVCVTK_HOST_CLOSURE_HEADERS}
)
BuildSdkHeaders(
    MVVCVTK_FEATURE_STAGED_SPI_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
    ${MVVCVTK_FEATURE_SPI_HEADERS}
)
BuildSdkHeaders(
    MVVCVTK_FEATURE_STAGED_SPI_CLOSURE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_FEATURE_SPI_INCLUDE_DIR}"
    ${MVVCVTK_FEATURE_SPI_CLOSURE_HEADERS}
)

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
    INTERFACE "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
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
    INTERFACE "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
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
ExportHeaders(Host ${MVVCVTK_SDK_CLOSURE_HEADERS})
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
        MVVCVTKInternal::OpenCVWorld
        MVVCVTKInternal::UIPhantomCalib
        MVVCVTKInternal::UIReconstruct3D
)

install(
    TARGETS mvvcvtk_host
    EXPORT MVVCVTKTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>"
        COMPONENT Host
)

add_subdirectory(features)

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
unset(MVVCVTK_FEATURE_STAGED_SPI_HEADERS)
unset(MVVCVTK_FEATURE_STAGED_SPI_CLOSURE_HEADERS)
unset(MVVCVTK_SDK_CLOSURE_HEADERS)
unset(MVVCVTK_HOST_CLOSURE_HEADERS)
unset(MVVCVTK_HOST_PUBLIC_HEADERS)
unset(MVVCVTK_HOST_SOURCES)
unset(MVVCVTK_HOST_STAGED_CLOSURE_HEADERS)
unset(MVVCVTK_HOST_STAGED_PUBLIC_HEADERS)
