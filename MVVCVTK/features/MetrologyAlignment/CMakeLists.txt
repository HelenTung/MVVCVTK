add_library(metrology_alignment STATIC)
add_library(MVVCVTK::MetrologyAlignment ALIAS metrology_alignment)
option(MVVCVTK_METROLOGY_ENABLE_CUDA "Build the optional repository-only CUDA deviation backend" OFF)
if(MVVCVTK_METROLOGY_ENABLE_CUDA)
    # 不指定 Toolkit 版本；使用发现的 nvcc，避免依赖某版 Visual Studio CUDA 扩展。
    find_package(CUDAToolkit REQUIRED)
    set(MVVCVTK_METROLOGY_CUDA_ARCH "" CACHE STRING "Optional nvcc GPU architecture; empty uses the compiler default")
    set(cudaArchitecture)
    if(MVVCVTK_METROLOGY_CUDA_ARCH)
        list(APPEND cudaArchitecture "--gpu-architecture=${MVVCVTK_METROLOGY_CUDA_ARCH}")
    endif()
    set(cudaObject "${CMAKE_CURRENT_BINARY_DIR}/cuda/$<CONFIG>/AlignmentCudaQuery.obj")
    add_custom_command(OUTPUT "${cudaObject}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/cuda/$<CONFIG>"
        COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}" --compiler-bindir "${CMAKE_CXX_COMPILER}"
            --std=c++17 --fmad=false ${cudaArchitecture}
            "-Xcompiler=$<IF:$<CONFIG:Debug>,/MDd,/MD>,/EHsc,/utf-8"
            "$<IF:$<CONFIG:Debug>,-O0,-O3>" -c "${CMAKE_CURRENT_SOURCE_DIR}/src/AlignmentCudaQuery.cu"
            -o "${cudaObject}"
        DEPENDS src/AlignmentCudaQuery.cu src/AlignmentCudaQuery.h
            "${CUDAToolkit_NVCC_EXECUTABLE}" "${CMAKE_CXX_COMPILER}" VERBATIM)
    set_source_files_properties("${cudaObject}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    add_library(metrology_alignment_cuda STATIC "${cudaObject}" src/AlignmentCudaQuery.h)
    set_target_properties(metrology_alignment_cuda PROPERTIES LINKER_LANGUAGE CXX FOLDER "MVVCVTK/Features/Private")
    target_link_libraries(metrology_alignment_cuda PRIVATE CUDA::cudart)
    # C API 动态运行时隔离供应商 CRT；只复制到本工作树运行目录，不进入 SDK。
    file(GLOB cudaRuntimeDll "${CUDAToolkit_BIN_DIR}/cudart64*.dll" "${CUDAToolkit_BIN_DIR}/x64/cudart64*.dll")
    list(LENGTH cudaRuntimeDll cudaRuntimeCount)
    if(NOT cudaRuntimeCount EQUAL 1)
        message(FATAL_ERROR "Expected one cudart DLL in the discovered CUDA Toolkit")
    endif()
    add_custom_target(metrology_alignment_cuda_runtime
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${cudaRuntimeDll} "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        VERBATIM)
    set_target_properties(metrology_alignment_cuda_runtime PROPERTIES FOLDER "MVVCVTK/Features/Private")
    add_dependencies(metrology_alignment_cuda metrology_alignment_cuda_runtime)
    target_link_libraries(metrology_alignment PRIVATE metrology_alignment_cuda)
    target_compile_definitions(metrology_alignment PRIVATE MVVCVTK_METROLOGY_HAS_CUDA=1)
else()
    target_compile_definitions(metrology_alignment PRIVATE MVVCVTK_METROLOGY_HAS_CUDA=0)
endif()
target_sources(metrology_alignment
    PRIVATE src/AlignmentMath.h src/AlignmentGeometry.h src/AlignmentGeometry.cpp
        src/AlignmentSolver.h src/AlignmentSolver.cpp
        src/AlignmentMeshQuery.h src/AlignmentMeshQuery.cpp
        src/AlignmentSurfaceSamples.h src/AlignmentSurfaceSamples.cpp
        src/AlignmentReference.h src/AlignmentReference.cpp
        src/AlignmentPrealign.h src/AlignmentPrealign.cpp
        src/AlignmentDeviation.h src/AlignmentDeviation.cpp
        src/AlignmentDeviationData.h src/AlignmentDeviationData.cpp
        src/AlignmentDeviationOverlay.h src/AlignmentDeviationOverlay.cpp
        src/AlignmentDeviationDisplay.h src/AlignmentDeviationDisplay.cpp
        src/AlignmentResultSource.h src/AlignmentResultSource.cpp
        src/AlignmentInspection.h src/AlignmentInspection.cpp
        src/AlignmentAllocation.h src/AlignmentSection.h src/AlignmentSection.cpp
        src/AlignmentInspectionData.h src/AlignmentInspectionData.cpp
        src/AlignmentReport.h src/AlignmentReport.cpp
        src/AlignmentReportDisplay.h src/AlignmentReportDisplay.cpp
        src/AlignmentDeviationColors.h src/AlignmentDeviationColors.cpp
        src/AlignmentModelPose.h src/AlignmentModelPose.cpp
        src/AlignmentSceneDisplay.h src/AlignmentSceneDisplay.cpp src/AlignmentSceneOverlay.cpp
        src/AlignmentSceneGeometry.h src/AlignmentSceneGeometry.cpp
        src/AlignmentVoxelVolume.h src/AlignmentVoxelVolume.cpp
        src/AlignmentVoxelVolumeData.h src/AlignmentVoxelVolumeData.cpp
        src/MetrologyReportWriter.cpp
        src/MetrologyReportFile.cpp
        src/AlignmentData.h src/AlignmentData.cpp
        src/AlignmentOverlay.h src/AlignmentOverlay.cpp
        src/MetrologyAlignmentHostFeature.cpp
    PUBLIC FILE_SET api_headers TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES include/Host/MetrologyAlignmentHostFeature.h include/Host/MetrologyAlignmentHostTypes.h include/Host/MetrologyReport.h include/Host/VoxelVolume.h)
target_include_directories(metrology_alignment
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(metrology_alignment PUBLIC MVVCVTK::FeatureSPI
    PRIVATE opencv_world bcrypt MVVCVTKInternal::FeatureSupport VTK::CommonCore
        VTK::CommonDataModel VTK::RenderingCore VTK::RenderingAnnotation)
SetTargetDefaults(metrology_alignment)
set_target_properties(metrology_alignment PROPERTIES
    OUTPUT_NAME MetrologyAlignment PROJECT_LABEL MetrologyAlignment FOLDER "MVVCVTK/Features")
