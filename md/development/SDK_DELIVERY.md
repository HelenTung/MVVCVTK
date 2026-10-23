# MVVCVTK SDK 交付规范

> 适用于 SDK 的构建、验证、封装、交接，以及公开契约、安装和运行时闭包变化。
>
> 总规则见根 `AGENTS.md`，Feature 接入见 `FEATURE_DEVELOPMENT.md`，通用测试与结果判定见 `TESTING.md`。

## 1. 文档目标

SDK 按批准白名单交付。本文件规定边界核对、正式流程、准入准出和停止条件，不以现有代码、工具建议或文档修订追认边界变化。普通开发按 `TESTING.md` 第 3.2 节定向验证，SDK 相关变更按本文第 15 节分级；正式交付执行第 11 至 13 节的完整门禁。

## 2. 事实来源与解释顺序

### 2.1 实际实现从哪里核对

先读当前源码和 CMake，再查发布脚本及测试；取证顺序见根 `AGENTS.md` 第 4 节。重点入口如下：

```text
CMakePresets.json、cmake/MVVCVTKBuildPolicy.cmake
MVVCVTK/CMakeLists.txt、相关 Feature 的 CMakeLists.txt
cmake/MVVCVTKConfig.cmake.in、MVVCVTKInternalDependencies.cmake.in
cmake/MVVCVTKInstall.cmake、tools/MVVCVTK.Dependencies.lock.psd1
test/SdkBoundary/
tools/release/Build-MVVCVTKSdk.ps1
tools/release/Test-MVVCVTKSdk.ps1
tools/release/Pack-MVVCVTKSdk.ps1
```

### 2.2 事实冲突如何处理

文档与实现不一致时，分别核实实际行为和授权。已批准的差异按授权范围实现、验证；未批准或无法确认的差异阻止正式交付，不能直接改预期让它通过。文档维护按根 `AGENTS.md` 第 7 节，分支和合并按 `GIT_WORKFLOW.md`。保留未知代码和差异，只暂停需要该授权的交付部分。

### 2.3 已批准白名单与候选差异

**已批准 SDK 白名单按本轮核实的主分支提交中的以下位置核对**，主分支定义见 `GIT_WORKFLOW.md` 第 1.1 节：

- `MVVCVTK/CMakeLists.txt` 的 `SetSdkHeaderMap` 调用：公开物理头及安装路径。
- `tools/release/Test-MVVCVTKSdk.ps1` 的预期清单：产品库、`lib/cmake` 文件、顶层目录、deps 目录和 DefX runtime 文件；其产品分类与对应 package component 一起核对。

components 的实际声明还要核对本轮核实的主分支提交的 `cmake/MVVCVTKConfig.cmake.in`，同时检查 Test 脚本是否有单独的 component 预期集合；header category 不能当作 component 授权。

任何分支对上述位置及对应 component、导出规则的改动，都是需要授权的 SDK 边界变化。审查相对本轮核实的主分支提交的完整 diff，逐项对照仍有效的具体授权，包含继承来的变化。本次允许交付的集合是本轮核实的主分支提交中的白名单加上明确批准的差异；未获准入的仓内内容不进入该集合。数量相同、代码已存在或脚本成功都不能代替授权核对。

实验分支的 SDK 面可能与本轮核实的主分支提交不同（例如新增的 component、头映射、preset 或安装态测试链）。交付或审查时，把这些差异单独列出，并分别核对产品 component 声明和 install 的 `COMPONENT` 标签；差异不因已存在于代码中而视为已获授权。

`Test-MVVCVTKSdk.ps1` 的预期头集合来自构建生成的 `MVVCVTKHeaderSurface.txt`，与 CMake 声明同源。它能发现安装偏差，不能独立证明头映射变化已获授权。

## 3. SDK 边界授权的项目细则

### 3.1 白名单原则

SDK 边界变更的独立授权门禁见 md/development/core/LOOP.core.md 第 14.1 节；本节定义项目边界范围及核对方法。

