add_executable(qt_feature_tests main.cpp TestCaseRunner.cpp BusinessWorkflowTests.cpp WorkbenchV1Tests.cpp
    ReportWorkflowTests.cpp ReportWorkflowTests.h ReportRepairTests.cpp ReportLargeTests.cpp ReportLargeTests.h
    Scenarios/business-primary.json Scenarios/business-crop.json
    Scenarios/business-artifact.json Scenarios/business-measurement.json
    Scenarios/frontend-context-regression.json Scenarios/frontend-analysis-regression.json Scenarios/workbench-v1.json
    RoiContinuousRegression.py ReportLargeRegression.py)
target_link_libraries(qt_feature_tests PRIVATE qt_feature_test_support)
target_include_directories(qt_feature_tests PRIVATE "${PROJECT_SOURCE_DIR}/MVVCVTK/src")
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
add_test(NAME QtFeature.CaptureConsistency COMMAND qt_feature_tests --background --capture-consistency-only
    --capture-output "${CMAKE_CURRENT_BINARY_DIR}/capture-consistency-$<CONFIG>")
SetTestRuntime(QtFeature.CaptureConsistency qt_feature_tests)
set_tests_properties(QtFeature.CaptureConsistency PROPERTIES RUN_SERIAL TRUE TIMEOUT 0 LABELS "capture")
add_test(NAME QtFeature.Reports COMMAND qt_feature_tests --background --reports-only)
SetTestRuntime(QtFeature.Reports qt_feature_tests)
set_tests_properties(QtFeature.Reports PROPERTIES TIMEOUT 0 LABELS "Reports;preview")
if(TARGET MVVCVTK::GapAnalysis AND TARGET MVVCVTK::OrthogonalCrop)
    set(_gap_report_args --background --gap-boundary-reports-only
        --gap-report-output "${CMAKE_CURRENT_BINARY_DIR}/gap-boundary-reports-$<CONFIG>")
    if(MVVCVTK_GAP_REAL_SMALL_INPUT)
        list(APPEND _gap_report_args --gap-report-real-input "${MVVCVTK_GAP_REAL_SMALL_INPUT}")
    endif()
    add_test(NAME QtFeature.GapBoundaryReports COMMAND qt_feature_tests ${_gap_report_args})
    SetTestRuntime(QtFeature.GapBoundaryReports qt_feature_tests)
    set_tests_properties(QtFeature.GapBoundaryReports PROPERTIES RUN_SERIAL TRUE TIMEOUT 0 LABELS "Reports;Gap")
    unset(_gap_report_args)
endif()
# 数据规模只影响耗时；调用方可显式取消或配置外层期限。
set_tests_properties(QtFeature.Automation PROPERTIES TIMEOUT 0)

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
        --output "${CMAKE_CURRENT_BINARY_DIR}/report-large-final-$<CONFIG>" --launcher "${CMAKE_BINARY_DIR}/RunQtFeatureTests-$<CONFIG>.cmd")
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
        # 以完整业务终态验收，耗时增加不自动中断已接纳的大数据任务。
        set_tests_properties(QtFeature.RoiContinuous.${scenario} PROPERTIES
            LABELS "real-data;roi;continuous;Reports;preview" RUN_SERIAL TRUE TIMEOUT 0)
    endforeach()
endif()
