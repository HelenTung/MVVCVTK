# AGENTS.md

## 开工闸门（唯一定义）

本节是开工前规范读取与加载确认的唯一定义，按顺序完成以下四步。规范路径均相对仓库根，必须读取全部 16 份文件：

- `AGENTS.md`
- `md/development/core/AGENTS.core.md`
- `md/development/core/ENGINEERING.core.md`
- `md/development/core/LOOP.core.md`
- `md/development/core/GIT_WORKFLOW.core.md`
- `md/development/core/COLLABORATION.core.md`
- `md/development/core/TESTING.core.md`
- `md/development/platform/paseo/DISPATCH.md`
- `md/development/platform/runtime/CLAUDE_CODE.md`
- `md/development/platform/runtime/CODEX.md`
- `md/development/GIT_WORKFLOW.md`
- `md/development/NAMING.md`
- `md/development/FEATURE_DEVELOPMENT.md`
- `md/development/TESTING.md`
- `md/development/SDK_DELIVERY.md`
- `md/development/LOOP.md`

1. 每个文件用 UTF-8 分段读取，取得完整可见正文，直到看到该文件末尾的固定结束标记 `<!-- 规范文件结束：<相对仓库根路径> -->`；路径引用、文件清单或单独读取末行不算读完整个文件。
2. 出现乱码、`truncated` 或其他截断、省略标记时，先补读缺失区间；未补齐前，不依赖该文件的规则行动。
3. 全部读完后，先发一条加载确认，逐项列出每个文件的相对仓库根路径及实际看到的结束标记。
4. 发出加载确认后，才判断角色权限、调用 Paseo 或做其他动作。

> 适用范围：MVVCVTK 仓库根目录及全部子目录。子目录里更近的 `AGENTS.md` 只覆盖它明确声明的规则。
>
> 文件分工：本文件定义项目细则；对话与加载见 `md/development/core/AGENTS.core.md`；工程原则见 `md/development/core/ENGINEERING.core.md`；工作推进见 `md/development/core/LOOP.core.md`；Git 通用模型见 `md/development/core/GIT_WORKFLOW.core.md`；三层协作见 `md/development/core/COLLABORATION.core.md`；测试证据见 `md/development/core/TESTING.core.md`；Git、分支、工作树与合并流程见 `md/development/GIT_WORKFLOW.md`；命名见 `md/development/NAMING.md`；Feature 开发见 `md/development/FEATURE_DEVELOPMENT.md`；测试与验收见 `md/development/TESTING.md`；SDK 交付见 `md/development/SDK_DELIVERY.md`；Loop 工作方式与任务进度文件见 `md/development/LOOP.md`；派发平台适配见 `md/development/platform/paseo/DISPATCH.md`；会话运行时适配见 `md/development/platform/runtime/CLAUDE_CODE.md`、`md/development/platform/runtime/CODEX.md`。
>
> 一条规则只在一个文件里定义，其他文件只引用，不复述。

> 本文件为项目层；与 md/development/core/AGENTS.core.md 及所引 core 文件合并适用。扩展范围与优先级见 md/development/core/AGENTS.core.md 第 1.1 节；本文件不改写通用角色关系与裁决规则。

## 0. 项目具体值

- 工程主题的触发、角色身份与会话关系见 md/development/core/AGENTS.core.md 第 0 节；本文件只提供本项目的具体值与实施细则。
- 风险等级唯一定义见 md/development/core/ENGINEERING.core.md 第 11 节；本文件第 11 节只列本项目具体判级细则与最低验证，既有门槛不降低。完成级别仍按第 12 节。

