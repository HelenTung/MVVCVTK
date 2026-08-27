include_guard(GLOBAL)

include(MVVCVTKDependencyPolicy)

cmake_path(NORMAL_PATH MVVCVTK_DEPS_ROOT OUTPUT_VARIABLE _mvvcvtk_deps_root)
set(MVVCVTK_DEPS_ROOT "${_mvvcvtk_deps_root}")
LoadMVVCVTKDeps("${MVVCVTK_DEPS_ROOT}" "${MVVCVTK_DEPS_VERSION}")

foreach(_mvvcvtk_required_path IN ITEMS
    "vtk/lib/cmake/vtk-9.4/vtk-config.cmake"
    "opencv/x64/vc16/lib/OpenCVConfig.cmake"
    "qt/lib/cmake/Qt5/Qt5Config.cmake"
    "ui/include/uireconstruct3d.h"
    "ui/lib/UIPhantomCalib.lib"
    "ui/lib/UIReconstruct3D.lib"
)
    if(NOT EXISTS "${MVVCVTK_DEPS_ROOT}/${_mvvcvtk_required_path}")
        message(FATAL_ERROR
            "Locked dependency bundle is incomplete: ${_mvvcvtk_required_path}"
        )
    endif()
endforeach()

set(MVVCVTK_VTK_COMPONENTS
    CommonCore
    CommonDataModel
    CommonExecutionModel
    CommonMath
    CommonMisc
    CommonTransforms
    FiltersCore
    FiltersGeneral
    FiltersGeometry
    FiltersSources
    IOCore
    IOGeometry
    IOImage
    IOPLY
    ImagingCore
    ImagingGeneral
    ImagingStatistics
    InteractionStyle
    InteractionWidgets
    RenderingAnnotation
    RenderingCore
    RenderingFreeType
    RenderingImage
    RenderingLOD
    RenderingOpenGL2
    RenderingVolume
    RenderingVolumeOpenGL2
)
set(MVVCVTK_VTK_PUBLIC_TARGETS
    VTK::CommonCore
    VTK::CommonDataModel
    VTK::RenderingCore
)

set(_mvvcvtk_needs_qt FALSE)
if(MVVCVTK_BUILD_TESTING)
    set(_mvvcvtk_needs_qt TRUE)
    find_package(
        Qt5 5.14.2 EXACT
        CONFIG REQUIRED
        COMPONENTS Core Gui Widgets OpenGL
        PATHS "${MVVCVTK_DEPS_ROOT}/qt/lib/cmake/Qt5"
        NO_DEFAULT_PATH
    )
endif()

set(_mvvcvtk_vtk_components ${MVVCVTK_VTK_COMPONENTS})
if(_mvvcvtk_needs_qt)
    list(APPEND _mvvcvtk_vtk_components GUISupportQt)
endif()
find_package(
    VTK 9.4.2 EXACT
    CONFIG REQUIRED
    COMPONENTS ${_mvvcvtk_vtk_components}
    PATHS "${MVVCVTK_DEPS_ROOT}/vtk/lib/cmake/vtk-9.4"
    NO_DEFAULT_PATH
)
find_package(
    OpenCV 4.12.0 EXACT
    CONFIG REQUIRED
    PATHS "${MVVCVTK_DEPS_ROOT}/opencv/x64/vc16/lib"
    NO_DEFAULT_PATH
)

set(MVVCVTK_VTK_TARGETS)
foreach(_mvvcvtk_vtk_component IN LISTS MVVCVTK_VTK_COMPONENTS)
    list(APPEND MVVCVTK_VTK_TARGETS "VTK::${_mvvcvtk_vtk_component}")
endforeach()
set(MVVCVTK_QT_VTK_TARGETS
    ${MVVCVTK_VTK_TARGETS}
    VTK::GUISupportQt
)

if(NOT TARGET opencv_world)
    message(FATAL_ERROR "The locked OpenCV package does not define opencv_world.")
endif()
add_library(MVVCVTKInternal::OpenCVWorld INTERFACE IMPORTED GLOBAL)
set_target_properties(
    MVVCVTKInternal::OpenCVWorld
    PROPERTIES INTERFACE_LINK_LIBRARIES opencv_world
)

foreach(_mvvcvtk_ui_name IN ITEMS UIPhantomCalib UIReconstruct3D)
    add_library(MVVCVTKInternal::${_mvvcvtk_ui_name} UNKNOWN IMPORTED GLOBAL)
    set_target_properties(
        MVVCVTKInternal::${_mvvcvtk_ui_name}
        PROPERTIES
            IMPORTED_LOCATION
                "${MVVCVTK_DEPS_ROOT}/ui/lib/${_mvvcvtk_ui_name}.lib"
            INTERFACE_INCLUDE_DIRECTORIES
                "${MVVCVTK_DEPS_ROOT}/ui/include"
    )
endforeach()

set(MVVCVTK_RUNTIME_DIRS
    "${MVVCVTK_DEPS_ROOT}/ui/bin"
    "${MVVCVTK_DEPS_ROOT}/opencv/x64/vc16/bin"
    "${MVVCVTK_DEPS_ROOT}/vtk/bin"
    "${MVVCVTK_DEPS_ROOT}/qt/bin"
)
set(MVVCVTK_QT_PLUGIN_DIR "${MVVCVTK_DEPS_ROOT}/qt/plugins")

unset(_mvvcvtk_deps_root)
unset(_mvvcvtk_needs_qt)
unset(_mvvcvtk_required_path)
unset(_mvvcvtk_ui_name)
unset(_mvvcvtk_vtk_component)
unset(_mvvcvtk_vtk_components)
