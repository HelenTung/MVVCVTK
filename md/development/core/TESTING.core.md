# 测试证据与完成声明

> 加载范围与角色身份见 md/development/core/AGENTS.core.md 第 0 节；文件路径以规范包根为基准。参数只承接目录、格式、命令、模型和预算等具体值；本文件不要求任何特定项目架构。

<a id="load-evidence"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-evidence -->
## 0. 证据与完成声明

- 测试范围只覆盖本次修改、实际受影响的路径和明确的验收范围，不跑无关模块。具体的范围、证据复用和结果状态以 md/development/core/TESTING.core.md 为准。

- 不写自证式测试，即没有独立预期、坐标或单位错误、或者为了通过而放宽判定的测试。测试的门限要有来源，不在用例中散落常量。

- 编译通过不等于边界通过；仓内通过不等于安装态通过；实验组合通过不等于主干已验证。

完成声明按证据级别分开说，不跨级概括为“全部完成”。

有实现但缺真实数据时，可以提交并报告“实现完成，真实数据验收未执行”。

分别报告静态检查与实际运行、单项与组合、源码环境与已安装或部署产物、实验结果与实际主分支结果。项目有完成级别细则时按其细则命名；没有细则也不能把某一层证据概括为其他层已经通过。

被跟踪的文件使用 `git diff --check` 检查差异；被忽略或未跟踪的文件直接检查文件本身，不能把空的 Git 差异检查当作通过证据。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-evidence -->

<a id="load-test-scope"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-test-scope -->
## 3. 测试范围

具体条款见以下子节。

### 3.2. 按阶段确定执行范围

优先按现有 target、用例、标签或参数筛选；入口不能继续细分时，运行最小可执行单元并说明原因。映射缺失时先查代码，有具体影响疑点再补测试。

已有有效覆盖按 md/development/core/TESTING.core.md 第 19.2 节复用。局部修改不默认触发全量清理重建、无关大数据、完整打包或长期压力测试。范围控制不允许永久禁用有效测试或隐去本次必需项。归因不明的失败先调查，已确认无关的历史失败单独说明。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-test-scope -->

<a id="load-admission"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-admission -->
## 4. 验收条件

具体条款见以下子节。

### 4.3. 先定义验收条件

查看候选结果前确定输出、参考、容差、失败分类和性能目标。改变旧预期时，先给出契约、参考或输入变化的独立依据；涉及验收标准变化的，按 md/development/core/COLLABORATION.core.md 第 13.2 节处理，测试纠错按 md/development/core/TESTING.core.md 第 7.6 节处理。不能为获得通过而放大容差、删掉困难样本、关闭承诺能力或把失败改为跳过。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-admission -->

<a id="load-reference"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-reference -->
## 6. 预期与参考

具体条款见以下子节。

### 6.1. 参考结果的来源

参考可来自经复核人工标注、已验收基准、适用独立方法或明确业务不变量。引用其方法、版本、适用范围、复核依据和局限。被测实现首次输出不能自动成为唯一答案；旧版本对拍只证明回归一致，同一底层算法对拍也不是独立验证。参考变更单独审查，保留旧预期和变更理由。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-reference -->

<a id="load-test-design"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-test-design -->
## 7. 用例有效性

具体条款见以下子节。

### 7.1. 用例的最小结构

用例说明保护的契约或缺陷、能检出的错误、输入、实际入口、事件顺序、输出断言和退出条件。已有测试代码能表达时直接使用。实例由用例 ID、数据/参数、构建配置和必要运行模式确定；循环、重试和独立样本分别计数。

### 7.6. 测试有效性、纠错与去重

每个有效用例应保护当前契约、具体风险或仍受支持的历史缺陷。已有覆盖充分时直接复用，适用门禁通过且无具体剩余风险时停止扩测。

怀疑测试错误时，先核对参考、公式、单位、坐标、前提、同步和入口契约；把原失败的必要证据写进测试报告或任务记录（记录方式见 md/development/core/LOOP.core.md），并暂停相关验收。依据独立证据区分测试缺陷与产品缺陷，修正后重跑，不能迎合当前实现。

### 7.7. 过时测试清理