具体名单以第 2.3 节所指本轮核实的主分支提交中的清单及获批差异为准，既不能多也不能缺。文件在 `include/`、target 可安装或 Feature 仓内可编译，都不等于允许交付。

### 3.2 什么属于 SDK 边界变化

下列对象的新增、删除、重命名、迁移或对外语义改变，都需要核对具体授权：

- package component、导出 target、产品库、DLL、公开物理头及分类。
- 公开类型、字段、枚举、方法、默认值、callback、所有权、线程和生命周期。
- PUBLIC 依赖、consumer 查找的包、编译要求、runtime、安装目录和附加文件。
- 平台、架构、生成器、工具集、运行库、C++ 标准、指令集和配置。

减少边界也是变化，不能以内部清理为由删除受支持的公开能力。

### 3.3 哪些话不构成准入

开发 Feature、修复编译、补齐 CMake、优化、模块化、商用化或方便接收方，都不自动授权 SDK 扩张。其他 Feature 已交付，也不能推导新 Feature 已获准入。

### 3.4 获批后如何落实

给出最小旧/新差异，涵盖 component、target、头与符号、依赖、runtime 和消费者影响。授权须足以判断允许交付什么，同任务内仍有效的具体授权可沿用。

### 3.5 授权内仍保持最小

只实现批准的变化，不顺带增加便利 API、预留字段、公开实现类型、兼容层或附加交付物；不为修编译把私有依赖改成 PUBLIC。

## 4. 当前产品、组件与 target 边界

### 4.1 产品 components

下表为产品 component 示例；实际交付时按第 2.3 节重新核对本轮核实的主分支提交的预期清单和对应 package 声明：

| Component | 产品 target | 制品 |
| --- | --- | --- |
| `Host` | `MVVCVTK::Host` | `MVVCVTKHost.lib` |
| `OrthogonalCrop` | `MVVCVTK::OrthogonalCrop` | `MVVCVTKOrthogonalCrop.lib` |
| `GapAnalysis` | `MVVCVTK::GapAnalysis` | `MVVCVTKGapAnalysis.lib` |

### 4.2 接口 target

`MVVCVTK::HostAPI`、`MVVCVTK::FeatureSPI` 是随 package 导出的自足接口 target，不是产品 component，也不反向链接 Host 实现库。

### 4.3 不导出的内部内容

不新增 `MVVCVTK::SDK`、`MVVCVTK::FeatureAPI` 或固定组合的聚合 target。FeatureSupport 仅用于 build interface；DefX、内部依赖包装、UI 和 Standalone target 不安装或导出。

### 4.4 消费者显式组合

以下是产品组合示例，调用方按本次批准集合和实际功能选择请求与链接项：

```cmake
find_package(MVVCVTK CONFIG REQUIRED
    COMPONENTS Host OrthogonalCrop GapAnalysis)
target_link_libraries(app PRIVATE
    MVVCVTK::Host
    MVVCVTK::OrthogonalCrop
    MVVCVTK::GapAnalysis)
```

## 5. 当前工具链与二进制边界

| 项目 | 固定要求 |
| --- | --- |
| 平台与架构 | Windows x64 |
| CMake | 4.2 或更高 |
| Generator | Visual Studio 18 2026 |
| Toolset | MSVC `v145,host=x64`，toolset 145、compiler 19.5x |
| Windows SDK | 10.0.x |
| 语言 | C++17，禁用 compiler extensions |
| 配置与运行库 | Debug `/MDd`、Release `/MD` |
| Release 指令集 | AVX2 |
| 主产品形态 | 静态库 |

Debug 和 Release 由同一正式流程生成，进入同一 SDK 根。此制品不承诺跨编译器、工具集、运行库、架构或配置的二进制兼容；扩展支持矩阵须明确授权。

## 6. 当前 SDK 根目录与文件闭包

### 6.1 顶层与目录结构

