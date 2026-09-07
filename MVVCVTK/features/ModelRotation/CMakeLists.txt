set(rotationApiHeaders
    include/Host/ModelRotationHostFeature.h
    include/Host/ModelRotationHostTypes.h
)
set(rotationStage "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-source-api-include")
ResetSdkStage("${rotationStage}")
BuildSourceHeaders(rotationHeaders "${CMAKE_CURRENT_SOURCE_DIR}"
    "${rotationStage}" ${rotationApiHeaders})
add_library(model_rotation STATIC
    ${rotationApiHeaders}
    include/Algorithms/ModelRotationAlgorithm.h
    src/Algorithms/ModelRotationAlgorithm.cpp
    src/Host/ModelRotationHostFeature.cpp
)
add_library(MVVCVTK::ModelRotation ALIAS model_rotation)
target_include_directories(model_rotation
    PUBLIC "$<BUILD_INTERFACE:${rotationStage}>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(model_rotation
    PUBLIC MVVCVTK::FeatureSPI
    PRIVATE ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(model_rotation)
SetApiDefaults(model_rotation)
set_target_properties(model_rotation PROPERTIES
    OUTPUT_NAME ModelRotation PROJECT_LABEL ModelRotation FOLDER "MVVCVTK/Host/Features")
