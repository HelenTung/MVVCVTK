# Git 与工作树模型

> 加载范围与角色身份见 md/development/core/AGENTS.core.md 第 0 节；文件路径以规范包根为基准。参数只承接目录、格式、命令、模型和预算等具体值；本文件不要求任何特定项目架构。

## 0. Git 权限

| 操作 | 默认权限 |
| --- | --- |
| 除 md/development/core/LOOP.core.md 第0节固定四项检查外的只读检查、diff、log、fetch | 可以直接执行 |
| **非主分支上本任务的 add/commit** | **可以直接执行**，须只含本任务内容 |
| 已批准集成批次内的 baseline 合并与冲突修复 | 可以直接执行，按 md/development/core/GIT_WORKFLOW.core.md 逐步验证 |
| 主干实现的提交与合并 | 授权按 md/development/core/LOOP.core.md 第 14.1 节 |
| push、tag、release、创建 PR、删除远端分支 | 授权按 md/development/core/LOOP.core.md 第 14.1 节 |
| rebase、amend、force push、hard reset、强制删除或丢弃未知内容 | 不默认执行 |

**提交前的暂存边界**：

- 提交前审查整个工作树和 index，用明确路径暂存。
- 不要用 `git add .`、`git add -A`、`git commit -a`。
- 不回退、不格式化与本任务无关的改动。

提交说明按 md/development/core/GIT_WORKFLOW.core.md 第 3 节 执行；删除条件见 md/development/core/GIT_WORKFLOW.core.md 第 6 节。

## 1. 基本约定

具体条款见以下子节。

### 1.1. 主工作树与主分支

- 同一批并行开发的任务分支共用原始主干 `baseCommit`。批次外的新任务重新核对主干；需要叠加未合并内容时，先列出依赖与继承影响，按 md/development/core/LOOP.core.md 第 14.1 节决定，遵守用户锁定的任务范围。

- 已合并、已关闭或属于其他任务的分支，不改名、不 reset 后挪作他用；派生工作树的用途按 md/development/core/GIT_WORKFLOW.core.md 第 2.1 节执行。同一个未结束的任务可以接着用原来的分支和工作树。

### 1.2. 远端状态

- 任务要求使用最新远端时，远端核实、派生起点选择与创建命令统一按 md/development/core/GIT_WORKFLOW.core.md 第 2.1 节执行。

- 网络不通时，记下实际使用的引用和 SHA，不把旧引用说成最新。

- 远端前进了，只有在会覆盖未知改动、改变已批准范围或让已有证据失效时，才阻塞相应的写入操作。

### 1.3. baseline 与基线字段

baseline 树指任务从中派生、或者要合回去的那棵工作树，可以是主工作树、实验工作树或其他工作树，不特指主分支。派发中没有写明 baseline 时，baseline 默认是该会话所在的工作树。根 baseline 指一个协调者目标最初从中派生任务的那棵树。一个目标可以派生多层工作树，最终都合回同一个根 baseline；例如 A 派生 B、C，B 再派生 D、E，这个目标的根 baseline 是 A。baseline 写入限制的适用范围见 md/development/core/GIT_WORKFLOW.core.md 第 2.1 节。

提交基线用下面的字段区分：

| 字段 | 含义 |
| --- | --- |
| `baseCommit` | 本批开发的原始主干 SHA，冻结后不改 |
| `devStartCommit` | 开发分支的实际起点 SHA，创建后记录，不假定等于 `baseCommit` |
| `experimentBaseCommit` | 实验分支创建时的主干 SHA，不可改写 |
| `integrationBaselineCommit` | 实验分支上最近一次合并并验证通过的 SHA，逐步前进 |
| `syncedBaselineCommit` | 某个待合分支最近实际吸收并验证过的集成 SHA |
| `sourceHead` / `targetHead` | 本次实际被审查、合并和测试的提交，不能用分支名代替 |

分支集成任务按需记录这些字段；`devStartCommit` 的记录见 md/development/core/GIT_WORKFLOW.core.md 第 2.1 节。字段放在会话或任务记录中，不另建登记文件；记录方式见 md/development/core/LOOP.core.md。文中同名的尖括号占位符沿用表中含义。

`<verifiedIntegrationSHA>` 是当前已验证的集成提交 SHA。

## 2. 工作树

具体条款见以下子节。

### 2.1. 目录与创建