`<package-root>` 是本次 stage 或解压后的 SDK 根目录。

```text
<package-root>/
├── include/MVVCVTK/
│   ├── API/
│   └── SPI/
├── lib/
│   ├── Debug/
│   ├── Release/
│   └── cmake/MVVCVTK/
└── deps/
    ├── official/
    │   ├── vtk/
    │   └── opencv/
    └── third_party/defx/bin/
        ├── Debug/
        └── Release/
```

具体目录和文件以本轮核实的主分支提交上的预期清单及批准差异为准。顶层不放项目说明、版本、manifest、checksum、示例、测试、脚本或兼容目录。

### 6.2 文件闭包

`<Debug|Release>` 表示按本次配置选择 `Debug` 或 `Release`。`lib/<Debug|Release>` 的产品库示例见第 4.1 节；`lib/cmake/MVVCVTK` 文件示例如下，实际交付时按第 2.3 节重新核对本轮核实的主分支提交：

```text
MVVCVTKConfig.cmake
MVVCVTKInternalDependencies.cmake
MVVCVTKTargets.cmake
MVVCVTKTargets-debug.cmake
MVVCVTKTargets-release.cmake
```

官方依赖是锁定 bundle 的完整定向副本，内部内容由依赖锁和脚本控制，不临时删减缩包。bundle 原有许可和元数据不等于新增项目级交付文件。

## 7. 当前公开头闭包

### 7.1 物理分面

项目头只能安装到 `include/MVVCVTK/API/` 和 `include/MVVCVTK/SPI/`。API 面向消费者，Host 入口位于 `API/Host/`，具体 Feature 入口位于 `API/Features/<Feature>/`；SPI 面向同一可信域、固定二进制契约内的 Feature 扩展，包含 `HostFeature` 及获批的 view/overlay/render 契约。源码目录名不决定公开性。以本轮核实的主分支提交的 `SetSdkHeaderMap` 和批准差异核对路径，再检查本次 header surface 与实际安装集合。

### 7.2 映射示例

以下为映射示例，用于说明分面；实际交付时按第 2.3 节重新核对本轮核实的主分支提交的完整映射：

源码侧按 `FEATURE_DEVELOPMENT.md` 第 2.3 节的方法检索符号，再用当前头映射确认来源。

| 定位符号 | 安装相对路径（省略 include/） |
| --- | --- |
| `VtkAppHostSession` | `MVVCVTK/API/Host/VtkAppHostSession.h` |
| `HostFeature` | `MVVCVTK/SPI/Host/HostFeature.h` |
| `CropHostFeature` | `MVVCVTK/API/Features/OrthogonalCrop/Host/CropHostFeature.h` |
| `GapHostFeature` | `MVVCVTK/API/Features/GapAnalysis/Host/GapHostFeature.h` |

### 7.3 公开符号

物理头集合不变也要审查公开契约：类型和布局、签名、默认值、可空性、所有权、异常、线程、回调和生命周期。物理闭包支持头不能未经授权变成新业务入口。

### 7.4 私有头隔离

具体 Algorithm、Service、Router、Widget、策略、worker、bridge、内部 registry/UI 和 Host 私有实现不得作为公开开发件交付。已批准的窄 SPI 按清单判断，不能只按 Service 等后缀判定。

私有头负例必须确认安装目录不存在该头，且消费者无法包含它；脚本列出的探测头只是样例，不能当作完整私有清单。`MVVCVTKHeaderSurface.txt` 只留构建树。

## 8. 当前依赖与运行时边界

### 8.1 官方依赖

以下为依赖示例；实际交付时按第 2.3 节重新核对本轮核实的主分支提交：安装态按 EXACT 版本定向查找 VTK 9.4.2 和 OpenCV 4.12.0，默认从 `deps/official` 解析，不使用系统默认路径。具体依赖名单以本轮核实的主分支提交上的预期清单为准，版本与 bundle 按依赖锁和 package 规则核对。

