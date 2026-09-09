# 独立编译纯值数据核心，保持测试不依赖 Host 实现或 VTK。
add_executable(mvvcvtk_data_graph_tests
    DataGraphTests.cpp
    DataLifetimeTests.cpp
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/DataGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Geometry/RoiEvaluator.cpp"
)
target_include_directories(
    mvvcvtk_data_graph_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
)
SetTargetDefaults(mvvcvtk_data_graph_tests)
set_target_properties(
    mvvcvtk_data_graph_tests
    PROPERTIES PROJECT_LABEL DataGraphTests FOLDER "MVVCVTK/test/Automation/DataGraph"
)
add_test(NAME DataGraph.All COMMAND mvvcvtk_data_graph_tests)
add_test(NAME DataGraph.Lifetime COMMAND mvvcvtk_data_graph_tests lifetime)

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
    PROPERTIES PROJECT_LABEL VtkDataBridgeTests FOLDER "MVVCVTK/test/Automation/DataGraph"
)
add_test(NAME DataGraph.VtkBridge COMMAND mvvcvtk_vtk_data_bridge_tests)
SetTestRuntime(DataGraph.VtkBridge mvvcvtk_vtk_data_bridge_tests)

add_executable(mvvcvtk_roi_tests
    RoiTests.cpp
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/RoiService.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/RoiArchive.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/DataGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Geometry/RoiEvaluator.cpp"
)
target_include_directories(mvvcvtk_roi_tests PRIVATE "${PROJECT_SOURCE_DIR}/MVVCVTK/include")
SetTargetDefaults(mvvcvtk_roi_tests)
set_target_properties(mvvcvtk_roi_tests PROPERTIES FOLDER "MVVCVTK/Tests/DataGraph")
add_test(NAME DataGraph.Roi COMMAND mvvcvtk_roi_tests)
