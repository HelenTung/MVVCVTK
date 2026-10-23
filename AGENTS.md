# AGENTS.md

<a id="load-gate"></a>
<!-- 规范节开始：AGENTS.md#load-gate -->
## 开工闸门（唯一定义）

本节是规范加载及加载确认的唯一定义；加载后的现状检查、目标材料读取与自检顺序见 md/development/core/LOOP.core.md 第 0、0.1 节。路径均相对仓库根。包是既有正文的读取集合，不复制规则；分包只决定加载时机，不改变身份、授权、风险、验证与停止义务。

**读取单元与标记**

- 本节及下表是加载入口。节级入口采用稳定锚点 `#load-<语义名>`，从对应的 `<!-- 规范节开始：<相对路径>#load-<语义名> -->` 完整读到同名的 `<!-- 规范节结束：<相对路径>#load-<语义名> -->`。按标记定位，不固定物理行号；节号供人工导航。
- 每个节级单元只包含标记之间的正文，不自动展开其他单元或子节；表中写“全文”时读取整个文件。每次首次节读某文件，还读文件标题与首个单元前的导言，并核对末尾的 `<!-- 规范文件结束：<相对路径> -->`；这不等于已读其余正文。没有节级标记的文件一律全文读取。
- 全文读取必须取得完整可见正文并看到文件结束标记；节级读取必须看到成对标记及完整正文。新增规范文件同样必须登记分工并带唯一的文件结束标记；新增节级单元同样成对标记。乱码、`truncated`、截断或省略时先补齐，文件清单、摘要或单独末行不算已加载。
- 通用包、本角色包、当前运行时与命中动作的范围取并集，同一正文只读一次；本会话已读且未变化的单元可复用。不得凭未读取单元的目录、文件名或上级摘要声称加载完成。

**通用小核心（所有工程角色必读）**

下列范围始终保留身份与直属关系、授权与决定阶梯、已批准决定的落实、自检、停止清理、证据诚实和报告入口。

| 文件 | 必读单元（`load-` 后缀；括号为节号） |
| --- | --- |
| `AGENTS.md` | `gate`（本闸门）、`project-settings`（0）、`priority`（1）、`reply-style`（2）、`startup`（5）、`git-permissions`（6）、`completion`（12）、`collaboration`（13总述）、`models`（13.1）、`report`（14） |
| `md/development/core/AGENTS.core.md` | 全文 |
| `md/development/core/COLLABORATION.core.md` | `collaboration`（13总述）、`roles`（13.1）、`session-boundaries`（13.7）、`runtime`（13.6）、`report-routing`（14） |
| `md/development/core/LOOP.core.md` | `startup`（0）、`startup-check`（0.1）、`work-loop`（1）、`results`（7.1）、`report-routing`（14总述）、`decision-authorization`（14.1） |
| `md/development/core/TESTING.core.md` | `evidence`（0）、`process-cleanup`（16.4）、`report-routing`（19.1） |
| `md/development/core/ENGINEERING.core.md` | `risk`（11，不含11.1）；风险与决定级不同，判级动作还须按下表补项目细则 |

**本角色包（在通用包上增加）**

身份与直属关系只按 core/AGENTS 第0节确定；本表不改变三层角色关系。

| 角色 | 必读范围 |
| --- | --- |
| 协调者 | 根 `load-coordinator`（13.2）；COLLABORATION `load-coordinator`（13.2）；LOOP `load-task-record`（2）、`load-dispatch-record`（2.1）、`load-coordinator-reply`（14.2，含首次按模板回复前读取已填示例的要求） |
| 执行者 | 根 `load-executor`（13.3）；COLLABORATION `load-executor`（13.3）、`load-change-instructions`（13.8）；自己的开工检查须自己完成 |
| 子代理 | 根 `load-subagent`（13.4）；COLLABORATION `load-subagent`（13.4）、`load-change-instructions`（13.8）；仍须完成自己的加载、自检与开工检查，上级快照不替代本人检查 |

**当前运行时**

仅全文读取当前运行时对应的 `md/development/platform/runtime/CODEX.md` 或 `CLAUDE_CODE.md`；派发其他运行时前，再补读目标适配。被派发者不因上级使用其他运行时而加载上级适配，也不为自检查询其他会话。派发平台接口只在实际管理下级前触发。

