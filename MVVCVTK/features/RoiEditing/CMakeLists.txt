set(roiEditingHeaders include/Host/RoiEditingHostFeature.h)
set(roiEditingStage "${CMAKE_CURRENT_BINARY_DIR}/mvvcvtk-source-api-include")
ResetSdkStage("${roiEditingStage}")
BuildSourceHeaders(roiEditingSourceHeaders "${CMAKE_CURRENT_SOURCE_DIR}" "${roiEditingStage}" ${roiEditingHeaders})
add_library(roi_editing STATIC ${roiEditingHeaders} src/RoiEditingHostFeature.cpp)
add_library(MVVCVTK::RoiEditing ALIAS roi_editing)
target_include_directories(roi_editing PUBLIC "$<BUILD_INTERFACE:${roiEditingStage}>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(roi_editing PUBLIC MVVCVTK::FeatureSPI
    PRIVATE MVVCVTKInternal::FeatureSupport ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(roi_editing)
SetApiDefaults(roi_editing)
set_target_properties(roi_editing PROPERTIES OUTPUT_NAME RoiEditing PROJECT_LABEL RoiEditing FOLDER "MVVCVTK/Host/Features")
# 仅仓内构建；不 install/export，不增加 SDK 产品或公开物理头。