| 配置名 | 本项目值 |
| --- | --- |
| 个人规则来源 | 所用会话运行时的配置 |
| 长期规范目录 | `md/development/` |
| 说明文档目录 | `md/` |
| 任务归档目录 | `md/feature/<任务目录>/` |
| 任务目录父级 | `md/feature/` |
| 任务目录命名示例 | `feature-<主题>-YYYYMMDD-NN`、`fix-<主题>-YYYYMMDD-NN`、`research-<主题>-YYYYMMDD-NN`、`validation-<主题>-YYYYMMDD-NN` |
| 图谱工具 | Graphify |
| 图谱机器状态目录 | `.graphify/` |
| 执行并发上限 | 每名协调者同时运行的直属执行者不超过 4 个 |
| 任务进度文件 | `TASK_LOOP.md` |
| 累计会话上限 | 每个执行任务默认累计创建不超过 7 个子代理会话；用户可对具体任务明确放宽 |
| 脚本工具 | PowerShell 工具 |
| 收尾笔记文件 | `归纳笔记.md` |
| 占用检查进程 | git、cmake、MSBuild、devenv 进程 |
| 临时输出目录 | `out/` |
| 实验分支格式 | `experiment/<批次标识>` |
| 临时子目录 | `out/tmp` |
| 派发平台 | Paseo；适配文件 `md/development/platform/paseo/DISPATCH.md` |
| 协调者运行时 | 按实际会话使用 Claude Code 或 Codex；分别适用 `md/development/platform/runtime/CLAUDE_CODE.md`、`md/development/platform/runtime/CODEX.md` |
| 执行者与子代理运行时 | Codex；适配文件 `md/development/platform/runtime/CODEX.md` |
| 执行者与子代理的派发提供方 ID | `codex` |
| 项目风险细则 | AGENTS.md 第 11 节 |
| 项目完成级别 | AGENTS.md 第 12 节 |
| 项目规范入口 | AGENTS.md |
| 分支命名规则 | md/development/NAMING.md 第 9 节 |
| 分支或任务命名格式 | md/development/NAMING.md 第 9 节 |
| 项目工作推进规范 | md/development/LOOP.md |
| 本项目现行规范路径示例 | 根 `AGENTS.md`、`md/development/*.md` 及实际被会话加载生效的其他规范 |
| 临时位置存放策略 | 工作树、临时文件与日志使用非系统盘；具体位置见 md/development/GIT_WORKFLOW.md 第 2.1、2.3 节 |
| 短文件名映射 | `GIT_WORKFLOW.md` → `md/development/GIT_WORKFLOW.md`；`LOOP.md` → `md/development/LOOP.md`；`NAMING.md` → `md/development/NAMING.md`；`FEATURE_DEVELOPMENT.md` → `md/development/FEATURE_DEVELOPMENT.md`；`TESTING.md` → `md/development/TESTING.md`；`SDK_DELIVERY.md` → `md/development/SDK_DELIVERY.md` |
## 1. 优先级与规则强度

通用定义见 md/development/core/AGENTS.core.md 第 1.1 节。

## 2. 回复风格

通用定义见 md/development/core/AGENTS.core.md 第 1.2 节。

通用定义见 md/development/core/AGENTS.core.md 第 1.3 节。

## 3. 架构不变量

以下是长期边界。代码变化时以当前源码和 CMake 为准核实，不凭本文记忆下结论。

- 平台：Windows x64、C++17、CMake preset、MSVC。不新增 Win32/x86 支持。

- `VtkAppHostSession` 是 Host 的组合根。外部只通过 Host API 的请求、结果、状态快照和 endpoint 交互，拿不到 `AppRuntime`、`Context`、窗口实现或内部 service。

- Host 不依赖任何具体 Feature；Feature 之间互不依赖；Feature 不包含 Host 私有头。

- `MVVCVTK::HostAPI` 和 `MVVCVTK::FeatureSPI` 是自足接口 target。Host 公开依赖 HostAPI、内部使用 FeatureSPI；Feature 公开依赖 FeatureSPI。

- SDK 按批准白名单交付；产品、公开分面、依赖和排除内容见 `SDK_DELIVERY.md` 第 4、7 至 9 节。

