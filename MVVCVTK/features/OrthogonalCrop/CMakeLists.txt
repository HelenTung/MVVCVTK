set(MVVCVTK_CROP_SOURCES
    include/Algorithms/CropAlgorithm.h
    include/Algorithms/CropGeometry.h
    include/Algorithms/CropMeshAlgorithm.h
    include/Algorithms/CropMeshExact.h
    include/Interaction/CropHistory.h
    include/Interaction/CropHistoryQueue.h
    include/Interaction/CropCurveWidget.h
    include/Interaction/CropBoxWidget.h
    include/Interaction/CropBridge.h
    include/Interaction/CropPlaneWidget.h
    include/Render/CropShaderController.h
    include/Routing/CropRouter.h
    src/Algorithms/CropAlgorithm.cpp
    src/Algorithms/CropGeometry.cpp
    src/Algorithms/CropMeshAlgorithm.cpp
    src/Interaction/CropHistory.cpp
    src/Interaction/CropHistoryQueue.cpp
    src/Host/CropHostFeature.cpp
    src/Interaction/CropCurveWidget.cpp
    src/Interaction/CropBoxWidget.cpp
    src/Interaction/CropBridge.cpp
    src/Interaction/CropPlaneWidget.cpp
    src/Render/CropShaderController.cpp
    src/Routing/CropRouter.cpp
)
if(MSVC)
    set_source_files_properties(src/Algorithms/CropMeshAlgorithm.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()
# CropHostFeature 是模块入口，OrthogonalCropTypes 是其公开签名的类型闭包。
SetHeaderSurface(
    MVVCVTK_CROP_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    include/Host/CropHostFeature.h
    include/OrthogonalCropTypes.h
)

# Feature target 与核心 API 一样只向 build-tree consumer 暴露批准视图。
ResetSdkStage("${MVVCVTK_CROP_SDK_INCLUDE_DIR}")
ResetSdkStage("${MVVCVTK_CROP_SOURCE_API_INCLUDE_DIR}")
BuildSdkHeaders(
    MVVCVTK_CROP_STAGED_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
    ${MVVCVTK_CROP_API_HEADERS}
)
BuildSourceHeaders(
    MVVCVTK_CROP_SOURCE_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_CROP_SOURCE_API_INCLUDE_DIR}"
    ${MVVCVTK_CROP_API_HEADERS}
)

add_library(orthogonal_crop STATIC)
target_sources(
    orthogonal_crop
    PRIVATE
        ${MVVCVTK_CROP_SOURCES}
        ${MVVCVTK_CROP_API_HEADERS}
)
target_include_directories(
    orthogonal_crop
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Algorithms"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Host"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Interaction"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Routing"
)
SetTargetDefaults(orthogonal_crop)
add_library(MVVCVTK::OrthogonalCrop ALIAS orthogonal_crop)
set_target_properties(
    orthogonal_crop
    PROPERTIES
        OUTPUT_NAME OrthogonalCrop
        EXPORT_NAME OrthogonalCrop
        PROJECT_LABEL OrthogonalCrop
        FOLDER "MVVCVTK/Host/Features"
)
target_sources(
    orthogonal_crop
    PUBLIC
        FILE_SET api_headers
        TYPE HEADERS
        BASE_DIRS "${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
        FILES ${MVVCVTK_CROP_STAGED_API_HEADERS}
)

set(
    _mvvcvtk_crop_ide_files
    ${MVVCVTK_CROP_SOURCES}
    ${MVVCVTK_CROP_API_HEADERS}
)
list(
    TRANSFORM _mvvcvtk_crop_ide_files
    PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
)
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${_mvvcvtk_crop_ide_files}
)
unset(_mvvcvtk_crop_ide_files)
unset(MVVCVTK_CROP_SOURCES)
unset(MVVCVTK_CROP_API_HEADERS)
unset(MVVCVTK_CROP_SOURCE_API_HEADERS)
unset(MVVCVTK_CROP_STAGED_API_HEADERS)
target_include_directories(
    orthogonal_crop
    PUBLIC
        "$<BUILD_INTERFACE:${MVVCVTK_CROP_SOURCE_API_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
SetApiDefaults(orthogonal_crop)
target_link_libraries(
    orthogonal_crop
    PUBLIC
        MVVCVTK::FeatureSPI
        ${MVVCVTK_VTK_PUBLIC_TARGETS}
    PRIVATE
        ${MVVCVTK_VTK_TARGETS}
)
install(
    TARGETS orthogonal_crop
    EXPORT MVVCVTKTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>"
        COMPONENT Host
    FILE_SET api_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT Host
)