不自行升级、换来源、引入 vcpkg runtime 或增加消费者依赖。VTK bundle 内的 QVTK 模块不表示 SDK 交付 Qt 本体。

### 8.2 DefX 私有运行时

DefX 只属于 GapAnalysis 的私有 bridge/runtime。当前结构每个配置只交付 `GapKernel.dll` 与 `DefXAnalysis.dll`，具体文件以本轮核实的主分支提交上的 DefX 预期清单及批准差异为准。

`GapKernel.dll` 替代旧名 `MVVCVTKGapKernel.dll`，旧名不再提供。接收方须更新部署脚本、复制与校验清单，以及按文件名显式加载该 DLL 的调用点。

宿主把 `MVVCVTK_GAP_RUNTIME_DIR` 指向 `<package-root>/deps/third_party/defx/bin/<Debug|Release>`，目录同时含 bridge 与 vendor DLL；不从 PATH、当前目录或另一配置隐式回退。

供应商头、导入库、源码、CMake target 和类型不进入 SDK 开发面；consumer package 不查找 DefX，不暴露 `DefX::Analysis`。

### 8.3 Qt

Qt 的头、库、DLL、plugin、CMake package、qmake 和部署由接收方宿主提供，不进入 Host、Feature 公共依赖或 SDK。

### 8.4 消费者 CMake 图

安装 metadata 不得引用内部 target、源码/构建绝对路径、FeatureSupport、DefX 开发根、header surface、测试或内部 UI。PRIVATE 声明不能替代实际传播和链接闭包核验。

## 9. 当前明确排除的交付内容

| 类别 | 排除内容 |
| --- | --- |
| 应用和适配 | Standalone、内部 UI、Qt 部署、MSBuild/qmake adapter、consumer 示例 |
| 源码和开发件 | `.cpp`、私有实现头、DefX 头和 import lib |
| 测试与数据 | 用例、clean-room 程序、真实数据、ROI、参考、输出和日志 |
| 项目资料 | AGENTS、md、项目级 README/NOTICE/LICENSE、设计和迁移文档 |
| 工具和附加件 | 发布/下载脚本、图谱、skill 产物、SDK manifest/checksum/版本文件 |
| 构建产物 | CMake cache、主产品 PDB/OBJ/EXP、临时文件 |

精确排除与允许集合以本轮核实的主分支提交上的预期清单及批准差异为准。锁定依赖内部不手工删改；必要的分发条件由交付方在包外确认，未满足时停止交接。

## 10. SDK 交付准入（Definition of Ready）

### 10.1 范围与授权

- 交付对象、允许变化和批准集合明确，相对本轮核实的主分支提交的完整边界差异已审查。
- 公开 API/SPI、安装和 runtime 变化都有具体授权；实验祖先继承的变化计入范围。

### 10.2 源码与构建输入

- 正式交付的项目变更已提交，工作树干净，没有未知输入、未解决冲突或生成污染。
- 实际源码、配置、输出与本次交付对应；外部依赖和数据另行核实，Git clean 不能证明它们正确。
- 新功能按 `FEATURE_DEVELOPMENT.md` 第 10.1 节接入；CMake 修改的解决方案同步与完整性核对按根 `AGENTS.md` 第 11.1 节的适用范围执行，配置变化已进入实际构建。

dirty 状态只生成诊断包；实验包按实际验证状态报告，不因此获得主干合并或外部发布授权。

### 10.3 工具链与依赖

- 第 5 节工具链可用，官方 bundle 通过依赖锁校验。
- DefX 构建输入根含当前构建脚本和锁定的供应商输入要求的完整头文件集合，以及 Debug/Release 的 DLL、导入库；输出仍只交付运行时文件。
- 磁盘、路径和权限支持第 11.1、11.5、12.5 节规定的构建目录、stage 和仓库外 clean-room，不能依赖系统偶然安装或另一工作树产物。

### 10.4 代码和业务前置