**动作专项索引与依赖闭包**

本索引中 ENGINEERING、COLLABORATION、LOOP分别指 `md/development/core/ENGINEERING.core.md`、`md/development/core/COLLABORATION.core.md`、`md/development/core/LOOP.core.md`；“测试 core”“Git core”分别指同目录的 `TESTING.core.md`、`GIT_WORKFLOW.core.md`；“项目 Git”“项目 TESTING”“项目 LOOP”分别指 `md/development/` 下的同名文件。其余项目专项按第0节短文件名映射解析。

任务明确要求理解、审查或修改某份规范正文时，完整读取被指定范围；修订本加载体系时读取全部现行规范。其他动作按下表取并集；导航引用不自动展开全篇，但实际依赖的独有条款必须在动作前读取。纯路径、符号定位、字面统计及原文提取不因目录归属触发工程专题；开始解释行为、判断边界或制定修改时重新判断。

| 动作／触发条件 | 动作前必读范围与依赖 |
| --- | --- |
| 派发工程执行任务、首次或新增变更定级；工程行为、正确性或风险判断；制定或实施修改（含文档与注释）；使用／维护技能或图谱形成上述判断 | ENGINEERING全文；根 `load-project-risk`（11，不含11.1）。涉及代码、架构或工程语义，再加根 `load-architecture`（3）、`load-information`（4）、`load-principles`（8）、`load-feature-boundaries`（9）、`load-code`（10，含其子节）；只做机械原文提取且无新增定级不触发 |
| 派发／管理下级 | COLLABORATION `load-dispatch`（13.5）；LOOP `load-dispatch-record`（2.1）；Paseo `DISPATCH.md`全文；目标运行时全文。协调者派执行者加 COLLABORATION `load-executor`；执行者派子代理加 `load-subagent`、`load-change-instructions`。派发定级同时命中上一行的风险与决定规则，决定阶梯已在通用包，不因派发而放宽 |
| 固定四项之外的 Git 核对、diff（含 `git diff --check`）、log、fetch | Git core `load-permissions`（0）；远端状态判断另加两份 Git 的 `load-baselines`（1）；其他 Git 动作按以下对应行补读。固定四项仅按 LOOP `load-startup`，不因此加载 Git 专项 |
| 提交、分支／工作树选择、创建或规范初始化、主干文档维护 | Git core `load-permissions`（0）、`load-baselines`（1）、`load-worktrees`（2总述）、`load-worktree-create`（2.1）、`load-rule-init`（2.2）、`load-commit`（3）；项目 Git `load-baselines`（1）、`load-worktrees`（2总述）、`load-worktree-create`（2.1）、`load-rule-init`（2.2）、`load-commit`（3）。文档维护另加两份 Git 的 `load-document-maintenance`（7）；新分支／目录命名另加 NAMING全文 |
| 证据文件、报告、日志或其他任务产物落盘；构建／测试输出安排 | Git core与项目 Git各 `load-output-isolation`（2.3）；项目 Git `load-worktree-create`（2.1，含非系统盘要求）；LOOP `load-results`（7.1，通用已读）。证据写入必须在派发中单独授权；不因源文件只读而扩展可写位置 |
| 开发／实验任务记录、接手、续接、收尾转移、baseline写入 | LOOP `load-task-record`（2）；接手加 `load-handoff`（2.2），续接加 `load-continuation`（4），收尾加 `load-record-transfer`（3）、`load-record-examples`（6），baseline写入加 `load-baseline-coordination`（5）。涉及 Git、产物或派发同时命中各对应行，不扩大记录写入者权限 |
| 说明／规范文档维护，或纯注释分类判断 | 根 `load-documents`（7）、`load-results`（7.1）；LOOP `load-documents`（7）；项目 LOOP全文。修改仍命中风险行；规范验证须读 ENGINEERING `load-rule-validation`（11.1，已在该行全文范围）；差异检查须补 Git权限。纯CMake注释不因文件类型触发 SDK 或产品构建 |
| Host、Feature、数据、算法、渲染、线程、构建或安装的测试／验收 | 测试 core全文；项目 TESTING `load-scope`（1）、`load-evidence`（2）、`load-test-scope`（3）、`load-admission`（4）、`load-execution`（16）、`load-results`（17）、`load-completion`（18）、`load-reuse`（19），并按下行补能力专项。定级、Git差异、产物与记录仍按各对应行补读；仅文档路径、编码、排版静态核对不触发产品测试专项 |
| 真实数据／参考／断言／精度研究；用例设计或调整；能力专项测试 | 项目 TESTING：真实数据与结果判断加 `load-data`（5）、`load-reference`（6）；用例设计调整加 `load-test-design`（7）；数据几何加 `load-state-geometry`（8），Feature加 `load-feature`（9），异步加 `load-async`（10），渲染组合加 `load-rendering`（11），性能加 `load-performance`（12），注入或runtime加 `load-fault-runtime`（13），集成加 `load-integration`（14），安装态加 `load-sdk`（15）；各动作同时加载上一行测试共同范围 |
| 开发、扩展或调研审查具体 Feature 的归属、行为、接口、生命周期；为此修改API/SPI/CMake/测试；Host新增业务的CMake接入 | FEATURE_DEVELOPMENT全文；工程判断、修改、测试、命名、Git等按各行补读。纯定位或原文提取不因位于Feature目录触发；Host无关私有修复和普通文档不触发 |
| 修改CMake非注释语义 | 根 `load-cmake-check`（11.1），并依实际影响补工程、测试、Feature、SDK专项；既有纯注释与仅测试注册例外仍按该单元判定，不能把所有CMake变更当成SDK边界变化 |
| 安装、导出、包配置、公开契约、依赖传播、运行时闭包、SDK边界判断／授权／验收或SDK构建交付；CMake语义变化涉及这些对象 | SDK_DELIVERY全文，并按实际工程、测试、Git、产物动作补读。仅私有源码列表、纯注释CMake、无SDK语义的文档维护不因路径或文件类型触发；不能借排除条件跳过实际SDK契约影响 |
| 新增、重命名或对外暴露类型、函数、字段、枚举、常量、宏、开关、别名；Feature、target、分支或目录命名；命名审查 | NAMING全文，并补工程风险与实际契约专项；原样使用既有名称不触发 |
| 实验集成、合入主干、删除清理、旧产物判定或其他未被上述节级Git范围覆盖的Git流程 | Git core与项目 Git全文；测试／验收、记录、产物与授权依赖仍按各对应行加载，不因只读状态核对触发完整集成或清理流程 |

