set(MVVCVTK_SURFACE_API_HEADERS
    include/Host/SurfaceDeterminationHostFeature.h
    include/Host/SurfaceDeterminationHostTypes.h
)
set(MVVCVTK_SURFACE_SOURCES
    src/SurfaceContracts.h
    src/SurfaceContracts.cpp
    src/SurfaceDeterminationAlgorithm.h
    src/SurfaceDeterminationAlgorithm.cpp
    src/SurfaceDeterminationHostFeature.cpp
    src/SurfaceDeterminationService.h
    src/SurfaceDeterminationService.cpp
    src/SurfaceGenerationStore.h
    src/SurfaceGenerationStore.cpp
    src/SurfaceOverlayStrategy.h
    src/SurfaceOverlayStrategy.cpp
)

add_library(surface_determination STATIC)
add_library(
    MVVCVTK::SurfaceDetermination
    ALIAS surface_determination
)
target_sources(
    surface_determination
    PRIVATE ${MVVCVTK_SURFACE_SOURCES}
    PUBLIC
        FILE_SET api_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES ${MVVCVTK_SURFACE_API_HEADERS}
)
target_include_directories(
    surface_determination
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
    surface_determination
    PUBLIC MVVCVTK::FeatureSPI
    PRIVATE
        MVVCVTKInternal::FeatureSupport
        VTK::CommonCore
        VTK::CommonDataModel
        VTK::FiltersCore
        VTK::FiltersGeneral
        VTK::RenderingCore
)
SetTargetDefaults(surface_determination)
set_target_properties(
    surface_determination
    PROPERTIES
        OUTPUT_NAME SurfaceDetermination
        PROJECT_LABEL SurfaceDetermination
        FOLDER "MVVCVTK/Host/Features"
)
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        ${MVVCVTK_SURFACE_API_HEADERS}
        ${MVVCVTK_SURFACE_SOURCES}
)

unset(MVVCVTK_SURFACE_API_HEADERS)
unset(MVVCVTK_SURFACE_SOURCES)
