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
