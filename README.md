# MVVCVTK

MVVCVTK 是 Windows x64 上的 C++17 / VTK 体数据可视化宿主。`VtkAppHostSession` 是组合根，负责多视图运行时、加载事务、输入路由、可信 Feature 和统一停止；Crop 与 GapAnalysis 作为可选 Feature 接入，不取得 App 内部对象身份。

Host 只保留一个真实 scalar 坐标的 `HostVolumeTransferFunction` 完整快照入口；颜色与透明度节点独立，reload 和 LOD 不重解释显式节点。同一数据版本的默认 percentile histogram 在 Session 内只扫描一次，各 View 仍保存独立 TF 值快照。Volume 质量为 `Auto/Low/High/XHigh/Ultra` 五档；`Low/High/XHigh/Ultra` 固定使用原始 dimensions 的 `25%/50%/75%/100%`，仅 `Auto` 在模型加载时依据系统内存、GPU mapper 预算和 CPU 核心数确定一次，运行期不按帧耗重新定档。非原生 LOD 直接持有已物化 resample output，不再追加整卷 `DeepCopy`。

## 支持范围

- Windows x64；仓库和 SDK 均不提供 Win32/x86 配置。
- CMake 4.2 或更高版本、Visual Studio 18 2026、MSVC `v145`、C++17。
- 日常源码构建固定使用 Visual Studio 2026 的 MSVC `v145`；Release 制品启用 AVX2。
- Debug 使用 `/MDd`，Release 使用 `/MD`。
- SDK 携带 VTK 9.4.2、OpenCV 4.12.0，以及仅供 GapAnalysis 私有 bridge 使用的 DefX Debug/Release 运行时；Qt 5.14.2 的头、库、DLL 与 CMake package 由接收方宿主自行提供。完整 VTK 环境保留 QVTK/VTK Qt 适配模块，用于对接宿主 Qt。
- SDK 是固定工具链静态库，不承诺跨编译器、工具集、运行库或配置的稳定二进制 ABI。

## 依赖 bootstrap

依赖 bundle 不进入 Git。受控制品服务器保存官方依赖 ZIP，具体版本与内容身份由仓库依赖锁记录；CI 通过 `MVVCVTK_DEPS_URI` secret 下载，本地也可使用已有归档：

```powershell
./tools/Get-MVVCVTKDependencies.ps1 -Uri $env:MVVCVTK_DEPS_URI
./tools/Get-MVVCVTKDependencies.ps1 -ArchivePath .\artifacts\MVVCVTK-Dependencies.zip
./tools/Get-MVVCVTKDependencies.ps1 -VerifyOnly
```

脚本先解包到独立临时目录，拒绝 reparse point，并按规范化相对路径、文件大小和每文件 SHA-256 重算内容树。只有内容树、manifest SHA-256、包身份和平台全部匹配 [依赖锁](tools/MVVCVTK.Dependencies.lock.psd1) 后，才安装到官方库总目录 `deps/official`。版本仅属于校验元数据，不再进入目录名；更换依赖必须显式更新锁。

`MVVCVTK/vcpkg.json` 的公共依赖使用固定 git baseline；项目构建所需的 VTK/OpenCV 以及仓库 Qt 测试使用的 Qt 以 `deps/official` 下的锁定 bundle 为准。内部 UI 不再属于产品构建或 SDK 依赖。

GapAnalysis 的 DefX 作为外部三方环境依赖放在 `MVVCVTK_DEFX_ROOT`（preset 默认 `deps/third_party/defx`）：供应商头位于 `include/`，Debug/Release 的 DLL 位于 `bin/<配置>/`，导入库位于 `lib/<配置>/`。Feature 源码树和仓库根临时 `gap/` 均不作为构建输入；中央依赖模块将工件封装为私有 `DefX::Analysis` 目标。供应商 C++/STL/VTK 接口只在 Feature 内的 `MVVCVTKGapKernel.dll` 兼容层消费，GapAnalysis 主库只接收固定宽度 POD、原始标签和区域/header 副本；兼容层不执行阈值、腐蚀、连通域、过滤或重编号。两套供应商工件必须分别匹配 x64、MSVC 运行库、iterator debug level 和 VTK 9.4.2 配置，禁止跨配置回退。

源码构建只把 DefX DLL 复制到生成目录，与私有 kernel 组成运行时对，不改动三方依赖根中的原始文件。GapAnalysis 加载器忽略当前工作目录和 PATH 查找：SDK/外部宿主必须用 `MVVCVTK_GAP_RUNTIME_DIR` 指定当前配置的私有运行时绝对目录；未配置时只接受应用程序同目录中的运行时对。选定后使用绝对路径加载 kernel，并核对实际 bridge 与 DefX 均来自该目录；显式目录非法时不会回退。

受控 CI runner 可通过仓库变量 `MVVCVTK_DEFX_ROOT` 指向预装的只读 DefX 根；未配置时使用工作区的 `deps/third_party/defx`。CI 会在配置前核对上述六个工件，第三方内容不由官方依赖下载脚本获取。