对应风险的审查、直接测试及异常路径已覆盖；真实输入、参考、容差和公共入口可用，真实 C3 VTP 的可选执行要求见第 11.2 节。区分脚本现有覆盖与未实现业务，缺口不能靠规则文本补齐。

## 11. 标准交付流程

### 11.1 生成入口与构建目录

正式、实验和诊断 SDK 均使用以下入口，在仓库根执行：

```powershell
./tools/release/Build-MVVCVTKSdk.ps1
```

新的 SDK 发布包按 `GIT_WORKFLOW.md` 第 2.1 节在 baseline 树中构建；单独的构建或验证任务按该节使用派生树。构建输入、数据参数和校验范围从本次发布脚本确认，记录方式见 `LOOP.md`。

SDK 产品构建的预设和构建目录按第 11.2 节选择，Debug/Release 按配置构建。执行前核对脚本、预设和批准集合，不另选目录或使用其他工作树的构建目录。

从本次工作树的 `CMakePresets.json` 和发布脚本核对 `binaryDir`、`buildRoot` 及 configure 路径的实际解析，差异按第 2.2 节处理。规定路径不存在时，由 CMake 或发布脚本创建。

第 12.5 节要求的仓库外 clean-room 使用既有流程规定、按工作树隔离的消费者验证目录，只验证本次 stage，不在其中构建 SDK 产品。

### 11.2 正式交付不跳过门禁

正式交付不使用 `-SkipTests` 或 `-SkipCleanRoom`，它们只用于诊断包。历史测试不能替代本次正式交付的必需执行。

`-MetrologyMesh` 为可选参数，真实 C3 VTP 不作为构建或打包前置。传入时仍须核对文件和 SHA-256，并按本次测试与 clean-room 执行范围完成对应验证；不传时仅该项不执行，脚本结束摘要和发布完成报告必须如实列为“未执行（NOT_RUN，非门禁）”，不得计为通过，按 md/development/core/TESTING.core.md 第 17.1 节处理状态与门禁的关系。其他必需门禁不变。

正式交付（不使用 `-SkipTests`）使用 `vs2026-sdk-tested-x64`；诊断包使用 `-SkipTests` 时使用 `vs2026-sdk-products-x64`；构建目录由预设的 `binaryDir` 决定。从本次工作树的发布脚本和 `CMakePresets.json` 核对 `-Preset` 接受集合，并确认符合批准集合；差异按第 2.2 节处理。

### 11.3 执行顺序

每次生成 SDK 前必须检查旧 SDK（含对应包）、构建及对应输出，并按 `GIT_WORKFLOW.md` 第 6.7 节核实、判定和处理；检查结果、保留或受阻原因写入本次交付记录。

```text
核对授权、源码、规定构建目录、旧产物处理结果和本次验证所需输入（真实 C3 VTP 见第 11.2 节）
-> 校验参数、安全路径、锁定依赖和 DefX 构建输入
-> configure 与固定工具链检查
-> Debug/Release 构建、CTest 和相关真实业务回归
-> 本次 stage 安装两个配置并复制锁定依赖
-> Test-MVVCVTKSdk.ps1 精确闭包验证
-> 仓库外 clean-room：安装边界和真实数据业务
-> Pack-MVVCVTKSdk.ps1
-> ZIP 与 stage 逐项内容核对
-> 按证据报告正式、实验或诊断状态
-> 交付物转移和中间目录清理
```

交付物转移和测试验证用工作树的保留、删除按 `GIT_WORKFLOW.md` 第 6.5 节执行；stage、clean-room 等中间目录按其第 6.6 节清理。

这是完整准出流程，实际覆盖按第 12.5 节核实并记录。可由已审查的测试或既有 CI 补齐业务，但结果必须绑定本次安装产物；不只运行 Pack，不拼旧 stage 和新构建。

### 11.4 包名与源码状态

