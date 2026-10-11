set(MVVCVTK_SURFACE_API_HEADERS
    include/SurfaceRecipe.h
    include/Host/SurfaceDeterminationHostFeature.h
    include/Host/SurfaceDeterminationHostTypes.h
)
set(MVVCVTK_SURFACE_SOURCES
    src/SurfaceSeedBuilder.h
    src/SurfaceSeedBuilder.cpp
    src/SurfaceEdgeTopology.h
    src/SurfaceCudaTypes.h
    src/SurfaceCudaCompute.h
    src/SurfaceCudaCompute.cpp
    src/SurfaceGpuFinalizer.h
    src/SurfaceBackendCost.h
    src/SurfaceRecipeCodec.cpp
    src/SurfaceContracts.h
    src/SurfaceContracts.cpp
    src/SurfaceDeterminationAlgorithm.h
    src/SurfaceDeterminationAlgorithm.cpp
    src/SurfaceDeterminationHostFeature.cpp
    src/SurfaceDeterminationService.h
    src/SurfaceDeterminationService.cpp
    src/SurfaceGenerationStore.h
    src/SurfaceGenerationStore.cpp
    src/SurfaceOverlayStrategy.h
    src/SurfaceOverlayStrategy.cpp
)

add_library(surface_determination STATIC)
add_library(
    MVVCVTK::SurfaceDetermination
    ALIAS surface_determination
)
target_sources(
    surface_determination
    PRIVATE ${MVVCVTK_SURFACE_SOURCES}
    PUBLIC
        FILE_SET api_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES ${MVVCVTK_SURFACE_API_HEADERS}
)
target_include_directories(
    surface_determination
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
    surface_determination
    PUBLIC MVVCVTK::FeatureSPI
    PRIVATE
        MVVCVTKInternal::FeatureSupport
        VTK::CommonCore
        VTK::CommonDataModel
        VTK::FiltersCore
        VTK::FiltersGeneral
        VTK::RenderingCore
)
SetTargetDefaults(surface_determination)
target_link_libraries(surface_determination PRIVATE Psapi)
option(MVVCVTK_SURFACE_CPU_PARALLEL "Plan request-local CPU sampling from available cores, with headroom" ON)
option(MVVCVTK_SURFACE_COMPONENT_PARALLEL "Dynamically schedule bounded Surface components within the shared CPU quota" ON)
option(MVVCVTK_SURFACE_GPU_FINALIZE_PARALLEL "Overlap bounded CPU sample finalization with the Surface CUDA owner" OFF)
option(MVVCVTK_SURFACE_ADAPTIVE_BACKEND "Experiment with bounded CPU/GPU cost probes and reuse computed records" OFF)
target_compile_definitions(surface_determination PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN MVVCVTK_SURFACE_CPU_PARALLEL=$<BOOL:${MVVCVTK_SURFACE_CPU_PARALLEL}>
    MVVCVTK_SURFACE_COMPONENT_PARALLEL=$<BOOL:${MVVCVTK_SURFACE_COMPONENT_PARALLEL}>
    MVVCVTK_SURFACE_GPU_FINALIZE_PARALLEL=$<BOOL:${MVVCVTK_SURFACE_GPU_FINALIZE_PARALLEL}>
    MVVCVTK_SURFACE_ADAPTIVE_BACKEND=$<BOOL:${MVVCVTK_SURFACE_ADAPTIVE_BACKEND}>)

set(MVVCVTK_SURFACE_SEED_CACHE_MIB 16 CACHE STRING "Bound for optional seed classification workspace in MiB; 0 disables")
if(NOT MVVCVTK_SURFACE_SEED_CACHE_MIB MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "MVVCVTK_SURFACE_SEED_CACHE_MIB must be a non-negative integer")
endif()
string(LENGTH "${MVVCVTK_SURFACE_SEED_CACHE_MIB}" _surface_seed_cache_digits)
if(_surface_seed_cache_digits GREATER 13 OR
   (_surface_seed_cache_digits EQUAL 13 AND MVVCVTK_SURFACE_SEED_CACHE_MIB STRGREATER "8796093022207"))
    message(FATAL_ERROR "MVVCVTK_SURFACE_SEED_CACHE_MIB exceeds the 64-bit byte limit")
endif()
math(EXPR _surface_seed_cache_bytes "${MVVCVTK_SURFACE_SEED_CACHE_MIB} * 1048576")
target_compile_definitions(surface_determination PRIVATE
    MVVCVTK_SURFACE_SEED_CACHE_BYTES=${_surface_seed_cache_bytes})