## 构建和测试

安装依赖后，以 CMake 作为主构建入口：

```powershell
cmake --preset vs2026-x64
cmake --open out/build/vs2026-x64
cmake --build --preset vs2026-debug
cmake --build --preset vs2026-release
ctest --preset vs2026-debug
ctest --preset vs2026-release
```

`MVVCVTK::Host`、`MVVCVTK::OrthogonalCrop` 和 `MVVCVTK::GapAnalysis` 分别生成独立静态库，不提供把三者固化在一起的 SDK 聚合 target。`examples/standalone/main.cpp` 显式组合这三个模块并生成默认可启动的 `MVVCVTK` 应用，只负责本地演示和运行，不进入 SDK 安装。Qt 本体只进入由 `MVVCVTK_BUILD_QT_TESTING` 控制的仓库测试，不是 Host 的公共依赖，也不进入 SDK；`deps/official/vtk` 中的 VTK Qt 适配模块仍随完整 VTK 环境交付。

`MVVCVTK_BUILD_ORTHOGONAL_CROP`、`MVVCVTK_BUILD_GAP_ANALYSIS` 可分别关闭两个 Feature；`MVVCVTK_BUILD_STANDALONE` 控制示例。Standalone 只有在两个 Feature 都启用时构建，关闭任一 Feature 都不会把示例的组合选择反向施加给 Host 或另一个 Feature。

Standalone 加载数据后可按 `g` 显式发送一次 `GapHostAction::Start`，用于触发当前 Gap/DefX 主链；该入口仍走 `GapHostFeature::SendRequest`，不直接调用私有算法。原有 `j` 键继续由 Gap Feature 自身处理 Start/Overlay 切换。

仓库不维护手写 `.sln`、生产 `.vcxproj` 或测试 `.vcxproj`。`cmake --preset vs2026-x64` 是唯一工程生成入口，Visual Studio 2026 generator 会在 `out/build/vs2026-x64/MVVCVTK.slnx` 生成现代 Solution 文件及其项目。Solution 顶层只保留一个 `MVVCVTK` 组，下面包含默认启动的 `Application/MVVCVTK`、主干 `Host`、Host API/Feature SPI 接口目标、位于 `Host/Features` 的两个扩展库、边界与行为测试以及 CMake 内建目标；三库项目显示各自完整的实现源码和私有头，但 SDK 安装仍只取批准的公开头与单独维护的物理闭包。静态库统一输出到 `lib/<配置>`，应用和测试程序统一输出到 `bin/<配置>`。Windows CI 应在带桌面 OpenGL、v145 和内部依赖访问权的受控 runner 上运行 Debug/Release 全套测试。

## SDK 构建与验证

```powershell
./tools/release/Build-MVVCVTKSdk.ps1
# 可选：覆盖 preset 的 DefX 三方环境根
./tools/release/Build-MVVCVTKSdk.ps1 -DefXRoot D:\third_party\defx
```

发布脚本复用同一个 CMake preset 构建并测试 Debug/Release，安装三个静态库及 GapAnalysis 私有的分配置 bridge/DefX 运行时，白名单复制 VTK/OpenCV，再在仓库外动态生成一次性 CMake clean-room probe，验证安装态组件矩阵、公开头闭包、私有头隔离和两种配置的消费链接。DefX 实际分析链由仓内 Debug/Release Gap/QtHost 测试覆盖；clean-room probe 只存在于临时验证目录，不作为仓库源码，也不进入 SDK。

stage 输出到 `out/stage/<version>-win-x64`，ZIP 输出到 `out/packages`；当前不生成归档 checksum 或 manifest。直接执行 `cmake --install` 安装项目模块、CMake 消费接口和 GapAnalysis 私有运行时，不复制 VTK/OpenCV；完整定向交付由发布脚本把 VTK/OpenCV 放到安装根的 `deps/official`。

SDK 根目录只保留 `include/`、`lib/` 和 `deps/`；官方依赖位于 `deps/official/{vtk,opencv}`，三方运行时位于 `deps/third_party/defx`，DefX 供应商头文件和导入库不进入 SDK。CMake package 位于 `lib/cmake/MVVCVTK/`；`lib/<配置>/` 每个配置只含 `MVVCVTKHost.lib`、`MVVCVTKOrthogonalCrop.lib`、`MVVCVTKGapAnalysis.lib`，`MVVCVTKGapKernel.dll` 与 `DefXAnalysis.dll` 只位于 `deps/third_party/defx/bin/<配置>`。安装包不携带 Qt 本体、内部 UI、README、NOTICE、manifest、standalone、clean-room 示例、MSBuild/qmake 适配层或验证工具；完整 VTK 目录中的 QVTK/VTK Qt 模块不视为 Qt 本体。