- **工作树选择**：验证、开发、构建和测试类工作一律新建派生工作树。只读和只写文档任务不建树，分类按 md/development/core/LOOP.core.md 执行，纯文档维护见 md/development/core/GIT_WORKFLOW.core.md 第 7 节。

- **baseline 写入范围**：仅限本节的发行和合并、md/development/core/LOOP.core.md 第 3 节规定的收尾记录写入或更新、md/development/core/GIT_WORKFLOW.core.md 第 7 节规定的纯文档维护。baseline 写入限制只针对当前任务把这棵树当作上游或接收方时的写入；这棵树自身作为派生树承担的原有用途（例如开发树本身的开发工作）不受影响。

- baseline 上的发行与合并如何记录，按 md/development/core/LOOP.core.md 开头的分类执行。

- 收尾记录的写入权限只覆盖 md/development/core/LOOP.core.md 第 3 节规定的文件，不授权源码修改或 baseline 构建。

- **派生工作树只用于一个用途**：从 baseline 派生出来的工作树（开发工作树、测试验证用工作树等）只服务一个用途，即一个已声明的开发任务、一次测试验证或一次复现。不同用途不能同时操作同一个派生工作树，也不能在用途结束后改作他用；新用途新建工作树。同一用途内由多个执行者分工时，按 md/development/core/COLLABORATION.core.md 第 13.2 节的写入隔离执行。用户明确要求在原树继续并调整目标的例外按 md/development/core/LOOP.core.md 第 4 节执行。

- **baseline 树**：含义见 md/development/core/GIT_WORKFLOW.core.md 第 1.3 节。主工作树和实验工作树可以承接多个用途，写入范围仍按本节“baseline 写入范围”执行。多个用途的任务调度按 md/development/core/LOOP.core.md 第 2.1 节执行。写入前的占用检查按 md/development/core/LOOP.core.md 第 5 节执行。

- `<repoParent>` 是主工作树的父目录；`<devBranch>` 是按 项目配置中的分支或任务命名格式确定的本次任务分支全名。

- `<branchKey>` 默认把分支名中的 `/` 换成 `-`。创建前检查名字冲突、大小写冲突、目录占用和远端同名分支；冲突时加任务后缀，不覆盖已有目录。

- `<baselineBranch>` 是 baseline 树当前所在分支，创建前核实该树的分支和 HEAD。

- 分支命名见 项目配置中的分支或任务命名格式。一个分支只挂一个工作树，新建工作树时不用 `--force` 绕过分支已检出的保护。

未要求使用最新远端时，从 baseline 树当前分支创建派生工作树：

```bash
git worktree add -b "<devBranch>" "<worktreePath>" "<baselineBranch>"
```

要求使用最新远端时，先核实 baseline 本地分支对应的远端分支：采用任务明确指定的对应关系；未指定时，使用 `<baselineBranch>` 的上游分支（upstream）。无法确定对应关系时先报告，不默认改用主分支。`<baselinePath>` 是已核实的 baseline 工作树绝对路径；`<remote>` 是对应远端名称，`<remoteTrackingRef>` 是对应远端跟踪引用的完整名称（如 `refs/remotes/<remote>/<remoteBranch>`）。

先 fetch，确认成功且覆盖对应分支后，再把 baseline 本地分支 HEAD 和本次 fetch 更新的对应远端提交分别固定为 `<localHead>`、`<remoteHead>`，均使用完整 SHA；不移动 baseline 的本地分支。按以下命令核实，两次 `rev-parse` 的输出依次作为这两个 SHA：

```bash
git -C "<baselinePath>" fetch --prune "<remote>"
git -C "<baselinePath>" rev-parse --verify "refs/heads/<baselineBranch>^{commit}"
git -C "<baselinePath>" rev-parse --verify "<remoteTrackingRef>^{commit}"
git -C "<baselinePath>" rev-list --left-right --count "<localHead>...<remoteHead>"
```

最后一条命令的两列依次是本地独有、远端独有的提交数。以提交包含关系判断起点，不按提交时间判断新旧：

| 本地独有提交数 | 远端独有提交数 | 关系 | `devStartCommit` |
| --- | --- | --- | --- |
| 0 | 大于 0 | 本地落后于远端 | `<remoteHead>` |
| 0 | 0 | 本地与远端相同 | `<localHead>` |
| 大于 0 | 0 | 本地领先，含未推送提交 | `<localHead>` |
| 大于 0 | 大于 0 | 本地与远端已分叉 | `<localHead>`，并在报告中列出起点未包含的远端提交 |

