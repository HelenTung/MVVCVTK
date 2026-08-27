foreach(requiredVariable IN ITEMS
    HOST_STAGING_ROOT
    SPI_STAGING_ROOT
    CROP_STAGING_ROOT
    GAP_STAGING_ROOT
    EXPECTED_HOST_HEADERS
    EXPECTED_SPI_HEADERS
    EXPECTED_CROP_HEADERS
    EXPECTED_GAP_HEADERS
)
    if(NOT DEFINED ${requiredVariable})
        message(FATAL_ERROR "Missing ${requiredVariable}.")
    endif()
endforeach()

function(GetStageHeaders outputName stagingRoot)
    file(
        GLOB_RECURSE stageHeaders
        LIST_DIRECTORIES false
        RELATIVE "${stagingRoot}"
        "${stagingRoot}/*"
    )
    list(TRANSFORM stageHeaders TOLOWER)
    list(SORT stageHeaders)
    set(${outputName} "${stageHeaders}" PARENT_SCOPE)
endfunction()

function(SetStageExpected outputName expectedHeaderCsv)
    string(REPLACE "," ";" stageHeaders "${expectedHeaderCsv}")
    set(headerKeys)
    foreach(stageHeader IN LISTS stageHeaders)
        string(TOLOWER "${stageHeader}" headerKey)
        list(FIND headerKeys "${headerKey}" duplicateIndex)
        if(NOT duplicateIndex EQUAL -1)
            message(FATAL_ERROR
                "Duplicate or case-conflicting staged declaration: ${stageHeader}"
            )
        endif()
        list(APPEND headerKeys "${headerKey}")
    endforeach()
    list(SORT headerKeys)
    set(${outputName} "${headerKeys}" PARENT_SCOPE)
endfunction()

GetStageHeaders(actualHostHeaders "${HOST_STAGING_ROOT}")
GetStageHeaders(actualSpiHeaders "${SPI_STAGING_ROOT}")
GetStageHeaders(actualCropHeaders "${CROP_STAGING_ROOT}")
GetStageHeaders(actualGapHeaders "${GAP_STAGING_ROOT}")
SetStageExpected(expectedHostHeaders "${EXPECTED_HOST_HEADERS}")
SetStageExpected(expectedSpiHeaders "${EXPECTED_SPI_HEADERS}")
SetStageExpected(expectedCropHeaders "${EXPECTED_CROP_HEADERS}")
SetStageExpected(expectedGapHeaders "${EXPECTED_GAP_HEADERS}")

if(NOT actualHostHeaders STREQUAL expectedHostHeaders)
    message(FATAL_ERROR
        "Host staging inventory mismatch. Actual=[${actualHostHeaders}] "
        "Expected=[${expectedHostHeaders}]"
    )
endif()
if(NOT actualSpiHeaders STREQUAL expectedSpiHeaders)
    message(FATAL_ERROR
        "Feature SPI staging inventory mismatch. Actual=[${actualSpiHeaders}] "
        "Expected=[${expectedSpiHeaders}]"
    )
endif()
if(NOT actualCropHeaders STREQUAL expectedCropHeaders)
    message(FATAL_ERROR
        "Crop staging inventory mismatch. Actual=[${actualCropHeaders}] "
        "Expected=[${expectedCropHeaders}]"
    )
endif()
if(NOT actualGapHeaders STREQUAL expectedGapHeaders)
    message(FATAL_ERROR
        "Gap staging inventory mismatch. Actual=[${actualGapHeaders}] "
        "Expected=[${expectedGapHeaders}]"
    )
endif()
