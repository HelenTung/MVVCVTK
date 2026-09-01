if(NOT DEFINED MVVCVTK_SOURCE_ROOT
    OR "${MVVCVTK_SOURCE_ROOT}" STREQUAL "")
    message(FATAL_ERROR "MVVCVTK_SOURCE_ROOT is required.")
endif()

set(_source_root "${MVVCVTK_SOURCE_ROOT}")
cmake_path(
    ABSOLUTE_PATH _source_root
    NORMALIZE
    OUTPUT_VARIABLE _source_root
)
set(_gap_root "${_source_root}/MVVCVTK/features/GapAnalysis")
if(NOT IS_DIRECTORY "${_gap_root}")
    message(FATAL_ERROR "GapAnalysis source root is missing: ${_gap_root}")
endif()

# 项目源码树只保存自有代码，供应商头、导入库和 DLL 必须留在外部依赖根。
set(_owned_roots
    "${_source_root}/cmake"
    "${_source_root}/MVVCVTK"
    "${_source_root}/examples"
    "${_source_root}/test"
    "${_source_root}/tools"
)
set(_owned_files)
# 根目录只检查直接文件；ignored 临时 gap 保留到用户自行删除，但永不进入构建扫描。
file(
    GLOB _root_owned_files
    LIST_DIRECTORIES false
    "${_source_root}/*"
)
list(APPEND _owned_files ${_root_owned_files})
foreach(_owned_root IN LISTS _owned_roots)
    if(IS_DIRECTORY "${_owned_root}")
        file(
            GLOB_RECURSE _root_owned_files
            LIST_DIRECTORIES false
            "${_owned_root}/*"
        )
        list(APPEND _owned_files ${_root_owned_files})
    endif()
endforeach()
list(REMOVE_DUPLICATES _owned_files)
foreach(_owned_file IN LISTS _owned_files)
    cmake_path(GET _owned_file FILENAME _owned_name)
    string(TOLOWER "${_owned_name}" _owned_name_key)
    if(_owned_name_key MATCHES
        "^defx.*\\.(h|hh|hpp|hxx|ipp|lib|dll|exp)$")
        message(FATAL_ERROR
            "Vendor artifact must not be stored in the project source tree: "
            "${_owned_file}"
        )
    endif()
endforeach()

set(_bridge_source
    "${_gap_root}/src/Services/GapKernelBridge.cpp"
)
set(_bridge_header
    "${_gap_root}/src/Services/GapKernelBridge.h"
)
foreach(_bridge_file IN ITEMS "${_bridge_source}" "${_bridge_header}")
    if(NOT EXISTS "${_bridge_file}" OR IS_DIRECTORY "${_bridge_file}")
        message(FATAL_ERROR "Gap kernel bridge file is missing: ${_bridge_file}")
    endif()
endforeach()

# 供应商 C++ API 只允许出现在唯一的私有 bridge 实现文件中。
file(
    GLOB _product_sources
    LIST_DIRECTORIES false
    "${_source_root}/*.c"
    "${_source_root}/*.cc"
    "${_source_root}/*.cpp"
    "${_source_root}/*.cxx"
    "${_source_root}/*.h"
    "${_source_root}/*.hpp"
    "${_source_root}/*.inl"
)
foreach(_scan_root IN LISTS _owned_roots)
    if(IS_DIRECTORY "${_scan_root}")
        file(
            GLOB_RECURSE _root_sources
            LIST_DIRECTORIES false
            "${_scan_root}/*.c"
            "${_scan_root}/*.cc"
            "${_scan_root}/*.cpp"
            "${_scan_root}/*.cxx"
            "${_scan_root}/*.h"
            "${_scan_root}/*.hpp"
            "${_scan_root}/*.inl"
        )
        list(APPEND _product_sources ${_root_sources})
    endif()
endforeach()
list(REMOVE_DUPLICATES _product_sources)

set(_vendor_api_identifiers
    "DEFXANALYSISSERVICE_EXPORTS"
    "DEFX_API"
    "DefXAlgorithmMode"
    "DefXAnalysisMode"
    "SurfaceValue"
    "ThresholdMode"
    "MaterialDefinition"
    "NoiseReductionMode"
    "ProbabilityCriterion"
    "AnalysisAreaMode"
    "DefXSeedStrategy"
    "AnalysisParams"
    "FilterCriteria"
    "DefXDefectRegion"
    "DefXAnalysisOutputHeader"
    "DefXAnalysisRequest"
    "DefXAnalysisService"
    "DefXTypes"
    "DefXAlgorithm_OnlyThreshold"
    "DefXAlgorithm_BestOperator"
    "DefXAnalysis_Pore"
    "DefXAnalysis_Inclusion"
    "ThresholdMode_Deviation"
    "ThresholdMode_Interpolation"
    "NoiseReduction_None"
    "NoiseReduction_Low"
    "NoiseReduction_Medium"
    "NoiseReduction_High"
    "NoiseReduction_Median"
    "Criterion_Standard"
    "Criterion_Size"
    "Criterion_Hole"
    "Criterion_Contrast"
    "Criterion_Expert"
    "Area_InsideAll"
    "Area_ROI"
    "Area_ClosedOnly"
    "DefXSeed_LocalMinima"
    "DefXSeed_NestedIntervals"
)
foreach(_product_source IN LISTS _product_sources)
    cmake_path(NORMAL_PATH _product_source OUTPUT_VARIABLE _product_source)
    if(_product_source STREQUAL _bridge_source)
        continue()
    endif()
    file(READ "${_product_source}" _source_content)
    string(TOLOWER "${_source_content}" _source_content_key)
    foreach(_vendor_api_identifier IN LISTS _vendor_api_identifiers)
        string(TOLOWER "${_vendor_api_identifier}" _vendor_identifier_key)
        if(_source_content_key MATCHES
            "(^|[^a-z0-9_])${_vendor_identifier_key}([^a-z0-9_]|$)")
            message(FATAL_ERROR
                "DefX API token '${_vendor_api_identifier}' escaped the private bridge: "
                "${_product_source}"
            )
        endif()
    endforeach()