分叉时，用以下命令列出起点未包含的远端提交，报告保留完整 SHA 和提交主题：

```bash
git -C "<baselinePath>" log --format="%H %s" "<devStartCommit>..<remoteHead>"
```

用选定的 SHA 创建派生工作树：

```bash
git worktree add -b "<devBranch>" "<worktreePath>" "<devStartCommit>"
```

创建后把实际起点提交 SHA 作为 `devStartCommit` 交给协调者，由协调者记入该目标的任务进度文件。同一批并行开发需要冻结同一起点，或从历史提交续接时，也使用提交 SHA 作起点，并写明原因。

任务续接与新任务的判定按 md/development/core/LOOP.core.md 第 4 节执行。原工作树已删除而同一未结束任务的分支仍在时，重新挂载：

```bash
git worktree add "<worktreePath>" "<devBranch>"
```

创建或重新挂载后，用 `git worktree list --porcelain` 核对分支和 HEAD。各工作树共享分支引用，不要从别的目录移动、reset 或删除正在使用的分支。

### 2.2. 规范文件的初始化

- 规范文件随基准提交正常检出，不另外复制。

- 如果本轮明确要求使用的规范在基准提交里缺失，新建工作树时可以从已核实的来源补齐一次；规范文件的位置见 md/development/core/LOOP.core.md 第 7 节。只补缺失的规范，不复制计划或其他文档。历史 任务进度文件 的续接复制按 md/development/core/LOOP.core.md 第 4 节执行。

- 目标文件已存在但内容不同、来源不可读或版本无法确认时，保留原文件并报告，不覆盖。不用软链接、修改 ignore 或隐藏 index 标志绕过检查。

- 初始化之后，规范文件在这个工作树里默认只读；维护按 md/development/core/LOOP.core.md 第 7 节执行，优先级按 md/development/core/AGENTS.core.md 第 1.1 节执行。

### 2.3. 构建隔离

- 每个工作树的可写构建状态和产物按树隔离，不共用可写状态，也不拿别的工作树的产物冒充本工作树的结果。

- 已锁定的依赖和原始测试数据可以只读共享；计算缓存、派生数据和测试结果按工作树隔离。

- **临时输出**：PNG、CSV 和日志默认放在派生工作树的 临时输出目录 下，随工作树删除。md/development/core/GIT_WORKFLOW.core.md 第 2.1 节规定的发行与合并可以使用 baseline 的输出目录；收尾回收范围按 md/development/core/GIT_WORKFLOW.core.md 第 6.6 节执行，删除失败按 md/development/core/GIT_WORKFLOW.core.md 第 6.4 节处理。

- 用户明确要求保留输出时，由协调者在派发中写明保留文件和位置。

## 3. 开发分支上的提交

- 一个分支只做已声明的任务，不顺带吸收别的功能、格式化或历史清理。

- 提交许可按 md/development/core/GIT_WORKFLOW.core.md 第 0 节 执行；用户明确要求不提交时除外。

- 可以暂存审查后选定的块；不取消别人已暂存的内容。其余提交前检查按 md/development/core/GIT_WORKFLOW.core.md 第 0 节 执行。

- 一次提交对应一个可审查的切片：一个业务切片、一个修复或一次 baseline 同步。WIP 提交不等于验证完成。

- 本地提交的许可不包括 push、tag、release、PR、主干提交、无关分支合并、amend 或 rebase。

- **提交说明使用中文**，包括合并提交和冲突修复提交：标题一句话说明做了什么，需要时空一行写正文。代码标识符、文件名、路径、命令和错误码原样保留；不使用 `feat:`、`fix:` 这类英文前缀，也不保留 Git 默认生成的 `Merge branch ...` 英文说明。

## 4. 实验集成

具体条款见以下子节。

### 4.1. 实验分支

- 普通合并默认进入实验分支，不进入主干。用户批准一组分支的集成，就等于授权这组分支内必要的 baseline 同步、冲突处理、本地提交和累计验证；不包括无关分支、push 或主干合并。

- 每一轮集成从本轮核实的主干精确提交新建 实验分支格式 和对应工作树，记下 `experimentBaseCommit`。

- 只有满足以下全部条件时，才能续用旧的实验分支：同一批次尚未结束、授权的分支组合不变、本次核实的主干 SHA 与原 `experimentBaseCommit` **完全相同**。名字相同或有共同祖先都不算。

