# MVVCVTK

MVVCVTK 是 Windows x64 上的 C++17 / VTK 体数据可视化宿主。`VtkAppHostSession` 是组合根，负责多视图运行时、加载事务、输入路由、可信 Feature 和统一停止；Crop 与 GapAnalysis 作为可选 Feature 接入，不取得 App 内部对象身份。

## 支持范围

- Windows x64；仓库和 SDK 均不提供 Win32/x86 配置。
- CMake 4.2 或更高版本、Visual Studio 18 2026、MSVC `v145`、C++17。
- 日常源码构建固定使用 Visual Studio 2026 的 MSVC `v145`；Release 制品启用 AVX2。
- Debug 使用 `/MDd`，Release 使用 `/MD`。
- VTK 9.4.2、OpenCV 4.12.0、Qt 5.14.2，以及授权的内部 UI 依赖。
- SDK 是固定工具链静态库，不承诺跨编译器、工具集、运行库或配置的稳定二进制 ABI。

## 依赖 bootstrap

依赖 bundle 不进入 Git。受控制品服务器保存 `2026.08.21-deps.1-win-x64` ZIP，CI 通过 `MVVCVTK_DEPS_URI` secret 下载；本地也可使用已有归档：

```powershell
./tools/Get-MVVCVTKDependencies.ps1 -Uri $env:MVVCVTK_DEPS_URI
./tools/Get-MVVCVTKDependencies.ps1 -ArchivePath .\artifacts\MVVCVTK-Dependencies.zip
./tools/Get-MVVCVTKDependencies.ps1 -VerifyOnly
```

脚本先解包到独立临时目录，拒绝 reparse point，并按规范化相对路径、文件大小和每文件 SHA-256 重算内容树。只有内容树、manifest SHA-256、包版本和平台全部匹配 [依赖锁](tools/MVVCVTK.Dependencies.lock.psd1) 后，才安装到 `deps/2026.08.21-deps.1-win-x64`。更换依赖必须发布新版本并显式更新锁，不能原地替换同名制品。

`MVVCVTK/vcpkg.json` 的公共依赖使用固定 git baseline；项目构建所需的 VTK/OpenCV/Qt/内部 UI 以以上版本化 bundle 为准。

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

`MVVCVTK::Host`、`MVVCVTK::OrthogonalCrop` 和 `MVVCVTK::GapAnalysis` 分别生成独立静态库，不提供把三者固化在一起的 SDK 聚合 target。`examples/standalone/main.cpp` 显式组合这三个模块并生成默认可启动的 `MVVCVTK` 应用，只负责本地演示和运行，不进入 SDK 安装。Qt 只进入 Qt 测试，不是 Host 的公共依赖。

`MVVCVTK_BUILD_ORTHOGONAL_CROP`、`MVVCVTK_BUILD_GAP_ANALYSIS` 可分别关闭两个 Feature；`MVVCVTK_BUILD_STANDALONE` 控制示例。Standalone 只有在两个 Feature 都启用时构建，关闭任一 Feature 都不会把示例的组合选择反向施加给 Host 或另一个 Feature。

仓库不维护手写 `.sln`、生产 `.vcxproj` 或测试 `.vcxproj`。`cmake --preset vs2026-x64` 是唯一工程生成入口，Visual Studio 2026 generator 会在 `out/build/vs2026-x64/MVVCVTK.slnx` 生成现代 Solution 文件及其项目。Solution 顶层只保留一个 `MVVCVTK` 组，下面包含默认启动的 `Application/MVVCVTK`、主干 `Host`、Host API/Feature SPI 接口目标、位于 `Host/Features` 的两个扩展库、边界与行为测试以及 CMake 内建目标；三库项目显示各自完整的实现源码和私有头，但 SDK 安装仍只取批准的公开头与单独维护的物理闭包。静态库统一输出到 `lib/<配置>`，应用和测试程序统一输出到 `bin/<配置>`。Windows CI 应在带桌面 OpenGL、v145 和内部依赖访问权的受控 runner 上运行 Debug/Release 全套测试。

## SDK 构建与验证

