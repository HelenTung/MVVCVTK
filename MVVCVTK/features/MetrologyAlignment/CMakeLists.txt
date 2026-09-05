add_library(mvvcvtk_metrology_alignment STATIC)
add_library(MVVCVTK::MetrologyAlignment ALIAS mvvcvtk_metrology_alignment)
target_sources(mvvcvtk_metrology_alignment
    PRIVATE src/AlignmentMath.h src/AlignmentGeometry.h src/AlignmentGeometry.cpp
        src/AlignmentSolver.h src/AlignmentSolver.cpp
        src/AlignmentData.h src/AlignmentData.cpp
        src/AlignmentOverlay.h src/AlignmentOverlay.cpp
        src/MetrologyAlignmentHostFeature.cpp
    PUBLIC FILE_SET api_headers TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES include/Host/MetrologyAlignmentHostFeature.h include/Host/MetrologyAlignmentHostTypes.h)
target_include_directories(mvvcvtk_metrology_alignment
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mvvcvtk_metrology_alignment PUBLIC MVVCVTK::FeatureSPI
    PRIVATE opencv_world MVVCVTKInternal::FeatureSupport VTK::CommonCore
        VTK::CommonDataModel VTK::RenderingCore)
SetTargetDefaults(mvvcvtk_metrology_alignment)
set_target_properties(mvvcvtk_metrology_alignment PROPERTIES
    OUTPUT_NAME MVVCVTKMetrologyAlignment FOLDER "MVVCVTK/Host/Features")