option(MVVCVTK_SURFACE_CUDA "Automatically discover an optional private CUDA compute backend" ON)
option(MVVCVTK_SURFACE_CUDA_SLAB_REUSE "Reuse overlapping Surface CUDA voxel slices without extra device buffers" ON)
set(MVVCVTK_SURFACE_CUDA_DEVICE_MIB 128 CACHE STRING "Bound for Surface CUDA device work buffers in MiB")
set(MVVCVTK_SURFACE_CUDA_HOST_MIB 512 CACHE STRING "Released host-memory credit required before optional CUDA initialization in MiB")
if(MVVCVTK_SURFACE_CUDA)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND AND CUDAToolkit_NVCC_EXECUTABLE AND TARGET CUDA::cuda_driver)
        if(NOT MVVCVTK_SURFACE_CUDA_DEVICE_MIB MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "MVVCVTK_SURFACE_CUDA_DEVICE_MIB must be a positive integer")
        endif()
        if(NOT MVVCVTK_SURFACE_CUDA_HOST_MIB MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "MVVCVTK_SURFACE_CUDA_HOST_MIB must be a positive integer")
        endif()
        math(EXPR _surface_device_bytes "${MVVCVTK_SURFACE_CUDA_DEVICE_MIB} * 1048576")
        math(EXPR _surface_host_bytes "${MVVCVTK_SURFACE_CUDA_HOST_MIB} * 1048576")
        set(_surface_ptx "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/SurfaceCudaKernels.ptx")
        set(_surface_ptx_header "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/SurfaceCudaPtx.h")
        # 由 FindCUDAToolkit 选择 nvcc；不依赖特定版本的 VS CUDA props。
        add_custom_command(OUTPUT "${_surface_ptx}" "${_surface_ptx_header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
            COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}" --ptx --std=c++17
                --fmad=false --ftz=false --prec-div=true --prec-sqrt=true
                -Xcompiler /utf-8
                -ccbin "${CMAKE_CXX_COMPILER}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/SurfaceCudaKernels.cu" -o "${_surface_ptx}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${_surface_ptx}" "-DOUTPUT=${_surface_ptx_header}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedCudaPtx.cmake"
            DEPENDS src/SurfaceCudaKernels.cu src/SurfaceCudaTypes.h EmbedCudaPtx.cmake
            VERBATIM)
        add_library(surface_cuda_backend SHARED src/SurfaceCudaBackend.cpp "${_surface_ptx_header}")
        if(MVVCVTK_BUILD_TESTING)
            target_compile_definitions(surface_cuda_backend PRIVATE MVVCVTK_SURFACE_CUDA_TESTING=1)
            target_compile_definitions(surface_determination PRIVATE MVVCVTK_SURFACE_CUDA_TESTING=1)
        endif()
        target_include_directories(surface_cuda_backend PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
        target_sources(surface_cuda_backend PRIVATE src/SurfaceCudaKernels.cu src/SurfaceCudaTypes.h)
        set_source_files_properties(src/SurfaceCudaKernels.cu PROPERTIES HEADER_FILE_ONLY TRUE)
        target_link_libraries(surface_cuda_backend PRIVATE CUDA::cuda_driver Psapi)
        SetTargetDefaults(surface_cuda_backend)
        target_compile_definitions(surface_cuda_backend PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
            MVVCVTK_SURFACE_CUDA_SLAB_REUSE=$<BOOL:${MVVCVTK_SURFACE_CUDA_SLAB_REUSE}>)
        target_compile_options(surface_cuda_backend PRIVATE /fp:strict)
        # NVIDIA 的 driver import lib 带 LIBCMT 默认项；本项目固定使用 /MD[d]。
        target_link_options(surface_cuda_backend PRIVATE /NODEFAULTLIB:LIBCMT)
        set_target_properties(surface_cuda_backend PROPERTIES OUTPUT_NAME SurfaceCudaBackend
            FOLDER "MVVCVTK/Features/SurfaceDetermination"
            # 核函数由独立 nvcc 命令生成 PTX，DLL 本身仅编译 C++；不继承随后启用的壁厚 CUDA 架构。
            CUDA_ARCHITECTURES OFF)
        add_dependencies(surface_determination surface_cuda_backend)
        target_compile_definitions(surface_determination PRIVATE
            MVVCVTK_SURFACE_CUDA_ENABLED=1 MVVCVTK_SURFACE_CUDA_DEVICE_BYTES=${_surface_device_bytes}
            MVVCVTK_SURFACE_CUDA_HOST_BYTES=${_surface_host_bytes})
        message(STATUS "Surface CUDA: discovered Toolkit ${CUDAToolkit_VERSION}; device work bound ${MVVCVTK_SURFACE_CUDA_DEVICE_MIB} MiB")
    else()
        message(STATUS "Surface CUDA: Toolkit unavailable, CPU backend retained")
    endif()
endif()
set_target_properties(
    surface_determination
    PROPERTIES
        OUTPUT_NAME SurfaceDetermination
        PROJECT_LABEL SurfaceDetermination
        FOLDER "MVVCVTK/Features"
)
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        ${MVVCVTK_SURFACE_API_HEADERS}
        ${MVVCVTK_SURFACE_SOURCES}
)

unset(MVVCVTK_SURFACE_API_HEADERS)
unset(MVVCVTK_SURFACE_SOURCES)
