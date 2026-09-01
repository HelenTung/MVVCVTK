include_guard(GLOBAL)

cmake_path(NORMAL_PATH MVVCVTK_DEPS_ROOT OUTPUT_VARIABLE _mvvcvtk_deps_root)
set(MVVCVTK_DEPS_ROOT "${_mvvcvtk_deps_root}")

if(DEFINED ENV{MVVCVTK_DEFX_ROOT}
    AND NOT "$ENV{MVVCVTK_DEFX_ROOT}" STREQUAL "")
    set(_mvvcvtk_defx_default_root "$ENV{MVVCVTK_DEFX_ROOT}")
else()
    set(_mvvcvtk_defx_default_root
        "${PROJECT_SOURCE_DIR}/deps/third_party/defx")
endif()
set(
    MVVCVTK_DEFX_ROOT
    "${_mvvcvtk_defx_default_root}"
    CACHE PATH
    "Root of the external DefX x64 dependency"
)
cmake_path(
    NORMAL_PATH MVVCVTK_DEFX_ROOT
    OUTPUT_VARIABLE _mvvcvtk_defx_root
)
set(MVVCVTK_DEFX_ROOT "${_mvvcvtk_defx_root}")

set(_mvvcvtk_required_paths
    "vtk/lib/cmake/vtk-9.4/vtk-config.cmake"
    "opencv/x64/vc16/lib/OpenCVConfig.cmake"
)
if(MVVCVTK_BUILD_QT_TESTING)
    list(APPEND _mvvcvtk_required_paths
        "qt/lib/cmake/Qt5/Qt5Config.cmake"
    )
endif()
foreach(_mvvcvtk_required_path IN LISTS _mvvcvtk_required_paths)
    if(NOT EXISTS "${MVVCVTK_DEPS_ROOT}/${_mvvcvtk_required_path}")
        message(FATAL_ERROR
            "Locked dependency bundle is incomplete: ${_mvvcvtk_required_path}"
        )
    endif()
endforeach()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    set(_mvvcvtk_defx_paths
        "include/DefXAnalysisService.h"
        "include/DefXTypes.h"
        "bin/Debug/DefXAnalysis.dll"
        "lib/Debug/DefXAnalysis.lib"
        "bin/Release/DefXAnalysis.dll"
        "lib/Release/DefXAnalysis.lib"
    )
    foreach(_mvvcvtk_defx_path IN LISTS _mvvcvtk_defx_paths)
        if(NOT EXISTS "${MVVCVTK_DEFX_ROOT}/${_mvvcvtk_defx_path}")
            message(FATAL_ERROR
                "DefX dependency is incomplete: ${_mvvcvtk_defx_path}. "
                "Set MVVCVTK_DEFX_ROOT to the external x64 dependency root."
            )
        endif()
    endforeach()
endif()

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
if(MVVCVTK_BUILD_QT_TESTING)
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

if(MVVCVTK_BUILD_GAP_ANALYSIS)
    add_library(DefX::Analysis SHARED IMPORTED GLOBAL)
    set_target_properties(
        DefX::Analysis
        PROPERTIES
            IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
            IMPORTED_LOCATION_DEBUG
                "${MVVCVTK_DEFX_ROOT}/bin/Debug/DefXAnalysis.dll"
            IMPORTED_IMPLIB_DEBUG
                "${MVVCVTK_DEFX_ROOT}/lib/Debug/DefXAnalysis.lib"
            IMPORTED_LOCATION_RELEASE
                "${MVVCVTK_DEFX_ROOT}/bin/Release/DefXAnalysis.dll"
            IMPORTED_IMPLIB_RELEASE
                "${MVVCVTK_DEFX_ROOT}/lib/Release/DefXAnalysis.lib"
            INTERFACE_INCLUDE_DIRECTORIES
                "${MVVCVTK_DEFX_ROOT}/include"
    )
endif()
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
set(MVVCVTK_QT_VTK_TARGETS)
if(_mvvcvtk_needs_qt)
    set(MVVCVTK_QT_VTK_TARGETS
        ${MVVCVTK_VTK_TARGETS}
        VTK::GUISupportQt
    )
endif()

if(NOT TARGET opencv_world)
    message(FATAL_ERROR "The locked OpenCV package does not define opencv_world.")
endif()

set(MVVCVTK_RUNTIME_DIRS
    "${MVVCVTK_DEPS_ROOT}/opencv/x64/vc16/bin"
    "${MVVCVTK_DEPS_ROOT}/vtk/bin"
)
set(MVVCVTK_QT_PLUGIN_DIR "")
if(_mvvcvtk_needs_qt)
    list(APPEND MVVCVTK_RUNTIME_DIRS
        "${MVVCVTK_DEPS_ROOT}/qt/bin"
    )
    set(MVVCVTK_QT_PLUGIN_DIR "${MVVCVTK_DEPS_ROOT}/qt/plugins")
endif()

unset(_mvvcvtk_deps_root)
unset(_mvvcvtk_defx_default_root)
unset(_mvvcvtk_defx_root)
unset(_mvvcvtk_defx_path)
unset(_mvvcvtk_defx_paths)
unset(_mvvcvtk_needs_qt)
unset(_mvvcvtk_required_path)
unset(_mvvcvtk_required_paths)
unset(_mvvcvtk_vtk_component)
unset(_mvvcvtk_vtk_components)
