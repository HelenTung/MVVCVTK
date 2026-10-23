# MVVCVTK 命名规范

> 适用于新增、重命名或对外暴露的项目类型、函数、变量、字段、枚举、常量与别名。构造/析构、运算符、框架强制 override、第三方 API 和 CMake 固有名称不受本文的动词规则约束，但这些例外不能扩散成新的项目命名。
>
> 目标：只看符号和所在类型，就能判断它是什么、做什么、是否跨边界、是否改变状态、处在哪个生命周期阶段。

> 通用定义按各节引用转接；本文件保留本项目的具体实施要求。

## 1. 依据与迁移

新名称按以下顺序决定：同一类或同一接口族已有的写法 → 同层同动作的既有名称 → 本文 → 一般 C++ 习惯。

通用定义见 md/development/core/ENGINEERING.core.md 第 20 节。

- 公开 SDK 符号的任何重命名、别名或迁移都属于 SDK 契约变化，需要针对具体符号的授权。

- 沿用已有的领域词（`View`、`Feature`、`Overlay`、`Render`、`ImageRead`、`Snapshot`、`Stage`），不为同一个概念再造近义词。

## 2. 字形与缩写

| 对象 | 形式 | 示例 |
| --- | --- | --- |
| 类、结构体、枚举、类型别名 | PascalCase | `VtkAppHostSession`、`ImageReadResult` |
| 函数、成员方法 | PascalCase | `BuildSession`、`GetViewState` |
| 局部变量、形参、聚合字段 | lowerCamelCase | `viewId`、`expectedSnapshot` |
| 类成员 | `m_` + lowerCamelCase | `m_impl`、`m_voxelCount` |
| 非宏常量、`constexpr` | lowerCamelCase（不加 `k` 前缀） | `imageReadLimit` |
| 宏、构建开关 | UPPER_SNAKE_CASE | `MVVCVTK_BUILD_<FEATURE>` |
| 枚举值 | PascalCase | `Accepted`、`QueueFull` |

- 长度只是可读性提示：函数名和变量名建议在 15 个字符左右、尽量不超过 25 个，类型名尽量不超过 25 个。语义准确优先；名字过长时，先检查是不是职责太宽、缺少 `Request`/`Result`/`State` 这类值类型。

- 新的跨模块接口不自造缩写，领域词沿用既有接口族。允许 `Id`、`Vtk`、`Gpu`、`Async` 等常用缩写，以及 `Config`、`Params` 等既有领域写法。

- 项目自有类型中的缩写按普通单词处理大小写（写 `VtkAppHostSession`，不写 `VTKAppHostSession`）；第三方名称保持上游的拼写。

## 3. 类型名与职责后缀

类型名的结构是：必要的领域前缀 + 核心对象 + 职责后缀。领域前缀（`Host`、`Feature`、`Crop`、`Gap`、`Render`）只在跨模块、属于公共 API 或有重名风险时才加。

| 后缀 | 含义 |
| --- | --- |
| `Config` | 构建前或运行前的配置，不是运行状态 |
| `Params` | 一个窄动作的参数组 |
| `Request` / `Result` | 一次调用的输入 / 完成后的输出 |
| `Admission` | 异步操作的同步接纳结果，不是最终结果 |
| `Error` / `FailureReason` | 失败分类，同一领域只用其中一种 |
| `State` | 当前状态值 |
| `Snapshot` | 冻结、只读、带 identity/version 的数据视图 |
| `Status` | 生命周期或状态机的离散状态 |
| `Stage` | 尚未提交为 active 的事务候选 |
| `Port` | 跨层暴露的一组窄能力 |
| `Service` | 一个业务域的操作或编排 |
| `Router` | 按命令或事件选择下游，不承载算法 |
| `Directory` | 受限的查询视图，不表示拥有被查询对象 |
| `Registry` | 按稳定 identity 登记和查找运行时对象 |
| `Coordinator` | 协调多个对象完成一个有提交/回滚边界的事务 |
| `Controller` | 控制一个明确的状态机或生命周期 |
| `Manager` | 拥有一个明确的资源域；不能当作“什么都管”的兜底名 |
| `Strategy` / `Factory` / `Handler` | 可替换的算法 / 只负责创建 / 框架事件处理者 |
| `Feature` | 通过 Feature SPI 接入 Host 的完整扩展入口 |
| `Binding` / `Lease` / `Endpoint` | 已解析的绑定 / 有所有者和有效期的使用权 / 对外的窄访问端点 |
| `Stamp` / `Entry` / `Callback` | 轻量身份戳 / 集合中的单条记录 / callable 别名 |
| `Kind` / `Mode` / `Role` / `Use` / `Flags` | 单一互斥轴的枚举 / 可按位组合的枚举 |