本轮触及的契约或流程已经移除、失效或被有效替代时，同步清理相关用例、无用 fixture/helper、target、注册和筛选引用。年代久、执行慢或失败本身不是删除理由。

变更说明写清失效依据和替代覆盖；重新配置、核对发现和筛选结果，并运行相关有效测试。删除不记通过，真实缺陷和仍承诺能力不能随用例一起消失；真实数据、参考和唯一证据不在顺带清理范围。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-test-design -->

<a id="load-performance"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-performance -->
## 12. 性能比较

具体条款见以下子节。

### 12.1. 性能准入与比较条件

优化与相对回归需要可比历史基线。全新动作先验证正确性、资源/响应约束和适用绝对目标，再以代表性实测建立后续基线；不能用同次结果自证无回归。

### 12.3. 可重复测量

中位数、尾部指标带样本数；样本不足不作稳定分位数结论。重启不代表冷盘，完整哈希读取也可能改变缓存条件。

绝对目标和相对回归分别判定；基线更新需独立依据，不能随退化滚动放宽。有效运行违反单次硬上限即按该上限判定；测试保护超时按 md/development/core/TESTING.core.md 第 16.4 节执行，剖析运行与正常运行分开。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-performance -->

<a id="load-integration"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-integration -->
## 14. 多分支集成与滚动基线回归

流程和强同步条件见 md/development/core/GIT_WORKFLOW.core.md 第 4.3 节、md/development/core/GIT_WORKFLOW.core.md 第 4.4 节。源分支同步后核验真实业务、变化契约、共享模块和冲突适配；实验合并后补测累计影响与新增组合路径。

最终组合补齐共享资源、切换和退出缺口。祖先关系只证明历史包含，不能替代这些验证。

按 md/development/core/GIT_WORKFLOW.core.md 第 4.6 节暂停受影响后续验收，把失败结果和上次有效证据写进测试报告或任务记录（记录方式见 md/development/core/LOOP.core.md）。源分支受阻、实验目标失败和强同步屏障未满足分别记录，不合并成整批通过。修复后重测原失败及受影响路径；未受影响证据可复用。恢复时重新核对当前源码、依赖组和已验证状态，不能继承失效的整体通过结论。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-integration -->

<a id="load-execution"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-execution -->
## 16. 执行与清理

具体条款见以下子节。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-execution -->

<a id="load-test-entry"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-test-entry -->
### 16.3. 专项入口

真实数据路径、参数、测试 ID 和质量审计入口从当前代码确认。没有运行器或注册时报告缺口，在授权范围补齐；不能编造 `--real-data` 等开关写成现有命令。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-test-entry -->

<a id="load-process-cleanup"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-process-cleanup -->
### 16.4. 进程与资源清理

- **测试保护超时**只用来防止测试进程跑飞，不作为验收判据，数值从测试参数中取。

- 各层 Agent 启动测试、构建、应用、脚本或服务时，记录进程 ID 并设置进程保护超时；测试保护超时按本节定义执行。

- **结束前核对**：提交完成报告前及任务结束前，确认本任务启动的任务进程及其子进程树已退出。

- **有界停止顺序**：对仍运行且归属明确的进程，先正常停止，超时后再终止。

- **未核实项**：资源释放无法观测或无法归属时，报告未核实项，不声明全部释放。

- 只终止自己启动的进程，不终止其他会话或用户的进程；无法确认归属的，报告给协调者，不擅自终止。

- 报告中写明清理结果：终止了哪些进程、是否还有残留。有残留且无法清理的，作为未完成事项上报。

- 发起测试的 Agent 结束后，不能留下仍在运行的测试或常驻进程。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-process-cleanup -->

<a id="load-results"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-results -->
## 17. 状态与恢复

具体条款见以下子节。

### 17.1. 用例状态

每个适用实例只有一个结果状态。N/A 是范围标记，不是执行结果；无关用例不登记，原计划排除项保留必要原因。本次承诺但未实现的能力是缺口，不能记为 N/A。执行结果与是否属于本轮必需门禁分别记录；只有本轮必需门禁的 NOT_RUN 阻塞对应准出，专项明确允许不执行的可选项仍如实记为 NOT_RUN 并标明“非门禁”，不能计为通过，也不能把本轮承诺项改标可选来解除阻塞。

