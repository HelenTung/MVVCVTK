add_executable(
    mvvcvtk_crop_tests
    AppTaskServiceTests.cpp
    CropAlgorithmTests.cpp
    CropBridgeTests.cpp
    CropBridgeTests.h
    CropShaderPreviewTests.cpp
    PlanarCropTests.cpp
    PlanarTestSuites.h
)

set_target_properties(
    mvvcvtk_crop_tests
    PROPERTIES
        OUTPUT_NAME PlanarCropTests
        PROJECT_LABEL OrthogonalCropTests
        FOLDER "MVVCVTK/Tests"
)

target_include_directories(
    mvvcvtk_crop_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Algorithms"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Interaction"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Render"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Routing"
)

target_link_libraries(
    mvvcvtk_crop_tests
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::Host
        MVVCVTK::OrthogonalCrop
        MVVCVTKInternal::FeatureSupport
        ${MVVCVTK_VTK_TARGETS}
)

SetTargetDefaults(mvvcvtk_crop_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_crop_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

add_test(
    NAME OrthogonalCrop.All
    COMMAND mvvcvtk_crop_tests all
)
SetTestRuntime(OrthogonalCrop.All mvvcvtk_crop_tests)
