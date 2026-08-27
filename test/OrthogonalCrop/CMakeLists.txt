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
        # 白盒测试显式获得 Host 内部头；产品 target 不传播该源码根。
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/App"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/App/Services"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/App/Tasks"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Data"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Geometry"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Host"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Interaction"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Platform"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Render"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Render/Strategies"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Render/Support"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Algorithms"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Interaction"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Render"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Routing"
)

target_link_libraries(
    mvvcvtk_crop_tests
    PRIVATE
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