- 主干已变化或批次已结束时，新开一轮。旧实验分支不 reset、不改名，本地分支按 md/development/core/GIT_WORKFLOW.core.md 第 6.4 节处理；旧实验工作树按 md/development/core/GIT_WORKFLOW.core.md 第 6.6 节确定清理时机。

- 对外说明状态时写成：“已合入实验分支，尚未合入主干”。

### 4.3. 每一步合并

默认做法是：**轮到哪个分支，哪个分支先吸收当前已验证的实验结果，然后再合入。** 不要求每合一次就让所有待合分支同步。

1. 把当前已验证的实验 HEAD 记为 `B`。

2. 在下一个源分支自己的工作树里合入 `B`，在授权范围内解决冲突，验证受影响的部分后提交。如果它已经包含 `B`，不制造空提交。

3. 审查这个源分支相对 `B` 的完整 diff，然后把这个精确提交合入实验工作树。

4. 从合并结果派生验证树，补测累计影响和覆盖缺口。验证通过后，这个结果成为新的 `B`；验证不通过，保留失败提交用于修复，暂停实验分支上的后续合并。

5. 其他待合分支保持原状，如实记录“尚未同步”，轮到它时再吸收届时最新的 `B`。

```text
主干 M；A、B、C 都从 M 开发；实验分支 E 从 M 创建
A 已包含 M → 合入 E，验证通过 → E1
B 吸收 E1，解决冲突，验证 → 合入 E，验证通过 → E2
C 直接吸收 E2（不必先吸收 E1），验证 → 合入 E，验证通过 → E3
E3 做最终组合验证；进入主干还需要另外的明确授权
```

在源分支工作树里的操作形式：`<pendingWorktree>` 是已核实的待合源分支工作树绝对路径；`<reviewedPath>` 是审查后选定的暂存路径，`<中文提交说明>` 按 md/development/core/GIT_WORKFLOW.core.md 第 3 节填写。

```bash
git -C "<pendingWorktree>" status --short --branch
git -C "<pendingWorktree>" merge --no-ff --no-commit "<verifiedIntegrationSHA>"
# 有冲突：在授权范围内修复并验证，不整侧取 ours/theirs
git -C "<pendingWorktree>" add -- "<reviewedPath>"
git -C "<pendingWorktree>" diff --cached --check
git -C "<pendingWorktree>" commit -m "<中文提交说明>"
git -C "<pendingWorktree>" merge-base --is-ancestor "<verifiedIntegrationSHA>" HEAD
```

`merge-base --is-ancestor` 只能证明历史包含关系，不能证明语义正确。

### 4.4. 强同步（例外情况）

只有共享契约变化、强耦合，或者用户明确要求同时收敛时，才对一个**有明确名单**的依赖组启用强同步：组内成员全部同步并验证后，才继续合并依赖这个组的分支。记录启用原因、成员和基线。独立分支不加入这个屏障。

### 4.5. 语义冲突与继承关系

- 没有 Git 文本冲突，也要检查改动之间的语义衔接；不以临时包装或平行状态掩盖冲突。

- B 吸收 E1 之后，B 里已经包含 A。之后如果单独把 B 合入主干，必须披露 A 的内容，不能当作只有 B。

- 冲突修复只覆盖本批次授权的范围。涉及超出任务既定范围的业务或依赖、契约时，暂停这一部分并按 md/development/core/LOOP.core.md 第 14.1 节裁决；不将 L2 决定级本身变成额外授权门禁。

### 4.6. 失败与阻塞

| 情况 | 处理 |
| --- | --- |
| 实验分支的合并结果验证失败 | 保留失败的提交和上一个已验证的 `B`；暂停实验分支上的后续合并，直到修复通过 |
| 某个源分支同步失败或环境阻塞 | 暂停这个分支和依赖它的分支；如果用户没有锁定顺序、批准集合不变、依赖关系允许，可以先合其他独立且就绪的分支 |
| 强同步组没有满足屏障 | 暂停依赖这个组的合并 |
| 需要改变批准的分支名单 | 说明原因和继承关系，按 md/development/core/LOOP.core.md 第 14.1 节裁决；不越过用户锁定范围 |

恢复中断的集成前，先核对目标工作树的实际合并状态（是否有未完成的合并、未提交的冲突修复）。不默认 rebase、squash、cherry-pick、自动 stash 或 reset。中止合并前，确认要中止的确实是当前这次合并。受阻的分支保留在本轮的状态记录中，不能说成整批通过。

