add_executable(
    mvvcvtk_surface_determination_tests
    SurfaceDeterminationAlgorithmTests.cpp
    SurfaceDeterminationDisplayTests.cpp
    SurfaceDeterminationGeometryTests.cpp
    SurfaceDeterminationLifecycleTests.cpp
    SurfaceDeterminationTestCases.h
    SurfaceDeterminationTestSupport.h
    SurfaceDeterminationTests.cpp
)

# 该 probe 不取得 Host 私有测试 include，专门证明仓内公共头只依赖
# MVVCVTK::SurfaceDetermination 的传递 include/link surface。
add_library(
    mvvcvtk_surface_header_probe
    OBJECT SurfaceDeterminationHeaderProbe.cpp
)
target_link_libraries(
    mvvcvtk_surface_header_probe
    PRIVATE MVVCVTK::SurfaceDetermination
)
SetTargetDefaults(mvvcvtk_surface_header_probe)
set_target_properties(
    mvvcvtk_surface_header_probe
    PROPERTIES FOLDER "MVVCVTK/Tests/HeaderProbes"
)
set_target_properties(
    mvvcvtk_surface_determination_tests
    PROPERTIES
        OUTPUT_NAME SurfaceDeterminationTests
        PROJECT_LABEL SurfaceDeterminationTests
        FOLDER "MVVCVTK/Tests"
)
target_include_directories(
    mvvcvtk_surface_determination_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/include"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/src"
)
target_link_libraries(
    mvvcvtk_surface_determination_tests
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::Host
        MVVCVTK::SurfaceDetermination
        MVVCVTKInternal::FeatureSupport
        ${MVVCVTK_VTK_TARGETS}
)
SetTargetDefaults(mvvcvtk_surface_determination_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_surface_determination_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

foreach(_mvvcvtk_surface_suite IN ITEMS algorithm geometry lifecycle display)
    add_test(
        NAME "SurfaceDetermination.${_mvvcvtk_surface_suite}"
        COMMAND mvvcvtk_surface_determination_tests ${_mvvcvtk_surface_suite}
    )
    SetTestRuntime(
        "SurfaceDetermination.${_mvvcvtk_surface_suite}"
        mvvcvtk_surface_determination_tests
    )
endforeach()
unset(_mvvcvtk_surface_suite)
