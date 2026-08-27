add_library(mvvcvtk_feature_support INTERFACE)
add_library(MVVCVTKInternal::FeatureSupport ALIAS mvvcvtk_feature_support)
set_target_properties(
    mvvcvtk_feature_support
    PROPERTIES FOLDER "MVVCVTK/Host/FeatureSupport"
)
target_sources(
    mvvcvtk_feature_support
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Support/FeatureVisualStrategy.h"
)
target_include_directories(
    mvvcvtk_feature_support
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
SetApiDefaults(mvvcvtk_feature_support)
target_link_libraries(
    mvvcvtk_feature_support
    INTERFACE MVVCVTK::FeatureSPI
)
