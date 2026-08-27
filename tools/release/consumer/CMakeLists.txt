cmake_minimum_required(VERSION 4.2)

project(MVVCVTKSdkConsumer LANGUAGES CXX)

option(MVVCVTK_CONSUMER_CROP "Link the OrthogonalCrop SDK module." ON)
option(MVVCVTK_CONSUMER_GAP "Link the GapAnalysis SDK module." ON)

set(_mvvcvtk_components HostAPI Host)
set(_mvvcvtk_links MVVCVTK::Host)
set(_mvvcvtk_surface_categories HostAPI)
if(MVVCVTK_CONSUMER_CROP OR MVVCVTK_CONSUMER_GAP)
    list(APPEND _mvvcvtk_components FeatureSPI)
    list(APPEND _mvvcvtk_surface_categories FeatureSPI)
endif()
if(MVVCVTK_CONSUMER_CROP)
    list(APPEND _mvvcvtk_components OrthogonalCrop)
    list(APPEND _mvvcvtk_links MVVCVTK::OrthogonalCrop)
    list(APPEND _mvvcvtk_surface_categories OrthogonalCrop)
endif()
if(MVVCVTK_CONSUMER_GAP)
    list(APPEND _mvvcvtk_components GapAnalysis)
    list(APPEND _mvvcvtk_links MVVCVTK::GapAnalysis)
    list(APPEND _mvvcvtk_surface_categories GapAnalysis)
endif()

find_package(
    MVVCVTK
    CONFIG REQUIRED
    COMPONENTS ${_mvvcvtk_components}
)

add_executable(mvvcvtk_sdk_consumer main.cpp)
target_compile_features(mvvcvtk_sdk_consumer PRIVATE cxx_std_17)
target_link_libraries(
    mvvcvtk_sdk_consumer
    PRIVATE ${_mvvcvtk_links}
)
if(MVVCVTK_CONSUMER_CROP)
    target_compile_definitions(
        mvvcvtk_sdk_consumer
        PRIVATE MVVCVTK_CONSUMER_CROP=1
    )
else()
    target_compile_definitions(
        mvvcvtk_sdk_consumer
        PRIVATE MVVCVTK_CONSUMER_CROP=0
    )
endif()
if(MVVCVTK_CONSUMER_GAP)
    target_compile_definitions(
        mvvcvtk_sdk_consumer
        PRIVATE MVVCVTK_CONSUMER_GAP=1
    )
else()
    target_compile_definitions(
        mvvcvtk_sdk_consumer
        PRIVATE MVVCVTK_CONSUMER_GAP=0
    )
endif()

file(STRINGS
    "${MVVCVTK_HEADER_SURFACE_FILE}"
    _mvvcvtk_surface_entries
    ENCODING UTF-8
)
set(_mvvcvtk_entry_headers)
set(_mvvcvtk_entry_keys)
foreach(_mvvcvtk_surface_entry IN LISTS _mvvcvtk_surface_entries)
    if(NOT _mvvcvtk_surface_entry MATCHES "^([^|]+)\\|([^|]+)$")
        message(FATAL_ERROR
            "Invalid MVVCVTK header surface entry: ${_mvvcvtk_surface_entry}"
        )
    endif()
    set(_mvvcvtk_surface_category "${CMAKE_MATCH_1}")
    set(_mvvcvtk_surface_header "${CMAKE_MATCH_2}")
    if(_mvvcvtk_surface_category IN_LIST _mvvcvtk_surface_categories)
        string(TOLOWER "${_mvvcvtk_surface_header}" _mvvcvtk_surface_key)
        list(FIND _mvvcvtk_entry_keys "${_mvvcvtk_surface_key}" _mvvcvtk_key_index)
        if(_mvvcvtk_key_index EQUAL -1)
            list(APPEND _mvvcvtk_entry_headers "${_mvvcvtk_surface_header}")
            list(APPEND _mvvcvtk_entry_keys "${_mvvcvtk_surface_key}")
        endif()
    endif()
endforeach()
if(NOT _mvvcvtk_entry_headers)
    message(FATAL_ERROR "MVVCVTK consumer selected no public entry headers.")
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/headers")
set(_mvvcvtk_header_sources)
foreach(_mvvcvtk_header IN LISTS _mvvcvtk_entry_headers)
    string(MAKE_C_IDENTIFIER "${_mvvcvtk_header}" _mvvcvtk_source_name)
    set(
        _mvvcvtk_source
        "${CMAKE_CURRENT_BINARY_DIR}/headers/${_mvvcvtk_source_name}.cpp"
    )
    file(CONFIGURE
        OUTPUT "${_mvvcvtk_source}"
        CONTENT "#include <${_mvvcvtk_header}>\n"
        NEWLINE_STYLE UNIX
    )
    list(APPEND _mvvcvtk_header_sources "${_mvvcvtk_source}")
endforeach()
add_library(
    mvvcvtk_installed_headers
    OBJECT
    ${_mvvcvtk_header_sources}
)
target_link_libraries(
    mvvcvtk_installed_headers
    PRIVATE ${_mvvcvtk_links}
)

# 负向检查使用 find_package 得到的真实 imported targets 及其传递 usage requirements。
include(CheckIncludeFileCXX)
set(CMAKE_REQUIRED_LIBRARIES ${_mvvcvtk_links})
set(CMAKE_REQUIRED_QUIET TRUE)
function(SetForbiddenHeader headerName)
    string(MAKE_C_IDENTIFIER "${headerName}" headerId)
    set(resultName "MVVCVTK_HAS_FORBIDDEN_${headerId}")
    unset(${resultName} CACHE)
    check_include_file_cxx("${headerName}" ${resultName})
    if(${resultName})
        message(FATAL_ERROR
            "Installed consumer can include a private header: ${headerName}"
        )
    endif()
endfunction()
SetForbiddenHeader(Data/DataManager.h)
SetForbiddenHeader(Host/HostViewRuntimeRegistry.h)
SetForbiddenHeader(Render/Strategies/SliceStrategy.h)
SetForbiddenHeader(Render/Support/FeatureVisualStrategy.h)
if(MVVCVTK_CONSUMER_CROP)
    SetForbiddenHeader(Algorithms/CropAlgorithm.h)
endif()
if(MVVCVTK_CONSUMER_GAP)
    SetForbiddenHeader(Services/GapAnalysisService.h)
endif()

unset(CMAKE_REQUIRED_LIBRARIES)
unset(CMAKE_REQUIRED_QUIET)
unset(_mvvcvtk_components)
unset(_mvvcvtk_entry_headers)
unset(_mvvcvtk_entry_keys)
unset(_mvvcvtk_header)
unset(_mvvcvtk_header_sources)
unset(_mvvcvtk_key_index)
unset(_mvvcvtk_links)
unset(_mvvcvtk_source)
unset(_mvvcvtk_source_name)
unset(_mvvcvtk_surface_categories)
unset(_mvvcvtk_surface_category)
unset(_mvvcvtk_surface_entries)
unset(_mvvcvtk_surface_entry)
unset(_mvvcvtk_surface_header)
unset(_mvvcvtk_surface_key)