```powershell
./tools/release/Build-MVVCVTKSdk.ps1
```

发布脚本复用同一个 CMake preset 构建并测试 Debug/Release，安装三个静态库，复制锁定依赖，再以源码树中的 CMake clean-room consumer 验证安装包。版本与 manifest 属于发布阶段，不进入日常 Solution，也不创建第二棵产品 CMake build tree。

stage 输出到 `out/stage/<version>-win-x64`，ZIP 输出到 `out/packages`；当前不生成归档 checksum，也不为 SDK 文件维护哈希闭包。直接执行 `cmake --install` 只安装项目模块和 CMake 消费接口，不复制锁定依赖；这种本地安装可在配置 consumer 时显式设置 `MVVCVTK_DEPS_ROOT` 复用仓库依赖。对外交付的自包含 SDK 仍由发布脚本把依赖放到安装根的 `deps/`。

SDK 根目录只保留 `manifest.json`、`README.md`、`NOTICE`、`include/`、`lib/` 和 `deps/`；CMake package 位于 `lib/cmake/MVVCVTK/`。`lib/<配置>/` 每个配置只含 `MVVCVTKHost.lib`、`MVVCVTKOrthogonalCrop.lib`、`MVVCVTKGapAnalysis.lib`。安装包不携带 standalone、clean-room 示例、MSBuild/qmake 适配层或身份校验工具。

脏工作树版本必须使用 `yyyy.MM.dd-rev.N`；干净工作树版本由目标解析为 `yyyy.MM.dd-git.<short-commit>`。不传 `-PackageRevision` 时脚本按工作树状态生成合法版本。SDK manifest 只记录版本来源、固定工具链、模块、依赖版本和公开头清单，不承担文件完整性或身份校验。

## SDK 消费

CMake consumer 使用 Windows x64、MSVC `v145`，指向安装目录中的包配置并按需选择模块。当前日常 CMake 消费只解析导出 target 和同目录 `deps/`，不要求 SDK 版本 manifest；版本、归档闭包与制品身份校验留在发布阶段：

```cmake
find_package(MVVCVTK CONFIG REQUIRED
    COMPONENTS HostAPI FeatureSPI Host OrthogonalCrop GapAnalysis)
target_link_libraries(app PRIVATE
    MVVCVTK::Host
    MVVCVTK::OrthogonalCrop
    MVVCVTK::GapAnalysis)
```

只编译 Host/Feature 头契约的 target 可分别链接 `MVVCVTK::HostAPI` 与
`MVVCVTK::FeatureSPI`；二者都是自足的纯接口目标，不反向链接 `MVVCVTKHost.lib`。
具体 Feature 只公开依赖 `FeatureSPI`；Host 实现公开依赖 `HostAPI`，并在内部使用
`FeatureSPI`。应用按实际功能显式组合 `Host`、`OrthogonalCrop`、`GapAnalysis`，不通过顶层 SDK target 继承 standalone 的组合选择。

## SDK 公开头边界

SDK 的语义入口只有四个：`Host/VtkAppHostSession.h`、`Host/HostFeature.h`、`Host/CropHostFeature.h`、`Host/GapHostFeature.h`。安装目录共 21 个批准头；除入口外还包括消费者请求/读取 DTO、Feature SPI，以及入口公开签名所需的物理闭包，它们不是额外顶层模块。`HostAPI`、`FeatureSPI` 和两个 Feature 产品 target 在 build tree 中分别只暴露各自 staging include 根，Feature 产品不能访问 Host 私有 include，也不能把自身 Algorithm、Service、Router 或具体渲染策略泄漏给 consumer。共享的 `FeatureVisualStrategy` 是 Host/Gap 的内部实现支持，不安装到 SDK。源码 `include/` 整树不属于 SDK 暴露面。

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

本仓库当前没有授予项目源码或二进制的通用再分发许可证；获得明确授权前，不应对外分发。版本化依赖 bundle 和 SDK 内保留 Qt、OpenCV、VTK 及其传递依赖的许可证文件。内部 UI 组件标记为 `internal-only`，其使用和再分发必须另行授权。详见 [NOTICE](NOTICE)。
