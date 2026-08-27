include_guard(GLOBAL)

function(GetMVVCVTKJson outputValue jsonValue fieldLabel)
    string(
        JSON _mvvcvtk_json_value
        ERROR_VARIABLE _mvvcvtk_json_error
        GET "${jsonValue}" ${ARGN}
    )
    if(_mvvcvtk_json_error)
        message(FATAL_ERROR
            "Cannot read ${fieldLabel} from an MVVCVTK manifest: "
            "${_mvvcvtk_json_error}"
        )
    endif()
    set(${outputValue} "${_mvvcvtk_json_value}" PARENT_SCOPE)
endfunction()

function(GetMVVCVTKJsonType outputValue jsonValue fieldLabel)
    string(
        JSON _mvvcvtk_json_type
        ERROR_VARIABLE _mvvcvtk_json_error
        TYPE "${jsonValue}" ${ARGN}
    )
    if(_mvvcvtk_json_error)
        message(FATAL_ERROR
            "Cannot read the type of ${fieldLabel} from an MVVCVTK manifest: "
            "${_mvvcvtk_json_error}"
        )
    endif()
    set(${outputValue} "${_mvvcvtk_json_type}" PARENT_SCOPE)
endfunction()

function(GetMVVCVTKTypedJson
    outputValue jsonValue fieldLabel expectedType)
    GetMVVCVTKJsonType(
        _mvvcvtk_json_type
        "${jsonValue}"
        "${fieldLabel}"
        ${ARGN}
    )
    if(NOT _mvvcvtk_json_type STREQUAL expectedType)
        message(FATAL_ERROR
            "${fieldLabel} has JSON type '${_mvvcvtk_json_type}'; "
            "expected '${expectedType}'."
        )
    endif()
    GetMVVCVTKJson(
        _mvvcvtk_json_value
        "${jsonValue}"
        "${fieldLabel}"
        ${ARGN}
    )
    set(${outputValue} "${_mvvcvtk_json_value}" PARENT_SCOPE)
endfunction()

