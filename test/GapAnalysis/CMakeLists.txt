function(BuildGapTests target outputName projectLabel)
    add_executable(
        ${target}
        GapAnalysisAlgorithmTests.cpp
        GapDisplayTests.cpp
        GapMaskBridgeTests.cpp
        GapDisplayTests.h
        GapKernelLabelViewTests.cpp
        GapKernelLabelViewTests.h
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/src/Services/GapKernelLabelView.cpp"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/src/Services/GapKernelLabelView.h"
    )

    set_target_properties(
        ${target}
        PROPERTIES
            OUTPUT_NAME ${outputName}
            PROJECT_LABEL ${projectLabel}
            FOLDER "MVVCVTK/test/Automation"
    )

    target_include_directories(
        ${target}
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include"
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include/Render/Strategies"
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include/Services"
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/src/Services"
    )

    target_link_libraries(
        ${target}
        PRIVATE
            mvvcvtk_host_test_internal
            MVVCVTK::Host
            MVVCVTK::GapAnalysis
            MVVCVTKInternal::FeatureSupport
            ${MVVCVTK_VTK_TARGETS}
    )

    SetTargetDefaults(${target})
    vtk_module_autoinit(
        TARGETS ${target}
        MODULES ${MVVCVTK_VTK_TARGETS}
    )
endfunction()

add_executable(gap_display_mesh_tests GapDisplayMeshTests.cpp)
target_include_directories(gap_display_mesh_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include")
target_link_libraries(gap_display_mesh_tests PRIVATE
    mvvcvtk_host_test_internal MVVCVTK::Host MVVCVTKInternal::FeatureSupport ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(gap_display_mesh_tests)
set_target_properties(gap_display_mesh_tests PROPERTIES FOLDER "MVVCVTK/test/Automation")
vtk_module_autoinit(TARGETS gap_display_mesh_tests MODULES ${MVVCVTK_VTK_TARGETS})
add_test(NAME GapAnalysis.DisplayMesh COMMAND gap_display_mesh_tests)
SetTestRuntime(GapAnalysis.DisplayMesh gap_display_mesh_tests)
# 调试 CRT 下逐个提取 7 万个独立表面耗时较长；此超时只限制测试运行，不改变几何/内存断言。
set_tests_properties(GapAnalysis.DisplayMesh PROPERTIES TIMEOUT 600)

add_executable(gap_result_reader_tests GapResultReaderTests.cpp)
target_include_directories(gap_result_reader_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/src")
target_link_libraries(gap_result_reader_tests PRIVATE MVVCVTK::GapAnalysis MVVCVTKInternal::FeatureSupport)
SetTargetDefaults(gap_result_reader_tests)
set_target_properties(gap_result_reader_tests PROPERTIES FOLDER "MVVCVTK/test/Automation")
add_test(NAME GapAnalysis.ResultReader COMMAND gap_result_reader_tests)
SetTestRuntime(GapAnalysis.ResultReader gap_result_reader_tests)

BuildGapTests(
    mvvcvtk_gap_main_tests
    GapAnalysisAlgorithmTests
    GapAnalysisTests
)

add_test(
    NAME GapAnalysis.All
    COMMAND mvvcvtk_gap_main_tests
)
SetTestRuntime(GapAnalysis.All mvvcvtk_gap_main_tests)
add_test(NAME GapAnalysis.MaskGeometry COMMAND mvvcvtk_gap_main_tests --mask-geometry)
SetTestRuntime(GapAnalysis.MaskGeometry mvvcvtk_gap_main_tests)
set_tests_properties(GapAnalysis.MaskGeometry PROPERTIES TIMEOUT 120)
add_test(NAME GapAnalysis.MaskBridge COMMAND mvvcvtk_gap_main_tests --mask-bridge)
SetTestRuntime(GapAnalysis.MaskBridge mvvcvtk_gap_main_tests)
add_test(NAME GapAnalysis.Connectivity COMMAND mvvcvtk_gap_main_tests --connectivity)
SetTestRuntime(GapAnalysis.Connectivity mvvcvtk_gap_main_tests)
set(MVVCVTK_GAP_REAL_SMALL_INPUT "" CACHE FILEPATH "Optional integration-20261011 consumer-real-32 float32 ROI for public Gap regression")
if(MVVCVTK_GAP_REAL_SMALL_INPUT)
    add_test(NAME GapAnalysis.RealSmallInput COMMAND mvvcvtk_gap_main_tests --real-small-input "${MVVCVTK_GAP_REAL_SMALL_INPUT}")
    SetTestRuntime(GapAnalysis.RealSmallInput mvvcvtk_gap_main_tests)
    set_tests_properties(GapAnalysis.RealSmallInput PROPERTIES TIMEOUT 90)
endif()
set_property(
    TEST GapAnalysis.All
    PROPERTY PASS_REGULAR_EXPRESSION
        "\\[GapAnalysis\\]\\[DefX\\] labels ready \\| scalar=int \\| transfer=borrowed"
)
set_property(
    TEST GapAnalysis.All
    PROPERTY FAIL_REGULAR_EXPRESSION
        "GapAnalysisAlgorithmTests failed:"
)

# 路径回归测试把测试程序放到独立目录，并在当前工作目录放置同名伪 DLL。
# 生产加载器必须忽略当前目录，只从 Feature 专属的绝对目录加载 runtime 对。
BuildGapTests(
    mvvcvtk_gap_path_tests
    GapKernelPathTests
    GapKernelPathTests
)
set_target_properties(
    mvvcvtk_gap_path_tests
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/path/$<CONFIG>"
)
set(_mvvcvtk_gap_decoy_dir
    "$<TARGET_FILE_DIR:mvvcvtk_gap_path_tests>/cwd"
)
add_custom_command(
    TARGET mvvcvtk_gap_path_tests
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_mvvcvtk_gap_decoy_dir}"
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${_mvvcvtk_gap_decoy_dir}/GapKernel.dll"
    VERBATIM
)
add_test(
    NAME GapAnalysis.KernelPath
    COMMAND mvvcvtk_gap_path_tests
)
SetTestRuntime(GapAnalysis.KernelPath mvvcvtk_gap_path_tests)
set_property(
    TEST GapAnalysis.KernelPath
    PROPERTY WORKING_DIRECTORY "${_mvvcvtk_gap_decoy_dir}"
)
set_property(
    TEST GapAnalysis.KernelPath
    APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "MVVCVTK_GAP_RUNTIME_DIR=set:$<TARGET_FILE_DIR:gap_kernel>"
)
set_property(
    TEST GapAnalysis.KernelPath
    PROPERTY TIMEOUT 30
)
unset(_mvvcvtk_gap_decoy_dir)

add_test(
    NAME GapAnalysis.LegacyPolicy
    COMMAND "${CMAKE_COMMAND}"
        "-DMVVCVTK_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckGapLegacy.cmake"
)

add_test(
    NAME GapAnalysis.DefXBoundary
    COMMAND "${CMAKE_COMMAND}"
        "-DMVVCVTK_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckDefXBoundary.cmake"
)
