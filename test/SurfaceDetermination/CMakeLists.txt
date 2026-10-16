add_executable(
    mvvcvtk_surface_determination_tests
    SurfaceDeterminationAlgorithmTests.cpp
    SurfaceDeterminationHistogramTests.cpp
    SurfaceHistogramAudit.cpp
    SurfaceHistogramSessionTests.cpp
    SurfaceDeterminationOptimizationTests.cpp
    SurfaceDeterminationBusinessTests.cpp
    SurfaceDeterminationContractTests.cpp
    SurfaceDeterminationDisplayTests.cpp
    SurfaceDeterminationGeometryTests.cpp
    SurfaceDeterminationLifecycleTests.cpp
    SurfaceObjectInterleavingTests.cpp
    SurfaceDeterminationTestCases.h
    SurfaceDeterminationTestSupport.h
    SurfaceMeasurementSerialization.h
    SurfaceDeterminationTests.cpp
)

# 该 probe 不取得 Host 私有测试 include，专门证明仓内公共头只依赖
# MVVCVTK::SurfaceDetermination 的传递 include/link surface。
add_library(
    mvvcvtk_surface_header_probe
    OBJECT SurfaceDeterminationHeaderProbe.cpp
)
target_link_libraries(
    mvvcvtk_surface_header_probe
    PRIVATE MVVCVTK::SurfaceDetermination
)
SetTargetDefaults(mvvcvtk_surface_header_probe)
set_target_properties(
    mvvcvtk_surface_header_probe
    PROPERTIES PROJECT_LABEL SurfaceDeterminationHeaderProbe FOLDER "MVVCVTK/test/Automation/HeaderProbes"
)
set_target_properties(
    mvvcvtk_surface_determination_tests
    PROPERTIES
        OUTPUT_NAME SurfaceDeterminationTests
        PROJECT_LABEL SurfaceDeterminationTests
        FOLDER "MVVCVTK/test/Automation"
)
target_include_directories(
    mvvcvtk_surface_determination_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/include"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/src"
)
target_link_libraries(
    mvvcvtk_surface_determination_tests
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::Host
        MVVCVTK::SurfaceDetermination
        MVVCVTKInternal::FeatureSupport
        ${MVVCVTK_VTK_TARGETS}
)
SetTargetDefaults(mvvcvtk_surface_determination_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_surface_determination_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

foreach(_mvvcvtk_surface_suite IN ITEMS algorithm histogram geometry lifecycle display seed-window optimization object-retirement object-owner-wait object-wakeup)
    add_test(
        NAME "SurfaceDetermination.${_mvvcvtk_surface_suite}"
        COMMAND mvvcvtk_surface_determination_tests ${_mvvcvtk_surface_suite}
    )
    SetTestRuntime(
        "SurfaceDetermination.${_mvvcvtk_surface_suite}"
        mvvcvtk_surface_determination_tests
    )
endforeach()
unset(_mvvcvtk_surface_suite)

# 复用现有测试工程承载真实灰度采样审计；说明检查不计为真实数据业务验收。
add_test(NAME SurfaceDetermination.histogram-audit-schema
    COMMAND mvvcvtk_surface_determination_tests --histogram-audit-schema)
SetTestRuntime(SurfaceDetermination.histogram-audit-schema mvvcvtk_surface_determination_tests)
set(MVVCVTK_SURFACE_SESSION_AUDIT_ARGS "" CACHE STRING "Optional real Host Surface session audit arguments")
if(MVVCVTK_SURFACE_SESSION_AUDIT_ARGS)
    add_test(NAME SurfaceDetermination.real-histogram-session
        COMMAND mvvcvtk_surface_determination_tests --real-histogram-session ${MVVCVTK_SURFACE_SESSION_AUDIT_ARGS})
    SetTestRuntime(SurfaceDetermination.real-histogram-session mvvcvtk_surface_determination_tests)
endif()

# 同一审计入口分别链接原始基线与候选，实现数值、资源及端到端对照。
add_executable(mvvcvtk_surface_acceleration_audit SurfaceAccelerationAudit.cpp)
target_include_directories(mvvcvtk_surface_acceleration_audit PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/src")
target_link_libraries(mvvcvtk_surface_acceleration_audit PRIVATE
    mvvcvtk_host_test_internal MVVCVTK::Host MVVCVTK::SurfaceDetermination
    MVVCVTKInternal::FeatureSupport ${MVVCVTK_VTK_TARGETS} Psapi)
SetTargetDefaults(mvvcvtk_surface_acceleration_audit)
set_target_properties(mvvcvtk_surface_acceleration_audit PROPERTIES
    OUTPUT_NAME SurfaceAccelerationAudit FOLDER "MVVCVTK/test/Automation")
vtk_module_autoinit(TARGETS mvvcvtk_surface_acceleration_audit MODULES ${MVVCVTK_VTK_TARGETS})

# 可选只读参考源必须由已核实提交提取；独立 target/object，不覆盖当前 Feature 实现。
set(MVVCVTK_SURFACE_REFERENCE_DIR "" CACHE PATH "Read-only exact original Surface algorithm sources for measurement audit")
if(MVVCVTK_SURFACE_REFERENCE_DIR)
    foreach(_source SurfaceDeterminationAlgorithm.cpp SurfaceDeterminationAlgorithm.h SurfaceSeedBuilder.cpp SurfaceSeedBuilder.h)
        if(NOT EXISTS "${MVVCVTK_SURFACE_REFERENCE_DIR}/${_source}")
            message(FATAL_ERROR "Surface reference source missing: ${_source}")
        endif()
    endforeach()
    add_library(mvvcvtk_surface_reference_algorithm STATIC
        "${MVVCVTK_SURFACE_REFERENCE_DIR}/SurfaceDeterminationAlgorithm.cpp"
        "${MVVCVTK_SURFACE_REFERENCE_DIR}/SurfaceSeedBuilder.cpp"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/src/SurfaceContracts.cpp"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/src/SurfaceRecipeCodec.cpp")
    target_include_directories(mvvcvtk_surface_reference_algorithm PRIVATE
        "${MVVCVTK_SURFACE_REFERENCE_DIR}"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/include"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/src")
    target_link_libraries(mvvcvtk_surface_reference_algorithm PRIVATE
        MVVCVTK::FeatureSPI MVVCVTKInternal::FeatureSupport ${MVVCVTK_VTK_TARGETS})
    target_compile_definitions(mvvcvtk_surface_reference_algorithm PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    SetTargetDefaults(mvvcvtk_surface_reference_algorithm)
    set_target_properties(mvvcvtk_surface_reference_algorithm PROPERTIES
        PROJECT_LABEL SurfaceReferenceAlgorithm FOLDER "MVVCVTK/test/Automation/Reference")
    add_executable(mvvcvtk_surface_reference_audit SurfaceAccelerationAudit.cpp)
    target_include_directories(mvvcvtk_surface_reference_audit BEFORE PRIVATE
        "${MVVCVTK_SURFACE_REFERENCE_DIR}"
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/include")
    target_link_libraries(mvvcvtk_surface_reference_audit PRIVATE
        mvvcvtk_surface_reference_algorithm mvvcvtk_host_test_internal MVVCVTK::Host
        MVVCVTKInternal::FeatureSupport ${MVVCVTK_VTK_TARGETS} Psapi)
    target_compile_definitions(mvvcvtk_surface_reference_audit PRIVATE MVVCVTK_SURFACE_REFERENCE_AUDIT=1)
    SetTargetDefaults(mvvcvtk_surface_reference_audit)
    set_target_properties(mvvcvtk_surface_reference_audit PROPERTIES
        OUTPUT_NAME SurfaceOriginalMeasurementAudit FOLDER "MVVCVTK/test/Automation")
    vtk_module_autoinit(TARGETS mvvcvtk_surface_reference_audit MODULES ${MVVCVTK_VTK_TARGETS})
endif()
if(TARGET surface_cuda_backend)
    add_executable(mvvcvtk_surface_cuda_precision_tests SurfaceCudaPrecisionTests.cpp)
    math(EXPR _surface_test_host_bytes "${MVVCVTK_SURFACE_CUDA_HOST_MIB} * 1048576")
    math(EXPR _surface_test_device_bytes "${MVVCVTK_SURFACE_CUDA_DEVICE_MIB} * 1048576")
    target_compile_definitions(mvvcvtk_surface_cuda_precision_tests PRIVATE
        MVVCVTK_SURFACE_TEST_HOST_BYTES=${_surface_test_host_bytes}
        MVVCVTK_SURFACE_TEST_DEVICE_BYTES=${_surface_test_device_bytes}
        MVVCVTK_SURFACE_TEST_SLAB_REUSE=$<BOOL:${MVVCVTK_SURFACE_CUDA_SLAB_REUSE}>)
    target_include_directories(mvvcvtk_surface_cuda_precision_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/src")
    target_link_libraries(mvvcvtk_surface_cuda_precision_tests PRIVATE MVVCVTK::SurfaceDetermination)
    SetTargetDefaults(mvvcvtk_surface_cuda_precision_tests)
    set_target_properties(mvvcvtk_surface_cuda_precision_tests PROPERTIES
        PROJECT_LABEL SurfaceCudaPrecisionTests FOLDER "MVVCVTK/test/Automation")
    add_test(NAME SurfaceDetermination.cuda-precision COMMAND mvvcvtk_surface_cuda_precision_tests)
    set_tests_properties(SurfaceDetermination.cuda-precision PROPERTIES SKIP_RETURN_CODE 77)
    SetTestRuntime(SurfaceDetermination.cuda-precision mvvcvtk_surface_cuda_precision_tests)
endif()

# Pure business header probe deliberately has no Host/VTK/Qt include or link dependency.
add_library(mvvcvtk_surface_recipe_header_probe OBJECT SurfaceRecipeHeaderProbe.cpp)
target_include_directories(mvvcvtk_surface_recipe_header_probe PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/include")
SetTargetDefaults(mvvcvtk_surface_recipe_header_probe)
set_target_properties(mvvcvtk_surface_recipe_header_probe PROPERTIES
    PROJECT_LABEL SurfaceRecipeHeaderProbe FOLDER "MVVCVTK/test/Automation/HeaderProbes")

# Repository acceptance tool; file I/O stays in the test harness, never in the Feature.
add_executable(mvvcvtk_surface_business_acceptance SurfaceBusinessAcceptance.cpp)
set_target_properties(mvvcvtk_surface_business_acceptance PROPERTIES
    OUTPUT_NAME SurfaceBusinessAcceptance PROJECT_LABEL SurfaceBusinessAcceptance FOLDER "MVVCVTK/test/Automation")
target_include_directories(mvvcvtk_surface_business_acceptance PRIVATE
    "${PROJECT_SOURCE_DIR}/MVVCVTK/features/SurfaceDetermination/src")
target_link_libraries(mvvcvtk_surface_business_acceptance PRIVATE mvvcvtk_host_test_internal
    MVVCVTK::Host MVVCVTK::SurfaceDetermination MVVCVTKInternal::FeatureSupport ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(mvvcvtk_surface_business_acceptance)
vtk_module_autoinit(TARGETS mvvcvtk_surface_business_acceptance MODULES ${MVVCVTK_VTK_TARGETS})
add_test(NAME SurfaceDetermination.acceptance-schema COMMAND mvvcvtk_surface_business_acceptance --describe)
SetTestRuntime(SurfaceDetermination.acceptance-schema mvvcvtk_surface_business_acceptance)
set(MVVCVTK_SURFACE_ACCEPTANCE_MANIFEST "" CACHE FILEPATH "Optional traceable real RAW acceptance manifest")
if(MVVCVTK_SURFACE_ACCEPTANCE_MANIFEST)
    add_test(NAME SurfaceDetermination.real-input COMMAND mvvcvtk_surface_business_acceptance "${MVVCVTK_SURFACE_ACCEPTANCE_MANIFEST}")
    SetTestRuntime(SurfaceDetermination.real-input mvvcvtk_surface_business_acceptance)
endif()