以上通用、角色、运行时和动作集合共同覆盖本入口登记的全部现行规范；未列为基础的正文归入对应动作，不因延后加载而失效。文件导言与导航随首次节读加载；长期规范分工仍由本文件下述“文件分工”登记。

**执行顺序**

1. 先读本闸门与 AGENTS core，依据直属关系明确角色；完整加载通用小核心、本角色包、当前运行时及当下命中动作。适用父级与更近的局部 AGENTS.md 仍须读取；不以旧摘要替代。
2. 加载命令只读规范，不在同一工具调用中夹带Git、Paseo、进程检查、写入、配置查询或其他命令。分段读取直到满足本节的完整可见正文与标记要求。
3. 发加载确认：角色、当前运行时、已读文件和全文／节级单元、实际看到的文件／成对节结束标记、命中与未命中动作专项。只列文件名不能冒充已加载该文件全文。
4. 加载完成后按 LOOP第0、0.1节核对现状、取得任务记录和上级实际设置快照、自检并读目标材料；各角色都完成自己的责任，不能外包或以他人检查取代。
5. 每次工具调用和回复前核对本角色允许范围；动作、范围或规范内容变化时，在依赖该变化前重判本索引，补读并确认新增单元。已批准的范围内执行与新增决定的区别仍按通用包的决定规则。

