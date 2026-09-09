add_executable(qt_feature_tests main.cpp TestCaseRunner.cpp)
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
    --record "${CMAKE_CURRENT_BINARY_DIR}/automation-$<CONFIG>-record.json")
SetTestRuntime(QtFeature.Automation qt_feature_tests)
set_tests_properties(QtFeature.Automation PROPERTIES TIMEOUT 120)
