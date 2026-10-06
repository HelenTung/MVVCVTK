set(_wall_headers
    include/Host/WallThicknessHostFeature.h
    include/Host/WallThicknessHostTypes.h)
add_library(wall_thickness STATIC)
add_library(MVVCVTK::WallThickness ALIAS wall_thickness)
# 连续材料路径的精确符号判定只在此仓内 Feature 的私有实现使用 Boost 头。
set(MVVCVTK_WALL_BOOST_ROOT "${MVVCVTK_DEPS_ROOT}/../third_party/boost" CACHE PATH
    "Private Boost 1.88.0 header root for WallThickness")
if(NOT EXISTS "${MVVCVTK_WALL_BOOST_ROOT}/boost/version.hpp")
    message(FATAL_ERROR "WallThickness requires private Boost 1.88.0 headers: MVVCVTK_WALL_BOOST_ROOT")
endif()
file(STRINGS "${MVVCVTK_WALL_BOOST_ROOT}/boost/version.hpp" _wall_boost_version
    REGEX "^#define BOOST_VERSION ")
if(NOT _wall_boost_version STREQUAL "#define BOOST_VERSION 108800")
    message(FATAL_ERROR "WallThickness requires exactly Boost 1.88.0 headers")
endif()
unset(_wall_boost_version)
target_include_directories(wall_thickness SYSTEM PRIVATE "${MVVCVTK_WALL_BOOST_ROOT}")
target_sources(wall_thickness PRIVATE
    src/ThicknessAlgorithm.h src/ThicknessAlgorithm.cpp
    src/ThicknessMath.h src/ThicknessMaterialField.h src/ThicknessEvaluation.cpp
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
target_compile_options(wall_thickness PRIVATE /fp:strict)
set_target_properties(wall_thickness PROPERTIES OUTPUT_NAME WallThickness
    PROJECT_LABEL WallThickness FOLDER "MVVCVTK/Host/Features")
unset(_wall_headers)