> 适用范围：MVVCVTK 仓库根目录及全部子目录。子目录里更近的 `AGENTS.md` 只覆盖它明确声明的规则。
>
> 文件分工：本文件定义项目细则；对话与加载见 `md/development/core/AGENTS.core.md`；工程原则见 `md/development/core/ENGINEERING.core.md`；工作推进见 `md/development/core/LOOP.core.md`；Git 通用模型见 `md/development/core/GIT_WORKFLOW.core.md`；三层协作见 `md/development/core/COLLABORATION.core.md`；测试证据见 `md/development/core/TESTING.core.md`；Git、分支、工作树与合并流程见 `md/development/GIT_WORKFLOW.md`；命名见 `md/development/NAMING.md`；Feature 开发见 `md/development/FEATURE_DEVELOPMENT.md`；测试与验收见 `md/development/TESTING.md`；SDK 交付见 `md/development/SDK_DELIVERY.md`；Loop 工作方式与任务进度文件见 `md/development/LOOP.md`；派发平台适配见 `md/development/platform/paseo/DISPATCH.md`；会话运行时适配见 `md/development/platform/runtime/CLAUDE_CODE.md`、`md/development/platform/runtime/CODEX.md`。
>
> 一条规则只在一个文件里定义，其他文件只引用，不复述。

> 本文件为项目层；与 md/development/core/AGENTS.core.md 及所引 core 文件合并适用。扩展范围与优先级见 md/development/core/AGENTS.core.md 第 1.1 节；本文件不改写通用角色关系与裁决规则。
<!-- 规范节结束：AGENTS.md#load-gate -->

<a id="load-project-settings"></a>
<!-- 规范节开始：AGENTS.md#load-project-settings -->
## 0. 项目具体值

- 工程主题的触发、角色身份与会话关系见 md/development/core/AGENTS.core.md 第 0 节；本文件只提供本项目的具体值与实施细则。
- 变更风险与验证等级唯一定义见 md/development/core/ENGINEERING.core.md 第 11 节；本文件第 11 节只列本项目具体判级细则与最低验证，既有门槛不降低。完成级别仍按第 12 节。

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
| 协作示例材料 | [协作记录与回复示例](md/agent-guides/协作记录与回复示例.md)，非规范材料，协调者按需读取 |
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
| 本项目现行规范集合登记 | 本开工闸门列出的规范文件，以及本轮实际生效的其他项目规范（含适用的局部 `AGENTS.md`）；范围与仓库外规则边界见 md/development/core/LOOP.core.md 第 14.1 节 |
| 临时位置存放策略 | 工作树、临时文件与日志使用非系统盘；具体位置见 md/development/GIT_WORKFLOW.md 第 2.1、2.3 节 |
| 临时环境变量 | 执行构建、测试和脚本前，把 `TEMP` 和 `TMP` 指向本工作树的 `out/tmp`（不存在就先创建），让 CMake、MSBuild、CTest 等工具的临时文件不写到系统盘。 |
| 短文件名映射 | `GIT_WORKFLOW.md` → `md/development/GIT_WORKFLOW.md`；`LOOP.md` → `md/development/LOOP.md`；`NAMING.md` → `md/development/NAMING.md`；`FEATURE_DEVELOPMENT.md` → `md/development/FEATURE_DEVELOPMENT.md`；`TESTING.md` → `md/development/TESTING.md`；`SDK_DELIVERY.md` → `md/development/SDK_DELIVERY.md` |
<!-- 规范节结束：AGENTS.md#load-project-settings -->

<a id="load-priority"></a>
<!-- 规范节开始：AGENTS.md#load-priority -->
## 1. 优先级与规则强度

通用定义见 md/development/core/AGENTS.core.md 第 1.1 节。
<!-- 规范节结束：AGENTS.md#load-priority -->

<a id="load-reply-style"></a>
<!-- 规范节开始：AGENTS.md#load-reply-style -->
## 2. 回复风格

通用定义见 md/development/core/AGENTS.core.md 第 1.2 节。

通用定义见 md/development/core/AGENTS.core.md 第 1.3 节。
<!-- 规范节结束：AGENTS.md#load-reply-style -->

<a id="load-architecture"></a>
<!-- 规范节开始：AGENTS.md#load-architecture -->
<a id="architecture-invariants"></a>

## 3. 架构不变量

以下是长期边界。代码变化时以当前源码和 CMake 为准核实，不凭本文记忆下结论。

- 平台：Windows x64、C++17、CMake preset、MSVC。不新增 Win32/x86 支持。

- `VtkAppHostSession` 是 Host 的组合根。外部只通过 Host API 的请求、结果、状态快照和 endpoint 交互，拿不到 `AppRuntime`、`Context`、窗口实现或内部 service。

<a id="feature-dependencies"></a>

