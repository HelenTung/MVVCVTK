# MVVCVTK

MVVCVTK 是 Windows x64 上的 C++17 / VTK 体数据可视化宿主。`VtkAppHostSession` 是组合根，负责多视图运行时、加载事务、输入路由、可信 Feature 和统一停止；Crop 与 GapAnalysis 作为可选 Feature 接入，不取得 App 内部对象身份。

## 支持范围

- Windows x64；仓库和 SDK 均不提供 Win32/x86 配置。
- MSVC `v145`、Windows SDK 10.0、C++17。
- Debug 使用 `/MDd`，Release 使用 `/MD`。
- VTK 9.4.2、OpenCV 4.12.0、Qt 5.14.2，以及授权的内部 UI 依赖。
- SDK 是固定工具链静态库，不承诺跨编译器、工具集、运行库或配置的稳定二进制 ABI。

## 依赖 bootstrap

依赖 bundle 不进入 Git。受控制品服务器保存 `2026.08.21-deps.1-win-x64` ZIP，CI 通过 `MVVCVTK_DEPS_URI` secret 下载；本地也可使用已有归档：

```powershell
./tools/deps/Get-MVVCVTKDependencies.ps1 -Uri $env:MVVCVTK_DEPS_URI
./tools/deps/Get-MVVCVTKDependencies.ps1 -ArchivePath D:\artifacts\MVVCVTK-Dependencies.zip
./tools/deps/Get-MVVCVTKDependencies.ps1 -VerifyOnly
```

脚本先解包到独立临时目录，拒绝 reparse point，并按规范化相对路径、文件大小和每文件 SHA-256 重算内容树。只有内容树、manifest SHA-256、包版本和平台全部匹配 [依赖锁](tools/deps/MVVCVTK.Dependencies.lock.psd1) 后，才安装到 `sdk/deps/2026.08.21-deps.1-win-x64`。更换依赖必须发布新版本并显式更新锁，不能原地替换同名制品。

`MVVCVTK/vcpkg.json` 的公共依赖使用固定 git baseline；项目构建所需的 VTK/OpenCV/Qt/内部 UI 以以上版本化 bundle 为准。

## 构建和测试

安装依赖后，在 Developer PowerShell 中执行：

```powershell
msbuild MVVCVTK.sln /m /t:Rebuild /p:Configuration=Debug /p:Platform=x64
msbuild MVVCVTK.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=x64
msbuild test/MVVCVTK.Tests.sln /m /t:Rebuild /p:Configuration=Debug /p:Platform=x64
msbuild test/MVVCVTK.Tests.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=x64
msbuild test/QtHost/QtHostMethodTests.vcxproj /m /t:Rebuild /p:Configuration=Debug /p:Platform=x64
msbuild test/QtHost/QtHostSessionSmoke.vcxproj /m /t:Rebuild /p:Configuration=Debug /p:Platform=x64
```

测试可执行文件位于各测试目录的 `x64/<Configuration>`。Windows CI 在带桌面 OpenGL、v145 和内部依赖访问权的受控 runner 上构建并运行 Debug/Release 全套测试。

## SDK 构建与验证

```powershell
msbuild MVVCVTK/MVVCVTK.vcxproj /m:1 /t:VerifyCleanRoom /p:Platform=x64 /p:MVVCVTKSdkVersion=2026.08.24-rev.1
```

该目标依次构建 Debug/Release 静态库、生成 manifest、校验所有 artifact 与依赖哈希、打包 ZIP、在隔离目录逐个编译 staged public header，并构建运行 Debug/Release clean-room consumer。consumer 会实现外部自定义 Feature，并真实调用 `GetFeaturePort`、`GetOverlayPort`、`GetImageReadResult`、`SetActiveViews` 和 `AttachInput`。归档及 `.sha256` 输出到 `sdk/packages/`；CI 仅发布本次验证生成的两项制品。

脏工作树版本必须使用 `yyyy.MM.dd-rev.N`；干净工作树版本由目标解析为 `yyyy.MM.dd-git.<short-commit>`。SDK manifest 同时记录基准提交、dirty 状态、工具集、运行库、依赖闭包和可信策略。

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