- Feature 是可信的进程内扩展，不是沙箱。普通 Host 数据读取用不暴露 VTK 对象身份的值类型；只有经过审查的 SPI 能力可以携带 VTK 快照或窄 VTK 契约。

- Qt 由接收方宿主提供，不进入 Host 或 Feature；SDK 交付边界见 `SDK_DELIVERY.md` 第 8.3 节。

- DefX 只存在于 GapAnalysis 的私有 bridge/runtime 中，供应商的 C++、STL、VTK 类型不能穿过这个边界。

- `FeatureSupport` 只是仓内构建期支持层：保持最小，不承载单个 Feature 的业务；安装边界见 `SDK_DELIVERY.md` 第 4.3 节。

- 不新增全局 SceneManager、全局 Renderer 扫描器或跨 Feature Manager。

- `examples/standalone` 只用于仓内组合与运行验证，不决定产品边界；交付排除项见 `SDK_DELIVERY.md` 第 9 节。

## 4. 事实来源与授权

本项目构建配置为 CMake；事实取证顺序见 md/development/core/ENGINEERING.core.md 第 4 节。

通用定义见 md/development/core/AGENTS.core.md 第 1.2 节。

裁决、通知与独立授权见 md/development/core/LOOP.core.md 第 14.1 节；SDK 边界的具体判定与落实见 md/development/SDK_DELIVERY.md 第 3 节。

## 5. 开始任务前

适用规范与 Loop 的读取、Git 四项现状检查、目标材料核对的完整开工顺序见 md/development/core/LOOP.core.md 第 0 节；启动自检见其第 0.1 节。

- 是否涉及 API/SPI、CMake/安装、线程、第三方、性能或破坏性迁移；涉及 SDK 时写清授权来源和具体变化；

## 6. Git 权限一览

常规提交权限、主干与远端操作权限、提交前的暂存边界统一见 md/development/core/GIT_WORKFLOW.core.md 第 0 节；提交范围与说明见其第 3 节。

本项目存放位置见 md/development/GIT_WORKFLOW.md 第 2.1、2.3 节，清理与笔记归档见其第 6.6 节。

## 7. 文档规则

通用定义见 md/development/core/LOOP.core.md 第 7 节。

### 7.1. 结果文件与报告

通用定义见 md/development/core/LOOP.core.md 第 7.1 节。

## 8. 修改原则

**在保证业务正确、精度和生命周期的前提下：内存尽量低，逻辑尽量简单，渲染尽量快。** 三者需要取舍时，依据当前需求和可比证据选择最简单的可行方案。

通用定义见 md/development/core/ENGINEERING.core.md 第 8 节。

机器相关的数字和测试门限不进入 Host 公共 API。

- 新增公共能力前先判断它属于哪一层：Host 通用能力、Feature 公共入口、Feature SPI，还是 Feature 私有实现。判断不了就先不写。

- 公开契约变化不能按私有实现处理；SDK 契约变化按 `SDK_DELIVERY.md` 第 3.2 节核对。

## 9. Feature、CMake 与 SDK 边界

- Feature 通过 `HostFeature` 和窄端口接入，不取得 Host 私有对象。具体规范见 `FEATURE_DEVELOPMENT.md`。

- 新功能须完成 CMake 构建和测试接入，见 `FEATURE_DEVELOPMENT.md` 第 10.1 节；CMake 修改的解决方案同步与完整性核对，按本文件第 11.1 节的适用范围执行。

- 仓内构建接入不等于 SDK 准入。新 Feature 的默认边界和隔离要求按 `SDK_DELIVERY.md` 第 16 节执行。

- Host 在 Feature 关闭时必须能独立配置和构建。已批准的公共契约变化需要同步修改其他 Feature 的调用点时，这属于范围内修改，不算跨 Feature 依赖。

- 测试 target 的复用、目录和 IDE 分组见 `TESTING.md` 第 7.2.1 节。

