add_executable(
    mvvcvtk_interaction_tests
    AppStateTests.cpp
    AppStateTests.h
    Fakes/AppPorts.h
    Fakes/Host/HostCoreServices.h
    Fakes/Host/HostRoutes.h
    Fakes/ViewContext.h
    Fakes/vtkImageData.h
    Fakes/vtkPolyData.h
    HostCommandRouterTests.cpp
    HostCommandRouterTests.h
    HostHotkeyRouterTests.cpp
    HostHotkeyRouterTests.h
    HostInputRegistryTests.cpp
    HostInputRegistryTests.h
    InteractionRouterTests.cpp
)

set_target_properties(
    mvvcvtk_interaction_tests
    PROPERTIES
        OUTPUT_NAME InteractionRouterTests
        PROJECT_LABEL InteractionTests
        FOLDER "MVVCVTK/Tests"
)

target_include_directories(
    mvvcvtk_interaction_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Fakes"
)

target_link_libraries(
    mvvcvtk_interaction_tests
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::Host
        ${MVVCVTK_VTK_TARGETS}
)

SetTargetDefaults(mvvcvtk_interaction_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_interaction_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

add_test(
    NAME Interaction.All
    COMMAND mvvcvtk_interaction_tests
)
SetTestRuntime(Interaction.All mvvcvtk_interaction_tests)
