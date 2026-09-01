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
    list(SORT stageHeaders)
    set(${outputName} "${stageHeaders}" PARENT_SCOPE)
endfunction()

function(SetStageLayout stageName allowedSurfaces)
    foreach(stageHeader IN LISTS ARGN)
        if(NOT stageHeader MATCHES "^MVVCVTK/(API|SPI)/.+\\.h$")
            message(FATAL_ERROR
                "${stageName} staging contains an unpartitioned SDK path: "
                "${stageHeader}"
            )
        endif()
        string(
            REGEX REPLACE
            "^MVVCVTK/([^/]+)/.*$"
            "\\1"
            headerSurface
            "${stageHeader}"
        )
        list(FIND allowedSurfaces "${headerSurface}" surfaceIndex)
        if(surfaceIndex EQUAL -1)
            message(FATAL_ERROR
                "${stageName} staging exposes the wrong SDK surface: "
                "${stageHeader}"
            )
        endif()
    endforeach()
endfunction()

GetStageHeaders(actualHostHeaders "${HOST_STAGING_ROOT}")
GetStageHeaders(actualSpiHeaders "${SPI_STAGING_ROOT}")
GetStageHeaders(actualCropHeaders "${CROP_STAGING_ROOT}")
GetStageHeaders(actualGapHeaders "${GAP_STAGING_ROOT}")
if(NOT actualHostHeaders OR NOT actualSpiHeaders)
    message(FATAL_ERROR "Required Host/API or Feature/SPI staging is empty.")
endif()
if(NOT EXPECTED_CROP_HEADERS STREQUAL "" AND NOT actualCropHeaders)
    message(FATAL_ERROR "Enabled OrthogonalCrop staging is empty.")
endif()
if(NOT EXPECTED_GAP_HEADERS STREQUAL "" AND NOT actualGapHeaders)
    message(FATAL_ERROR "Enabled GapAnalysis staging is empty.")
endif()
SetStageLayout(Host "API" ${actualHostHeaders})
SetStageLayout(FeatureSPI "API;SPI" ${actualSpiHeaders})
SetStageLayout(OrthogonalCrop "API" ${actualCropHeaders})
SetStageLayout(GapAnalysis "API" ${actualGapHeaders})
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
