add_executable(mvvcvtk_alignment_tests AlignmentAlgorithmTests.cpp AlignmentConstraintTests.cpp)
target_include_directories(mvvcvtk_alignment_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_tests PRIVATE MVVCVTK::MetrologyAlignment opencv_world)
SetTargetDefaults(mvvcvtk_alignment_tests)
set_target_properties(mvvcvtk_alignment_tests PROPERTIES PROJECT_LABEL MetrologyAlignmentTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME MetrologyAlignment.Algorithm COMMAND mvvcvtk_alignment_tests)
SetTestRuntime(MetrologyAlignment.Algorithm mvvcvtk_alignment_tests)
add_executable(mvvcvtk_alignment_surface_tests AlignmentSurfaceTests.cpp)
target_include_directories(mvvcvtk_alignment_surface_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_surface_tests PRIVATE MVVCVTK::MetrologyAlignment opencv_world)
SetTargetDefaults(mvvcvtk_alignment_surface_tests)
set_target_properties(mvvcvtk_alignment_surface_tests PROPERTIES
    PROJECT_LABEL AlignmentSurfaceTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME MetrologyAlignment.Surface COMMAND mvvcvtk_alignment_surface_tests)
SetTestRuntime(MetrologyAlignment.Surface mvvcvtk_alignment_surface_tests)
add_executable(mvvcvtk_alignment_prealign_tests AlignmentPrealignTests.cpp AlignmentPrealignTestSupport.h)
target_include_directories(mvvcvtk_alignment_prealign_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_prealign_tests PRIVATE MVVCVTK::MetrologyAlignment opencv_world)
SetTargetDefaults(mvvcvtk_alignment_prealign_tests)
set_target_properties(mvvcvtk_alignment_prealign_tests PROPERTIES
    PROJECT_LABEL AlignmentPrealignTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME MetrologyAlignment.Prealign COMMAND mvvcvtk_alignment_prealign_tests)
