set(MVVCVTK_GAP_SOURCES
    include/Algorithms/SurfaceRefiner.h
    include/Algorithms/VoidDetector.h
    include/Algorithms/VolumeBuffer.h
    include/Render/Strategies/GapOverlayStrategies.h
    include/Services/GapAnalysisService.h
    src/Host/GapHostFeature.cpp
    src/Services/GapAnalysisService.cpp
)
# GapHostFeature 是模块入口，GapHostTypes 是其公开签名的值类型闭包。
set(MVVCVTK_GAP_API_HEADERS
    include/Host/GapHostFeature.h
    include/Host/GapHostTypes.h
)

# worker 结果和内部策略继续使用源码树，SDK consumer 只看到 Host 契约。
ResetSdkStage("${MVVCVTK_GAP_SDK_INCLUDE_DIR}")
BuildSdkHeaders(
    MVVCVTK_GAP_STAGED_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
    ${MVVCVTK_GAP_API_HEADERS}
)

add_library(mvvcvtk_gap_analysis STATIC)
target_sources(
    mvvcvtk_gap_analysis
    PRIVATE
        ${MVVCVTK_GAP_SOURCES}
        include/GapAnalysisTypes.h
        ${MVVCVTK_GAP_API_HEADERS}
)
target_include_directories(
    mvvcvtk_gap_analysis
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Algorithms"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Host"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Strategies"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Services"
)
SetTargetDefaults(mvvcvtk_gap_analysis)
add_library(MVVCVTK::GapAnalysis ALIAS mvvcvtk_gap_analysis)
set_target_properties(
    mvvcvtk_gap_analysis
    PROPERTIES
        OUTPUT_NAME MVVCVTKGapAnalysis
        EXPORT_NAME GapAnalysis
        PROJECT_LABEL MVVCVTKGapAnalysis
        FOLDER "MVVCVTK/Host/Features"
)
target_sources(
    mvvcvtk_gap_analysis
    PUBLIC
        FILE_SET api_headers
        TYPE HEADERS
        BASE_DIRS "${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
        FILES ${MVVCVTK_GAP_STAGED_API_HEADERS}
)

set(
    _mvvcvtk_gap_ide_files
    ${MVVCVTK_GAP_SOURCES}
    include/GapAnalysisTypes.h
    ${MVVCVTK_GAP_API_HEADERS}
)
list(
    TRANSFORM _mvvcvtk_gap_ide_files
    PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
)
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${_mvvcvtk_gap_ide_files}
)
unset(_mvvcvtk_gap_ide_files)
unset(MVVCVTK_GAP_SOURCES)
unset(MVVCVTK_GAP_API_HEADERS)
unset(MVVCVTK_GAP_STAGED_API_HEADERS)
target_include_directories(
    mvvcvtk_gap_analysis
    PUBLIC
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
SetApiDefaults(mvvcvtk_gap_analysis)
target_link_libraries(
    mvvcvtk_gap_analysis
    PUBLIC
        MVVCVTK::FeatureSPI
        ${MVVCVTK_VTK_PUBLIC_TARGETS}
    PRIVATE
        "$<BUILD_INTERFACE:MVVCVTKInternal::FeatureSupport>"
        ${MVVCVTK_VTK_TARGETS}
)
install(
    TARGETS mvvcvtk_gap_analysis
    EXPORT MVVCVTKTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>"
        COMPONENT Host
    FILE_SET api_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT Host
)
