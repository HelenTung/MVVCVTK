function(SetTestRuntime testName target)
    set(_runtime_changes)
    foreach(_runtime_dir IN LISTS MVVCVTK_RUNTIME_DIRS)
        list(APPEND _runtime_changes
            "PATH=path_list_prepend:${_runtime_dir}"
        )
    endforeach()
    if(MVVCVTK_QT_PLUGIN_DIR)
        list(APPEND _runtime_changes
            "QT_PLUGIN_PATH=set:${MVVCVTK_QT_PLUGIN_DIR}"
        )
    endif()
    set_tests_properties(
        ${testName}
        PROPERTIES
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
            ENVIRONMENT_MODIFICATION "${_runtime_changes}"
    )
endfunction()

get_property(
    MVVCVTK_HOST_TEST_INCLUDE_DIRS
    GLOBAL PROPERTY MVVCVTK_HOST_PRIVATE_INCLUDE_DIRS
)
if(NOT MVVCVTK_HOST_TEST_INCLUDE_DIRS)
    message(FATAL_ERROR "Host private include surface is unavailable to tests.")
endif()

# 唯一允许访问 Host 私有头的仓内接口；不安装、不导出，也不传播给产品 target。
add_library(mvvcvtk_host_test_internal INTERFACE)
target_include_directories(
    mvvcvtk_host_test_internal
    BEFORE
    INTERFACE
        "${PROJECT_SOURCE_DIR}/MVVCVTK/include"
        ${MVVCVTK_HOST_TEST_INCLUDE_DIRS}
)
target_compile_features(mvvcvtk_host_test_internal INTERFACE cxx_std_17)
set_target_properties(
    mvvcvtk_host_test_internal
    PROPERTIES FOLDER "MVVCVTK/Tests/Internal"
)
unset(MVVCVTK_HOST_TEST_INCLUDE_DIRS)

if(MVVCVTK_BUILD_ORTHOGONAL_CROP)
    add_subdirectory(OrthogonalCrop)
endif()
if(MVVCVTK_BUILD_GAP_ANALYSIS)
    add_subdirectory(GapAnalysis)
endif()
add_subdirectory(Interaction)
if(MVVCVTK_BUILD_QT_TESTING)
    add_subdirectory(QtHost)
endif()
add_subdirectory(SdkBoundary)