function(LoadMVVCVTKDeps depsRoot expectedVersion)
    set(_mvvcvtk_manifest "${depsRoot}/manifest.json")
    if(NOT EXISTS "${_mvvcvtk_manifest}")
        message(FATAL_ERROR
            "The MVVCVTK dependency manifest is missing from '${depsRoot}'."
        )
    endif()

    file(READ "${_mvvcvtk_manifest}" _mvvcvtk_json)
    GetMVVCVTKJsonType(_mvvcvtk_root_type "${_mvvcvtk_json}"
        "dependency manifest root")
    if(NOT _mvvcvtk_root_type STREQUAL "OBJECT")
        message(FATAL_ERROR "MVVCVTK dependency manifest root must be an object.")
    endif()
    GetMVVCVTKTypedJson(_mvvcvtk_schema "${_mvvcvtk_json}"
        "dependency schemaVersion" NUMBER schemaVersion)
    GetMVVCVTKTypedJson(_mvvcvtk_id "${_mvvcvtk_json}"
        "dependency packageId" STRING packageId)
    GetMVVCVTKTypedJson(_mvvcvtk_version "${_mvvcvtk_json}"
        "dependency packageVersion" STRING packageVersion)
    GetMVVCVTKTypedJson(_mvvcvtk_platform "${_mvvcvtk_json}"
        "dependency platform" STRING platform)
    GetMVVCVTKTypedJson(_mvvcvtk_arch "${_mvvcvtk_json}"
        "dependency architecture" STRING architecture)
    GetMVVCVTKTypedJson(_mvvcvtk_toolset "${_mvvcvtk_json}"
        "dependency platformToolset" STRING
        validatedConsumer platformToolset)
    GetMVVCVTKTypedJson(_mvvcvtk_windows_sdk "${_mvvcvtk_json}"
        "dependency Windows SDK" STRING
        validatedConsumer windowsTargetPlatformVersion)

    if(NOT _mvvcvtk_schema EQUAL 1
        OR NOT _mvvcvtk_id STREQUAL "MVVCVTK.Dependencies"
        OR NOT _mvvcvtk_version STREQUAL expectedVersion
        OR NOT _mvvcvtk_platform STREQUAL "windows"
        OR NOT _mvvcvtk_arch STREQUAL "x64"
        OR NOT _mvvcvtk_toolset STREQUAL "v145"
        OR NOT _mvvcvtk_windows_sdk STREQUAL "10.0")
        message(FATAL_ERROR
            "MVVCVTK dependency manifest identity or toolchain mismatch: "
            "expected ${expectedVersion}/windows/x64/v145/Windows SDK 10.0."
        )
    endif()

    set(_mvvcvtk_configs)
    GetMVVCVTKJsonType(_mvvcvtk_configs_type "${_mvvcvtk_json}"
        "dependency configurations" configurations)
    if(NOT _mvvcvtk_configs_type STREQUAL "ARRAY")
        message(FATAL_ERROR
            "MVVCVTK dependency configurations must be an array."
        )
    endif()
    string(JSON _mvvcvtk_config_count LENGTH
        "${_mvvcvtk_json}" configurations)
    if(NOT _mvvcvtk_config_count EQUAL 2)
        message(FATAL_ERROR
            "MVVCVTK dependency configurations must be exactly Debug and Release."
        )
    endif()
    if(_mvvcvtk_config_count GREATER 0)
        math(EXPR _mvvcvtk_config_last "${_mvvcvtk_config_count} - 1")
        foreach(_mvvcvtk_index RANGE 0 ${_mvvcvtk_config_last})
            GetMVVCVTKTypedJson(_mvvcvtk_config "${_mvvcvtk_json}"
                "dependency configuration" STRING
                configurations ${_mvvcvtk_index})
            if(_mvvcvtk_config IN_LIST _mvvcvtk_configs)
                message(FATAL_ERROR
                    "MVVCVTK dependency configuration '${_mvvcvtk_config}' is duplicated."
                )
            endif()
            list(APPEND _mvvcvtk_configs "${_mvvcvtk_config}")
        endforeach()
    endif()
    foreach(_mvvcvtk_required_config IN ITEMS Debug Release)
        if(NOT _mvvcvtk_required_config IN_LIST _mvvcvtk_configs)
            message(FATAL_ERROR
                "MVVCVTK dependency bundle lacks ${_mvvcvtk_required_config}."
            )
        endif()
    endforeach()

    set(_mvvcvtk_expected_ids vtk opencv qt ui)
    set(_mvvcvtk_expected_vtk_version "9.4.2")
    set(_mvvcvtk_expected_vtk_abi "msvc-x64")
    set(_mvvcvtk_expected_opencv_version "4.12.0")
    set(_mvvcvtk_expected_opencv_abi "vc16-x64")
    set(_mvvcvtk_expected_qt_version "5.14.2")
    set(_mvvcvtk_expected_qt_abi "msvc2017_64")
    set(_mvvcvtk_expected_ui_version "ct-1209-sha256")
    set(_mvvcvtk_expected_ui_abi "qt5-msvc-x64")
    GetMVVCVTKJsonType(_mvvcvtk_components_type "${_mvvcvtk_json}"
        "dependency components" components)
    if(NOT _mvvcvtk_components_type STREQUAL "ARRAY")
        message(FATAL_ERROR "MVVCVTK dependency components must be an array.")
    endif()
    string(JSON _mvvcvtk_component_count LENGTH
        "${_mvvcvtk_json}" components)
    if(NOT _mvvcvtk_component_count EQUAL 4)
        message(FATAL_ERROR
            "MVVCVTK dependency components must be exactly vtk, opencv, qt, and ui."
        )
    endif()
    if(_mvvcvtk_component_count GREATER 0)
        math(EXPR _mvvcvtk_component_last "${_mvvcvtk_component_count} - 1")
        foreach(_mvvcvtk_index RANGE 0 ${_mvvcvtk_component_last})
            GetMVVCVTKJsonType(_mvvcvtk_component_type "${_mvvcvtk_json}"
                "dependency component" components ${_mvvcvtk_index})
            if(NOT _mvvcvtk_component_type STREQUAL "OBJECT")
                message(FATAL_ERROR
                    "Every MVVCVTK dependency component must be an object."
                )
            endif()
            GetMVVCVTKTypedJson(_mvvcvtk_component_id "${_mvvcvtk_json}"
                "dependency component id" STRING
                components ${_mvvcvtk_index} id)
            if(NOT _mvvcvtk_component_id IN_LIST _mvvcvtk_expected_ids)
                message(FATAL_ERROR
                    "Unknown MVVCVTK dependency component '${_mvvcvtk_component_id}'."
                )
            endif()
            if(_mvvcvtk_found_${_mvvcvtk_component_id})
                message(FATAL_ERROR
                    "MVVCVTK dependency component '${_mvvcvtk_component_id}' is duplicated."
                )
            endif()
            GetMVVCVTKTypedJson(_mvvcvtk_component_version "${_mvvcvtk_json}"
                "dependency component version" STRING
                components ${_mvvcvtk_index} version)
            GetMVVCVTKTypedJson(_mvvcvtk_component_abi "${_mvvcvtk_json}"
                "dependency component ABI" STRING
                components ${_mvvcvtk_index} abi)
            if(NOT _mvvcvtk_component_version STREQUAL
                    _mvvcvtk_expected_${_mvvcvtk_component_id}_version
                OR NOT _mvvcvtk_component_abi STREQUAL
                    _mvvcvtk_expected_${_mvvcvtk_component_id}_abi)
                message(FATAL_ERROR
                    "MVVCVTK dependency component '${_mvvcvtk_component_id}' "
                    "has an unexpected version or ABI."
                )
            endif()
            set(_mvvcvtk_found_${_mvvcvtk_component_id} TRUE)
        endforeach()
    endif()
    foreach(_mvvcvtk_component_id IN LISTS _mvvcvtk_expected_ids)
        if(NOT _mvvcvtk_found_${_mvvcvtk_component_id})
            message(FATAL_ERROR
                "MVVCVTK dependency component '${_mvvcvtk_component_id}' is missing."
            )
        endif()
    endforeach()

    file(REAL_PATH "${depsRoot}" _mvvcvtk_root_real)
    GetMVVCVTKJsonType(_mvvcvtk_sentinels_type "${_mvvcvtk_json}"
        "dependency sentinels" sentinels)
    if(NOT _mvvcvtk_sentinels_type STREQUAL "ARRAY")
        message(FATAL_ERROR "MVVCVTK dependency sentinels must be an array.")
    endif()
    string(JSON _mvvcvtk_sentinel_count LENGTH
        "${_mvvcvtk_json}" sentinels)
    if(_mvvcvtk_sentinel_count LESS 1)
        message(FATAL_ERROR "MVVCVTK dependency manifest has no sentinels.")
    endif()
    math(EXPR _mvvcvtk_sentinel_last "${_mvvcvtk_sentinel_count} - 1")
    foreach(_mvvcvtk_index RANGE 0 ${_mvvcvtk_sentinel_last})
        GetMVVCVTKJsonType(_mvvcvtk_sentinel_type "${_mvvcvtk_json}"
            "dependency sentinel" sentinels ${_mvvcvtk_index})
        if(NOT _mvvcvtk_sentinel_type STREQUAL "OBJECT")
            message(FATAL_ERROR
                "Every MVVCVTK dependency sentinel must be an object."
            )
        endif()
        GetMVVCVTKTypedJson(_mvvcvtk_path "${_mvvcvtk_json}"
            "dependency sentinel path" STRING
            sentinels ${_mvvcvtk_index} path)
        GetMVVCVTKTypedJson(_mvvcvtk_bytes "${_mvvcvtk_json}"
            "dependency sentinel bytes" NUMBER
            sentinels ${_mvvcvtk_index} bytes)
        GetMVVCVTKTypedJson(_mvvcvtk_sha "${_mvvcvtk_json}"
            "dependency sentinel SHA-256" STRING
            sentinels ${_mvvcvtk_index} sha256)
        if("${_mvvcvtk_path}" STREQUAL ""
            OR _mvvcvtk_path MATCHES
            "(^/|\\\\|:|(^|/)\\.\\.?(/|$)|//)")
            message(FATAL_ERROR
                "Unsafe MVVCVTK dependency sentinel path: '${_mvvcvtk_path}'."
            )
        endif()
        set(_mvvcvtk_file "${depsRoot}/${_mvvcvtk_path}")
        if(NOT EXISTS "${_mvvcvtk_file}")
            message(FATAL_ERROR
                "MVVCVTK dependency sentinel is missing: '${_mvvcvtk_path}'."
            )
        endif()
        if(IS_DIRECTORY "${_mvvcvtk_file}")
            message(FATAL_ERROR
                "MVVCVTK dependency sentinel is not a file: '${_mvvcvtk_path}'."
            )
        endif()
        string(REPLACE "/" ";" _mvvcvtk_segments "${_mvvcvtk_path}")
        set(_mvvcvtk_candidate "${depsRoot}")
        if(IS_SYMLINK "${_mvvcvtk_candidate}")
            message(FATAL_ERROR
                "MVVCVTK dependency root must not be a symbolic link."
            )
        endif()
        foreach(_mvvcvtk_segment IN LISTS _mvvcvtk_segments)
            set(_mvvcvtk_candidate
                "${_mvvcvtk_candidate}/${_mvvcvtk_segment}")
            if(IS_SYMLINK "${_mvvcvtk_candidate}")
                message(FATAL_ERROR
                    "MVVCVTK dependency sentinel traverses a symbolic link: "
                    "'${_mvvcvtk_path}'."
                )
            endif()
        endforeach()
        file(REAL_PATH "${_mvvcvtk_file}" _mvvcvtk_file_real)
        cmake_path(IS_PREFIX _mvvcvtk_root_real "${_mvvcvtk_file_real}"
            NORMALIZE _mvvcvtk_is_inside)
        if(NOT _mvvcvtk_is_inside)
            message(FATAL_ERROR
                "MVVCVTK dependency sentinel escapes its bundle: '${_mvvcvtk_path}'."
            )
        endif()
        file(SIZE "${_mvvcvtk_file}" _mvvcvtk_actual_bytes)
        if(NOT _mvvcvtk_actual_bytes EQUAL _mvvcvtk_bytes)
            message(FATAL_ERROR
                "MVVCVTK dependency sentinel length mismatch: '${_mvvcvtk_path}'."
            )
        endif()
        file(SHA256 "${_mvvcvtk_file}" _mvvcvtk_actual_sha)
        string(TOUPPER "${_mvvcvtk_actual_sha}" _mvvcvtk_actual_sha)
        string(TOUPPER "${_mvvcvtk_sha}" _mvvcvtk_sha)
        if(NOT _mvvcvtk_actual_sha STREQUAL _mvvcvtk_sha)
            message(FATAL_ERROR
                "MVVCVTK dependency sentinel hash mismatch: '${_mvvcvtk_path}'."
            )
        endif()
    endforeach()
endfunction()