| 状态 | 定义 | 必需门禁 |
| --- | --- | --- |
| `PASS` | 满足全部断言和退出检查；历史通过复用须标明 | 形成有效覆盖 |
| `FAIL` | 输出、行为或崩溃违反预期，或超出本次验收约定的有效期限 | 阻塞 |
| `BLOCKED` | 缺必需数据、环境、参考、目标或测量能力，无法有效判定 | 阻塞 |
| `NOT_RUN` | 适用但未运行，或必需测试未实现/注册 | 本轮必需门禁阻塞；明确可选的非门禁项按本节记录与披露 |
| `SKIP` | 本应执行的实例被跳过 | 阻塞，不能计通过 |

`FLAKY` 是执行不一致的风险标记。未解释的间歇失败仍阻塞，不能用最后一次成功覆盖。

### 17.2. 数量对账

```text
本次适用实例数 = PASS + FAIL + BLOCKED + NOT_RUN + SKIP
```

核对计划、发现/注册、实际执行和重试；同一实例只计一次覆盖，历史复用与本次执行分开。开始次数不必等于 PASS+FAIL，开始后也可能因环境问题而阻塞。

人工核验单列，未注册必需自动化记未运行；零命中不能判通过。通过比例或代码覆盖率不能抵消关键失败。

### 17.3. 环境问题与产品缺陷

必要输入或元信息缺失、错配、损坏导致无法有效运行时，记阻塞。普通测试未生成来源清单或哈希不构成阻塞。有效输入下崩溃、错误输出、旧版本覆盖、越界和错误 runtime 回退记失败；预期拒绝正确命中的负例可通过。

### 17.5. 修复、重试与中止

依据 md/development/core/TESTING.core.md 第 7.6 节、md/development/core/TESTING.core.md 第 7.7 节核对失败用例有效性，修复后重跑对应有效用例及必要相邻回归，把原失败和处理依据写进测试报告或任务记录（记录方式见 md/development/core/LOOP.core.md）。

中止只终止本次隔离进程，不能杀其他工作树任务或释放活动任务使用的对象。输入身份、关键日志和必要诊断写进测试报告或任务记录（记录方式见 md/development/core/LOOP.core.md）。

### 17.6. 暂停后恢复

恢复前先读已有结果，核对进程、资源、源码、二进制和输出的实际状态；同一测试尚未结束时不重复启动，已完成且仍有效的证据按 md/development/core/TESTING.core.md 第 19.2 节复用。确认阻塞消除、输入和依赖可用后，再运行尚缺或已失效的原失败或阻塞项及必要回归，不能从最后一步继续并继承旧的整体通过。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-results -->

<a id="load-reuse"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-reuse -->
## 19. 证据复用

具体条款见以下子节。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-reuse -->

<a id="load-report-routing"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-report-routing -->
### 19.1. 追溯与报告

证据追溯与报告信息要求见 md/development/core/LOOP.core.md 第 7.1 节；报告入口按收件人区分：执行者给协调者按 md/development/core/COLLABORATION.core.md 第 13.3 节，子代理给执行者按其第 13.4 节，协调者给用户按 md/development/core/LOOP.core.md 第 14.2 节。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-report-routing -->

<a id="load-evidence-reuse"></a>
<!-- 规范节开始：md/development/core/TESTING.core.md#load-evidence-reuse -->
### 19.2. 证据失效与复用

实现、测试、输入、参数、参考、配置、依赖/runtime 或契约变化，只使受影响证据失效。相同相关行为和条件已有有效证据时可复用，不因无关提交或阶段切换重跑。提交数量不作为判断证据是否失效的依据。

从完整差异、配置和执行记录核实适用性；合并和冲突适配补测新增组合影响。不能只凭同分支名或原始基线沿用结论，也不把历史执行改称本次执行。
<!-- 规范节结束：md/development/core/TESTING.core.md#load-evidence-reuse -->

<!-- 规范文件结束：md/development/core/TESTING.core.md -->
