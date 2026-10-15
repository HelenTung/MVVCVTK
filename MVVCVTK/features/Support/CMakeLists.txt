add_library(mvvcvtk_feature_support INTERFACE)
add_library(MVVCVTKInternal::FeatureSupport ALIAS mvvcvtk_feature_support)
set_target_properties(
    mvvcvtk_feature_support
    PROPERTIES FOLDER "MVVCVTK/Host/FeatureSupport"
)
target_sources(
    mvvcvtk_feature_support
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Support/FeatureOverlayBase.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Support/AnalysisColorStyle.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Support/AnalysisLegend.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Support/SliceContourPlane.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Support/RenderTextStyle.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/FeatureSupport/WorkLimit.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/FeatureSupport/WidgetBindings.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/FeatureSupport/RoutedWidgets.h"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/Support/LabelMapBuilder.h"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/Support/PagedArray.h"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/Support/MeshVtkStorage.h"
)
target_include_directories(
    mvvcvtk_feature_support
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/Support>"
)
SetApiDefaults(mvvcvtk_feature_support)
target_link_libraries(
    mvvcvtk_feature_support
    INTERFACE MVVCVTK::FeatureSPI
)