- Host、Host API 和 Feature SPI 不依赖任何具体 Feature；Feature 之间互不依赖；Feature 不包含 Host 私有头。

<a id="interface-targets"></a>

- `MVVCVTK::HostAPI` 和 `MVVCVTK::FeatureSPI` 是自足接口 target。Host 公开依赖 HostAPI、内部使用 FeatureSPI；Feature 公开依赖 FeatureSPI。

- SDK 按批准白名单交付；产品、公开分面、依赖和排除内容见 `SDK_DELIVERY.md` 第 4、7 至 9 节。

<a id="feature-trust-data"></a>

- Feature 是可信的进程内扩展，不是沙箱。普通 Host 数据读取用不暴露 VTK 对象身份、scalar 指针或内部 mapper/actor 身份的值类型；只有经过审查的 SPI 能力可以携带 VTK 快照或窄 VTK 契约。

<a id="qt-boundary"></a>

- Qt 由接收方宿主提供，不进入 Host 或 Feature；Qt 类型不进入 Host API、Feature SPI 或 Feature 公共 API。SDK 交付边界见 `SDK_DELIVERY.md` 第 8.3 节。

<a id="vendor-boundary"></a>

- DefX 只存在于 GapAnalysis 的私有 bridge/runtime 中；私有三方 bridge 的供应商 C++、STL、VTK 类型不能穿过对应的 C/POD 边界。

- `FeatureSupport` 只是仓内构建期支持层：保持最小，不承载单个 Feature 的业务；安装边界见 `SDK_DELIVERY.md` 第 4.3 节。

- 不新增全局 SceneManager、全局 Renderer 扫描器或跨 Feature Manager。

- `examples/standalone` 只用于仓内组合与运行验证，不决定产品边界；交付排除项见 `SDK_DELIVERY.md` 第 9 节。
<!-- 规范节结束：AGENTS.md#load-architecture -->

<a id="load-information"></a>
<!-- 规范节开始：AGENTS.md#load-information -->
## 4. 事实来源与授权

本项目构建配置为 CMake；事实取证顺序见 md/development/core/ENGINEERING.core.md 第 4 节。

通用定义见 md/development/core/AGENTS.core.md 第 1.2 节。

裁决、通知与独立授权见 md/development/core/LOOP.core.md 第 14.1 节；SDK 边界的具体判定与落实见 md/development/SDK_DELIVERY.md 第 3 节。
<!-- 规范节结束：AGENTS.md#load-information -->

<a id="load-startup"></a>
<!-- 规范节开始：AGENTS.md#load-startup -->
## 5. 开始任务前

适用规范与 Loop 的读取、Git 四项现状检查、目标材料核对的完整开工顺序见 md/development/core/LOOP.core.md 第 0 节；启动自检见其第 0.1 节。

- 是否涉及 API/SPI、CMake/安装、线程、第三方、性能或破坏性迁移；涉及 SDK 时写清授权来源和具体变化；
<!-- 规范节结束：AGENTS.md#load-startup -->

<a id="load-git-permissions"></a>
<!-- 规范节开始：AGENTS.md#load-git-permissions -->
## 6. Git 权限一览

开工固定四项检查及其权限见 md/development/core/LOOP.core.md 第 0 节；其他 Git 权限、主干与远端操作权限、提交前的暂存边界见 md/development/core/GIT_WORKFLOW.core.md 第 0 节；提交范围与说明见其第 3 节。

本项目存放位置见 md/development/GIT_WORKFLOW.md 第 2.1、2.3 节，清理与笔记归档见其第 6.6 节。
<!-- 规范节结束：AGENTS.md#load-git-permissions -->

<a id="load-documents"></a>
<!-- 规范节开始：AGENTS.md#load-documents -->
## 7. 文档规则

通用定义见 md/development/core/LOOP.core.md 第 7 节。
<!-- 规范节结束：AGENTS.md#load-documents -->

<a id="load-results"></a>
<!-- 规范节开始：AGENTS.md#load-results -->
### 7.1. 结果文件与报告

通用定义见 md/development/core/LOOP.core.md 第 7.1 节。
<!-- 规范节结束：AGENTS.md#load-results -->

<a id="load-principles"></a>
<!-- 规范节开始：AGENTS.md#load-principles -->
## 8. 修改原则