## 10. 代码规则

本节条款见以下子节。

### 10.1. 通用

通用定义见 md/development/core/ENGINEERING.core.md 第 10.1 节。

- 公共头只 include 签名需要的内容，私有依赖放进 `.cpp` 或私有头；不依赖偶然的传递包含。

- 只被一个类使用的 helper，做成这个类的私有成员或放进 `.cpp` 里的 `Impl`。

- 优先使用 RAII、值语义和 `unique_ptr`/`shared_ptr`；裸指针只表示不拥有、可为空的外部对象。

- 新增的类型和函数按 `NAMING.md` 命名。

### 10.2. 注释与 Pimpl

通用注释要求见 md/development/core/ENGINEERING.core.md 第 10.2 节。

- 需要隐藏实现时使用 Pimpl：头文件的私有部分只保留 `class Impl;` 和 `std::unique_ptr<Impl> m_impl;`，完整定义放在 `.cpp`，不在头文件中展开实现；`Impl` 内部不再嵌套另一层 `Impl`。没有私有依赖的小型值类型或策略类不强制使用 Pimpl。

### 10.3. 线程、异步与回调

通用线程、期限、回调及失败状态要求见 md/development/core/ENGINEERING.core.md 第 10.3 节；测试保护超时见 md/development/core/TESTING.core.md 第 16.4 节。本项目停止与分离操作为 Stop/Detach，回调接口族契约见 md/development/FEATURE_DEVELOPMENT.md 第 6.2.1 节。

- Session 构建、Feature/Timer/Hotkey 绑定、渲染写入和显式 Stop 都在 owner thread（所属线程）上进行；其他线程结果通过已批准的 dispatcher 回到 owner thread。
- 工作线程不得修改 owner thread 管理的 renderer、interactor、widget、actor、mapper 等渲染和交互对象、Host 可变状态或已发布共享快照；遵守所用 VTK 类线程约束时，可创建处理线程私有、尚未发布的候选，例如私有 vtkImageData。

### 10.4. 状态、快照与几何

通用定义见 md/development/core/ENGINEERING.core.md 第 10.4 节。

`GetSceneViewState(s)` 是只读的每 View 投影，不提供写权限；`presentationRevision` 只覆盖 presentation，不能当作全场景版本号。

### 10.5. 渲染资源

不靠隐藏降采样、旧结果或遗漏更新换取速度。

- 只处理实际失效的数据，复用仍然有效的管线和 GPU 资源；空闲时不靠持续 Render 或轮询制造帧率。

- 显示 LOD、计算分辨率和导出分辨率分开处理，不能把显示降采样静默带进分割、测量或导出。

- Volume 质量切换沿用既有资源链：等待旧 draw → 释放旧纹理 → 查询预算 → 分块 → 提交或恢复。Iso 按自己的候选管线处理，两者不互相套用。

资源准入唯一定义见 md/development/FEATURE_DEVELOPMENT.md 第 9.2 节。

- 显示准备失败要如实报告，但不能把已经成功的数据加载或业务结果改判为失败。

## 11. 本项目判级细则

等级定义与通用判级方法见 md/development/core/ENGINEERING.core.md 第 11 节。本节只补充本项目的具体命中条件与最低验证，不重新定义等级。涉及 CMake 修改时，configure 与解决方案完整性核对的适用范围另见 本文件第 11.1 节；该节同时指明测试注册与纯注释的既有例外。

