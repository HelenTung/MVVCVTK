add_executable(
    mvvcvtk_host_frame_tests
    HostFrameCoordinatorTests.cpp
    "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Host/HostFrameCoordinator.cpp"
)

set_target_properties(
    mvvcvtk_host_frame_tests
    PROPERTIES
        OUTPUT_NAME HostFrameCoordinatorTests
        PROJECT_LABEL HostFrameCoordinatorTests
        FOLDER "MVVCVTK/test/Automation"
)

target_link_libraries(
    mvvcvtk_host_frame_tests
    PRIVATE
        mvvcvtk_host_test_internal
        MVVCVTK::FeatureSPI
        ${MVVCVTK_VTK_TARGETS}
)

SetTargetDefaults(mvvcvtk_host_frame_tests)
vtk_module_autoinit(
    TARGETS mvvcvtk_host_frame_tests
    MODULES ${MVVCVTK_VTK_TARGETS}
)

add_test(
    NAME Host.FrameCoordinator
    COMMAND mvvcvtk_host_frame_tests
)
SetTestRuntime(Host.FrameCoordinator mvvcvtk_host_frame_tests)

add_executable(mvvcvtk_host_runtime_tests HostRuntimeTests.cpp DataTransitionTests.cpp ResourceLifetimeTests.cpp AutoDisplayDefaultsTests.cpp)
target_link_libraries(mvvcvtk_host_runtime_tests PRIVATE
    mvvcvtk_host_test_internal MVVCVTK::Host ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(mvvcvtk_host_runtime_tests)
set_target_properties(mvvcvtk_host_runtime_tests PROPERTIES PROJECT_LABEL HostRuntimeTests FOLDER "MVVCVTK/test/Automation")
vtk_module_autoinit(TARGETS mvvcvtk_host_runtime_tests MODULES ${MVVCVTK_VTK_TARGETS})
add_test(NAME Host.Runtime COMMAND mvvcvtk_host_runtime_tests)
SetTestRuntime(Host.Runtime mvvcvtk_host_runtime_tests)

add_test(NAME Host.DataTransition COMMAND mvvcvtk_host_runtime_tests transition)
SetTestRuntime(Host.DataTransition mvvcvtk_host_runtime_tests)

add_test(NAME Host.ResourceLifetime COMMAND mvvcvtk_host_runtime_tests resources)
SetTestRuntime(Host.ResourceLifetime mvvcvtk_host_runtime_tests)

add_test(NAME Host.AutoDisplayDefaults COMMAND mvvcvtk_host_runtime_tests defaults)
SetTestRuntime(Host.AutoDisplayDefaults mvvcvtk_host_runtime_tests)

# 显式真实数据审计；仅依赖 Host，不随 Part Feature 开关消失，不安装。
# 运行：HostLoadAudit raw sourceSide [Low|High|Ultra] [--range-audit] [--quality-audit]
#       [--geometry spacingMm originX originY originZ] [--output directory]
#       [--view-size pixels] [--frames count] [--timeout-seconds count]。
# raw 须为无文件头的 float32 立方 RAW，sourceSide 为 2..8192，文件恰为 sourceSide^3 * 4 字节。
# 默认 Ultra、等距 spacingMm=0.1537、LPS 原点 (0,0,0)；--geometry 接受正有限间距和三个有限原点分量。
# 默认两个审计开关关闭、视图边长 256 像素（>0）、稳态帧数 0（>=0）、加载等待上限 300 秒（>0）。
# --output 可选，指定时创建目录并输出 report.json 和四视图 PNG；省略时仅输出控制台结果。
# 需要可用的 Windows OpenGL；手动运行时须能从 PATH 等 DLL 搜索位置找到锁定的 VTK runtime。
# 工具不读取专用数据环境变量；缺必需参数、RAW 不存在/尺寸不符或审计失败时输出 [FAIL] 并返回 1，成功返回 0。
# 不注册为缺数据即跳过的常规 CTest，运行时须显式提供真实数据。
add_executable(host_load_audit HostLoadAudit.cpp)
target_link_libraries(host_load_audit PRIVATE MVVCVTK::Host ${MVVCVTK_VTK_TARGETS} VTK::IOImage Psapi dxgi)
SetTargetDefaults(host_load_audit)
set_target_properties(host_load_audit PROPERTIES
    OUTPUT_NAME HostLoadAudit PROJECT_LABEL HostLoadAudit FOLDER "MVVCVTK/test/Automation")
vtk_module_autoinit(TARGETS host_load_audit MODULES ${MVVCVTK_VTK_TARGETS})

add_executable(mvvcvtk_ruler_tests RulerTests.cpp RulerStateTests.cpp)
target_link_libraries(mvvcvtk_ruler_tests PRIVATE
    mvvcvtk_host_test_internal MVVCVTK::Host ${MVVCVTK_VTK_TARGETS})
SetTargetDefaults(mvvcvtk_ruler_tests)
set_target_properties(mvvcvtk_ruler_tests PROPERTIES
    PROJECT_LABEL RulerTests FOLDER "MVVCVTK/test/Automation")
vtk_module_autoinit(TARGETS mvvcvtk_ruler_tests MODULES ${MVVCVTK_VTK_TARGETS})
foreach(ruler_case IN ITEMS metrics render host state)
    add_test(NAME Ruler.${ruler_case} COMMAND mvvcvtk_ruler_tests ${ruler_case})
    SetTestRuntime(Ruler.${ruler_case} mvvcvtk_ruler_tests)
endforeach()

add_executable(atomic_file_output_tests AtomicFileOutputTests.cpp "${PROJECT_SOURCE_DIR}/MVVCVTK/src/Data/Internal/AtomicFileOutput.cpp")
target_link_libraries(atomic_file_output_tests PRIVATE mvvcvtk_host_test_internal)
SetTargetDefaults(atomic_file_output_tests)
set_target_properties(atomic_file_output_tests PROPERTIES
    PROJECT_LABEL AtomicFileOutputTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME Host.AtomicFileOutput COMMAND atomic_file_output_tests)
SetTestRuntime(Host.AtomicFileOutput atomic_file_output_tests)

# 不补 vtk_module_autoinit，验证 Host 静态库的最小消费路径。
add_executable(host_unload_tests HostUnloadTests.cpp)
target_link_libraries(host_unload_tests PRIVATE MVVCVTK::Host MVVCVTK::FeatureSPI)
SetTargetDefaults(host_unload_tests)
set_target_properties(host_unload_tests PROPERTIES
    PROJECT_LABEL HostUnloadTests FOLDER "MVVCVTK/test/Automation")
add_test(NAME Host.Unload COMMAND host_unload_tests)
SetTestRuntime(Host.Unload host_unload_tests)
