add_library(qt_feature_test_support STATIC
    App/TestWindow.cpp App/TestSession.cpp App/QtHostPump.cpp App/FeatureSetup.cpp App/TestWorkflow.cpp
    "${PROJECT_SOURCE_DIR}/test/Host/FeatureInput.cpp"
    Modules/ModulePanel.cpp Modules/DataTest.cpp Modules/ViewTest.cpp
    Support/JsonInput.cpp Support/ReferenceDataSource.cpp Support/TestRecordWriter.cpp Support/TestResources.cpp Support/UiText.cpp Support/ActionPolicy.cpp Support/SceneNodes.cpp Support/CatalogNodes.cpp Support/SceneGraph.cpp Support/PathInput.cpp Support/ParameterEditor.cpp)
target_include_directories(qt_feature_test_support PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(qt_feature_test_support PUBLIC MVVCVTK::Host MVVCVTK::FeatureSPI
    Qt5::Core Qt5::Gui Qt5::Widgets Qt5::OpenGL VTK::GUISupportQt ${MVVCVTK_VTK_TARGETS})
foreach(spec IN ITEMS "CROP;OrthogonalCrop;Crop" "GAP;GapAnalysis;Gap" "PART;PartSegmentation;Part"
    "SURFACE;SurfaceDetermination;Surface" "ARTIFACT;ArtifactReduction;Artifact" "ROTATION;ModelRotation;Rotation"
    "ALIGNMENT;MetrologyAlignment;Alignment" "WALL;WallThickness;Wall")
    list(GET spec 0 key)
    list(GET spec 1 feature)
    list(GET spec 2 module)
    if(TARGET MVVCVTK::${feature})
        target_link_libraries(qt_feature_test_support PUBLIC MVVCVTK::${feature})
        target_compile_definitions(qt_feature_test_support PUBLIC MANUAL_${key})
        target_sources(qt_feature_test_support PRIVATE Modules/${module}Test.cpp)
        if(key STREQUAL "PART")
            target_sources(qt_feature_test_support PRIVATE Modules/PartInput.cpp Modules/PartEditTest.cpp)
        elseif(key STREQUAL "ALIGNMENT")
            target_sources(qt_feature_test_support PRIVATE Modules/AlignmentInput.cpp)
        endif()
    endif()
endforeach()
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE manual_head OUTPUT_STRIP_TRAILING_WHITESPACE)
file(GLOB_RECURSE manual_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Automation/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/../Automation/*.cpp")
list(APPEND manual_inputs
    "${PROJECT_SOURCE_DIR}/test/Host/FeatureInput.h"
    "${PROJECT_SOURCE_DIR}/test/Host/FeatureInput.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/src/Interaction/CropBridge.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/src/Render/CropShaderController.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/src/Host/CropHostFeature.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/Host/CropHostFeature.h"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/OrthogonalCrop/include/OrthogonalCropTypes.h"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/ArtifactReduction/src/ArtifactReductionHostFeature.cpp"
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/MetrologyAlignment/src/MetrologyAlignmentHostFeature.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${manual_inputs})
set(manual_fingerprints "")
foreach(input IN LISTS manual_inputs)
    file(SHA256 "${input}" input_hash)
    file(RELATIVE_PATH input_name "${PROJECT_SOURCE_DIR}" "${input}")
    string(APPEND manual_fingerprints "${input_name}:${input_hash}\n")
endforeach()
string(SHA256 manual_source_fingerprint "${manual_fingerprints}")
target_compile_definitions(qt_feature_test_support PRIVATE
    MANUAL_CODE_HEAD="${manual_head}" MANUAL_BUILD_CONFIG="$<CONFIG>"
    MANUAL_SOURCE_FINGERPRINT="${manual_source_fingerprint}")
SetQtTargetDefaults(qt_feature_test_support)
set_target_properties(qt_feature_test_support PROPERTIES
    PROJECT_LABEL QtFeatureTestSupport FOLDER "MVVCVTK/test/Support")
vtk_module_autoinit(TARGETS qt_feature_test_support MODULES ${MVVCVTK_QT_VTK_TARGETS})

if(MVVCVTK_BUILD_QT_MANUAL)
    add_executable(qt_feature_manual main.cpp)
    target_link_libraries(qt_feature_manual PRIVATE qt_feature_test_support)
    SetQtTargetDefaults(qt_feature_manual)
    set_target_properties(qt_feature_manual PROPERTIES OUTPUT_NAME QtFeatureManual
        PROJECT_LABEL QtFeatureManual FOLDER "MVVCVTK/test/Manual")
    list(JOIN MVVCVTK_RUNTIME_DIRS ";" manual_runtime_path)
    set_property(TARGET qt_feature_manual PROPERTY VS_DEBUGGER_ENVIRONMENT
        "PATH=${manual_runtime_path};%PATH%\nQT_PLUGIN_PATH=${MVVCVTK_QT_PLUGIN_DIR}")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/RunQtFeatureManual-$<CONFIG>.cmd" CONTENT
        "@echo off\nsetlocal\nset \"PATH=${manual_runtime_path};%PATH%\"\nset \"QT_PLUGIN_PATH=${MVVCVTK_QT_PLUGIN_DIR}\"\n\"$<TARGET_FILE:qt_feature_manual>\" %*\n")
    vtk_module_autoinit(TARGETS qt_feature_manual MODULES ${MVVCVTK_QT_VTK_TARGETS})
endif()
