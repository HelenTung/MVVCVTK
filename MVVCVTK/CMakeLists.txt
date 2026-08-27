set(MVVCVTK_HOST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
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
# Host 的两个语义入口是 VtkAppHostSession 与 HostFeature；其余头文件是
# 这两个入口公开签名所需的最小编译闭包，并不是并列的顶层模块。
set(MVVCVTK_HOST_API_HEADERS
    include/App/AppTypes.h
    include/App/Services/FeatureViewService.h
    include/Data/ImageReadTypes.h
    include/Data/TrustedImageState.h
    include/Host/HostFeature.h
    include/Host/Types/HostFeatureViewTypes.h
    include/Host/Types/HostInputTypes.h
    include/Host/Types/HostRequest.h
    include/Host/Types/HostRequestTypes.h
    include/Host/Types/HostSessionTypes.h
    include/Host/Types/HostValueTypes.h
    include/Host/VtkAppHostSession.h
    include/Interaction/InteractionTypes.h
    include/Render/Contracts/OverlayService.h
    include/Render/Contracts/RenderEffect.h
    include/Render/Contracts/VisualStrategy.h
)

add_library(mvvcvtk_host STATIC)
target_sources(mvvcvtk_host PRIVATE ${MVVCVTK_HOST_SOURCES})
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
target_sources(
    mvvcvtk_host
    PUBLIC
        FILE_SET api_headers
        TYPE HEADERS
        BASE_DIRS "${MVVCVTK_HOST_INCLUDE_DIR}"
        FILES ${MVVCVTK_HOST_API_HEADERS}
)

set(
    _mvvcvtk_host_ide_files
    ${MVVCVTK_HOST_SOURCES}
    ${MVVCVTK_HOST_API_HEADERS}
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
unset(MVVCVTK_HOST_SOURCES)
unset(MVVCVTK_HOST_API_HEADERS)
target_include_directories(
    mvvcvtk_host
    PUBLIC
        "$<BUILD_INTERFACE:${MVVCVTK_HOST_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
SetApiDefaults(mvvcvtk_host)
target_link_libraries(
    mvvcvtk_host
    PUBLIC
        ${MVVCVTK_VTK_PUBLIC_TARGETS}
    PRIVATE
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
    FILE_SET api_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT Host
)

add_subdirectory(features)

add_library(mvvcvtk_sdk INTERFACE)
add_library(MVVCVTK::SDK ALIAS mvvcvtk_sdk)
set_target_properties(
    mvvcvtk_sdk
    PROPERTIES
        EXPORT_NAME SDK
        FOLDER "MVVCVTK/Host"
)
target_compile_features(mvvcvtk_sdk INTERFACE cxx_std_17)
target_link_libraries(
    mvvcvtk_sdk
    INTERFACE
        MVVCVTK::Host
        MVVCVTK::OrthogonalCrop
        MVVCVTK::GapAnalysis
)
install(
    TARGETS mvvcvtk_sdk
    EXPORT MVVCVTKTargets
    COMPONENT Host
)
