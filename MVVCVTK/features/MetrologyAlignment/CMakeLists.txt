add_library(metrology_alignment STATIC)
add_library(MVVCVTK::MetrologyAlignment ALIAS metrology_alignment)
target_sources(metrology_alignment
    PRIVATE src/AlignmentMath.h src/AlignmentGeometry.h src/AlignmentGeometry.cpp
        src/AlignmentSolver.h src/AlignmentSolver.cpp
        src/AlignmentData.h src/AlignmentData.cpp
        src/AlignmentOverlay.h src/AlignmentOverlay.cpp
        src/MetrologyAlignmentHostFeature.cpp
    PUBLIC FILE_SET api_headers TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES include/Host/MetrologyAlignmentHostFeature.h include/Host/MetrologyAlignmentHostTypes.h)
target_include_directories(metrology_alignment
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(metrology_alignment PUBLIC MVVCVTK::FeatureSPI
    PRIVATE opencv_world MVVCVTKInternal::FeatureSupport VTK::CommonCore
        VTK::CommonDataModel VTK::RenderingCore)
SetTargetDefaults(metrology_alignment)
set_target_properties(metrology_alignment PROPERTIES
    OUTPUT_NAME MetrologyAlignment PROJECT_LABEL MetrologyAlignment FOLDER "MVVCVTK/Host/Features")