脏工作树目录名使用 `yyyy.MM.dd-rev.N`；干净工作树目录名使用 `yyyy.MM.dd-git.<short-commit>`。不传 `-PackageRevision` 时脚本按工作树状态生成合法目录名和 ZIP 名，包内不再保存独立版本文件。

## SDK 消费

CMake consumer 使用 Windows x64、MSVC `v145`，指向安装目录中的包配置并按需选择三个产品模块。包配置只解析导出 target 与 VTK/OpenCV；DefX target、头文件和导入库不进入 consumer CMake 图：

```cmake
find_package(MVVCVTK CONFIG REQUIRED
    COMPONENTS Host OrthogonalCrop GapAnalysis)
target_link_libraries(app PRIVATE
    MVVCVTK::Host
    MVVCVTK::OrthogonalCrop
    MVVCVTK::GapAnalysis)
```

运行含 GapAnalysis 的程序时，将 `MVVCVTK_GAP_RUNTIME_DIR` 设为当前配置的 `deps/third_party/defx/bin/<配置>` 绝对目录，并把 `deps/official` 下匹配配置的 VTK/OpenCV runtime 目录加入 PATH。私有运行时目录必须同时包含 `MVVCVTKGapKernel.dll` 与 `DefXAnalysis.dll`；GapAnalysis 不从 PATH 或当前工作目录发现这两个 DLL。

只编译 Host/Feature 头契约的 target 可分别链接 `MVVCVTK::HostAPI` 与
`MVVCVTK::FeatureSPI`；二者都是自足的纯接口目标，不反向链接 `MVVCVTKHost.lib`。
具体 Feature 只公开依赖 `FeatureSPI`；Host 实现公开依赖 `HostAPI`，并在内部使用
`FeatureSPI`。应用按实际功能显式组合 `Host`、`OrthogonalCrop`、`GapAnalysis`，不通过顶层 SDK target 继承 standalone 的组合选择。

## SDK 公开头边界

SDK 的顶层业务入口是 `Host/VtkAppHostSession.h`、`Host/HostFeature.h`、`Host/CropHostFeature.h`、`Host/GapHostFeature.h`；完整的 Host API、Feature SPI、Feature 入口和物理支持闭包由同一份 CMake 声明生成。`MVVCVTKHeaderSurface.txt` 只留在构建树供发布验证使用，不进入 SDK。`HostAPI`、`FeatureSPI` 和两个 Feature 产品 target 在 build tree 中分别只暴露各自 staging include 根，Feature 产品不能访问 Host 私有 include，也不能把自身 Algorithm、Service、Router 或具体渲染策略泄漏给 consumer。`FeatureOverlayBase` 只作为仓内构建支持，不安装到 SDK；Feature SPI 也不携带 `AppTypes`、`RenderParams` 或主体 `VisualStrategy`。源码 `include/` 整树不属于 SDK 暴露面。

## 线程与停止规则

- `BuildSession`、Feature/Timer/Hotkey 绑定、渲染写入和 `Stop` 属于 Session owner thread。
- Qt 宿主应在 `HostSessionConfig::sendOwnerTask` 中把任务投递到 owner event loop。非 owner 析构只会登记拥有型 StopPending entry；不会在错误线程销毁 VTK 对象。
- owner dispatcher 拒绝或抛异常时，Session 仍由 reaper 持有，可在 owner thread 调用 `SendPendingStops()` 重试。token 防止延迟回调停止后来创建的新 Session。
- 后台加载、导出和普通图像读取使用固定 worker 与有界队列。取消是 C++17 协作式 stop token；任务必须在切片、行和写入边界观察取消。
- Session Stop 先广播取消，再使用同一绝对 deadline 等待。超时会保留完整 runtime aggregate 并进入可重试状态，不 detach 线程、不泄漏子对象，也不伪报已停止。
- 已接纳异步回调只在 owner timer 上消费；未接纳请求不取得 callback。

## 图像读取与信任边界

普通调用者只包含 `Data/ImageReadTypes.h`。同步读取支持相对源图像的半开 region；chunk 每次最多复制 8 MiB，并通过 region 内 x-fast voxel offset 续读；`StartImageRead` 在固定 executor 上复制不可变快照并在 owner timer 回调。所有尺寸、字节数、region 边界和 offset 都在分配前检查。

外部 Feature 属于 `trusted-in-process` 插件，不是沙箱或安全隔离边界。它可以通过 `TrustedFeatureDataPort` 取得 VTK-backed 不可变快照并执行基于 snapshot identity/version 的 CAS 发布，因此只能加载同一信任域、同一固定 ABI 构建的代码。普通 Session 读取 DTO 不包含 VTK identity 或可写 scalar 指针；只读端口与可信写入端口是不同能力。

## 许可证与第三方通知

本仓库当前没有授予项目源码或二进制的通用再分发许可证；获得明确授权前，不应对外分发。最小定向 SDK 不安装 README 或 NOTICE，相关授权与第三方依赖合规仍由交付双方另行确认。仓库内说明详见 [NOTICE](NOTICE)。
