# MVVCVTK

MVVCVTK 是 Windows x64 上基于 C++17 / VTK 的体数据可视化与分析宿主。本实验分支以 `VtkAppHostSession` 为组合根，提供多视图、数据加载与修订、渲染、输入路由和会话生命周期；业务能力通过可选 Feature 接入。当前仓内交互入口是 Qt 功能工作台 `QtFeatureManual`。

## 架构与模块

应用负责创建 Session、选择 Feature 并调用公共入口；Host 不链接具体 Feature，Feature 之间互不依赖，也不包含 Host 私有头。

| 层次 | CMake target | 职责 |
| --- | --- | --- |
| Host API | `MVVCVTK::HostAPI` | Session、请求、结果、状态快照与数据读取等公开契约；接口 target，不链接 Host 实现库 |
| Feature SPI（扩展接口） | `MVVCVTK::FeatureSPI` | `HostFeature`、可信数据、视图、输入、叠加与渲染扩展契约；接口 target |
| Host 实现 | `MVVCVTK::Host` | 组合数据、窗口、渲染与生命周期；公开依赖 HostAPI，内部使用 FeatureSPI |
| 仓内共享支持 | `MVVCVTKInternal::FeatureSupport` | Host / Feature 私有使用的构建期支持，不安装、不导出 |
| 外部宿主 | 仓内示例为 `qt_feature_manual` | 提供 Qt 窗口、事件循环和操作界面，显式组合 Host 与所选 Feature |

Feature 公开依赖 FeatureSPI；PartSegmentation、ModelRotation、WallThickness 还公开依赖 HostAPI。具体依赖以各模块 CMake 为准。Qt 不进入 Host / Feature 的公共接口或 SDK 部署；DefX 仅由 GapAnalysis 的私有 `GapKernel.dll` bridge 使用。

当前 `MVVCVTK/features/` 接入九个 Feature，下表是源码能力概览，不表示全部已进入 SDK 或完成业务验收。target 别名均为 `MVVCVTK::<模块名>`。

| 模块 | 主要能力 | 构建开关 |
| --- | --- | --- |
| OrthogonalCrop | 交互裁剪、裁剪结果与历史 | `MVVCVTK_BUILD_ORTHOGONAL_CROP` |
| GapAnalysis | 基于 DefX 的缺陷／间隙分析及结果显示 | `MVVCVTK_BUILD_GAP_ANALYSIS` |
| PartSegmentation | 零件分割、编辑、拆分、合并与历史 | `MVVCVTK_BUILD_PART_SEGMENTATION` |
| SurfaceDetermination | 全局自动表面测定、预览与正式表面结果 | `MVVCVTK_BUILD_SURFACE_DETERMINATION` |
| ArtifactReduction | 扩散降噪、环状伪影处理及候选提交 | `MVVCVTK_BUILD_ARTIFACT_REDUCTION` |
| ModelRotation | 模型显示旋转、取消与撤销 | `MVVCVTK_BUILD_MODEL_ROTATION` |
| MetrologyAlignment | 计量配准、参考导入、偏差评估与检测 | `MVVCVTK_BUILD_METROLOGY_ALIGNMENT` |
| WallThickness | 基于源灰度、标签与测量表面的壁厚计算及公差评估 | `MVVCVTK_BUILD_WALL_THICKNESS` |
| RoiEditing | ROI（感兴趣区域）交互绘制、编辑与组合操作 | `MVVCVTK_BUILD_ROI_EDITING` |

Host 提供体渲染、等值面和正交切片等显示能力，应用通过公共请求调整视图参数。显示质量与业务数据处理分开；Feature 通过公共入口读取输入、提交结果并管理自己的显示资源。

## 支持范围与依赖

- Windows x64、CMake 4.2 或更高版本、Visual Studio 18 2026、MSVC `v145`、C++17。
- Debug 使用 `/MDd`，Release 使用 `/MD`；当前日常 preset 启用 Release AVX2。
- 锁定依赖为 VTK 9.4.2、OpenCV 4.12.0；仓内 Qt 工作台和 Qt 测试使用 Qt 5.14.2。
- SDK 为固定工具链静态库，不承诺跨编译器、工具集、运行库或配置的稳定二进制兼容。

依赖 bundle 不进入 Git，版本与内容身份由[依赖锁](tools/MVVCVTK.Dependencies.lock.psd1)记录。在仓库根执行，按来源选择下载或本地归档，再校验：

```powershell
# 从受控制品服务器获取（先设置 MVVCVTK_DEPS_URI）
./tools/Get-MVVCVTKDependencies.ps1 -Uri $env:MVVCVTK_DEPS_URI
# 或使用已有归档；路径替换为实际文件
./tools/Get-MVVCVTKDependencies.ps1 -ArchivePath .\artifacts\MVVCVTK-Dependencies.zip
./tools/Get-MVVCVTKDependencies.ps1 -VerifyOnly
```

脚本校验 manifest 和内容树后安装到 `deps/official`。日常 preset 的 Qt 工作台／测试需要该目录中的 Qt；SDK 只复制锁定 VTK/OpenCV，接收方自行提供 Qt 本体及其部署。

