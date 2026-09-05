add_executable(
    mvvcvtk_part_segmentation_tests
    PartSegmentationAlgorithmTests.cpp
    PartSegmentationCatalogTests.cpp
    PartSegmentationDisplayTests.cpp
    PartSegmentationLifecycleTests.cpp
    PartSegmentationLineageTests.cpp
    PartSegmentationScaleTests.cpp
    PartSegmentationTestCases.h
    PartSegmentationTests.cpp
)
set_target_properties(
    mvvcvtk_part_segmentation_tests
    PROPERTIES
        OUTPUT_NAME PartSegmentationTests
        PROJECT_LABEL PartSegmentationTests
        FOLDER "MVVCVTK/Tests"
)
target_include_directories(
    mvvcvtk_part_segmentation_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/PartSegmentation/include"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/PartSegmentation/include/Algorithms"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/PartSegmentation/include/Model"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/PartSegmentation/include/Render"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/PartSegmentation/include/Render/Strategies"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/PartSegmentation/include/Services"
)
target_link_libraries(
    mvvcvtk_part_segmentation_tests
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::PartSegmentation
        MVVCVTKInternal::FeatureSupport
        ${MVVCVTK_VTK_TARGETS}
)
SetTargetDefaults(mvvcvtk_part_segmentation_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_part_segmentation_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

foreach(_mvvcvtk_part_suite IN ITEMS algorithm catalog lifecycle lineage display scale)
    add_test(
        NAME "PartSegmentation.${_mvvcvtk_part_suite}"
        COMMAND mvvcvtk_part_segmentation_tests ${_mvvcvtk_part_suite}
    )
    SetTestRuntime(
        "PartSegmentation.${_mvvcvtk_part_suite}"
        mvvcvtk_part_segmentation_tests
    )
endforeach()
unset(_mvvcvtk_part_suite)
