cmake_minimum_required(VERSION 4.2)

cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0141 NEW)

project(
    MVVCVTK
    VERSION 1.0.0
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
)

include(MVVCVTKBuildPolicy)
include(MVVCVTKDependencies)

if(MVVCVTK_ENABLE_AVX2)
    set(MVVCVTK_RELEASE_INSTRUCTION_SET "AVX2")
else()
    set(MVVCVTK_RELEASE_INSTRUCTION_SET "Default")
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MVVCVTKBuildInfo.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MVVCVTKBuildInfo.json"
    @ONLY
)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "MVVCVTK/CMake")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(MVVCVTK)
add_subdirectory(examples/standalone)
set_property(
    DIRECTORY
    PROPERTY VS_STARTUP_PROJECT MVVCVTKStandalone
)

if(MVVCVTK_BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()

include(MVVCVTKInstall)
