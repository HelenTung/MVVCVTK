set(MVVCVTK_HOST_METHOD_SOURCES
    QtHostExportTests.cpp
    QtHostLifecycleTests.cpp
    QtHostLabelMapTests.cpp
    QtHostLoadTests.cpp
    QtHostMethodCases.h
    QtHostMethodTests.cpp
    QtHostRenderProductTests.cpp
    QtHostViewTests.cpp
    RenderProductTestSupport.h
)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    list(APPEND MVVCVTK_HOST_METHOD_SOURCES QtHostCropTests.cpp)
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    list(APPEND MVVCVTK_HOST_METHOD_SOURCES QtHostGapTests.cpp)
endif()
if(MVVCVTK_BUILD_PART_SEGMENTATION)
    list(APPEND MVVCVTK_HOST_METHOD_SOURCES QtHostPartSceneTests.cpp)
endif()

add_executable(
    mvvcvtk_host_method_tests
    ${MVVCVTK_HOST_METHOD_SOURCES}
)
unset(MVVCVTK_HOST_METHOD_SOURCES)

set_target_properties(
    mvvcvtk_host_method_tests
    PROPERTIES
        OUTPUT_NAME QtHostMethodTests
        PROJECT_LABEL HostMethodTests
        FOLDER "MVVCVTK/Tests"
)

