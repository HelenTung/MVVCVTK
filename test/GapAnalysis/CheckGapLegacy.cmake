if(NOT DEFINED MVVCVTK_SOURCE_ROOT)
    message(FATAL_ERROR "MVVCVTK_SOURCE_ROOT is required.")
endif()

set(_gap_root
    "${MVVCVTK_SOURCE_ROOT}/MVVCVTK/features/GapAnalysis"
)
foreach(_legacy_file IN ITEMS
        "${_gap_root}/include/Algorithms/SurfaceRefiner.h"
        "${_gap_root}/include/Algorithms/VolumeBuffer.h")
    if(EXISTS "${_legacy_file}")
        message(FATAL_ERROR
            "Retired Gap algorithm file still exists: ${_legacy_file}"
        )
    endif()
endforeach()

file(
    GLOB_RECURSE _gap_sources
    LIST_DIRECTORIES false
    "${_gap_root}/*.cpp"
    "${_gap_root}/*.h"
    "${_gap_root}/CMakeLists.txt"
)
set(_legacy_tokens
    "Algorithms/"
    "SurfaceRefiner"
    "VolumeBuffer.h"
    "GapVolumeBuffer"
    "GapAdvancedParams"
    "SetAdvanced"
    "GetVoxelValue"
    "GetTrilinearValue"
    "SetOwnedMask"
    "SetSharedMask"
    "ClearMask"
    "GetVoxelValid"
    "VoidDetector"
    "CreateInteriorMask"
    "BuildCandidates"
    "BuildRegions"
)
foreach(_gap_source IN LISTS _gap_sources)
    file(READ "${_gap_source}" _gap_content)
    foreach(_legacy_token IN LISTS _legacy_tokens)
        string(FIND "${_gap_content}" "${_legacy_token}" _token_index)
        if(NOT _token_index EQUAL -1)
            message(FATAL_ERROR
                "Retired Gap token '${_legacy_token}' remains in ${_gap_source}"
            )
        endif()
    endforeach()
endforeach()