- L0 的本项目范围：Markdown 文档或纯注释，包含 CMake 注释；通用验证见 md/development/core/ENGINEERING.core.md 第 11 节，Git 跟踪状态对应的文件检查见 md/development/core/TESTING.core.md 第 0 节。
- L1：外部契约不变、影响局限且可恢复的私有实现，包括往已有 target 加源文件（只调整私有源码列表）；仅给已有 target 增加或修改测试注册及测试属性（`add_test`、`set_tests_properties` 等，不新增 target、不改 target 属性和依赖）也属 L1。受影响 target 必须完成 Debug x64 构建、相关真实小数据回归和无关 diff 审查；Release 敏感路径补 Release。直接相关测试与失败路径审查按 core 的 L1 要求执行。
- L1 的测试注册例外：仅有上述测试注册或测试属性改动时，最低验证改为重新 configure，确认目标用例已注册（`ctest -N` 只用于注册确认，不算通过），构建该已有 target，并实际运行受影响的用例。
- **L2、L3 共同的最低验证**：Debug/Release 相关构建和测试；只验证实际受影响的 Feature 开关、Host 独立构建和消费者边界。
- L2：公共头、HostAPI/FeatureSPI、新增 target，或改变现有 target 的类型、对外编译接口、依赖传播、构建开关、导出或安装行为；header surface、PUBLIC 依赖、安装/package 均按 L2。最低验证按本节上述共同要求执行。
- L3：Host 核心机制、线程/停止、数据发布、渲染资源、第三方 bridge/runtime 按 L3。除 core 的 L3 完整测试与独立复核要求外，在本节上述 L2、L3 共同最低验证基础上，执行并发、取消、回滚、负例专项和真实数据或质量审计。三层协作中的独立复核按 md/development/core/COLLABORATION.core.md 第 13.2 节执行。
- 性能改动按实际改动范围判级，须提供改动前后可比较的测量；不把性能主题本身作为自动升入 L3 的理由。
- 只调整已有 target 的私有源码列表按 L1；命中本项目其他 L2/L3 条件时升级。各级验证仍按实际受影响的范围选择，不把级别继承理解为重跑无关模块。
- 缺少环境或数据时，列出未执行项和可复现的命令。

### 11.1. CMake 修改的配置与解决方案完整性核对（唯一定义）

本节适用于 CMake 修改，包括新功能、修 bug、新增或删除测试、调整开关和依赖。风险等级不免除本节义务。

- **私有源码列表变更：必须重新 configure，并完成本节的解决方案完整性核对。**
- **新增 target（包括测试 target）：必须重新 configure，并完成本节的解决方案完整性核对。**

**既有例外**：

- 满足本文件第 11 节“仅测试注册或测试属性改动”条件时，按该例外执行，不要求本节的完整解决方案核对；该例外中的重新 configure、构建与实际运行测试要求不变。
- 纯注释改动按 `LOOP.md` 开头的任务分类执行，不要求重新 configure 和解决方案核对。

先按上述修改类型与既有例外确定本节是否适用，再按以下流程取得实际配置与项目清单。

`Visual Studio 18 2026`、`out/build/vs2026-x64/MVVCVTK.slnx` 是当前命令与路径示例；执行时使用当前 `CMakePresets.json` 中对应的预设和生成器。

1. 修改 CMake 后，在本工作树的独立 `binaryDir` 中重新 configure 并生成解决方案。上述生成器示例的产物是 `binaryDir` 根目录下的 `MVVCVTK.slnx`。
2. 依据该 `binaryDir` 的 `CMakeCache.txt` 实际开关，以及当前 CMake 文件中的目标定义、依赖和启用条件，列出应生成工程的 target。
3. 对照当前配置下应启用的 target，逐个确认它们都出现在解决方案中，并归入正确的分组：Feature target 按现有分组，测试目标按 `TESTING.md` 第 7.2.1 节归入 `MVVCVTK/test/...`。用 PowerShell 的 `Select-Xml` 读取 `.slnx` 中的 `//Project[@Path]`，将 `Path` 对应的 target 及父 `Folder` 的 `Name` 与清单和 `FOLDER` 属性逐项对照，不把别名、导入目标和不生成工程的接口目标计为独立项目。
4. 缺失、多余或分组错误时回到 CMake 修正（`add_subdirectory`、开关、启用条件、`FOLDER`），不手工编辑生成的 `.sln`/`.slnx`/`.vcxproj`/`.filters`。因为开关关闭或 CMake 的依赖条件不满足而本应不出现的 target，在报告中写明原因，不能悄悄缺失。
5. `.slnx` 内容不变时 CMake 不会改写它，修改时间可能不变；判断是否重新 configure，以 configure 的执行结果和 `generate.stamp` 等生成标记为证据，不只看解决方案文件的修改时间。