启用 GapAnalysis 时，另需 DefX 开发环境。`MVVCVTK_DEFX_ROOT` 默认指向 `deps/third_party/defx`，也可通过同名环境变量或 CMake cache 指定外部根：头文件在 `include/`，Debug/Release DLL 在 `bin/<配置>/`，导入库在 `lib/<配置>/`。这些输入不由官方依赖下载脚本获取；两套工件必须分别匹配工具链、运行库与 VTK 配置。

## 构建和测试

在仓库根设置临时目录并使用 CMake preset：

```powershell
New-Item -ItemType Directory -Force .\out\tmp | Out-Null
$env:TEMP = $env:TMP = (Resolve-Path .\out\tmp).Path
cmake --preset vs2026-x64
cmake --open out/build/vs2026-x64
cmake --build --preset vs2026-debug
cmake --build --preset vs2026-release
ctest --preset vs2026-debug
ctest --preset vs2026-release
```

`vs2026-x64` 开启上述九个 Feature 中除 WallThickness 以外的八个，并开启 `MVVCVTK_BUILD_TESTING`、`MVVCVTK_BUILD_QT_TESTING` 和 `MVVCVTK_BUILD_QT_MANUAL`。这与未使用 preset 时的原始 option 默认值不同：原始默认只开启 OrthogonalCrop、GapAnalysis 和普通测试。

各 Feature 可通过表中开关独立选择；`MVVCVTK_BUILD_QT_MANUAL` 控制工作台，`MVVCVTK_BUILD_QT_TESTING` 控制仓内 Qt 自动化测试，后者要求 `MVVCVTK_BUILD_TESTING=ON`。Host 始终构建，不依赖某个固定 Feature 组合。

生成的解决方案为 `out/build/vs2026-x64/MVVCVTK.slnx`，启用工作台时启动项目为 QtFeatureManual。Host、Feature、测试分别归入相应 IDE 分组；接口 target 不等同于独立实现库。静态库输出到构建目录的 `lib/<配置>`，应用和测试程序输出到 `bin/<配置>`。仓库不维护手写的解决方案或生产项目文件。

上述是执行入口，不代表在当前机器或本次文档修订中已运行。具体测试条件与验收范围见[测试规范](md/development/TESTING.md)。

## Qt 功能工作台

```powershell
cmake --build --preset vs2026-release --target qt_feature_manual
.\out\build\vs2026-x64\RunQtFeatureManual-Release.cmd
```

入口源码为 `test/QtHost/Manual/main.cpp`，生成程序为 `bin/Release/QtFeatureManual.exe`；启动脚本设置匹配的依赖路径和 Qt plugin 路径。当前树已没有旧 `examples/standalone/main.cpp`、`MVVCVTK_BUILD_STANDALONE` 或 `MVVCVTK.exe --quality-audit` 入口。

工作台显示一个 3D 视窗和三个正交切片视窗，可切换体渲染／等值面，通过功能页调用已启用的模块。使用“打开体数据…”（`Ctrl+O`）或拖入文件后，先核对输入尺寸、间距等参数再加载。场景、结果目录、参数和操作日志由工作台管理；业务命令使用 Feature 公共入口。

SurfaceDetermination 当前公开的测定方法为 `SurfaceDeterminationMethod::GlobalAutomatic`；预览与正式测定是不同用途。壁厚页使用同一输入的零件标签与有效正式测量表面，阈值和长度单位应与源数据一致。

普通 preset 默认不构建壁厚。仓内 CUDA 配置 `vs2026-x64-cuda` 开启 WallThickness 及 `MVVCVTK_WALL_CUDA`，当前预设指向 CUDA 13.2 安装路径，使用前需具备该环境：

```powershell
cmake --preset vs2026-x64-cuda
cmake --build --preset vs2026-cuda-release --target qt_feature_manual
.\out\build\vs2026-x64-cuda\RunQtFeatureManual-Release.cmd
```

运行自动化前，先用对应 build preset 构建全部目标或 `qt_feature_tests`。Qt 自动化入口示例为 `ctest --preset vs2026-release -R '^QtFeature\.Automation$'`；壁厚配置对应 `vs2026-cuda-release` 测试 preset。合成用例只证明其覆盖的路径，真实业务与大数据性能需另行验收。

## SDK 配置与消费

仓内模块集合与安装包集合不同。当前实验分支的 package 声明 `Host`、`OrthogonalCrop`、`GapAnalysis`、`MetrologyAlignment` 四个 component；两个 SDK preset 只开启这四个模块，关闭 Qt 工作台和 Qt 测试，其余六个 Feature 仅接入仓内构建。接口 target HostAPI / FeatureSPI 随包导出，FeatureSupport 不安装或导出，不提供固定组合的 SDK 聚合 target。

这段说明反映当前分支的构建配置，不代替 SDK 准入授权或交付验收。正式允许交付的集合、相对主干的差异与验证要求见[SDK 交付规范](md/development/SDK_DELIVERY.md)。

