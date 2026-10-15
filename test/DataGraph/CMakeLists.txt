# 独立编译纯值数据核心，保持测试不依赖 Host 实现或 VTK。
add_executable(mvvcvtk_data_graph_tests
    DataGraphTests.cpp
    DataLifetimeTests.cpp
    ResultRetentionTests.cpp
    DataArrayTests.cpp
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/DataGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Geometry/RoiVoxelSet.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Geometry/RoiEvaluator.cpp"
)
target_include_directories(
    mvvcvtk_data_graph_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/MVVCVTK/include" "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/Support"
)
SetTargetDefaults(mvvcvtk_data_graph_tests)
set_target_properties(
    mvvcvtk_data_graph_tests
    PROPERTIES PROJECT_LABEL DataGraphTests FOLDER "MVVCVTK/test/Automation/DataGraph"
)
add_test(NAME DataGraph.All COMMAND mvvcvtk_data_graph_tests)
add_test(NAME DataGraph.Lifetime COMMAND mvvcvtk_data_graph_tests lifetime)
add_test(NAME DataGraph.ResultRetention COMMAND mvvcvtk_data_graph_tests retention)
add_test(NAME DataGraph.ResultArrays COMMAND mvvcvtk_data_graph_tests arrays)

add_executable(mvvcvtk_spatial_mesh_tests SpatialMeshTests.cpp)
target_link_libraries(mvvcvtk_spatial_mesh_tests PRIVATE MVVCVTKInternal::FeatureSupport
    VTK::CommonCore VTK::CommonDataModel)
SetTargetDefaults(mvvcvtk_spatial_mesh_tests)
target_compile_definitions(mvvcvtk_spatial_mesh_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(mvvcvtk_spatial_mesh_tests PROPERTIES FOLDER "MVVCVTK/test/Automation/DataGraph")
add_test(NAME DataGraph.SpatialMesh COMMAND mvvcvtk_spatial_mesh_tests)
SetTestRuntime(DataGraph.SpatialMesh mvvcvtk_spatial_mesh_tests)

add_executable(mvvcvtk_spatial_mesh_view_tests SpatialMeshViewTests.cpp)
target_link_libraries(mvvcvtk_spatial_mesh_view_tests PRIVATE MVVCVTKInternal::FeatureSupport ${MVVCVTK_VTK_TARGETS})
target_compile_definitions(mvvcvtk_spatial_mesh_view_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
SetTargetDefaults(mvvcvtk_spatial_mesh_view_tests)
set_target_properties(mvvcvtk_spatial_mesh_view_tests PROPERTIES FOLDER "MVVCVTK/test/Automation/DataGraph")
vtk_module_autoinit(TARGETS mvvcvtk_spatial_mesh_view_tests MODULES ${MVVCVTK_VTK_TARGETS})
add_test(NAME DataGraph.SpatialMeshView COMMAND mvvcvtk_spatial_mesh_view_tests)
SetTestRuntime(DataGraph.SpatialMeshView mvvcvtk_spatial_mesh_view_tests)

add_executable(mvvcvtk_vtk_data_bridge_tests
    VtkDataBridgeTests.cpp
)
target_include_directories(
    mvvcvtk_vtk_data_bridge_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
)
target_link_libraries(
    mvvcvtk_vtk_data_bridge_tests
    PRIVATE MVVCVTK::Host MVVCVTKInternal::FeatureSupport
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
    RoiReadExecutionTests.cpp
    RoiMaskArchiveTests.cpp
    RoiArchiveTestReader.h
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/RoiService.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/RoiOperations.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/RoiArchive.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/DataGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Geometry/RoiVoxelSet.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Geometry/RoiEvaluator.cpp"
)
target_include_directories(mvvcvtk_roi_tests PRIVATE "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data")
SetTargetDefaults(mvvcvtk_roi_tests)
set_target_properties(mvvcvtk_roi_tests PROPERTIES
    PROJECT_LABEL RoiTests FOLDER "MVVCVTK/test/Automation/DataGraph")
add_test(NAME DataGraph.Roi COMMAND mvvcvtk_roi_tests)
add_test(NAME DataGraph.RoiMaskArchive COMMAND mvvcvtk_roi_tests mask-archive)

add_executable(mvvcvtk_roi_operation_tests RoiOperationsTests.cpp
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/RoiService.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/RoiOperations.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/RoiArchive.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/DataGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Geometry/RoiVoxelSet.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Geometry/RoiEvaluator.cpp")
target_include_directories(mvvcvtk_roi_operation_tests PRIVATE "${PROJECT_SOURCE_DIR}/MVVCVTK/include")
SetTargetDefaults(mvvcvtk_roi_operation_tests)
set_target_properties(mvvcvtk_roi_operation_tests PROPERTIES PROJECT_LABEL RoiOperationTests FOLDER "MVVCVTK/test/Automation/DataGraph")
add_test(NAME DataGraph.RoiOperations COMMAND mvvcvtk_roi_operation_tests)
add_test(NAME DataGraph.RoiProcessingRegression COMMAND mvvcvtk_roi_operation_tests processing)
add_test(NAME DataGraph.RoiCompressed COMMAND mvvcvtk_roi_operation_tests compressed)