## 5. 合入主干

- “继续开发”“合并支线”都不等于合入主干。只有用户明确指定主分支和要合入的范围，才能推进主干。

- 授权核对的是**完整的有效变更**：源分支继承来的改动也计入范围。批准 B 不等于批准 B 里带进来的 A。

- 申请合并前报告：原始基线、滚动基线、源和目标的精确 SHA、完整 diff、已执行的验证和未验证项。

- 在主工作树的主分支上执行合并，默认保留合并祖先关系，不擅自 squash 或 rebase。

- 合并后从实际主干结果派生验证树，补测主干差异；复用的实验结果要注明，不能写成主干本次执行的结果。然后按最终结果同步文档。

- push 需要另外的授权。

## 6. 合并后的文档转移与回收

适用删除条件与保护要求只在本节定义。满足适用条件并完成本节检查、协调者验收与删除派发时，视为已获默认授权，可以执行，无需另行请示用户；不满足任一适用条件时仍需用户明确授权。默认授权不改变工具策略拒绝时停止、不绕过保护及精确范围要求。

### 6.1. 什么时候删除工作树

本节规定开发和实验工作树的删除条件；测试验证用工作树按 md/development/core/GIT_WORKFLOW.core.md 第 6.5 节处理。

- 开发工作树满足以下任一条件时，视为已获默认授权，无需另行请示用户，但仍须按第 6.4 节检查、协调者验收并派发删除：
  1. 分支的精确提交已合入指定的目标工作树（实验工作树或主工作树），且目标上的合并验证已通过。这种情况要先按 md/development/core/GIT_WORKFLOW.core.md 第 6.2 节转移文档；
  2. 工作树没有任何改动：没有未提交的修改，分支在 baseline 之外也没有独有提交；
  3. 分支落后于当前已验证的 baseline：分支 HEAD 是 baseline 的祖先（`git merge-base --is-ancestor "<sourceHead>" "<verifiedIntegrationSHA>"` 成立），没有独有提交。

- 三条都不满足时（例如有未合入的独有提交，或有未提交的修改），不删除，需要用户授权。

- 删除工作树时先保留本地分支，便于追溯和披露继承关系；本轮整组进入主干并验证后，再按 md/development/core/GIT_WORKFLOW.core.md 第 6.4 节安全删除。需要精确恢复历史任务的，清理分支前确认对应提交仍被保留的引用可达。远端分支不删除。

- 合并验证失败、还在修复中的分支，即使满足条件 2 或 3，也不删除工作树。

- 放弃的任务等其他情况，需要用户另外授权清理。

- 实验工作树的清理时机按 md/development/core/GIT_WORKFLOW.core.md 第 6.6 节执行，文档转移和删除按 md/development/core/GIT_WORKFLOW.core.md 第 6.2 节、md/development/core/GIT_WORKFLOW.core.md 第 6.3 节、md/development/core/GIT_WORKFLOW.core.md 第 6.4 节执行。

### 6.2. 文档转移

子树结束时只把结果和证据交给根任务负责人，不整理或转移目标收尾文档。整个目标结束后，按 md/development/core/LOOP.core.md 第 3 节确定实际应有的收尾记录，本节只按字节复制这些文件；任务进度文件由协调者转移，执行者不直接修改它。其他收尾资料由执行者按已确认清单准备和转移。

- 普通转移的保留目标为根 baseline。

- 结果再合入上一级时，保留目标为该已批准的接收树。

- 根 baseline 自身将回收且没有接收树时，由协调者明确保留位置，不自动写入主工作树。

1. 核对源与实际保留目标不是同一位置，源与自身比较不算转移。核对跟踪状态，列出需要转移的文件；可以用 `git ls-files`、`git status --short --ignored` 加目录比对。

2. 与实际保留目标的对应文件比较：目标没有，就按字节复制过去；内容相同，就跳过。md/development/core/LOOP.core.md 第 4 节规定的同一历史任务续接收尾时，用新版本更新；其他内容差异不覆盖，列出差异。

3. 需要裁决的差异，上报协调者按 md/development/core/COLLABORATION.core.md 第 13.2 节处理；决定之前不删除源工作树。

4. 在实际保留目标核对文件与源逐字节一致；未完成转移和校验，不删除源树。转移只复制文件，不修改内容。

