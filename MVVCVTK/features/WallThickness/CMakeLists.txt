set(_wall_headers
    include/Host/WallThicknessHostFeature.h
    include/Host/WallThicknessHostTypes.h)
add_library(wall_thickness STATIC)
add_library(MVVCVTK::WallThickness ALIAS wall_thickness)
option(MVVCVTK_WALL_CUDA "Build the CUDA-only repository WallThickness engine" ON)
if(NOT MVVCVTK_WALL_CUDA)
    message(FATAL_ERROR "WallThickness uses CUDA only. Select the vs2026-x64-cuda preset; no CPU wall engine is provided.")
endif()
if(MVVCVTK_WALL_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit 13.2 REQUIRED)
    add_library(wall_thickness_cuda STATIC src/ThicknessCudaPath.cu src/ThicknessCudaPath.h src/ThicknessPositivePath.h
        src/ThicknessCudaMath.h src/ThicknessCudaMaterialPath.h src/ThicknessCudaExactSign.h src/ThicknessCudaExactPolynomial.h)
    set_target_properties(wall_thickness_cuda PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES 89 CUDA_RUNTIME_LIBRARY Shared MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        FOLDER "MVVCVTK/Features/Private")
    target_compile_options(wall_thickness_cuda PRIVATE --fmad=false --expt-relaxed-constexpr --resource-usage
        -Xcompiler=/fp:strict -Xcompiler=/utf-8
        "-Xcompiler=$<IF:$<CONFIG:Debug>,/MDd,/MD>")
    target_link_libraries(wall_thickness_cuda PRIVATE CUDA::cudart)
    find_file(_wall_cudart NAMES cudart64_13.dll PATHS "${CUDAToolkit_BIN_DIR}/x64" "${CUDAToolkit_BIN_DIR}" NO_DEFAULT_PATH REQUIRED)
    # 只部署到本工作树仓内程序的运行目录；不加入 SDK install/export。
    add_custom_target(wall_thickness_cuda_runtime
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_wall_cudart}" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        VERBATIM)
    add_dependencies(wall_thickness_cuda wall_thickness_cuda_runtime)
    target_link_libraries(wall_thickness PRIVATE wall_thickness_cuda)
    target_compile_definitions(wall_thickness PRIVATE MVVCVTK_HAS_WALL_CUDA=1)
endif()
target_sources(wall_thickness PRIVATE
    src/ThicknessAlgorithm.h src/ThicknessAlgorithm.cpp
    src/ThicknessMath.h src/ThicknessDisplaySampling.h src/ThicknessGrayField.h src/ThicknessProfile.h src/ThicknessPositivePath.h src/ThicknessEvaluation.cpp
    src/ThicknessData.h src/ThicknessData.cpp
    src/ThicknessOverlay.h src/ThicknessOverlay.cpp
    src/WallThicknessHostFeature.cpp
    PUBLIC FILE_SET api_headers TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include" FILES ${_wall_headers})
target_include_directories(wall_thickness
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(wall_thickness PUBLIC MVVCVTK::FeatureSPI MVVCVTK::HostAPI
    PRIVATE MVVCVTKInternal::FeatureSupport VTK::CommonCore VTK::CommonDataModel
        VTK::FiltersCore VTK::FiltersGeneral VTK::RenderingCore VTK::RenderingAnnotation)
SetTargetDefaults(wall_thickness)
target_compile_options(wall_thickness PRIVATE /fp:strict)
set_target_properties(wall_thickness PROPERTIES OUTPUT_NAME WallThickness
    PROJECT_LABEL WallThickness FOLDER "MVVCVTK/Features")
unset(_wall_headers)
