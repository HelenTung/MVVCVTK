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

add_subdirectory(OrthogonalCrop)
add_subdirectory(GapAnalysis)
add_subdirectory(Interaction)
add_subdirectory(QtHost)