CMake 修改合入目标工作树（包括用户日常打开的工作树）后，在目标工作树的 preset 上重新 configure，使其解决方案同步更新，并按本节核对；只在其他工作树或 binaryDir 中重新生成不算完成。

临时输出及 baseline 构建目录的保留按 `GIT_WORKFLOW.md` 第 2.3、6.6 节执行，报告要求见根 `AGENTS.md` 第 13.3 节。

## 12. 验证与完成声明

通用定义见 md/development/core/TESTING.core.md 第 0 节。

通用定义见 md/development/core/AGENTS.core.md 第 1.2 节。

- 合成数据和 mock 只能证明数值、边界和失败路径；**Feature 的正向验收必须通过实际公共入口跑真实数据。** 小数据不能证明整卷数据的性能和资源上限。

1. 静态检查或编译通过；

2. 单元测试通过；

3. 指定真实数据的业务验收通过；

4. 指定实验组合验证通过；

5. SDK 安装态验证通过；

6. 已合入主干，文档已同步。

标准构建和测试命令见 `TESTING.md` 第 16.2 节；正式 SDK 交付入口是 `./tools/release/Build-MVVCVTKSdk.ps1`，规则见 `SDK_DELIVERY.md`。

## 13. Skill、图谱与多 Agent

通用定义见 md/development/core/ENGINEERING.core.md 第 13 节。

通用定义见 md/development/core/COLLABORATION.core.md 第 13 节。

### 13.1. 三层分工

| 层级 | 模型与思考强度 |
| --- | --- |
| 协调者 | — |
| 执行者 | Codex `gpt-6.1-sol`，思考强度 max |
| 子代理 | Codex `gpt-6-luna`，思考强度 low |

通用定义见 md/development/core/COLLABORATION.core.md 第 13.1 节。

运行时具体值见本文件第 0 节；配置路径及权限适配见 md/development/platform/runtime/CODEX.md 第 1、2 节。

### 13.2. 协调者

通用定义见 md/development/core/COLLABORATION.core.md 第 13.2 节。

本项目资源占用重的任务包括全量构建、整卷数据和 GPU 任务。

本项目公共契约包括根 CMake、API/SPI、SDK 头映射和共享状态机。

- 按 本文件第 11.1 节需要核对解决方案时，必查核对结果；缺少就退回补查。

关键 SDK 边界改动同样触发 md/development/core/COLLABORATION.core.md 第 13.2 节 的独立复核流程。

### 13.3. 执行者

通用定义见 md/development/core/COLLABORATION.core.md 第 13.3 节。

本项目摘要还需写明是否触及公开头、CMake、线程、数据发布或 SDK 边界。

- 按 本文件第 11.1 节需要核对解决方案时，列出重新 configure 的工作树和 preset、解决方案应有与实际的项目数，以及缺失、多余和分组差异；差异应为空，非空时说明原因并按该节处理。

### 13.4. 子代理

通用定义见 md/development/core/COLLABORATION.core.md 第 13.4 节。

## 14. 提问与完成报告

协调者给用户的裁决、通知与回复模板见 md/development/core/LOOP.core.md 第 14 节；执行者报告与子代理回执见 md/development/core/COLLABORATION.core.md 第 13.3、13.4 节。

本项目报告中的主要修改还须说明 CMake 接入方式及 SDK 边界实际变化；SDK 具体边界引用 md/development/SDK_DELIVERY.md 第 3 节。
<!-- 规范文件结束：AGENTS.md -->
