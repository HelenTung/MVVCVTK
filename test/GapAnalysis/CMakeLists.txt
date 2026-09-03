function(BuildGapTests target outputName projectLabel)
    add_executable(
        ${target}
        GapAnalysisAlgorithmTests.cpp
        GapDisplayTests.cpp
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
            FOLDER "MVVCVTK/Tests"
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
            MVVCVTK::GapAnalysis
            ${MVVCVTK_VTK_TARGETS}
    )

    SetTargetDefaults(${target})
    vtk_module_autoinit(
        TARGETS ${target}
        MODULES ${MVVCVTK_VTK_TARGETS}
    )
endfunction()

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
set_property(
    TEST GapAnalysis.All
    PROPERTY PASS_REGULAR_EXPRESSION
        "\\[GapAnalysis\\]\\[DefX\\] labels ready \\| scalar=int \\| transfer=borrowed"
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
        "${_mvvcvtk_gap_decoy_dir}/MVVCVTKGapKernel.dll"
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
        "MVVCVTK_GAP_RUNTIME_DIR=set:$<TARGET_FILE_DIR:mvvcvtk_gap_kernel>"
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
