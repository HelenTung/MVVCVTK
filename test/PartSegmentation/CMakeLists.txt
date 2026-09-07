add_executable(
    mvvcvtk_part_segmentation_tests
    PartSegmentationAlgorithmTests.cpp
    PartSegmentationEditTests.cpp
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
        MVVCVTK::Host
        MVVCVTK::PartSegmentation
        MVVCVTKInternal::FeatureSupport
        ${MVVCVTK_VTK_TARGETS}
        Psapi
)
SetTargetDefaults(mvvcvtk_part_segmentation_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_part_segmentation_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

foreach(_mvvcvtk_part_suite IN ITEMS algorithm catalog lifecycle lineage display scale edit)
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

# 需要显式真实数据参数的仓内审计程序，不作为缺数据即跳过的常规 CTest。
add_executable(part_performance_audit PartPerformanceAudit.cpp)
target_link_libraries(part_performance_audit PRIVATE MVVCVTK::Host MVVCVTK::PartSegmentation ${MVVCVTK_VTK_TARGETS} Psapi)
SetTargetDefaults(part_performance_audit)
set_target_properties(part_performance_audit PROPERTIES FOLDER "MVVCVTK/Tests" OUTPUT_NAME PartPerformanceAudit)
vtk_module_autoinit(TARGETS part_performance_audit MODULES ${MVVCVTK_VTK_TARGETS})

# 仅显式提供同配置、同依赖的已归档基线库时构建 A/B 对照；不链接当前实现。
if(PART_PERFORMANCE_BASELINE_DIR)
    foreach(baselineLib IN ITEMS MVVCVTKHost.lib PartSegmentation.lib)
        if(NOT EXISTS "${PART_PERFORMANCE_BASELINE_DIR}/${baselineLib}")
            message(FATAL_ERROR "Missing performance baseline library: ${baselineLib}")
        endif()
    endforeach()
    add_executable(part_performance_audit_baseline EXCLUDE_FROM_ALL PartPerformanceAudit.cpp)
    target_include_directories(part_performance_audit_baseline PRIVATE
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/PartSegmentation/include")
    target_link_libraries(part_performance_audit_baseline PRIVATE MVVCVTK::HostAPI MVVCVTK::FeatureSPI
        "${PART_PERFORMANCE_BASELINE_DIR}/MVVCVTKHost.lib"
        "${PART_PERFORMANCE_BASELINE_DIR}/PartSegmentation.lib"
        ${MVVCVTK_VTK_TARGETS} Psapi)
    SetTargetDefaults(part_performance_audit_baseline)
    set_target_properties(part_performance_audit_baseline PROPERTIES FOLDER "MVVCVTK/Tests" OUTPUT_NAME PartPerformanceBaseline)
    vtk_module_autoinit(TARGETS part_performance_audit_baseline MODULES ${MVVCVTK_VTK_TARGETS})
endif()
