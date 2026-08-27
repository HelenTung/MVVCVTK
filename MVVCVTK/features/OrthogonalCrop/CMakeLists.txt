set(MVVCVTK_CROP_SOURCES
    include/Algorithms/CropAlgorithm.h
    include/Interaction/CropBoxWidget.h
    include/Interaction/CropBridge.h
    include/Interaction/CropPlaneWidget.h
    include/Render/CropShaderController.h
    include/Routing/CropRouter.h
    src/Algorithms/CropAlgorithm.cpp
    src/Host/CropHostFeature.cpp
    src/Interaction/CropBoxWidget.cpp
    src/Interaction/CropBridge.cpp
    src/Interaction/CropPlaneWidget.cpp
    src/Render/CropShaderController.cpp
    src/Routing/CropRouter.cpp
)
# CropHostFeature 是模块入口，OrthogonalCropTypes 是其公开签名的类型闭包。
set(MVVCVTK_CROP_API_HEADERS
    include/Host/CropHostFeature.h
    include/OrthogonalCropTypes.h
)

# Feature target 与核心 API 一样只向 build-tree consumer 暴露批准视图。
ResetSdkStage("${MVVCVTK_CROP_SDK_INCLUDE_DIR}")
BuildSdkHeaders(
    MVVCVTK_CROP_STAGED_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_CROP_SDK_INCLUDE_DIR}"
    ${MVVCVTK_CROP_API_HEADERS}
)

add_library(mvvcvtk_orthogonal_crop STATIC)
target_sources(
    mvvcvtk_orthogonal_crop
    PRIVATE
        ${MVVCVTK_CROP_SOURCES}
        ${MVVCVTK_CROP_API_HEADERS}
)
target_include_directories(
    mvvcvtk_orthogonal_crop
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Algorithms"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Host"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Interaction"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Routing"
)
SetTargetDefaults(mvvcvtk_orthogonal_crop)
add_library(MVVCVTK::OrthogonalCrop ALIAS mvvcvtk_orthogonal_crop)
set_target_properties(
    mvvcvtk_orthogonal_crop
    PROPERTIES
        OUTPUT_NAME MVVCVTKOrthogonalCrop
        EXPORT_NAME OrthogonalCrop
        PROJECT_LABEL MVVCVTKOrthogonalCrop
        FOLDER "MVVCVTK/Host/Features"
)
target_sources(
    mvvcvtk_orthogonal_crop
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
unset(MVVCVTK_CROP_STAGED_API_HEADERS)
target_include_directories(
    mvvcvtk_orthogonal_crop
    PUBLIC
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
SetApiDefaults(mvvcvtk_orthogonal_crop)
target_link_libraries(
    mvvcvtk_orthogonal_crop
    PUBLIC
        MVVCVTK::FeatureSPI
        ${MVVCVTK_VTK_PUBLIC_TARGETS}
    PRIVATE
        ${MVVCVTK_VTK_TARGETS}
)
install(
    TARGETS mvvcvtk_orthogonal_crop
    EXPORT MVVCVTKTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>"
        COMPONENT Host
    FILE_SET api_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT Host
)
