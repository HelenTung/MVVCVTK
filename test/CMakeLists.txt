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
if(MVVCVTK_BUILD_PART_SEGMENTATION)
    add_subdirectory(PartSegmentation)
endif()
if(MVVCVTK_BUILD_SURFACE_DETERMINATION)
    add_subdirectory(SurfaceDetermination)
endif()
add_subdirectory(Interaction)
if(MVVCVTK_BUILD_QT_TESTING)
    add_subdirectory(QtHost)
endif()
add_subdirectory(SdkBoundary)

if(MVVCVTK_BUILD_GAP_ANALYSIS)
    # 所有子目录创建完成后检查完整目标图；DefX 只能由私有 kernel 直接消费。
    if(NOT TARGET DefX::Analysis)
        message(FATAL_ERROR "DefX::Analysis target is unavailable to GapAnalysis.")
    endif()

    foreach(_mvvcvtk_defx_property IN ITEMS
            IMPORTED_LOCATION_DEBUG
            IMPORTED_IMPLIB_DEBUG
            IMPORTED_LOCATION_RELEASE
            IMPORTED_IMPLIB_RELEASE
            INTERFACE_INCLUDE_DIRECTORIES)
        get_target_property(
            _mvvcvtk_defx_value
            DefX::Analysis
            ${_mvvcvtk_defx_property}
        )
        if(_mvvcvtk_defx_property STREQUAL "IMPORTED_LOCATION_DEBUG")
            set(_mvvcvtk_defx_expected
                "${MVVCVTK_DEFX_ROOT}/bin/Debug/DefXAnalysis.dll")
        elseif(_mvvcvtk_defx_property STREQUAL "IMPORTED_IMPLIB_DEBUG")
            set(_mvvcvtk_defx_expected
                "${MVVCVTK_DEFX_ROOT}/lib/Debug/DefXAnalysis.lib")
        elseif(_mvvcvtk_defx_property STREQUAL "IMPORTED_LOCATION_RELEASE")
            set(_mvvcvtk_defx_expected
                "${MVVCVTK_DEFX_ROOT}/bin/Release/DefXAnalysis.dll")
        elseif(_mvvcvtk_defx_property STREQUAL "IMPORTED_IMPLIB_RELEASE")
            set(_mvvcvtk_defx_expected
                "${MVVCVTK_DEFX_ROOT}/lib/Release/DefXAnalysis.lib")
        else()
            set(_mvvcvtk_defx_expected "${MVVCVTK_DEFX_ROOT}/include")
        endif()
        if(NOT _mvvcvtk_defx_value STREQUAL _mvvcvtk_defx_expected)
            message(FATAL_ERROR
                "DefX::Analysis has an invalid ${_mvvcvtk_defx_property}: "
                "${_mvvcvtk_defx_value}"
            )
        endif()
    endforeach()

    function(GetGapBuildTargets outputName sourceDirectory)
        get_property(
            directTargets
            DIRECTORY "${sourceDirectory}"
            PROPERTY BUILDSYSTEM_TARGETS
        )
        get_property(
            importedTargets
            DIRECTORY "${sourceDirectory}"
            PROPERTY IMPORTED_TARGETS
        )
        list(APPEND directTargets ${importedTargets})
        get_property(
            childDirectories
            DIRECTORY "${sourceDirectory}"
            PROPERTY SUBDIRECTORIES
        )
        foreach(childDirectory IN LISTS childDirectories)
            GetGapBuildTargets(childTargets "${childDirectory}")
            list(APPEND directTargets ${childTargets})
        endforeach()
        set(${outputName} "${directTargets}" PARENT_SCOPE)
    endfunction()

    GetGapBuildTargets(_mvvcvtk_build_targets "${PROJECT_SOURCE_DIR}")
    list(REMOVE_DUPLICATES _mvvcvtk_build_targets)
    set(_mvvcvtk_has_private_defx FALSE)
    foreach(_mvvcvtk_build_target IN LISTS _mvvcvtk_build_targets)
        if(NOT TARGET ${_mvvcvtk_build_target})
            continue()
        endif()
        if(_mvvcvtk_build_target STREQUAL "DefX::Analysis")
            continue()
        endif()
        foreach(_mvvcvtk_target_property IN ITEMS
                LINK_LIBRARIES
                INTERFACE_LINK_LIBRARIES
                INCLUDE_DIRECTORIES
                INTERFACE_INCLUDE_DIRECTORIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
            get_target_property(
                _mvvcvtk_target_value
                ${_mvvcvtk_build_target}
                ${_mvvcvtk_target_property}
            )
            string(FIND
                "${_mvvcvtk_target_value}"
                "DefX::Analysis"
                _mvvcvtk_defx_index
            )
            if(NOT _mvvcvtk_defx_index EQUAL -1)
                if(_mvvcvtk_build_target STREQUAL "mvvcvtk_gap_kernel"
                    AND _mvvcvtk_target_property STREQUAL "LINK_LIBRARIES")
                    set(_mvvcvtk_has_private_defx TRUE)
                else()
                    message(FATAL_ERROR
                        "${_mvvcvtk_build_target} exposes or consumes DefX through "
                        "${_mvvcvtk_target_property}."
                    )
                endif()
            endif()
            string(FIND
                "${_mvvcvtk_target_value}"
                "${MVVCVTK_DEFX_ROOT}"
                _mvvcvtk_defx_index
            )
            if(NOT _mvvcvtk_defx_index EQUAL -1)
                message(FATAL_ERROR
                    "${_mvvcvtk_build_target} bypasses DefX::Analysis through "
                    "${_mvvcvtk_target_property}."
                )
            endif()
        endforeach()
    endforeach()
    if(NOT _mvvcvtk_has_private_defx)
        message(FATAL_ERROR
            "mvvcvtk_gap_kernel must link DefX::Analysis directly and privately."
        )
    endif()

    unset(_mvvcvtk_build_target)
    unset(_mvvcvtk_build_targets)
    unset(_mvvcvtk_defx_expected)
    unset(_mvvcvtk_defx_index)
    unset(_mvvcvtk_defx_property)
    unset(_mvvcvtk_defx_value)
    unset(_mvvcvtk_has_private_defx)
    unset(_mvvcvtk_target_property)
    unset(_mvvcvtk_target_value)
endif()