5. 在报告中写明：转移了哪些、更新了哪些、跳过了哪些、哪些冲突还在等裁决。

其他已有任务文档按 md/development/core/LOOP.core.md 第 3 节处理，未提交或未跟踪文件按 md/development/core/GIT_WORKFLOW.core.md 第 6.3 节核对，输出与共享输入按 md/development/core/GIT_WORKFLOW.core.md 第 6.6 节处理。

### 6.3. 删除前检查

- 路径和分支与登记的工作树对得上，确认满足第 6.1 或 6.5 节的适用删除条件，没有其他任务依赖这个工作树。

- 按 md/development/core/GIT_WORKFLOW.core.md 第 6.2 节核对该树实际需要转移的文件，完成转移与差异处理，没有待裁决的内容冲突。

- 按 md/development/core/LOOP.core.md 第 2 节、md/development/core/LOOP.core.md 第 3 节只核对该树实际应有的记录，确认内容完整、份数和位置正确。任务进度文件应写而未写的由协调者补齐，其他记录由相应写入者补齐；同一目标的子树只核对证据已交给根任务负责人。

- 逐项核对 `git status --short --untracked-files=all`，未提交的修改和未跟踪的源码、文档均已处理；已有任务文档按 md/development/core/LOOP.core.md 第 3 节处理。

- 其他未跟踪文件和被忽略的唯一文件（真实数据、日志、产物）按 md/development/core/GIT_WORKFLOW.core.md 第 6.6 节确认归属和处理方式。Git 显示干净，不代表没有被忽略的唯一副本。

- 没有进程占用这个目录，当前 shell 也不在要删除的路径里；进程归属、终止和资源清理按 md/development/core/TESTING.core.md 第 16.4 节执行。

### 6.4. 执行

目录与工作树删除都由执行者完成，子代理不执行删除。普通 `git worktree remove` 与失败后的目录处理均遵循“检查 → 协调者验收 → 再派执行者删除”的顺序。

1. 检查执行者完成第 6.3 节检查，报告适用的第 6.1 或 6.5 节条件、精确路径、分支、记录转移与归属、占用及安全检查结果。
2. 协调者验收后，再派执行者执行删除。派发写明精确路径、删除范围和授权来源；可以注明本节适用条件条款的默认授权依据，不要求重复取得相同授权。不满足适用条件时先按 md/development/core/LOOP.core.md 第 14.1 节取得许可。不得以工具放行扩大范围。
3. 执行删除的执行者只做删除前复核、指定删除和删除后复查。开发、实验工作树先用普通 `git worktree remove`；该命令因占用、权限、未跟踪文件等普通 Git 或文件系统原因失败时，保留目录和登记，报告原因。失败后的目录处理重新提交检查，由协调者验收后再派执行者处理，不能把失败视为额外删除授权。

任何一方遇到工具策略明确拒绝（例如 `blocked by policy`），立即停止，不更换执行途径；协调者向用户报告被拒绝的精确路径供手动处理。

```bash
git worktree remove "<worktreePath>"
# 仅在对应条件成立、检查与协调者验收完成，且删除派发明确允许时：
git worktree remove --force "<worktreePath>"
# 开发和实验分支在本轮整组进入主干并验证后；测试临时分支按第 6.5 节：
git branch -d "<devBranch>"
```

- 任务进度文件不算开发改动；保留与转移按 md/development/core/LOOP.core.md 第 3 节执行，完成后源副本随工作树删除。
- 普通失败后，只有剩余内容仅为可重建输出和本节本地过程文件、满足第 6.1 或 6.5 节条件并完成第 6.3 节检查时，删除派发才可明确允许强制移除或必要的目录处理。
- 自动放行（含 `--force`）只表示免逐次工具确认，不改变条件、授权、安全检查或验收顺序。
- 不先手动删目录，不用 `git clean` 或批量清空绕过保护。路径必须在执行前解析为绝对路径并核对处于明确授权范围；不跨 shell 拼接删除，按当前平台的安全文件操作方式执行。
- 删除后核对目录及 `git worktree list --porcelain` 登记均消失；只有确认过期登记时才 prune，并先 dry-run。
- 归属不明、存在唯一副本、条件未满足或仍有依赖时保留对象，不强制删除。报告逐项列出文档转移、删除工作树、保留分支和受阻对象及原因。

### 6.5. 测试验证用工作树