target_include_directories(
    mvvcvtk_host_method_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    target_include_directories(
        mvvcvtk_host_method_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Algorithms"
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Interaction"
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Render"
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Routing"
    )
    target_compile_definitions(
        mvvcvtk_host_method_tests
        PRIVATE MVVCVTK_HAS_ORTHOGONAL_CROP=1
    )
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    target_include_directories(
        mvvcvtk_host_method_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include/Algorithms"
            "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include/Services"
    )
    target_compile_definitions(
        mvvcvtk_host_method_tests
        PRIVATE MVVCVTK_HAS_GAP_ANALYSIS=1
    )
endif()
if(MVVCVTK_BUILD_PART_SEGMENTATION)
    target_compile_definitions(
        mvvcvtk_host_method_tests
        PRIVATE MVVCVTK_HAS_PART_SEGMENTATION=1
    )
endif()

target_link_libraries(
    mvvcvtk_host_method_tests
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::Host
        MVVCVTKInternal::FeatureSupport
        ${MVVCVTK_VTK_TARGETS}
)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    target_link_libraries(
        mvvcvtk_host_method_tests
        PRIVATE MVVCVTK::OrthogonalCrop
    )
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    target_link_libraries(
        mvvcvtk_host_method_tests
        PRIVATE MVVCVTK::GapAnalysis
    )
endif()
if(MVVCVTK_BUILD_PART_SEGMENTATION)
    target_link_libraries(
        mvvcvtk_host_method_tests
        PRIVATE MVVCVTK::PartSegmentation
    )
endif()

SetTargetDefaults(mvvcvtk_host_method_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_host_method_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

add_test(
    NAME QtHost.Methods
    COMMAND mvvcvtk_host_method_tests
)
SetTestRuntime(QtHost.Methods mvvcvtk_host_method_tests)
if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    add_test(NAME OrthogonalCrop.Lifecycle COMMAND mvvcvtk_host_method_tests --case crop-lifecycle)
    SetTestRuntime(OrthogonalCrop.Lifecycle mvvcvtk_host_method_tests)
endif()

add_executable(
    mvvcvtk_qt_host_smoke
    QtHostSmoke.cpp
)

set_target_properties(
    mvvcvtk_qt_host_smoke
    PROPERTIES
        OUTPUT_NAME QtHostSmoke
        PROJECT_LABEL QtHostSmoke
        FOLDER "MVVCVTK/Tests"
)

target_link_libraries(
    mvvcvtk_qt_host_smoke
    PRIVATE
        MVVCVTK::Host
        MVVCVTK::FeatureSPI
        Qt5::Core
        Qt5::Gui
        Qt5::Widgets
        Qt5::OpenGL
        VTK::GUISupportQt
        ${MVVCVTK_VTK_TARGETS}
)

SetQtTargetDefaults(mvvcvtk_qt_host_smoke)
vtk_module_autoinit(
    TARGETS mvvcvtk_qt_host_smoke
    MODULES ${MVVCVTK_QT_VTK_TARGETS}
)

add_test(
    NAME QtHost.Smoke
    COMMAND mvvcvtk_qt_host_smoke
)
SetTestRuntime(QtHost.Smoke mvvcvtk_qt_host_smoke)

add_executable(mvvcvtk_qt_host_scheduling_tests QtHostSchedulingTests.cpp)
target_link_libraries(mvvcvtk_qt_host_scheduling_tests PRIVATE
    MVVCVTK::Host MVVCVTK::FeatureSPI
    Qt5::Core Qt5::Gui Qt5::Widgets Qt5::OpenGL
    VTK::GUISupportQt ${MVVCVTK_VTK_TARGETS})
SetQtTargetDefaults(mvvcvtk_qt_host_scheduling_tests)
set_target_properties(mvvcvtk_qt_host_scheduling_tests PROPERTIES
    FOLDER "MVVCVTK/Tests")
vtk_module_autoinit(TARGETS mvvcvtk_qt_host_scheduling_tests
    MODULES ${MVVCVTK_QT_VTK_TARGETS})
add_test(NAME QtHost.Scheduling COMMAND mvvcvtk_qt_host_scheduling_tests)
SetTestRuntime(QtHost.Scheduling mvvcvtk_qt_host_scheduling_tests)
foreach(feature_spec IN ITEMS "GAP;GapAnalysis" "CROP;OrthogonalCrop" "PART;PartSegmentation" "SURFACE;SurfaceDetermination")
    list(GET feature_spec 0 test_key)
    list(GET feature_spec 1 feature_target)
    if(TARGET MVVCVTK::${feature_target})
        target_link_libraries(mvvcvtk_qt_host_scheduling_tests PRIVATE MVVCVTK::${feature_target})
        target_compile_definitions(mvvcvtk_qt_host_scheduling_tests PRIVATE MVVCVTK_TEST_${test_key})
    endif()
endforeach()

if(MVVCVTK_BUILD_GAP_ANALYSIS)
    add_executable(
        mvvcvtk_qt_gap_smoke
        QtHostGapSmoke.cpp
    )

    set_target_properties(
        mvvcvtk_qt_gap_smoke
        PROPERTIES
            OUTPUT_NAME QtHostGapSmoke
            PROJECT_LABEL QtGapSmoke
            FOLDER "MVVCVTK/Tests"
    )

    target_link_libraries(
        mvvcvtk_qt_gap_smoke
        PRIVATE
            MVVCVTK::Host
            MVVCVTK::GapAnalysis
            Qt5::Core
            Qt5::Gui
            Qt5::Widgets
            Qt5::OpenGL
            VTK::GUISupportQt
            ${MVVCVTK_VTK_TARGETS}
    )

    SetQtTargetDefaults(mvvcvtk_qt_gap_smoke)
    vtk_module_autoinit(
        TARGETS mvvcvtk_qt_gap_smoke
        MODULES ${MVVCVTK_QT_VTK_TARGETS}
    )

    add_test(
        NAME QtHost.GapSmoke
        COMMAND mvvcvtk_qt_gap_smoke
    )
    SetTestRuntime(QtHost.GapSmoke mvvcvtk_qt_gap_smoke)
    set_tests_properties(QtHost.GapSmoke PROPERTIES TIMEOUT 45)
endif()

add_executable(
    mvvcvtk_qt_session_smoke
    QtHostSessionSmoke.cpp
)

set_target_properties(
    mvvcvtk_qt_session_smoke
    PROPERTIES
        OUTPUT_NAME QtHostSessionSmoke
        PROJECT_LABEL QtSessionSmoke
        FOLDER "MVVCVTK/Tests"
)

target_link_libraries(
    mvvcvtk_qt_session_smoke
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::Host
        MVVCVTKInternal::FeatureSupport
        Qt5::Core
        Qt5::Gui
        Qt5::Widgets
        Qt5::OpenGL
        VTK::GUISupportQt
        ${MVVCVTK_VTK_TARGETS}
)

SetQtTargetDefaults(mvvcvtk_qt_session_smoke)
vtk_module_autoinit(
    TARGETS mvvcvtk_qt_session_smoke
    MODULES ${MVVCVTK_QT_VTK_TARGETS}
)

add_test(
    NAME QtHost.SessionSmoke
    COMMAND mvvcvtk_qt_session_smoke
)
SetTestRuntime(QtHost.SessionSmoke mvvcvtk_qt_session_smoke)