`<12位短提交>` 是本次源码提交 SHA 的前 12 位，`<HHmmss>` 是本次诊断打包时刻的时分秒。默认正式目录名为 `yyyy.MM.dd-git.<12位短提交>-win-x64`；dirty 诊断目录使用 `yyyy.MM.dd-rev.<HHmmss>-win-x64`。ZIP 为 `MVVCVTK-SDK-<目录名>.zip`。

`<目录名>` 是带 `-win-x64` 后缀的完整 SDK 目录名；`<revision>` 是去掉该平台后缀后的部分，不包含 `-win-x64`。

`-PackageRevision` 须符合脚本允许的形式且与源码状态一致，不以重命名掩盖来源，也不另加包内版本文件。

包名与本次提交和工作树状态的一致性必须核对，结果写进本次交付的任务记录。

### 11.5 同次打包、Stage 与 ZIP

每次生成新的 SDK，无论正式、实验或诊断，都必须在同次生成流程中形成对应 ZIP，按第 12.6、12.8 节核对内容及来源；只有散装 stage 不能声明 SDK 生成完成。沿用第 11.2、11.3 节适用的验证和打包流程；失败如实报告，不为出包跳过检查或降低完成标准。

stage 位于 `out/stage/<revision>-win-x64`，ZIP 位于 `out/packages/`。stage、ZIP 和约定交付位置都遵循 `GIT_WORKFLOW.md` 第 2.1 节的非系统盘要求。不得手工修改已验证的 stage 和 ZIP 内容；手工变化使结论失效，须从受控输入重建并重新执行发布门禁。交付物转移和中间目录清理按第 11.3 节执行，不能删除共享数据或其他工作树产物。

## 12. 必须通过的交付门禁

### 12.1 固定工具链

用构建树 `MVVCVTKBuildInfo.json` 核对第 5 节全部要求，包括 MSVC 19.5x、toolset 145、Windows SDK 10.0.x 和 Release AVX2。不匹配即停止，不能生成兼容版绕过。

### 12.2 公开头

本轮核实的主分支提交中的白名单加获批差异、本次 header surface 和实际安装头三者一致；检查路径、大小写、重复映射和关闭 Feature 后的残留。生成集合与安装集合相同不能证明授权。

每个安装头在 clean-room 单独编译，私有头拒绝命中目标原因；物理分面和符号契约分别检查。

### 12.3 库与 CMake package

Debug/Release 产品库、导出 targets、components 和 `lib/cmake` 文件以本轮核实的主分支提交上的预期清单及获批差异为准，并对照实际 package 声明。

不携带未批准库、主产品 PDB/EXP、DefX import lib、`MVVCVTKConfigVersion.cmake`、内部或聚合 target，不泄漏源码/构建路径。

### 12.4 依赖与 runtime

按本轮核实的主分支提交上的预期清单精确核对 deps 层次、官方 bundle、DefX 文件和配置；检查 VTK/OpenCV config 的定向解析及版本。不能把 Qt 或 DefX 开发件带进消费者闭包。

### 12.5 Clean-room

位于源码仓库和所有开发工作树之外，输出独立，只从本次 stage、定向官方依赖和批准 runtime 解析。组合从本轮核实的主分支提交中的产品集合及批准差异确定，不能照抄实验脚本追认新增 component。clean-room 的清理按第 11.3 节执行。

下表为组合示例，列出每种组合在 Debug/Release 应完成的真实业务准出要求；实际交付时按第 2.3 节重新核对本轮核实的主分支提交：

| 组合 | 真实业务核验 |
| --- | --- |
| Host | 实际 Session、真实加载/读取、数值、几何、状态和退出 |
| Host + OrthogonalCrop | Host 链路及实际裁切请求、结果和资源恢复 |
| Host + GapAnalysis | Host 链路、对应配置真实 runtime、Gap 结果和失败/退出 |
| Host + OrthogonalCrop + GapAnalysis | 两项业务及共享状态、切换、取消和整体退出 |

