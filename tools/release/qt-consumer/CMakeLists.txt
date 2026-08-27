cmake_minimum_required(VERSION 4.2)

project(MVVCVTKQtCleanRoom LANGUAGES CXX)

find_package(
    Qt5 5.14.2 EXACT
    CONFIG REQUIRED
    COMPONENTS Core Gui Widgets OpenGL
)
find_package(
    MVVCVTK
    CONFIG REQUIRED
    COMPONENTS Host
)
find_package(
    VTK 9.4.2 EXACT
    CONFIG REQUIRED
    COMPONENTS GUISupportQt
)

add_executable(MVVCVTKQtCleanRoom main.cpp)
target_compile_features(MVVCVTKQtCleanRoom PRIVATE cxx_std_17)
target_compile_options(MVVCVTKQtCleanRoom PRIVATE /W3 /utf-8 /sdl)
set_target_properties(
    MVVCVTKQtCleanRoom
    PROPERTIES
        MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
target_link_libraries(
    MVVCVTKQtCleanRoom
    PRIVATE
        MVVCVTK::Host
        Qt5::Core
        Qt5::Gui
        Qt5::Widgets
        Qt5::OpenGL
        VTK::GUISupportQt
        VTK::RenderingOpenGL2
        VTK::InteractionStyle
        VTK::RenderingVolumeOpenGL2
        VTK::RenderingFreeType
)
vtk_module_autoinit(
    TARGETS MVVCVTKQtCleanRoom
    MODULES
        VTK::GUISupportQt
        VTK::RenderingOpenGL2
        VTK::InteractionStyle
        VTK::RenderingVolumeOpenGL2
        VTK::RenderingFreeType
)