endforeach()

file(READ "${_bridge_source}" _bridge_source_content)
string(FIND
    "${_bridge_source_content}"
    "#include \"DefXAnalysisService.h\""
    _bridge_include_index
)
if(_bridge_include_index EQUAL -1)
    message(FATAL_ERROR
        "The private bridge must consume the external DefX header directly."
    )
endif()

# ABI 头只能暴露固定宽度 POD、原始指针和 C 调用约定。
file(READ "${_bridge_header}" _bridge_header_content)
foreach(_bridge_forbidden_token IN ITEMS
        "DefX"
        "std::vector"
        "std::string"
        "std::basic_string"
        "std::unique_ptr"
        "std::shared_ptr")
    string(FIND
        "${_bridge_header_content}"
        "${_bridge_forbidden_token}"
        _bridge_token_index
    )
    if(NOT _bridge_token_index EQUAL -1)
        message(FATAL_ERROR
            "Private bridge ABI header exposes '${_bridge_forbidden_token}'."
        )
    endif()
endforeach()
if(_bridge_header_content MATCHES
    "(^|[^A-Za-z0-9_])vtk[A-Z][A-Za-z0-9_]*")
    message(FATAL_ERROR "Private bridge ABI header exposes a VTK type.")
endif()
if(_bridge_header_content MATCHES
    "(^|[^A-Za-z0-9_])bool([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR "Private bridge ABI header exposes native bool.")
endif()
if(_bridge_header_content MATCHES
    "(^|[^A-Za-z0-9_])enum([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR "Private bridge ABI header exposes a native enum.")
endif()

# 活跃构建文件不得从根临时 gap 取依赖，DefX imported target 也只能由中央
# 依赖模块定义，并由 GapAnalysis 私有 kernel 消费、复制和安装。
set(_boundary_script "${CMAKE_CURRENT_LIST_FILE}")
cmake_path(NORMAL_PATH _boundary_script OUTPUT_VARIABLE _boundary_script)
set(_allowed_defx_cmake
    "${_source_root}/cmake/MVVCVTKDependencies.cmake"
    "${_gap_root}/CMakeLists.txt"
    "${_source_root}/test/CMakeLists.txt"
)
set(_build_files "${_source_root}/CMakeLists.txt")
foreach(_build_root IN ITEMS
        "${_source_root}/cmake"
        "${_source_root}/MVVCVTK"
        "${_source_root}/examples"
        "${_source_root}/test"
        "${_source_root}/tools")
    if(IS_DIRECTORY "${_build_root}")
        file(
            GLOB_RECURSE _root_build_files
            LIST_DIRECTORIES false
            "${_build_root}/CMakeLists.txt"
            "${_build_root}/*.cmake"
            "${_build_root}/*.cmake.in"
        )
        list(APPEND _build_files ${_root_build_files})
    endif()
endforeach()
list(REMOVE_DUPLICATES _build_files)

set(_temporary_gap_tokens
    [=[${PROJECT_SOURCE_DIR}/gap]=]
    [=[${CMAKE_SOURCE_DIR}/gap]=]
    [=[${PROJECT_SOURCE_DIR}\gap]=]
    [=[${CMAKE_SOURCE_DIR}\gap]=]
    "gap/DefX"
    "gap\\DefX"
    "add_subdirectory(gap"
)
foreach(_build_file IN LISTS _build_files)
    cmake_path(NORMAL_PATH _build_file OUTPUT_VARIABLE _build_file)
    if(_build_file STREQUAL _boundary_script)
        continue()
    endif()
    file(READ "${_build_file}" _build_content)
    string(FIND "${_build_content}" "DefX::Analysis" _defx_target_index)
    if(NOT _defx_target_index EQUAL -1)
        list(FIND _allowed_defx_cmake "${_build_file}" _allowed_cmake_index)
        if(_allowed_cmake_index EQUAL -1)
            message(FATAL_ERROR
                "DefX::Analysis escaped its central/private CMake boundary: "
                "${_build_file}"
            )
        endif()
    endif()
    foreach(_temporary_gap_token IN LISTS _temporary_gap_tokens)
        string(FIND
            "${_build_content}"
            "${_temporary_gap_token}"
            _temporary_gap_index
        )
        if(NOT _temporary_gap_index EQUAL -1)
            message(FATAL_ERROR
                "Active build file references the temporary gap tree: "
                "${_build_file}"
            )
        endif()
    endforeach()
endforeach()
