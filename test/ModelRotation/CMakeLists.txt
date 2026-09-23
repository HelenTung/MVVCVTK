add_executable(mvvcvtk_model_rotation_tests ModelRotationTests.cpp ModelRotationSessionTests.cpp RealInputBindingsTests.cpp)
target_include_directories(mvvcvtk_model_rotation_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/ModelRotation/include")
target_link_libraries(mvvcvtk_model_rotation_tests PRIVATE
    mvvcvtk_host_test_internal MVVCVTK::Host MVVCVTK::ModelRotation
    ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(mvvcvtk_model_rotation_tests)
vtk_module_autoinit(TARGETS mvvcvtk_model_rotation_tests MODULES ${MVVCVTK_VTK_TARGETS})
set_target_properties(mvvcvtk_model_rotation_tests PROPERTIES PROJECT_LABEL ModelRotationTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME ModelRotation.behavior COMMAND mvvcvtk_model_rotation_tests)
SetTestRuntime(ModelRotation.behavior mvvcvtk_model_rotation_tests)
# 真实数据用例仅在提供经过核验的 ROI 时注册；默认 CTest 不依赖本地数据。
set(MVVCVTK_REAL_INPUT_BINDINGS_ROI "" CACHE FILEPATH
    "Verified 128^3 float32 CT ROI for input binding audit")
if(MVVCVTK_REAL_INPUT_BINDINGS_ROI)
    if(NOT EXISTS "${MVVCVTK_REAL_INPUT_BINDINGS_ROI}")
        message(FATAL_ERROR "Real input bindings ROI does not exist")
    endif()
    add_test(NAME ModelRotation.real_input_bindings
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/InvokeRealInputBindings.ps1"
            -Executable "$<TARGET_FILE:mvvcvtk_model_rotation_tests>"
            -RoiPath "${MVVCVTK_REAL_INPUT_BINDINGS_ROI}"
            -ExpectedSha256
                "8d170587f36e1eb29e4f21f888133437e70641a7c1424ce70199695caf71e677")
    SetTestRuntime(ModelRotation.real_input_bindings mvvcvtk_model_rotation_tests)
    set_tests_properties(ModelRotation.real_input_bindings PROPERTIES
        TIMEOUT 120 LABELS "real-data")
endif()
add_executable(mvvcvtk_rotation_header_probe ModelRotationHeaderProbe.cpp)
target_link_libraries(mvvcvtk_rotation_header_probe PRIVATE MVVCVTK::ModelRotation)
SetTargetDefaults(mvvcvtk_rotation_header_probe)
set_target_properties(mvvcvtk_rotation_header_probe PROPERTIES PROJECT_LABEL ModelRotationHeaderProbe FOLDER "MVVCVTK/test/Automation/HeaderProbes")
add_test(NAME ModelRotation.independent_link COMMAND mvvcvtk_rotation_header_probe)
SetTestRuntime(ModelRotation.independent_link mvvcvtk_rotation_header_probe)