- **验收反馈结束后删除**：测试或验证得出结论后先保留工作树，待协调者验收（含需要时的独立复核）结束、不再需要补证据后，由创建它的执行者按 md/development/core/GIT_WORKFLOW.core.md 第 6.3 节、md/development/core/GIT_WORKFLOW.core.md 第 6.4 节、md/development/core/GIT_WORKFLOW.core.md 第 6.6 节完成检查、协调者验收和执行删除，一起删除为它临时创建、没有独有提交的分支。任务被明确终止时按已有授权清理；运行中的进程仍按 md/development/core/TESTING.core.md 第 16.4 节及时退出。

- **删除前检查证据已留下**：按 md/development/core/LOOP.core.md 第 2 节、md/development/core/LOOP.core.md 第 3 节确认该树应交的证据和应有的记录已留在接收位置。通过时写验证范围和关键结果；失败时写断言或差异、关键日志片段、复现命令和输入标识，足以脱离该树复现和定位。

- **测试工作树中有开发改动时先保留**：本地过程文件按 md/development/core/GIT_WORKFLOW.core.md 第 6.4 节处理。工作树中有其他未提交的源码或文档改动，或者分支有独有提交时，不按本节删除。先把改动转到按 md/development/core/GIT_WORKFLOW.core.md 第 2.1 节确定的开发工作树，或者上报协调者裁决；改动处理完毕后，再按本节核对删除条件。

- **删除方式**：按 md/development/core/GIT_WORKFLOW.core.md 第 6.3 节检查后，统一按 md/development/core/GIT_WORKFLOW.core.md 第 6.4 节验收、派发并删除。剩下的只有可重建输出和该节本地过程文件时，可在该流程内明确允许 `git worktree remove --force`。共享输入和数据的处理按 md/development/core/GIT_WORKFLOW.core.md 第 6.6 节执行。

- 验收期间需要补充证据的，先核对源码、配置和已有结果，在原测试工作树继续；清理后确需补证据时，重新创建测试工作树并报告实际执行，证据复用按 md/development/core/TESTING.core.md 第 19.2 节执行。

### 6.6. 任务完成后的清理与笔记归档

- **什么时候清理**：按 md/development/core/LOOP.core.md 第 3 节的时机和以下条件清理：开发工作树在满足 md/development/core/GIT_WORKFLOW.core.md 第 6.1 节适用删除条件并完成 md/development/core/GIT_WORKFLOW.core.md 第 6.3 节检查后；测试验证用工作树按 md/development/core/GIT_WORKFLOW.core.md 第 6.5 节；实验工作树在本批次整体进入主干并验证后，或者批次被放弃且用户授权清理后。执行分工按 md/development/core/GIT_WORKFLOW.core.md 第 6.4 节。

- **清理什么**：待回收的派生工作树本身，以及其中的构建输出、临时文件（临时子目录）、日志、测试输出和缓存；为本任务独占创建的仓库外 clean-room、stage 等目录也一并回收。

- **baseline 临时输出**：本条只适用于 md/development/core/GIT_WORKFLOW.core.md 第 2.1 节规定的发行与合并，只回收本任务独占的临时目录。baseline 原有的 构建目录、用户日常使用的项目环境和共享输出按原用途保留。

- 共享的只读输入、真实数据和参考结果不删除。本地分支仍按 md/development/core/GIT_WORKFLOW.core.md 第 6.4 节处理。

- **记录归档**：收尾记录的根 baseline、写回时机、内容和子任务结果汇总按 md/development/core/LOOP.core.md 第 3 节执行；需要转移时按 md/development/core/GIT_WORKFLOW.core.md 第 6.2 节执行。

## 7. 主干上的纯文档维护

只写文档任务按 md/development/core/LOOP.core.md 开头的任务分类选择工作树，是 md/development/core/GIT_WORKFLOW.core.md 第 2.1 节派生工作树要求的例外。用户明确要求的规范和说明维护，默认直接在主工作树的主分支上进行。主干上的 commit 仍然需要授权。按 md/development/core/ENGINEERING.core.md 第 11 节及项目风险细则的适用等级要求核对，工作与记录方式见 md/development/core/LOOP.core.md。发现超出 md/development/core/LOOP.core.md 开头只写文档任务范围的工作时，这部分另外按开发分支流程处理，不夹带在文档维护里。
<!-- 规范文件结束：md/development/core/GIT_WORKFLOW.core.md -->
