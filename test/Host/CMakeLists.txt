add_executable(
    mvvcvtk_host_frame_tests
    HostFrameCoordinatorTests.cpp
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Host/HostFrameCoordinator.cpp"
)

set_target_properties(
    mvvcvtk_host_frame_tests
    PROPERTIES
        OUTPUT_NAME HostFrameCoordinatorTests
        PROJECT_LABEL HostFrameCoordinatorTests
        FOLDER "MVVCVTK/Tests"
)

target_link_libraries(
    mvvcvtk_host_frame_tests
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::FeatureSPI
        ${MVVCVTK_VTK_TARGETS}
)

SetTargetDefaults(mvvcvtk_host_frame_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_host_frame_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

add_test(
    NAME Host.FrameCoordinator
    COMMAND mvvcvtk_host_frame_tests
)
SetTestRuntime(Host.FrameCoordinator mvvcvtk_host_frame_tests)
