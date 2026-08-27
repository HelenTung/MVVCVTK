foreach(requiredVariable IN ITEMS
    DEPENDENCY_FILES
    STAGING_ROOT
    EXPECTED_HEADERS
)
    if(NOT DEFINED ${requiredVariable})
        message(FATAL_ERROR "Missing ${requiredVariable}.")
    endif()
endforeach()

cmake_path(
    ABSOLUTE_PATH STAGING_ROOT
    NORMALIZE
    OUTPUT_VARIABLE normalizedStagingRoot
)
string(TOLOWER "${normalizedStagingRoot}" comparisonStagingRoot)
string(REPLACE "," ";" dependencyFiles "${DEPENDENCY_FILES}")
set(actualHeaders)
foreach(dependencyFile IN LISTS dependencyFiles)
    if(NOT EXISTS "${dependencyFile}")
        message(FATAL_ERROR
            "MSVC source dependency output is missing: ${dependencyFile}"
        )
    endif()

    file(READ "${dependencyFile}" dependencyJson)
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
                cmake_path(
                    CONVERT "${relativeHeader}"
                    TO_CMAKE_PATH_LIST relativeHeader
                )
                list(APPEND actualHeaders "${relativeHeader}")
            endif()
        endforeach()
    endif()
endforeach()
list(REMOVE_DUPLICATES actualHeaders)
list(SORT actualHeaders)

string(REPLACE "," ";" expectedHeaders "${EXPECTED_HEADERS}")
set(expectedHeaderKeys)
foreach(expectedHeader IN LISTS expectedHeaders)
    string(TOLOWER "${expectedHeader}" expectedHeaderKey)
    list(FIND expectedHeaderKeys "${expectedHeaderKey}" duplicateIndex)
    if(NOT duplicateIndex EQUAL -1)
        message(FATAL_ERROR
            "Duplicate or case-conflicting declared header: ${expectedHeader}"
        )
    endif()
    list(APPEND expectedHeaderKeys "${expectedHeaderKey}")
endforeach()
set(expectedHeaders ${expectedHeaderKeys})
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
        "Header surface closure mismatch. Missing=[${missingText}] "
        "Unexpected=[${unexpectedText}]"
    )
endif()
