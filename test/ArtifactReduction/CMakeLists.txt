add_library(mvvcvtk_tomopy_reference STATIC reference/remove_ring.c)
target_include_directories(mvvcvtk_tomopy_reference PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/reference")
target_compile_options(mvvcvtk_tomopy_reference PRIVATE /W3 /utf-8 /wd4244 /wd4305)
set_target_properties(mvvcvtk_tomopy_reference PROPERTIES PROJECT_LABEL TomoPyReference C_STANDARD 11
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    FOLDER "MVVCVTK/test/Automation/Reference")

add_library(mvvcvtk_artifact_header_probe OBJECT ArtifactReductionHeaderProbe.cpp)
target_link_libraries(mvvcvtk_artifact_header_probe PRIVATE MVVCVTK::ArtifactReduction)
SetTargetDefaults(mvvcvtk_artifact_header_probe)
set_target_properties(mvvcvtk_artifact_header_probe PROPERTIES PROJECT_LABEL ArtifactReductionHeaderProbe FOLDER "MVVCVTK/test/Automation/HeaderProbes")

add_executable(mvvcvtk_artifact_tests ArtifactReductionTests.cpp
    ArtifactAlgorithmTests.cpp ArtifactLifecycleTests.cpp ArtifactNativeTests.cpp)
target_include_directories(mvvcvtk_artifact_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/test"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/ArtifactReduction/src"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/ArtifactReduction/third_party/tomopy")
target_link_libraries(mvvcvtk_artifact_tests PRIVATE
    mvvcvtk_host_test_internal MVVCVTK::Host MVVCVTK::ArtifactReduction
    tomopy_ring mvvcvtk_tomopy_reference VTK::ImagingGeneral ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(mvvcvtk_artifact_tests)
set_target_properties(mvvcvtk_artifact_tests PROPERTIES PROJECT_LABEL ArtifactReductionTests FOLDER "MVVCVTK/test/Automation")
vtk_module_autoinit(TARGETS mvvcvtk_artifact_tests MODULES ${MVVCVTK_VTK_TARGETS})
foreach(suite IN ITEMS native algorithm lifecycle)
    add_test(NAME "ArtifactReduction.${suite}" COMMAND mvvcvtk_artifact_tests ${suite})
    SetTestRuntime("ArtifactReduction.${suite}" mvvcvtk_artifact_tests)
endforeach()