每种组合完成两类检查：安装边界验证头自足、私有头拒绝、编译链接和定向依赖；真实业务经安装头和库的公共入口执行输入、请求、完成、结果核验和退出。涉及 View、渲染和交互时建立实际运行环境。空对象、空 main 或纯链接探针仍可证明边界，不能代替业务；没有图形条件时相应项如实阻塞。

**实现差距**：每次交付核实发布脚本与补充验证的实际业务覆盖，具体缺口和补齐结果写进本次交付的任务记录。

必需业务链未实现或未执行时记为 `NOT_RUN`；缺必需输入或环境时为 `BLOCKED`，不能因脚本成功写成通过。

### 12.6 ZIP

Pack 只读已验证 stage，ZIP 只有一个顶层 SDK 目录；拒绝危险相对路径、重复 entry 和 reparse point。逐项核对数量、路径、长度及 SHA-256 内容，沿用脚本清理旧 sidecar 的行为，不生成新 checksum 交付物。

### 12.7 真实数据、真实小数据与验收结果

真实输入、ROI 与派生关系、几何、参考和容差按 `TESTING.md` 第 5、6 节执行；用例有效性和过时清理按其第 7.6、7.7 节，性能按其第 12 节，状态与数量对账按其第 17 节执行。安装态业务与产物绑定见第 12.5、12.8 节；报告不扩大到未验证的规模、计量溯源、不确定度或标准符合性。

### 12.8 证据绑定与完成声明

本次源码、配置、二进制、安装库、runtime 和 ZIP 对应同一次交付，不混用其他工作树、旧构建或不同配置。可追溯靠 Git、CI 和测试记录，必要证据写进测试报告或任务记录（记录方式见 `LOOP.md`），不重复抄写来源路径和哈希；按根 `AGENTS.md` 第 7.1 节保留关键条件、结果、异常和限制。

安装边界、真实业务和 ZIP 校验分别报告。正式执行要求见第 11.2 节，完成声明与格式见根 `AGENTS.md` 第 12、14 节。

## 13. SDK 交付准出（Definition of Done）

以下全部满足，才能声明正式交付验证完成：

- 本轮核实的主分支提交中的白名单及获批差异与 components、targets、头/符号、库、依赖、runtime 和目录完全一致。
- 正式源码干净且项目变更已提交；固定工具链、同次 Debug/Release 构建、有效必需测试和真实业务通过。
- CMake 接入、测试注册、二进制与 stage 对应，无旧配置或其他工作树产物。
- clean-room 批准组合的头、链接、隔离和真实业务均通过，含 Gap 的组合使用正确 runtime。
- 必需项无 `FAIL/BLOCKED/NOT_RUN/SKIP` 或未解释的 `FLAKY`；第 12.5 节要求的业务链已用本次安装产物实测。
- 已满足第 11.2 节的正式执行要求；ZIP 与 stage 内容一致，验证后未手改或二次拼包。
- 说明未超出实际样本、规模、平台、工具链和宿主条件；包内没有未批准附加件。

## 14. 立即停止交付的条件

出现以下情况，停止受影响的打包、准出或交接，保留事实和最小修复范围：

- 边界变化未获授权或授权不明；代码和脚本已有差异却无法与相对本轮核实的主分支提交的授权核对。
- 头映射、header surface、stage、package 或 ZIP 不一致，有旧文件、残留配置或未声明内容。
- 私有头、FeatureSupport、DefX 开发件、Qt、Standalone、测试、数据、项目文档或其他排除内容进入包或消费者依赖。
- 工具链、配置、运行库、指令集或依赖不符，缺少 Debug/Release，或从源码树、全局安装、PATH 偶然补齐。
- 正式 SDK 使用了诊断参数，或必需测试失败、缺失、跳过时，停止正式打包、准出或交接。诊断包按第 11.2、11.5 节处理，不得据此声明正式验证通过。
- 用合成、零命中、空业务结果宣称真实通过。
- 为通过而放宽正确断言、删除有效检查、改 PRIVATE 为 PUBLIC、安装私有头或增加隐式回退。
- 正式源码 dirty、产物身份无法对应、包名掩盖来源、stage 手改或 ZIP 二次拼接。
- 外部转交权限或必要分发条件未确认，或说明承诺未验证的平台、二进制兼容、Qt 部署、性能或精度。
- 工具或 Agent 以最佳实践扩大 SDK，或交付操作超出根 `AGENTS.md` 第 6 节及 `GIT_WORKFLOW.md` 的授权范围。

