set(_wall_headers
    include/Host/WallThicknessHostFeature.h
    include/Host/WallThicknessHostTypes.h)
add_library(wall_thickness STATIC)
add_library(MVVCVTK::WallThickness ALIAS wall_thickness)
target_sources(wall_thickness PRIVATE
    src/ThicknessAlgorithm.h src/ThicknessAlgorithm.cpp
    src/ThicknessMath.h src/ThicknessEvaluation.cpp
    src/ThicknessData.h src/ThicknessData.cpp
    src/ThicknessOverlay.h src/ThicknessOverlay.cpp
    src/WallThicknessHostFeature.cpp
    PUBLIC FILE_SET api_headers TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include" FILES ${_wall_headers})
target_include_directories(wall_thickness
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(wall_thickness PUBLIC MVVCVTK::FeatureSPI
    PRIVATE MVVCVTKInternal::FeatureSupport VTK::CommonCore VTK::CommonDataModel
        VTK::FiltersCore VTK::FiltersGeneral VTK::RenderingCore VTK::RenderingAnnotation)
SetTargetDefaults(wall_thickness)
set_target_properties(wall_thickness PROPERTIES OUTPUT_NAME WallThickness
    PROJECT_LABEL WallThickness FOLDER "MVVCVTK/Host/Features")
unset(_wall_headers)
