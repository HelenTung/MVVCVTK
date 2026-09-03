set(MVVCVTK_PART_API_HEADERS
    include/Host/PartSegmentationHostFeature.h
    include/Host/PartSegmentationHostTypes.h
)
set(MVVCVTK_PART_SOURCES
    include/Algorithms/ClassicalPartSegmenter.h
    include/Render/Strategies/PartOverlayStrategies.h
    include/Services/PartSegmentationService.h
    src/Algorithms/ClassicalPartSegmenter.cpp
    src/Host/PartSegmentationHostFeature.cpp
    src/Render/Strategies/PartOverlayStrategies.cpp
    src/Services/PartSegmentationService.cpp
)

ResetSdkStage("${MVVCVTK_PART_SOURCE_API_INCLUDE_DIR}")
BuildSourceHeaders(
    MVVCVTK_PART_SOURCE_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_PART_SOURCE_API_INCLUDE_DIR}"
    ${MVVCVTK_PART_API_HEADERS}
)

add_library(mvvcvtk_part_segmentation STATIC)
add_library(
    MVVCVTK::PartSegmentation
    ALIAS mvvcvtk_part_segmentation
)
target_sources(
    mvvcvtk_part_segmentation
    PRIVATE
        ${MVVCVTK_PART_SOURCES}
        ${MVVCVTK_PART_API_HEADERS}
)
target_include_directories(
    mvvcvtk_part_segmentation
    PUBLIC
        "$<BUILD_INTERFACE:${MVVCVTK_PART_SOURCE_API_INCLUDE_DIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Algorithms"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Host"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Strategies"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Services"
)
target_link_libraries(
    mvvcvtk_part_segmentation
    PUBLIC MVVCVTK::FeatureSPI
    PRIVATE
        MVVCVTKInternal::FeatureSupport
        ${MVVCVTK_VTK_TARGETS}
)
SetTargetDefaults(mvvcvtk_part_segmentation)
SetApiDefaults(mvvcvtk_part_segmentation)
set_target_properties(
    mvvcvtk_part_segmentation
    PROPERTIES
        OUTPUT_NAME MVVCVTKPartSegmentation
        PROJECT_LABEL MVVCVTKPartSegmentation
        FOLDER "MVVCVTK/Host/Features"
)
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        ${MVVCVTK_PART_SOURCES}
        ${MVVCVTK_PART_API_HEADERS}
)

unset(MVVCVTK_PART_API_HEADERS)
unset(MVVCVTK_PART_SOURCES)
