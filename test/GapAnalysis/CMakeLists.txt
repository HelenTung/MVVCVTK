add_executable(
    mvvcvtk_gap_tests
    GapAnalysisAlgorithmTests.cpp
    GapDisplayTests.cpp
    GapDisplayTests.h
)

set_target_properties(
    mvvcvtk_gap_tests
    PROPERTIES
        OUTPUT_NAME GapAnalysisAlgorithmTests
        PROJECT_LABEL GapAnalysisTests
        FOLDER "MVVCVTK/Tests"
)

target_include_directories(
    mvvcvtk_gap_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include/Algorithms"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include/Render/Strategies"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include/Services"
)

target_link_libraries(
    mvvcvtk_gap_tests
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::GapAnalysis
        ${MVVCVTK_VTK_TARGETS}
)

SetTargetDefaults(mvvcvtk_gap_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_gap_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

add_test(
    NAME GapAnalysis.All
    COMMAND mvvcvtk_gap_tests
)
SetTestRuntime(GapAnalysis.All mvvcvtk_gap_tests)
