include_guard(GLOBAL)

option(MVVCVTK_BUILD_TESTING "Build MVVCVTK tests" ON)
option(MVVCVTK_BUILD_ORTHOGONAL_CROP "Build OrthogonalCrop feature" ON)
option(MVVCVTK_BUILD_GAP_ANALYSIS "Build GapAnalysis feature" ON)
option(MVVCVTK_BUILD_STANDALONE "Build the full standalone example" ON)
option(MVVCVTK_ENABLE_AVX2 "Enable AVX2 for Release builds" ON)

set(
    MVVCVTK_DEPS_VERSION
    "2026.08.21-deps.1"
    CACHE STRING
    "Locked MVVCVTK dependency bundle version"
)

if(DEFINED ENV{MVVCVTK_DEPS_ROOT}
    AND NOT "$ENV{MVVCVTK_DEPS_ROOT}" STREQUAL "")
    set(_mvvcvtk_default_deps "$ENV{MVVCVTK_DEPS_ROOT}")
else()
    get_filename_component(
        _mvvcvtk_repo_root
        "${CMAKE_CURRENT_LIST_DIR}/.."
        ABSOLUTE
    )
    set(
        _mvvcvtk_default_deps
        "${_mvvcvtk_repo_root}/deps/${MVVCVTK_DEPS_VERSION}-win-x64"
    )
endif()

set(
    MVVCVTK_DEPS_ROOT
    "${_mvvcvtk_default_deps}"
    CACHE PATH
    "Root of the locked MVVCVTK dependency bundle"
)
unset(_mvvcvtk_default_deps)
unset(_mvvcvtk_repo_root)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(
    CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)

if(CMAKE_CONFIGURATION_TYPES)
    set(
        CMAKE_CONFIGURATION_TYPES
        "Debug;Release"
        CACHE STRING
        "Supported MVVCVTK configurations"
        FORCE
    )
endif()

if(NOT WIN32)
    message(FATAL_ERROR "MVVCVTK currently supports Windows only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MVVCVTK currently supports x64 only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "MVVCVTK currently requires the MSVC toolchain.")
endif()
if(NOT MSVC_TOOLSET_VERSION STREQUAL "145")
    message(FATAL_ERROR
        "MVVCVTK requires MSVC v145; detected toolset ${MSVC_TOOLSET_VERSION}."
    )
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Visual Studio 18 2026")
    message(FATAL_ERROR
        "MVVCVTK requires the Visual Studio 18 2026 generator."
    )
endif()

function(SetTargetDefaults target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_definitions(${target} PRIVATE VTK_DEPRECATION_LEVEL=0)
    target_compile_options(
        ${target}
        PRIVATE
            /W3
            /utf-8
            /permissive-
            /sdl
            "$<$<CONFIG:Release>:/Gy>"
            "$<$<CONFIG:Release>:/Oi>"
            "$<$<AND:$<CONFIG:Release>,$<BOOL:${MVVCVTK_ENABLE_AVX2}>>:/arch:AVX2>"
    )
    set_target_properties(
        ${target}
        PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION FALSE
            MSVC_DEBUG_INFORMATION_FORMAT Embedded
            MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endfunction()

function(SetQtTargetDefaults target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_definitions(${target} PRIVATE VTK_DEPRECATION_LEVEL=0)
    target_compile_options(
        ${target}
        PRIVATE
            /W3
            /utf-8
            /sdl
    )
    set_target_properties(
        ${target}
        PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION FALSE
            MSVC_DEBUG_INFORMATION_FORMAT Embedded
            MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endfunction()

function(SetApiDefaults target)
    target_compile_features(${target} INTERFACE cxx_std_17)
    target_compile_definitions(
        ${target}
        INTERFACE VTK_DEPRECATION_LEVEL=0
    )
    target_compile_options(
        ${target}
        INTERFACE
            /utf-8
    )
endfunction()
