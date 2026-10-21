add_executable(
    mvvcvtk_part_segmentation_tests
    PartSegmentationAlgorithmTests.cpp
    PartSegmentationEditTests.cpp
    PartSegmentationCatalogTests.cpp
    PartSegmentationDisplayTests.cpp
    PartSurfaceProductTests.cpp
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
        FOLDER "MVVCVTK/test/Automation"
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

foreach(_mvvcvtk_part_suite IN ITEMS algorithm catalog lifecycle scope-lifecycle lineage display scale edit surface-product)
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
# 运行：PartPerformanceAudit raw sourceSide roiStart roiSide threshold outputPrefix，六个参数均必需。
# raw 须为无文件头的 float32 立方 RAW，文件恰为 sourceSide^3 * 4 字节，x 方向连续。
# sourceSide 为 1..8192，roiSide 为 2..512；三轴共用 roiStart，须满足 0 <= roiStart <= sourceSide-roiSide。
# threshold 须为有限数值；几何固定为等距 0.1537 mm、三轴原点均为 roiStart * spacing，不接受几何覆盖参数。
# outputPrefix 的父目录须已存在且可写；输出 <prefix>.<start|split0|split1|split2>.metrics.txt 和 .labels.bin。
# 工具不读取专用数据环境变量；手动运行时须能从 PATH 等 DLL 搜索位置找到锁定的 VTK runtime。
# 缺参数、RAW 打不开/尺寸不符、输出不可写或审计失败均输出 [FAIL] 并返回 1，成功返回 0，不按缺数据跳过。
add_executable(part_performance_audit PartPerformanceAudit.cpp)
target_link_libraries(part_performance_audit PRIVATE MVVCVTK::Host MVVCVTK::PartSegmentation ${MVVCVTK_VTK_TARGETS} Psapi)
SetTargetDefaults(part_performance_audit)
set_target_properties(part_performance_audit PROPERTIES
    OUTPUT_NAME PartPerformanceAudit PROJECT_LABEL PartPerformanceAudit FOLDER "MVVCVTK/test/Automation")
vtk_module_autoinit(TARGETS part_performance_audit MODULES ${MVVCVTK_VTK_TARGETS})

# 仅显式提供同配置、同依赖的已归档基线库时构建 A/B 对照；不链接当前实现。
# 配置输入为 -DPART_PERFORMANCE_BASELINE_DIR=<目录>（CMake 变量，不读取同名环境变量）。
# 未设置或为假值时不创建目标；启用后目录内必须有 MVVCVTKHost.lib 和 PartSegmentation.lib，缺任一库即 FATAL_ERROR。
# 目标 part_performance_audit_baseline 需显式构建（EXCLUDE_FROM_ALL），可执行文件为 PartPerformanceBaseline。
# 运行参数、真实 RAW、输出前缀与失败行为同上，使用同一个 PartPerformanceAudit.cpp 入口。
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
    set_target_properties(part_performance_audit_baseline PROPERTIES
        OUTPUT_NAME PartPerformanceBaseline PROJECT_LABEL PartPerformanceBaseline FOLDER "MVVCVTK/test/Automation")
    vtk_module_autoinit(TARGETS part_performance_audit_baseline MODULES ${MVVCVTK_VTK_TARGETS})
endif()
