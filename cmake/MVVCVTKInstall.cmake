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
        "${CMAKE_CURRENT_BINARY_DIR}/MVVCVTKInternalDependencies.cmake"
    DESTINATION "${MVVCVTK_CMAKE_INSTALL_DIR}"
    COMPONENT Host
)