**在保证业务正确、精度和生命周期的前提下：内存尽量低，逻辑尽量简单，渲染尽量快。** 三者需要取舍时，依据当前需求和可比证据选择最简单的可行方案。

通用定义见 md/development/core/ENGINEERING.core.md 第 8 节。

机器相关的数字和测试门限不进入 Host 公共 API。

- 新增公共能力前先判断它属于哪一层：Host 通用能力、Feature 公共入口、Feature SPI，还是 Feature 私有实现。判断不了就先不写。

- 公开契约变化不能按私有实现处理；SDK 契约变化按 `SDK_DELIVERY.md` 第 3.2 节核对。
<!-- 规范节结束：AGENTS.md#load-principles -->

<a id="load-feature-boundaries"></a>
<!-- 规范节开始：AGENTS.md#load-feature-boundaries -->
## 9. Feature、CMake 与 SDK 边界

- Feature 通过 `HostFeature` 和窄端口接入，不取得 Host 私有对象。具体规范见 `FEATURE_DEVELOPMENT.md`。

- 新功能须完成 CMake 构建和测试接入，见 `FEATURE_DEVELOPMENT.md` 第 10.1 节；CMake 修改的解决方案同步与完整性核对，按本文件第 11.1 节的适用范围执行。

- 仓内构建接入不等于 SDK 准入。新 Feature 的默认边界和隔离要求按 `SDK_DELIVERY.md` 第 16 节执行。

- Host 在 Feature 关闭时必须能独立配置和构建。已批准的公共契约变化需要同步修改其他 Feature 的调用点时，这属于范围内修改，不算跨 Feature 依赖。

- 测试 target 的复用、目录和 IDE 分组见 `TESTING.md` 第 7.2.1 节。
<!-- 规范节结束：AGENTS.md#load-feature-boundaries -->

<a id="load-code"></a>
<!-- 规范节开始：AGENTS.md#load-code -->
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
<!-- 规范节结束：AGENTS.md#load-code -->

<a id="load-project-risk"></a>
<!-- 规范节开始：AGENTS.md#load-project-risk -->
## 11. 本项目判级细则

等级定义与通用判级方法见 md/development/core/ENGINEERING.core.md 第 11 节。本节只补充本项目的具体命中条件与最低验证，不重新定义等级。只修改现行项目规范时，按该文件第 11.1 节的规范验证映射执行，不套用下列产品构建与业务测试清单。涉及 CMake 修改时，configure 与解决方案完整性核对的适用范围另见 本文件第 11.1 节；该节同时指明测试注册与纯注释的既有例外。

- D0 的本项目范围：普通 Markdown 文档或纯注释，包含 CMake 注释；现行规范的专门定级与通用验证见 md/development/core/ENGINEERING.core.md 第 11 节，Git 跟踪状态对应的文件检查见 md/development/core/TESTING.core.md 第 0 节。
- R1：外部契约不变、影响局限且可恢复的私有实现，包括往已有 target 加源文件（只调整私有源码列表）；仅给已有 target 增加或修改测试注册及测试属性（`add_test`、`set_tests_properties` 等，不新增 target、不改 target 属性和依赖）也属 R1。受影响 target 必须完成 Debug x64 构建、相关真实小数据回归和无关 diff 审查；Release 敏感路径补 Release。直接相关测试与失败路径审查按 core 的 R1 要求执行。
- R1 的测试注册例外：仅有上述测试注册或测试属性改动时，最低验证改为重新 configure，确认目标用例已注册（`ctest -N` 只用于注册确认，不算通过），构建该已有 target，并实际运行受影响的用例。
- **R2、R3 共同的最低验证**：Debug/Release 相关构建和测试；只验证实际受影响的 Feature 开关、Host 独立构建和消费者边界。
- R2：公共头、HostAPI/FeatureSPI、新增 target，或改变现有 target 的类型、对外编译接口、依赖传播、构建开关、导出或安装行为；header surface、PUBLIC 依赖、安装/package 均按 R2。最低验证按本节上述共同要求执行。
- R3：Host 核心机制、线程/停止、数据发布、渲染资源、第三方 bridge/runtime 按 R3。除 core 的 R3 完整测试与独立复核要求外，在本节上述 R2、R3 共同最低验证基础上，执行并发、取消、回滚、负例专项和真实数据或质量审计。三层协作中的独立复核按 md/development/core/COLLABORATION.core.md 第 13.2 节执行。
- 性能改动按实际改动范围判级，须提供改动前后可比较的测量；不把性能主题本身作为自动升入 R3 的理由。
- 只调整已有 target 的私有源码列表按 R1；命中本项目其他 R2/R3 条件时升级。各级验证仍按实际受影响的范围选择，不把级别继承理解为重跑无关模块。
- 缺少环境或数据时，列出未执行项和可复现的命令。
<!-- 规范节结束：AGENTS.md#load-project-risk -->

