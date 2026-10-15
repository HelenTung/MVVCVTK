add_executable(qt_feature_tests main.cpp TestCaseRunner.cpp BusinessWorkflowTests.cpp WorkbenchV1Tests.cpp
    ReportWorkflowTests.cpp ReportWorkflowTests.h ReportRepairTests.cpp ReportLargeTests.cpp ReportLargeTests.h
    Scenarios/business-primary.json Scenarios/business-crop.json
    Scenarios/business-artifact.json Scenarios/business-measurement.json
    Scenarios/frontend-context-regression.json Scenarios/frontend-analysis-regression.json Scenarios/workbench-v1.json
    RoiContinuousRegression.py ReportLargeRegression.py)
target_link_libraries(qt_feature_tests PRIVATE qt_feature_test_support)
SetQtTargetDefaults(qt_feature_tests)
set_target_properties(qt_feature_tests PROPERTIES OUTPUT_NAME QtFeatureTests
    PROJECT_LABEL QtFeatureTests FOLDER "MVVCVTK/test/Automation")
list(JOIN MVVCVTK_RUNTIME_DIRS ";" automation_runtime_path)
set_property(TARGET qt_feature_tests PROPERTY VS_DEBUGGER_ENVIRONMENT
    "PATH=${automation_runtime_path};%PATH%\nQT_PLUGIN_PATH=${MVVCVTK_QT_PLUGIN_DIR}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/RunQtFeatureTests-$<CONFIG>.cmd" CONTENT
    "@echo off\nsetlocal\nset \"PATH=${automation_runtime_path};%PATH%\"\nset \"QT_PLUGIN_PATH=${MVVCVTK_QT_PLUGIN_DIR}\"\n\"$<TARGET_FILE:qt_feature_tests>\" %*\n")
vtk_module_autoinit(TARGETS qt_feature_tests MODULES ${MVVCVTK_QT_VTK_TARGETS})
add_test(NAME QtFeature.Automation COMMAND qt_feature_tests
    --background --record "${CMAKE_CURRENT_BINARY_DIR}/automation-$<CONFIG>-record.json")
SetTestRuntime(QtFeature.Automation qt_feature_tests)
add_test(NAME QtFeature.Reports COMMAND qt_feature_tests --background --reports-only)
SetTestRuntime(QtFeature.Reports qt_feature_tests)
set_tests_properties(QtFeature.Reports PROPERTIES TIMEOUT 60 LABELS "Reports;preview")
# Debug 的完整 UI 回归包含各业务及截图；实测约 94 s，预留冷启动余量。
set_tests_properties(QtFeature.Automation PROPERTIES TIMEOUT "$<IF:$<CONFIG:Debug>,240,120>")

# 大报告专项完整读取真实 600 整卷；不裁 ROI、不截取结果行。
set(MVVCVTK_QT_REPORT_INPUT "" CACHE FILEPATH "600x1800x600 float32 报告专项真实 CT")
set(MVVCVTK_QT_REPORT_SPACING_MM "" CACHE STRING "报告专项真实 CT 的等距体素间距，mm")
if(MVVCVTK_QT_REPORT_INPUT)
    if(NOT EXISTS "${MVVCVTK_QT_REPORT_INPUT}" OR NOT MVVCVTK_QT_REPORT_SPACING_MM)
        message(FATAL_ERROR "真实大报告回归需要存在的 RAW 和明确的 MVVCVTK_QT_REPORT_SPACING_MM")
    endif()
    foreach(feature IN ITEMS SurfaceDetermination PartSegmentation GapAnalysis MetrologyAlignment WallThickness)
        if(NOT TARGET MVVCVTK::${feature})
            message(FATAL_ERROR "真实大报告回归缺少 ${feature}")
        endif()
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME QtFeature.ReportsLarge COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ReportLargeRegression.py"
        --input "${MVVCVTK_QT_REPORT_INPUT}" --spacing "${MVVCVTK_QT_REPORT_SPACING_MM}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/report-large-capacity-$<CONFIG>" --launcher "${CMAKE_BINARY_DIR}/RunQtFeatureTests-$<CONFIG>.cmd")
    # 整卷按全量完成判定，不因固定小时数终止正常的大数据任务。
    set_tests_properties(QtFeature.ReportsLarge PROPERTIES LABELS "real-data;Reports;large" RUN_SERIAL TRUE TIMEOUT 0)
endif()

# 真实整卷回归显式启用，不用合成输入替代缺失的 CT 或采集间距。
set(MVVCVTK_QT_ROI_CHAIN_INPUT "" CACHE FILEPATH "600x1800x600 float32 真实 CT")
set(MVVCVTK_QT_ROI_CHAIN_SPACING_MM "" CACHE STRING "真实 CT 的等距体素间距，mm")
if(MVVCVTK_QT_ROI_CHAIN_INPUT)
    if(NOT EXISTS "${MVVCVTK_QT_ROI_CHAIN_INPUT}" OR NOT MVVCVTK_QT_ROI_CHAIN_SPACING_MM)
        message(FATAL_ERROR "连续 ROI 真实回归需要存在的 RAW 和明确的 MVVCVTK_QT_ROI_CHAIN_SPACING_MM")
    endif()
    foreach(feature IN ITEMS RoiEditing OrthogonalCrop SurfaceDetermination PartSegmentation
            GapAnalysis ArtifactReduction MetrologyAlignment WallThickness)
        if(NOT TARGET MVVCVTK::${feature})
            message(FATAL_ERROR "连续 ROI 真实回归缺少 ${feature}")
        endif()
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(scenario IN ITEMS 00-multi-roi-operations 01-alternating-rois 02-roi-revisions
            03-repeat-same-crop 04-mixed-shape-crop 05-independent-crops-complete
            06-multi-level-loop 07-preview-to-roi 08-crop-history-branches
            09-label-edit-wall-repeat 10-surface-metrology-cycle 11-interleaved-old-chain)
        add_test(NAME QtFeature.RoiContinuous.${scenario}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/RoiContinuousRegression.py"
                --input "${MVVCVTK_QT_ROI_CHAIN_INPUT}" --spacing "${MVVCVTK_QT_ROI_CHAIN_SPACING_MM}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/roi-continuous-$<CONFIG>"
                --launcher "${CMAKE_BINARY_DIR}/RunQtFeatureTests-$<CONFIG>.cmd" --case "${scenario}")
        # 最大单步壁厚等待为 1 h；进程上限另留前后处理和退出余量，不是性能达标阈值。
        set_tests_properties(QtFeature.RoiContinuous.${scenario} PROPERTIES
            LABELS "real-data;roi;continuous;Reports;preview" RUN_SERIAL TRUE TIMEOUT 7200)
    endforeach()
endif()
