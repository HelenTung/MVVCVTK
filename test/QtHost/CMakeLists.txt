add_executable(
    mvvcvtk_host_method_tests
    QtHostCropTests.cpp
    QtHostExportTests.cpp
    QtHostGapTests.cpp
    QtHostLifecycleTests.cpp
    QtHostLoadTests.cpp
    QtHostMethodCases.h
    QtHostMethodTests.cpp
    QtHostViewTests.cpp
)

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
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/App"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/App/Services"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/App/Tasks"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Data"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Geometry"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Host"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Interaction"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Platform"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Render"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Render/Strategies"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Render/Support"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include/Algorithms"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/GapAnalysis/include/Services"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Algorithms"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Interaction"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Render"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Routing"
)

target_link_libraries(
    mvvcvtk_host_method_tests
    PRIVATE
        MVVCVTK::SDK
        ${MVVCVTK_VTK_TARGETS}
)

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

target_include_directories(
    mvvcvtk_qt_session_smoke
    PRIVATE
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/App"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Data"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Host"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Interaction"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Render"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Render/Strategies"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include/Render/Support"
)

target_link_libraries(
    mvvcvtk_qt_session_smoke
    PRIVATE
        MVVCVTK::Host
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
