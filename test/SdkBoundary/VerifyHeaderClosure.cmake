foreach(requiredVariable IN ITEMS
    DEPENDENCY_FILE
    STAGING_ROOT
    EXPECTED_HEADERS
)
    if(NOT DEFINED ${requiredVariable})
        message(FATAL_ERROR "Missing ${requiredVariable}.")
    endif()
endforeach()

if(NOT EXISTS "${DEPENDENCY_FILE}")
    message(FATAL_ERROR
        "MSVC source dependency output is missing: ${DEPENDENCY_FILE}"
    )
endif()

file(READ "${DEPENDENCY_FILE}" dependencyJson)
string(
    JSON includeCount
    ERROR_VARIABLE dependencyError
    LENGTH "${dependencyJson}" Data Includes
)
if(dependencyError)
    message(FATAL_ERROR
        "Invalid MSVC source dependency JSON: ${dependencyError}"
    )
endif()

cmake_path(
    ABSOLUTE_PATH STAGING_ROOT
    NORMALIZE
    OUTPUT_VARIABLE normalizedStagingRoot
)
string(TOLOWER "${normalizedStagingRoot}" comparisonStagingRoot)
set(actualHeaders)
if(includeCount GREATER 0)
    math(EXPR lastInclude "${includeCount} - 1")
    foreach(includeIndex RANGE 0 ${lastInclude})
        string(
            JSON includePath
            GET "${dependencyJson}" Data Includes ${includeIndex}
        )
        cmake_path(
            ABSOLUTE_PATH includePath
            NORMALIZE
            OUTPUT_VARIABLE normalizedIncludePath
        )
        string(TOLOWER "${normalizedIncludePath}" comparisonIncludePath)
        cmake_path(
            IS_PREFIX comparisonStagingRoot
            "${comparisonIncludePath}"
            NORMALIZE
            isStagedHeader
        )
        if(isStagedHeader)
            cmake_path(
                RELATIVE_PATH comparisonIncludePath
                BASE_DIRECTORY "${comparisonStagingRoot}"
                OUTPUT_VARIABLE relativeHeader
            )
            cmake_path(CONVERT "${relativeHeader}" TO_CMAKE_PATH_LIST relativeHeader)
            list(APPEND actualHeaders "${relativeHeader}")
        endif()
    endforeach()
endif()
list(REMOVE_DUPLICATES actualHeaders)
list(SORT actualHeaders)

string(REPLACE "," ";" expectedHeaders "${EXPECTED_HEADERS}")
list(TRANSFORM expectedHeaders TOLOWER)
list(REMOVE_DUPLICATES expectedHeaders)
list(SORT expectedHeaders)

set(missingHeaders ${expectedHeaders})
set(unexpectedHeaders ${actualHeaders})
foreach(actualHeader IN LISTS actualHeaders)
    list(REMOVE_ITEM missingHeaders "${actualHeader}")
endforeach()
foreach(expectedHeader IN LISTS expectedHeaders)
    list(REMOVE_ITEM unexpectedHeaders "${expectedHeader}")
endforeach()

if(missingHeaders OR unexpectedHeaders)
    list(JOIN missingHeaders ", " missingText)
    list(JOIN unexpectedHeaders ", " unexpectedText)
    message(FATAL_ERROR
        "Header closure mismatch. Missing=[${missingText}] "
        "Unexpected=[${unexpectedText}]"
    )
endif()