SetTestRuntime(MetrologyAlignment.Prealign mvvcvtk_alignment_prealign_tests)
add_executable(mvvcvtk_alignment_reference_io_tests AlignmentReferenceTestsIo.cpp)
target_include_directories(mvvcvtk_alignment_reference_io_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_reference_io_tests PRIVATE mvvcvtk_host_test_internal
    MVVCVTK::Host MVVCVTK::MetrologyAlignment opencv_world psapi ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(mvvcvtk_alignment_reference_io_tests)
set_target_properties(mvvcvtk_alignment_reference_io_tests PROPERTIES
    PROJECT_LABEL AlignmentReferenceIoTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME MetrologyAlignment.ReferenceIo COMMAND mvvcvtk_alignment_reference_io_tests
    "${CMAKE_CURRENT_BINARY_DIR}/reference-fixtures-$<CONFIG>")
SetTestRuntime(MetrologyAlignment.ReferenceIo mvvcvtk_alignment_reference_io_tests)
add_library(mvvcvtk_alignment_header_probe OBJECT AlignmentHeaderProbe.cpp)
target_link_libraries(mvvcvtk_alignment_header_probe PRIVATE MVVCVTK::MetrologyAlignment)
SetTargetDefaults(mvvcvtk_alignment_header_probe)
set_target_properties(mvvcvtk_alignment_header_probe PROPERTIES PROJECT_LABEL AlignmentHeaderProbe FOLDER "MVVCVTK/test/Automation/HeaderProbes")
add_executable(mvvcvtk_alignment_lifecycle_tests AlignmentLifecycleTests.cpp AlignmentPrealignTestSupport.h)
target_include_directories(mvvcvtk_alignment_lifecycle_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_lifecycle_tests PRIVATE mvvcvtk_host_test_internal
    MVVCVTK::Host MVVCVTK::MetrologyAlignment MVVCVTKInternal::FeatureSupport opencv_world ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(mvvcvtk_alignment_lifecycle_tests)
set_target_properties(mvvcvtk_alignment_lifecycle_tests PROPERTIES PROJECT_LABEL AlignmentLifecycleTests FOLDER "MVVCVTK/test/Automation")
vtk_module_autoinit(TARGETS mvvcvtk_alignment_lifecycle_tests MODULES ${MVVCVTK_VTK_TARGETS})
add_test(NAME MetrologyAlignment.Lifecycle COMMAND mvvcvtk_alignment_lifecycle_tests)
SetTestRuntime(MetrologyAlignment.Lifecycle mvvcvtk_alignment_lifecycle_tests)
add_executable(mvvcvtk_alignment_reference_tests AlignmentReferenceTests.cpp)
target_include_directories(mvvcvtk_alignment_reference_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_reference_tests PRIVATE MVVCVTK::MetrologyAlignment opencv_world)
SetTargetDefaults(mvvcvtk_alignment_reference_tests)
set_target_properties(mvvcvtk_alignment_reference_tests PROPERTIES PROJECT_LABEL AlignmentReferenceTests FOLDER "MVVCVTK/test/Automation/Reference")
set(MVVCVTK_ALIGNMENT_REFERENCE_ROOT "" CACHE PATH "Optional external NIST practice data for repository tests")
if(MVVCVTK_ALIGNMENT_REFERENCE_ROOT)
    add_test(NAME MetrologyAlignment.NistPractice COMMAND mvvcvtk_alignment_reference_tests "${MVVCVTK_ALIGNMENT_REFERENCE_ROOT}")
    SetTestRuntime(MetrologyAlignment.NistPractice mvvcvtk_alignment_reference_tests)
endif()

add_executable(mvvcvtk_alignment_deviation_tests AlignmentDeviationTests.cpp)
target_include_directories(mvvcvtk_alignment_deviation_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_deviation_tests PRIVATE MVVCVTK::MetrologyAlignment opencv_world)
SetTargetDefaults(mvvcvtk_alignment_deviation_tests)
set_target_properties(mvvcvtk_alignment_deviation_tests PROPERTIES
    PROJECT_LABEL AlignmentDeviationTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME MetrologyAlignment.Deviation COMMAND mvvcvtk_alignment_deviation_tests)
SetTestRuntime(MetrologyAlignment.Deviation mvvcvtk_alignment_deviation_tests)

add_executable(mvvcvtk_alignment_deviation_display_tests AlignmentDeviationDisplayTests.cpp)
target_include_directories(mvvcvtk_alignment_deviation_display_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_deviation_display_tests PRIVATE MVVCVTK::MetrologyAlignment
    MVVCVTKInternal::FeatureSupport ${MVVCVTK_VTK_TARGETS} VTK::RenderingAnnotation VTK::IOImage)
SetTargetDefaults(mvvcvtk_alignment_deviation_display_tests)
set_target_properties(mvvcvtk_alignment_deviation_display_tests PROPERTIES
    PROJECT_LABEL AlignmentDeviationDisplayTests FOLDER "MVVCVTK/test/Automation")
vtk_module_autoinit(TARGETS mvvcvtk_alignment_deviation_display_tests MODULES ${MVVCVTK_VTK_TARGETS})
add_test(NAME MetrologyAlignment.DeviationDisplay COMMAND mvvcvtk_alignment_deviation_display_tests
    "${CMAKE_CURRENT_BINARY_DIR}/deviation-display-$<CONFIG>.png")
SetTestRuntime(MetrologyAlignment.DeviationDisplay mvvcvtk_alignment_deviation_display_tests)

add_executable(mvvcvtk_alignment_display_audit AlignmentDisplayAudit.cpp)
target_include_directories(mvvcvtk_alignment_display_audit PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_display_audit PRIVATE mvvcvtk_host_test_internal MVVCVTK::Host
    MVVCVTK::MetrologyAlignment MVVCVTKInternal::FeatureSupport opencv_world psapi
    ${MVVCVTK_VTK_TARGETS} VTK::RenderingAnnotation VTK::IOImage)
SetTargetDefaults(mvvcvtk_alignment_display_audit)
set_target_properties(mvvcvtk_alignment_display_audit PROPERTIES
    PROJECT_LABEL AlignmentDisplayAudit FOLDER "MVVCVTK/test/Automation/Reference")
vtk_module_autoinit(TARGETS mvvcvtk_alignment_display_audit MODULES ${MVVCVTK_VTK_TARGETS})

add_executable(mvvcvtk_alignment_inspection_tests AlignmentInspectionTests.cpp)
target_include_directories(mvvcvtk_alignment_inspection_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_inspection_tests PRIVATE MVVCVTK::MetrologyAlignment opencv_world)
SetTargetDefaults(mvvcvtk_alignment_inspection_tests)
set_target_properties(mvvcvtk_alignment_inspection_tests PROPERTIES
    PROJECT_LABEL AlignmentInspectionTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME MetrologyAlignment.Inspection COMMAND mvvcvtk_alignment_inspection_tests)
SetTestRuntime(MetrologyAlignment.Inspection mvvcvtk_alignment_inspection_tests)

add_executable(mvvcvtk_alignment_section_tests AlignmentSectionTests.cpp)
target_include_directories(mvvcvtk_alignment_section_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src")
target_link_libraries(mvvcvtk_alignment_section_tests PRIVATE MVVCVTK::MetrologyAlignment opencv_world)
SetTargetDefaults(mvvcvtk_alignment_section_tests)
set_target_properties(mvvcvtk_alignment_section_tests PROPERTIES
    PROJECT_LABEL AlignmentSectionTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME MetrologyAlignment.Section COMMAND mvvcvtk_alignment_section_tests)
SetTestRuntime(MetrologyAlignment.Section mvvcvtk_alignment_section_tests)

if(MVVCVTK_BUILD_SURFACE_DETERMINATION)
    add_executable(mvvcvtk_metrology_timepoint_audit MetrologyTimepointAudit.cpp)
    target_link_libraries(mvvcvtk_metrology_timepoint_audit PRIVATE mvvcvtk_host_test_internal MVVCVTK::Host
        MVVCVTK::SurfaceDetermination MVVCVTK::MetrologyAlignment bcrypt psapi ${MVVCVTK_VTK_TARGETS} VTK::IOXML)
    SetTargetDefaults(mvvcvtk_metrology_timepoint_audit)
    set_target_properties(mvvcvtk_metrology_timepoint_audit PROPERTIES
        PROJECT_LABEL MetrologyTimepointAudit FOLDER "MVVCVTK/test/Automation/Reference")
    vtk_module_autoinit(TARGETS mvvcvtk_metrology_timepoint_audit MODULES ${MVVCVTK_VTK_TARGETS})
    set(MVVCVTK_METROLOGY_TIMEPOINT_MANIFEST "" CACHE FILEPATH "Optional frozen RAW ROI timepoint manifest for public pipeline checks")
    set(MVVCVTK_METROLOGY_REFERENCE_PYTHON "" CACHE FILEPATH "Optional Python 3.11+ with numpy/scipy for independent real-section verification; test-only")
    if(MVVCVTK_METROLOGY_TIMEPOINT_MANIFEST)
        if(MVVCVTK_METROLOGY_REFERENCE_PYTHON)
            add_test(NAME MetrologyAlignment.RealSurfaceControls COMMAND "${MVVCVTK_METROLOGY_REFERENCE_PYTHON}"
                "${CMAKE_CURRENT_SOURCE_DIR}/VerifySectionAudit.py" --audit "$<TARGET_FILE:mvvcvtk_metrology_timepoint_audit>"
                --manifest "${MVVCVTK_METROLOGY_TIMEPOINT_MANIFEST}" --output "${CMAKE_CURRENT_BINARY_DIR}/timepoint-runs-$<CONFIG>")
        else()
            add_test(NAME MetrologyAlignment.RealSurfaceControls COMMAND mvvcvtk_metrology_timepoint_audit
                "${MVVCVTK_METROLOGY_TIMEPOINT_MANIFEST}" "${CMAKE_CURRENT_BINARY_DIR}/timepoint-runs-$<CONFIG>" --controls-only --new-run)
        endif()
        SetTestRuntime(MetrologyAlignment.RealSurfaceControls mvvcvtk_metrology_timepoint_audit)
    endif()
endif()