SDK 构建入口如下；执行前须完成该规范要求的范围、输入和授权核对：

```powershell
./tools/release/Build-MVVCVTKSdk.ps1
# 可选：覆盖 DefX 开发环境根
./tools/release/Build-MVVCVTKSdk.ps1 -DefXRoot D:\third_party\defx
```

脚本默认使用 `vs2026-sdk-tested-x64`，依次执行 Debug/Release 构建、CTest、安装、闭包检查、仓库外 clean-room 消费验证和 ZIP 打包。`vs2026-sdk-products-x64` 供 `-SkipTests` 诊断流程使用。可选真实 C3 VTP 由 `-MetrologyMesh` 传入，未传入时该项为 `NOT_RUN`；不能把边界检查或脚本完成等同于所有真实业务验收通过。

stage 位于 `out/stage/<revision>-win-x64`，ZIP 位于 `out/packages/`。默认 revision 在干净树上为 `yyyy.MM.dd-git.<12位短提交>`，脏树上为 `yyyy.MM.dd-rev.<HHmmss>`；脏树产物只能报告为诊断状态。

当前安装配置的主要布局如下；实际交付仍按批准清单核对：

| 路径 | 内容 |
| --- | --- |
| `include/MVVCVTK/API/` | Host 与已安装 Feature 的公开入口及必要类型 |
| `include/MVVCVTK/SPI/` | 可信 Feature 扩展契约及物理闭包 |
| `lib/<Debug或Release>/` | `MVVCVTKHost.lib`、`OrthogonalCrop.lib`、`GapAnalysis.lib`、`MetrologyAlignment.lib`（对应模块启用时） |
| `lib/cmake/MVVCVTK/` | package 配置及导出 targets |
| `deps/official/{vtk,opencv}` | 发布脚本复制的锁定官方依赖 |
| `deps/third_party/defx/bin/<Debug或Release>/` | `GapKernel.dll` 与 `DefXAnalysis.dll` |

直接 `cmake --install` 不复制 VTK/OpenCV，完整依赖布局由发布脚本生成。SDK 不包含 Qt 本体、工作台、测试、真实数据、私有实现头或项目说明文档。VTK bundle 内的 QVTK 适配模块不等于 Qt 本体。

消费者使用 Windows x64 / MSVC `v145`，把 `MVVCVTK_DIR` 指向安装根的 `lib/cmake/MVVCVTK`，按实际安装与业务需要选择 component，例如：

```cmake
find_package(MVVCVTK CONFIG REQUIRED COMPONENTS Host OrthogonalCrop GapAnalysis)
target_link_libraries(app PRIVATE
    MVVCVTK::Host
    MVVCVTK::OrthogonalCrop
    MVVCVTK::GapAnalysis)
```

需要计量配准时另请求 `MetrologyAlignment` component 并链接 `MVVCVTK::MetrologyAlignment`；仅使用 Host 时只请求 `Host` component 并链接 `MVVCVTK::Host`。包配置解析 VTK/OpenCV，不将 DefX 开发头、导入库或 target 传播给消费者。

运行含 GapAnalysis 的程序时，把 `MVVCVTK_GAP_RUNTIME_DIR` 指向当前配置的 `deps/third_party/defx/bin/<配置>` 绝对目录，其中须同时存在 `GapKernel.dll` 与 `DefXAnalysis.dll`；旧名 `MVVCVTKGapKernel.dll` 已不使用。加载器不从 PATH 或当前工作目录发现私有运行时。VTK/OpenCV 的匹配运行时仍需可被程序找到，Qt 则由宿主部署。

## 宿主接入与生命周期

`VtkAppHostSession` 通过 `BuildSession` 建立会话，通过 `AttachFeature` / `DetachFeature` 接入和移除业务。外部使用请求、结果、状态快照和受生命周期约束的 endpoint，不取得 Host 私有 AppRuntime 或 Context。

Qt 宿主使用外部事件循环；当前工作台的 `QtHostPump` 使用 `HostDriven` 模式，分别调用 `SendUpdates` 提交更新、`SendRender` 绘制，不调用面向原生 VTK 循环的 `Start()`。Session 构建、Feature 绑定、渲染写入和显式 `Stop` 在 owner thread（所属线程）执行，跨线程任务通过配置的 `sendOwnerTask` 投递。停止失败保留可重试状态，不能当作已停止。

普通图像读取返回不暴露 VTK 对象身份或可写 scalar 指针的值数据；可信 Feature 通过 SPI 取得受约束的数据与渲染能力。Feature 是同一信任域内的进程内扩展，不是沙箱。接口用法与边界另见[Qt 接入指南](md/report/QtIntegrationGuide.md)和[Feature 开发规范](md/development/FEATURE_DEVELOPMENT.md)，具体行为以当前公开头与源码为准。

## 许可证与第三方通知

本仓库当前没有授予项目源码或二进制的通用再分发许可证；获得明确授权前，不应对外分发。最小定向 SDK 不安装 README 或 NOTICE，相关授权与第三方依赖合规由交付双方另行确认。详见 [NOTICE](NOTICE)。