修复回到批准边界；不要为消除失败扩大 SDK。错误测试修正和过时清理仍按 `TESTING.md` 第 7.6、7.7 节审查，不能与隐藏缺陷混淆。

## 15. SDK 相关变更分级

S0/S1/S2 衡量 SDK 契约和交付闭包，与根 `AGENTS.md` 第 11 节的代码风险分别判断。例如私有线程改动可为 S0，同时属于 R3。

### 15.1 S0：契约和闭包不变

已批准产品的私有算法、渲染、线程、测试调整，或恢复原契约的修复。核对实际依赖传播，不能只凭签名未变或 PRIVATE 标签判断。日常执行相关风险测试和真实小数据回归；正式交付时执行完整门禁。

### 15.2 S1：现有公开契约变化

现有公开方法、类型、默认行为、错误/取消/完成、所有权或生命周期的明确变化。需要具体授权、完整签名与行为影响及消费者迁移说明。实现与相关头契约测试同步，准出前完成 Debug/Release、SDK 边界、clean-room 和封装验证；日常阶段仍按实际影响缩放。

### 15.3 S2：包装或产品边界变化

component、导出 target、产品库、公开物理头、依赖、runtime、目录、附加交付物或工具链支持变化。仅新增未安装、未导出的仓内 target 不因此成为 S2。实现前按根 `AGENTS.md` 第 13.2 节处理方向决策，明确获批的旧/新白名单、影响文件、迁移和验证变化。可作为当前任务中的独立审查项；未获授权不写入对应 SDK 边界。

## 16. SDK 与 Feature 开发的关系

新 Feature 默认只接入仓内构建和测试，SDK 准入另行核对；未获准入时不改头映射、install/export、components、发布预期或 clean-room 矩阵来容纳它。

新功能的 CMake 接入见 `FEATURE_DEVELOPMENT.md` 第 10.1 节；CMake 修改的解决方案同步与完整性核对按根 `AGENTS.md` 第 11.1 节的适用范围执行，风险按根 `AGENTS.md` 第 11 节。

公开 SPI 的变化须属于 Host 职责、满足真实需求且形成窄契约，并取得对应授权；不能用单个 Feature 的私有便利扩大 SPI。

## 17. 多 Agent 交付规则

通用 skill、图谱和多 Agent 规则见根 `AGENTS.md` 第 13 节。SDK 并行工作还要遵守：

- SDK 边界只有一个负责人，统一解释批准集合和候选差异。
- 头映射、install/export、package 规则和发布脚本同时只有一个写入者，其他 Agent 只读核对。
- 发现需要扩大 SDK 时返回精确变更请求，不自行写入 expected closure。
- 关键边界和最终 ZIP 由执行者复核，协调者审查执行者返回的证据；独立复核按根 `AGENTS.md` 第 13.2 节执行。子代理 摘要不代替门禁。

## 18. 交付记录模板

沿用根 `AGENTS.md` 第 14 节的完成报告格式，在已有会话、任务或 CI 记录中说明：

- 交付对象与正式、实验或诊断状态。
- 主要业务和 SDK 边界变化、批准范围。
- Debug/Release、真实业务、安装边界、clean-room 和 ZIP 的实际结果。
- 未完成项、失败、限制及外部交接条件。

报告不进入 ZIP；数量、指标和异常保留必要解释，不复制逐文件哈希或完整环境。
<!-- 规范文件结束：md/development/SDK_DELIVERY.md -->
