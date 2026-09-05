set(rotationApiHeaders
    include/Host/ModelRotationHostFeature.h
    include/Host/ModelRotationHostTypes.h
)
set(rotationStage "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-source-api-include")
ResetSdkStage("${rotationStage}")
BuildSourceHeaders(rotationHeaders "${CMAKE_CURRENT_SOURCE_DIR}"
    "${rotationStage}" ${rotationApiHeaders})
add_library(mvvcvtk_model_rotation STATIC
    ${rotationApiHeaders}
    include/Algorithms/ModelRotationAlgorithm.h
    src/Algorithms/ModelRotationAlgorithm.cpp
    src/Host/ModelRotationHostFeature.cpp
)
add_library(MVVCVTK::ModelRotation ALIAS mvvcvtk_model_rotation)
target_include_directories(mvvcvtk_model_rotation
    PUBLIC "$<BUILD_INTERFACE:${rotationStage}>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mvvcvtk_model_rotation
    PUBLIC MVVCVTK::FeatureSPI
    PRIVATE ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(mvvcvtk_model_rotation)
SetApiDefaults(mvvcvtk_model_rotation)
set_target_properties(mvvcvtk_model_rotation PROPERTIES
    OUTPUT_NAME MVVCVTKModelRotation FOLDER "MVVCVTK/Host/Features")
