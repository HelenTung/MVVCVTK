add_library(tomopy_ring STATIC third_party/tomopy/remove_ring.c)
target_include_directories(tomopy_ring PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomopy>")
# 上游数值路径含有有意的double到float舍入；仅此C目标保留该转换语义。
target_compile_options(tomopy_ring PRIVATE /W3 /utf-8 /sdl /wd4244)
set_target_properties(tomopy_ring PROPERTIES
    OUTPUT_NAME TomoPyRing
    PROJECT_LABEL TomoPyRing
    C_STANDARD 11 C_STANDARD_REQUIRED YES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    MSVC_DEBUG_INFORMATION_FORMAT Embedded
    FOLDER "MVVCVTK/Host/Features/Private")

add_library(artifact_reduction STATIC)
add_library(MVVCVTK::ArtifactReduction ALIAS artifact_reduction)

target_sources(artifact_reduction
    PRIVATE
        src/ArtifactReductionAlgorithm.h
        src/ArtifactReductionAlgorithm.cpp
        src/TomoPyRingAdapter.h
        src/TomoPyRingAdapter.cpp
        src/VtkDiffusionAdapter.h
        src/VtkDiffusionAdapter.cpp
        src/ArtifactQualityEvaluator.h
        src/ArtifactQualityEvaluator.cpp
        src/ArtifactReductionHostFeature.cpp
    PUBLIC
        FILE_SET api_headers TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
            include/Host/ArtifactReductionHostFeature.h
            include/Host/ArtifactReductionHostTypes.h
)
target_include_directories(artifact_reduction
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(artifact_reduction
    PUBLIC MVVCVTK::FeatureSPI
    PRIVATE tomopy_ring VTK::ImagingGeneral)
SetTargetDefaults(artifact_reduction)
set_target_properties(artifact_reduction PROPERTIES
    OUTPUT_NAME ArtifactReduction
    PROJECT_LABEL ArtifactReduction
    FOLDER "MVVCVTK/Host/Features"
)
# 仓内目标：不 staging 到 SDK，不 install/export，不添加 package component。