- 接口前缀跟随所在的接口族：多数窄接口不加前缀（`HostInputPort`）；`I...` 只在相邻接口族已经使用时才用；`Abstract...` 只用于已有的抽象家族；`Base...` 只用于提供部分实现的基类。

- `Impl` 只表示 Pimpl 的私有实现；声明和定义位置见根 `AGENTS.md` 第 10.2 节。

- 生产代码中不新增 `Helper`、`Util`、`Common`、`Misc` 这类名字。`Manager`、`Service`、`Controller`、`Context` 不是同义词，说不清职责时先检查是否放错了层或缺少值类型，不为此新增类型。

- `Types.h` 只放同一窄领域内共同演进的值类型。

## 4. 函数名

结构是：**动词 + 核心对象 + 必要限定**，例如 `BuildSession`、`GetImageReadChunk`、`SetCurrentFromPending`、`SendOwnerComplete`。

- `By` 表示查找键，`From` 表示来源，`To` 表示目标，`For` 表示适用对象。

- 类名已经说明了对象时，可以省略对象名，例如 `HostCommandRouter::Dispatch`、`VtkAppHostSession::Stop`。

- `Current`、`Pending`、`Candidate`、`Active`、`Applied`、`Published`、`Expected` 只在确实有事务含义时使用。

- 名字里不带调用方、目录、临时实现或版本号（`ForQt`、`NewV2`、`TempFast`）。

- 重载必须是同一个动作、同一个语义轴。

## 5. 动词表

公共 API/SPI 和跨文件的业务函数默认使用下表的动词。这不是封闭清单：`Fit`、`Measure`、`Compute` 这类准确的领域动词可以使用，前提是对象、输入输出和副作用都清楚。

| 动词 | 含义 |
| --- | --- |
| `Get` | 读取，不改变调用方能观察到的业务状态 |
| `Set` | 写入、替换、提交或显式转换现有状态 |
| `Create` | 从无到有创建一个独立对象 |
| `Build` | 用已有输入组装组合根、候选、Binding 或 Endpoint |
| `Remove` / `Clear` | 移除集合中的一项 / 清空整个集合或槽位 |
| `Reset` | 保留对象身份，把状态恢复到初始值 |
| `Start` / `Stop` | 启动有后续状态的操作 / 结束运行、任务或 lease |
| `Send` | 跨对象、Port、线程或事件边界投递 |
| `Dispatch` | Router 按命令或事件选择下游 |
| `Load` / `Reload` / `Export` | 从外部加载 / 重新加载 / 写出到外部 |
| `Attach` / `Detach` | 建立 / 解除挂载关系，不转移所有权、不销毁对象 |
| `Exit` / `Switch` | 退出明确的业务模式 / 在既有模式之间切换 |
| `On` | 框架、Port 或生命周期的回调入口 |

默认不用 `Process`、`Run`、`Execute`、`Apply`、`Update`、`Handle`、`Manage`、`Do`、`Make`、`Toggle` 这类泛化动词：Router 用 `Dispatch`，写状态用 `Set...`，发布更新用 `SendUpdates`，指定显示状态用 `SetOverlayState`。`Update` 可以作为名词出现，例如 `UpdateFlags`、`StartUpdate`。

## 6. 查询、写入与异步

- **查询统一使用 `Get` 开头，返回 bool 也一样。** 判断自身状态用 `GetIsStopped`；判断给定对象用 `GetRoleIs3DView`；领域属性用 `GetQualityValid`。同一个类里不另起一套裸 `Is...`/`Has...`。

- `Get` 不提交事务、不接纳异步任务、不触发外部 I/O。可能没有结果时返回指针或 `optional`；需要说明原因时返回 `Result`。

- `Set` 也用于把已验证的候选提交到现有状态：`SetCurrentFromPending`、`SetLoadCommit`。跨多个 owner 的原子事务放进 `Coordinator` 或 `Service`，由它来调用 `Set...Commit`。

- 事务阶段的变量名：`expected`（CAS 前置值）、`pending`（已暂存未发布）、`candidate`/`stage`（构建中的候选）、`current`/`active`（当前生效）、`applied`（已写入资源）、`published`（对外可见）、`previous`（回滚用）。

