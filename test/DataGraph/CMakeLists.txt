add_executable(mvvcvtk_data_graph_tests
    DataGraphTests.cpp
    $<TARGET_OBJECTS:mvvcvtk_data_core>
)
target_include_directories(
    mvvcvtk_data_graph_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
)
SetTargetDefaults(mvvcvtk_data_graph_tests)
set_target_properties(
    mvvcvtk_data_graph_tests
    PROPERTIES FOLDER "MVVCVTK/Tests/DataGraph"
)
add_test(NAME DataGraph.All COMMAND mvvcvtk_data_graph_tests)

add_executable(mvvcvtk_vtk_data_bridge_tests
    VtkDataBridgeTests.cpp
)
target_include_directories(
    mvvcvtk_vtk_data_bridge_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
)
target_link_libraries(
    mvvcvtk_vtk_data_bridge_tests
    PRIVATE MVVCVTK::Host
)
SetTargetDefaults(mvvcvtk_vtk_data_bridge_tests)
set_target_properties(
    mvvcvtk_vtk_data_bridge_tests
    PROPERTIES FOLDER "MVVCVTK/Tests/DataGraph"
)
add_test(NAME DataGraph.VtkBridge COMMAND mvvcvtk_vtk_data_bridge_tests)