<a id="load-cmake-check"></a>
<!-- 规范节开始：AGENTS.md#load-cmake-check -->
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
<!-- 规范节结束：AGENTS.md#load-cmake-check -->

<a id="load-completion"></a>
<!-- 规范节开始：AGENTS.md#load-completion -->
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
<!-- 规范节结束：AGENTS.md#load-completion -->

<a id="load-collaboration"></a>
<!-- 规范节开始：AGENTS.md#load-collaboration -->
## 13. Skill、图谱与多 Agent

通用定义见 md/development/core/ENGINEERING.core.md 第 13 节。

通用定义见 md/development/core/COLLABORATION.core.md 第 13 节。
<!-- 规范节结束：AGENTS.md#load-collaboration -->

<a id="load-models"></a>
<!-- 规范节开始：AGENTS.md#load-models -->
### 13.1. 三层分工

| 层级 | 模型与思考强度 |
| --- | --- |
| 协调者 | — |
| 执行者 | Codex `gpt-6.1-sol`，思考强度 high |
| 子代理 | Codex `gpt-6-luna`，思考强度 low |

通用定义见 md/development/core/COLLABORATION.core.md 第 13.1 节。

运行时具体值见本文件第 0 节；配置路径及权限按开工闸门选择当前运行时适配；Codex 的对应正文见 md/development/platform/runtime/CODEX.md 第 1、2 节。
<!-- 规范节结束：AGENTS.md#load-models -->

<a id="load-coordinator"></a>
<!-- 规范节开始：AGENTS.md#load-coordinator -->
### 13.2. 协调者

通用定义见 md/development/core/COLLABORATION.core.md 第 13.2 节。

本项目资源占用重的任务包括全量构建、整卷数据和 GPU 任务。

本项目公共契约包括根 CMake、API/SPI、SDK 头映射和共享状态机。

- 按 本文件第 11.1 节需要核对解决方案时，必查核对结果；缺少就退回补查。

关键 SDK 边界改动同样触发 md/development/core/COLLABORATION.core.md 第 13.2 节 的独立复核流程。
<!-- 规范节结束：AGENTS.md#load-coordinator -->

<a id="load-executor"></a>
<!-- 规范节开始：AGENTS.md#load-executor -->
### 13.3. 执行者

通用定义见 md/development/core/COLLABORATION.core.md 第 13.3 节。

本项目摘要还需写明是否触及公开头、CMake、线程、数据发布或 SDK 边界。

- 按 本文件第 11.1 节需要核对解决方案时，列出重新 configure 的工作树和 preset、解决方案应有与实际的项目数，以及缺失、多余和分组差异；差异应为空，非空时说明原因并按该节处理。
<!-- 规范节结束：AGENTS.md#load-executor -->

<a id="load-subagent"></a>
<!-- 规范节开始：AGENTS.md#load-subagent -->
### 13.4. 子代理

通用定义见 md/development/core/COLLABORATION.core.md 第 13.4 节。
<!-- 规范节结束：AGENTS.md#load-subagent -->

<a id="load-report"></a>
<!-- 规范节开始：AGENTS.md#load-report -->
## 14. 提问与完成报告

协调者给用户的裁决、通知与回复模板见 md/development/core/LOOP.core.md 第 14 节；执行者报告与子代理回执见 md/development/core/COLLABORATION.core.md 第 13.3、13.4 节。

本项目报告中的主要修改还须说明 CMake 接入方式及 SDK 边界实际变化；SDK 具体边界引用 md/development/SDK_DELIVERY.md 第 3 节。
<!-- 规范节结束：AGENTS.md#load-report -->

<!-- 规范文件结束：AGENTS.md -->