- 异步：`StartX` 启动有后续状态的操作；`SendX` 跨边界投递；`XAsync` 只在同一接口族需要同时提供同步和异步版本时使用。

- 返回类型：调用方不需要失败原因的二态结果用 `bool`；需要区分原因时用枚举或 `Admission`/`Result`；无意义结果的局部写入用 `void`。高层 API 不用输出参数返回正常结果。

## 7. 变量与字段

- 布尔值：`is` + 形容词（`isDone`），`has` + 名词（`hasResult`）。不用 `active`、`enable`、`isNotReady` 这类名字。存放 callable 的字段用 `on...`（`onComplete`），不用 `cb`、`fn`。

- 集合用复数（`viewIds`），不用匈牙利前缀（`pData`、`vecViews`）。

- `source`/`target` 表示方向，`input`/`output` 表示数据流，`owner` 表示生命周期或线程的所有者，三者不能混用。

- 文件和目录用 `filePath`、`outputDir`。

- 数量单位：`Count` 表示个数，`Bytes` 表示字节数，`Index` 表示下标，`Offset` 表示偏移，`Version`/`Revision` 表示版本/修订号；`Size` 只表示容器或空间尺寸。`Budget`、`Threshold`、`Limit` 分开使用；`Min`/`Max` 只表示数学边界。

- 物理单位放在名字末尾：`timeoutMs`、`angleDeg`。

## 8. 枚举与几何

- 优先使用 `enum class`；枚举值不重复类型名（写 `ImageReadAdmission::Accepted`）。

- `None` 表示明确没有选择，`Unknown` 表示有值但无法识别，`Invalid` 表示不满足约束，三者不能互换。位标志枚举提供 `None`。

- 跨函数传递的坐标要在名字里带上坐标系：`worldPosition`、`imageBounds`。

- 变换按 `sourceToTarget` 命名，含义固定为 `targetPoint = sourceToTarget * sourcePoint`。`MatrixData` 是扁平数组，`Matrix` 是矩阵对象，`Transform` 是可执行的变换。

- bounds 默认顺序为 `[minX, maxX, minY, maxY, minZ, maxZ]`；仿射变换默认 4×4、16 个 `double`，其他矩阵在类型或注释中写明维度。

- `dimensions` 表示各轴体素数量，`extent` 表示含端点的索引范围，`spacing` 表示相邻采样点沿各轴的物理间距，`origin` 表示接口契约规定的物理原点，四者不能互换。

## 9. Feature、target 与分支

- `<Feature>` 是 PascalCase 的 Feature 业务名；`<FEATURE>` 是当前 CMake 中对应构建开关的大写下划线片段，两者不能互换。

- `<主题>` 是概括任务的简短小写英文短语，单词用 `-` 连接。

- `YYYYMMDD-NN` 表示任务开始日期和同日同主题的两位序号，序号从 `01` 起；不需要区分时可以省略序号。

- `<任务标识>` 默认取 `YYYYMMDD-NN`；`<批次标识>` 取 `<主题>-YYYYMMDD-NN`，两者的序号均按上一条取舍。

| 对象 | 写法 | 不写成 |
| --- | --- | --- |
| Feature 业务名和目录 | `SurfaceDetermination` | `MVVCVTKSurfaceDetermination` |
| Feature ID | `surface-determination` | `mvvcvtk-surface-determination` |
| 新的仓内 target | `surface_determination` | `mvvcvtk_surface_determination` |
| 开发分支 | `feature/surface-determination-<任务标识>` | `feature/MVVCVTK-...` |
| 实验分支 | `experiment/<批次标识>` | 用某个 Feature 名或主干的替身命名 |

- `MVVCVTK::` 是 package/target 命名空间，不是业务前缀，予以保留；`MVVCVTK_BUILD_<FEATURE>` 开关体系也保留。

- 已交付的 `MVVCVTKHost.lib` 等制品和已发布的符号不因本条改名。

- 类型可以带业务限定和职责后缀，例如 `SurfaceDeterminationHostFeature`。

## 10. 新名称自查

新增或改名之前，确认能回答以下问题：

- 返回值表示接纳、最终结果，还是简单的成功/失败？

- 状态处于 candidate、pending、active、applied 还是 published？

- 是否制造了第二套同义词？是否改变了 API、SPI、target 或 SDK 契约？
<!-- 规范文件结束：md/development/NAMING.md -->
