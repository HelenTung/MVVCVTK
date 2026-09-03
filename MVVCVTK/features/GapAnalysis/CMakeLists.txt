set(MVVCVTK_GAP_SOURCES
    include/Render/Strategies/GapOverlayStrategies.h
    include/Services/GapAnalysisService.h
    src/Host/GapHostFeature.cpp
    src/Services/GapAnalysisService.cpp
    src/Services/GapInputBuffer.h
)
set(MVVCVTK_GAP_KERNEL_SOURCES
    src/Services/GapKernelBridge.cpp
    src/Services/GapKernelBridge.h
    src/Services/GapKernelLabelView.cpp
    src/Services/GapKernelLabelView.h
)

add_library(mvvcvtk_gap_kernel MODULE ${MVVCVTK_GAP_KERNEL_SOURCES})
target_include_directories(
    mvvcvtk_gap_kernel
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Services"
)
target_compile_definitions(
    mvvcvtk_gap_kernel
    PRIVATE MVVCVTK_GAP_KERNEL_EXPORTS=1
)
target_link_libraries(
    mvvcvtk_gap_kernel
    PRIVATE
        DefX::Analysis
        ${MVVCVTK_VTK_TARGETS}
)
SetTargetDefaults(mvvcvtk_gap_kernel)
set_target_properties(
    mvvcvtk_gap_kernel
    PROPERTIES
        OUTPUT_NAME MVVCVTKGapKernel
        PROJECT_LABEL MVVCVTKGapKernel
        FOLDER "MVVCVTK/Host/Features/Private"
)
# build tree 与 SDK install tree 都让私有 kernel 和 DefX runtime 成对出现；
# C++ 加载器据此解析唯一绝对路径，不修改外部供应商根中的原始工件。
add_custom_command(
    TARGET mvvcvtk_gap_kernel
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:DefX::Analysis>"
        "$<TARGET_FILE_DIR:mvvcvtk_gap_kernel>"
    VERBATIM
)
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${MVVCVTK_GAP_KERNEL_SOURCES}
)
# GapHostFeature 是模块入口，GapHostTypes 是其公开签名的值类型闭包。
SetHeaderSurface(
    MVVCVTK_GAP_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    include/Host/GapHostFeature.h
    include/Host/GapHostTypes.h
)

# worker 结果和内部策略继续使用源码树，SDK consumer 只看到 Host 契约。
ResetSdkStage("${MVVCVTK_GAP_SDK_INCLUDE_DIR}")
ResetSdkStage("${MVVCVTK_GAP_SOURCE_API_INCLUDE_DIR}")
BuildSdkHeaders(
    MVVCVTK_GAP_STAGED_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_GAP_SDK_INCLUDE_DIR}"
    ${MVVCVTK_GAP_API_HEADERS}
)
BuildSourceHeaders(
    MVVCVTK_GAP_SOURCE_API_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MVVCVTK_GAP_SOURCE_API_INCLUDE_DIR}"
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
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Host"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Render/Strategies"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/Services"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Services"
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
unset(MVVCVTK_GAP_KERNEL_SOURCES)
unset(MVVCVTK_GAP_API_HEADERS)
unset(MVVCVTK_GAP_SOURCE_API_HEADERS)
unset(MVVCVTK_GAP_STAGED_API_HEADERS)
target_include_directories(
    mvvcvtk_gap_analysis
    PUBLIC
        "$<BUILD_INTERFACE:${MVVCVTK_GAP_SOURCE_API_INCLUDE_DIR}>"
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
add_dependencies(mvvcvtk_gap_analysis mvvcvtk_gap_kernel)

install(
    TARGETS mvvcvtk_gap_analysis
    EXPORT MVVCVTKTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>"
        COMPONENT Host
    FILE_SET api_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT Host
)
install(
    TARGETS mvvcvtk_gap_kernel
    LIBRARY DESTINATION "deps/third_party/defx/bin/$<CONFIG>"
        COMPONENT Host
    RUNTIME DESTINATION "deps/third_party/defx/bin/$<CONFIG>"
        COMPONENT Host
)
install(
    IMPORTED_RUNTIME_ARTIFACTS DefX::Analysis
    RUNTIME DESTINATION "deps/third_party/defx/bin/$<CONFIG>"
    COMPONENT Host
)
