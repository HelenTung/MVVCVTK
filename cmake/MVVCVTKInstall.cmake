include_guard(GLOBAL)

set(
    MVVCVTK_CMAKE_INSTALL_DIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/MVVCVTK"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MVVCVTKConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MVVCVTKConfig.cmake"
    INSTALL_DESTINATION "${MVVCVTK_CMAKE_INSTALL_DIR}"
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MVVCVTKInternalDependencies.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MVVCVTKInternalDependencies.cmake"
    @ONLY
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MVVCVTKConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY ExactVersion
)

install(
    EXPORT MVVCVTKTargets
    FILE MVVCVTKTargets.cmake
    NAMESPACE MVVCVTK::
    DESTINATION "${MVVCVTK_CMAKE_INSTALL_DIR}"
    COMPONENT Host
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/MVVCVTKConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/MVVCVTKConfigVersion.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/MVVCVTKInternalDependencies.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MVVCVTKDependencyPolicy.cmake"
    DESTINATION "${MVVCVTK_CMAKE_INSTALL_DIR}"
    COMPONENT Host
)

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/msbuild/"
    DESTINATION "msbuild"
    COMPONENT Host
    FILES_MATCHING PATTERN "*.props"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/qmake/"
    DESTINATION "qmake"
    COMPONENT Host
    FILES_MATCHING PATTERN "*.pri"
)
install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/Confirm-MVVCVTKSdkIdentity.ps1"
    DESTINATION "tools"
    COMPONENT Host
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/consumer/"
    DESTINATION "examples/CMake"
    COMPONENT Host
    FILES_MATCHING
        PATTERN "CMakeLists.txt"
        PATTERN "*.cpp"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/qt-consumer/"
    DESTINATION "examples/QtCleanRoom"
    COMPONENT Host
    FILES_MATCHING
        PATTERN "CMakeLists.txt"
        PATTERN "*.cpp"
)
install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/msbuild-consumer/MVVCVTKSdkConsumer.vcxproj"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/consumer/main.cpp"
    DESTINATION "examples/MSBuild"
    COMPONENT Host
)
install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/qmake-consumer/MVVCVTKSdkConsumer.pro"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/consumer/main.cpp"
    DESTINATION "examples/Qmake"
    COMPONENT Host
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    install(
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        DESTINATION "."
        COMPONENT Host
    )
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE")
    install(
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
        DESTINATION "."
        COMPONENT Host
    )
endif()
