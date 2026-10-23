# MVVCVTK Feature 开发规范

> 适用范围：`MVVCVTK/features/` 下的新 Feature 和现有 Feature 扩展，以及为此修改 Host API、Feature SPI、CMake 或测试的工作。第 10.1 节的 CMake 接入要求同样适用于 Host 新增业务能力。仓库内 CMake 修改的解决方案核对见根 `AGENTS.md` 第 11.1 节。
>
> 本文件只讲 Feature 本身：归属、边界、接口、生命周期、实现和准入准出。Git 与合并流程见 `GIT_WORKFLOW.md`，风险分级见根 `AGENTS.md` 第 11 节，测试范围与结果判定见 `TESTING.md`，SDK 准入见 `SDK_DELIVERY.md`，命名见 `NAMING.md`。

## 1. 本文解决什么

1. 判断一项能力该不该成为 Feature；
2. 规定 Feature 与 Host、其他 Feature、VTK/Qt/第三方之间的依赖和所有权；
3. 给出设计、实现、集成和验证的流程；
4. 定义准入（Definition of Ready）和准出（Definition of Done）。

不要求每个 Feature 新建 README、设计文档、性能文档、consumer 示例、版本层或 SDK 聚合 target。

## 2. 当前架构

### 2.1 产品结构示例

```text
应用 / Qt 宿主 / Standalone
├── MVVCVTK::Host
│   ├── 公开：MVVCVTK::HostAPI
│   └── 内部：MVVCVTK::FeatureSPI + Host 私有实现
├── MVVCVTK::OrthogonalCrop
│   ├── 公开：Feature 入口 + 最小公开类型
│   └── 依赖：MVVCVTK::FeatureSPI
└── MVVCVTK::GapAnalysis
    ├── 公开：Feature 入口 + 最小公开类型
    ├── 依赖：MVVCVTK::FeatureSPI
    └── 私有：GapKernel.dll -> DefX runtime
```

上图是产品组合示例，实际批准集合按 `SDK_DELIVERY.md` 第 2.3 节核对。`VtkAppHostSession` 通过 `AttachFeature(const std::shared_ptr<HostFeature>&)` 接入 Feature；Host 不静态依赖具体 Feature，Standalone 负责显式组合。

### 2.2 公开分面

SDK 的公开分面及公开头判定按 `SDK_DELIVERY.md` 第 7 节执行。

### 2.3 参考文件

开发时从 `CMakePresets.json`、`MVVCVTK/CMakeLists.txt` 和 `tools/release/` 定位当前构建、公开头映射和发布入口；不复制旧文档或旧 patch。各 Feature 的 Host 入口头文件按 `features/<Feature>/include/Host/` 的目录规律查找，具体根目录和文件以当前源码检索为准。按 `VtkAppHostSession`、`HostFeature`、`FeatureViewService`、`FeatureOverlay` 等符号查找契约定义，再沿 CMake 定位所属 target、Standalone 组合和 SDK 边界测试。

事实冲突时按根 `AGENTS.md` 第 4 节的顺序取证。

## 3. 归属判定

### 3.1 什么是 Feature

Feature 是可选的、可信的进程内产品能力，同时满足以下条件：

- 通过 `HostFeature` 和批准的 Feature SPI 接入 Host；
- 关闭它之后，Host 仍能独立配置、构建、安装和运行；
- 不需要 `AppRuntime`、`Context`、Host 私有 service、窗口实现或全局场景对象；
- 有独立的业务入口、状态和生命周期，不是一组无归属的 helper；
- 公开面能收敛为入口类加上签名所需的最小类型；
- 应用可以按需链接它。

### 3.2 应该放进 Host 的能力

满足任一条件，默认属于 Host：所有视图、数据加载、请求路由或 Session 生命周期都依赖的基础能力；维护 Host 自身不变量、线程、停止、窗口拓扑或通用数据状态的能力；补齐现有 Host API/SPI 通用缺口、没有独立业务入口的能力；关闭后会让 Host 基本模型不完整的能力。

即使属于 Host，也不能把 Feature 的私有算法、策略或第三方实现搬进 Host。

### 3.3 应该留在 Feature 私有实现里的能力

只被一个 Feature 使用的算法、service、router、widget、render strategy、缓存或状态机；只服务一个公开方法的辅助函数；供应商 SDK 适配、动态库加载、格式转换或临时缓冲；只用于测试、诊断、性能审计或 Standalone 的入口。

### 3.4 什么时候可以放进 FeatureSupport

`FeatureSupport` 是仓内构建期共享支持层，用于 Host 与 Feature 或多个 Feature 之间已经存在的通用实现复用。抽取必须有实际使用者并减少已有重复；只由 Host 使用的实现留在 Host，只由单个 Feature 使用的实现留在该 Feature，不为未来使用者预建通用工具箱。

Host 和 Feature 只能通过 `PRIVATE` 依赖在构建树内使用 `FeatureSupport`；进入安装导出集合的 target 必须用 `$<BUILD_INTERFACE:...>` 隔离该依赖。该层可以依赖 Feature SPI 和稳定的通用内部辅助，不依赖任何具体 Feature，不承载具体 Feature 业务或业务状态，不安装、不导出。

## 4. 依赖边界

### 4.1 边界矩阵

FeatureSupport 的依赖、使用方式和边界见第 3.4 节。

| 区域 | 可以依赖 | 可以公开 | 禁止 |
| --- | --- | --- | --- |
| Host API | 稳定值类型、批准的 endpoint/契约 | `VtkAppHostSession`、请求/结果/状态等 | 具体 Feature、Qt、DefX、AppRuntime/Context |
| Feature SPI | Host 提供的窄端口、可信 view/data/overlay/render 契约 | `HostFeature` 及其物理闭包 | 具体 Feature、Host 私有实现、宿主 UI |
| Feature 公共 API | Feature SPI、签名必需类型、经审查的 VTK 类型 | 入口、Config/Request/Result/State、必要的值类型 | Algorithm/Service/Router/Widget 实现、私有 bridge、Qt/DefX 类型 |
| Feature 私有实现 | 本 Feature 公共 API、Feature SPI、VTK、FeatureSupport、批准的第三方 | 不安装、不导出 | Host 私有头、其他 Feature |
| 私有三方 bridge | 批准的供应商 API | 只对所属 Feature 暴露窄 C/POD 边界 | 让供应商 C++/STL/VTK 类型穿过边界 |
| Standalone | Host 与启用的 Feature | 不安装 | 绕过 Feature 公共入口、决定 SDK 组合 |

### 4.2 依赖方向

```text
允许：应用 -> Host、应用 -> 具体 Feature、Host -> HostAPI、Host -> FeatureSPI（内部）
      具体 Feature -> FeatureSPI、GapAnalysis -> 私有 GapKernel -> DefX
禁止：Host -> 任何具体 Feature、Feature A -> Feature B、Feature -> Host 私有 include
      FeatureSPI -> 具体 Feature
```

### 4.3 Qt

Qt 由接收方宿主提供，Feature 不负责 Qt 的版本、部署、DLL 或宿主 UI。Qt 类型不进入 Host API、Feature SPI 或 Feature 公共 API。不为 Feature 新建 Qt wrapper；需要 UI 时，由外部宿主把用户操作转换成 Feature 请求。

### 4.4 VTK

- Feature SPI 是可信边界，可以使用经过审查的 VTK 快照、renderer 或窄渲染契约。
- 新增的普通 Host 数据 DTO 不引入可写的 VTK 对象身份、scalar 指针或内部 mapper/actor 身份。
- Feature 公共 API 只有在业务输入本身就是 VTK 数据、转换成值类型明显不合理时，才可以新增 VTK 类型；必须检查所有权、可空性、线程、ABI 和安装依赖传播。
- 不把 `vtkRenderer`、`vtkRenderWindowInteractor` 或完整的 App 视图对象保存成跨生命周期的全局引用，只通过 SPI 取得受约束的 view/lease/port。

### 4.5 第三方供应商

引入或扩展非公共的第三方 SDK 之前，先确认：为什么 VTK/OpenCV/标准库不够用；Debug/Release、x64、MSVC 运行库、iterator debug level 是否一致；public/private 链接传播；运行时定位、校验和错误语义（不做 fallback）；固定宽度的 POD/C 边界是否足以隔离 ABI；失败时能否保持原状态并释放资源。第三方依赖变化按 md/development/core/ENGINEERING.core.md 第 11 节判级，裁决与通知按 md/development/core/LOOP.core.md 第 14.1 节；涉及 SDK 边界时另按交付规范第 3 节核对。

## 5. 目录与公开面

### 5.1 目录模板

按职责选目录，不建空目录占位：

```text
MVVCVTK/features/<Feature>/
├── CMakeLists.txt
├── include/
│   ├── Host/<Feature>HostFeature.h   # 默认公共入口
│   ├── Host/<Feature>HostTypes.h     # 仅在公开签名确实需要时
│   ├── <Feature>Types.h              # 领域值类型，是否公开由 CMake 决定
│   └── Algorithms/ Interaction/ Render/ Routing/ Services/   # 私有
└── src/（同样的子目录）
```

仓内的 build-interface 公开不等于 SDK 安装公开，见第 10 节。

### 5.2 默认入口形态

```cpp
class ExampleHostFeature final
    : public HostFeature
    , public std::enable_shared_from_this<ExampleHostFeature> {
public:
    explicit ExampleHostFeature(ExampleHostConfig config);
    ~ExampleHostFeature() noexcept override;

    ExampleHostFeature(const ExampleHostFeature&) = delete;
    ExampleHostFeature& operator=(const ExampleHostFeature&) = delete;

    std::string_view GetFeatureId() const noexcept override;
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;

    bool SendRequest(ExampleHostRequest request,
                     ExampleHostCallback onComplete = nullptr);
    ExampleHostState GetState() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
```

这是组织参考，不要求照抄全部方法，也不为了套模板制造空的 State、Result、Callback。入口复杂、有私有依赖或有生命周期状态时，默认使用 Pimpl，具体写法见根 `AGENTS.md` 第 10.2 节。

### 5.3 默认不新建的内容

每个 Feature 单独的 README 或 consumer 示例；Feature API V1/V2 并行层；FeatureManager、FeatureRegistry、FeatureFacade 这类只做一层转发的类型；单独的 SDK、安装根或聚合 target；只包一个函数的 Service/Controller；`FeatureTypes` 大杂烩；Qt adapter、UI model 或部署脚本。

### 5.4 命名

Feature 业务名、目录、ID、target 和分支的写法见 `NAMING.md` 第 9 节。`CropHostFeature`、`GapHostFeature` 等已发布的名称不因规则修订而改名。

## 6. 公共 API 设计

### 6.1 最小公开面

Feature 的公开头默认只包含：一个 Host 接入入口；构造它所需的 Config；业务动作所需的 Request/Action；调用方确实需要观察的 State/Result/FailureReason；公开签名避不开的稳定领域类型。

SDK 公开面的禁入内容和已批准窄 SPI 的判定按 `SDK_DELIVERY.md` 第 7.3、7.4 节执行。

### 6.2 Request 与 Action

- Action 表达稳定的业务动作，不暴露内部步骤。
- Request 只带这个 Action 真正需要的数据；字段之间互斥时要校验组合，不静默忽略冲突字段。
- 必需的输入不藏在 optional 或默认值里。optional 只表示“可以不提供”或“沿用已保存的配置”。
- 不同生命周期或不同结果语义的动作，拆成不同的请求，不往一个无限扩张的 variant 里塞。
- 异步请求的同步返回值表示“是否接纳”，不能冒充最终成功。
- 调用方需要区分失败原因、版本冲突、取消和结果数据时，使用结构化的 Result；只有单纯的成功/失败、调用方也没有恢复分支时，才用 `bool`。

#### 6.2.1 拒绝与完成回调（各接口族分别声明）

| 接口族 / 情形 | 同步返回 | 回调要求 |
| --- | --- | --- |
| admission 风格端口 | `Accepted` 或明确的拒绝原因 | 只有被接纳的请求才有后续完成；拒绝时是否同步回调，以该端口的声明为准 |
| `SendRequestResult` 风格的完成包装 | 按该接口的返回契约 | callback 有效时，同步拒绝也完成一次，不再排入后续队列 |
| 已接纳的请求 | 接纳不等于成功 | 成功、失败、取消三者恰好完成一次；Stop/Detach 引起的取消也算 |

- “拒绝后没有后续回调”不等于“禁止同步失败回调”；但同一个请求不能先同步回调失败、再异步完成一次。
- 每个入口都要写明：回调在哪个线程、是否可能在发送函数返回前执行、重入规则和异常策略。
- 测试按接口族分别覆盖，不为了迎合一条通用断言而修改已有的公开回调语义。

### 6.3 State 与 Result

State 是当前状态的快照，不是内部可变对象的引用；Result 描述一次操作的结果，不混入长期配置；FailureReason 要稳定、可测试，能帮调用方决定重试、修改输入还是终止，不能只透传第三方错误码；不为每个内部阶段都加公开状态位；admission、progress 和 outcome 不合并成一个 `isSuccess`。

### 6.4 版本与数据发布

Feature 读取 Host 数据并可能发布新状态时，默认流程是：

1. 获取不可变的 snapshot；
2. 基于 snapshot 的 identity/version 计算候选结果；
3. 完成边界、内存、几何和业务校验；
4. 用 expected version 做 CAS 或等价的条件发布；
5. 版本已经变化时，拒绝旧结果，不覆盖新数据；
6. 发布成功后，才更新 Feature 自己的 active 状态。

### 6.5 破坏性收敛

破坏性收敛按根 `AGENTS.md` 第 8 节执行。需要外部迁移窗口时，按 md/development/core/LOOP.core.md 第 14.1 节决定兼容范围、移除条件和验证方式；涉及 SDK 边界时核对独立授权。

## 7. 生命周期、线程与所有权

### 7.1 生命周期

每个 Feature 都要能解释 `Constructed -> Attaching -> Attached/Idle -> Active（可反复进入、退出）-> Detaching -> Detached -> Destroyed` 这些状态，不要求公开同名枚举。Attach 失败要回到可以安全重试或销毁的未挂载状态；Detach 失败要保留可重试的一致状态，不能谎报 Detached。

### 7.2 AttachHost

1. 校验必需的端口、Feature ID、配置和当前状态；
2. 只保存所需的窄 port 或 weak lease，不保存完整的 Host 实现；
3. 按明确的顺序注册 input、view、overlay、effect；
4. 全部注册完成后，才把状态设为已挂载；
5. 任一步失败时，按相反顺序撤销已完成的步骤；
6. 重复 attach 返回明确结果，不叠加 observer 或资源；
7. callback 捕获优先使用 `weak_from_this()`，避免循环持有。

### 7.3 DetachHost

1. 已分离时幂等返回；
2. 停止接纳新请求；
3. 广播取消，处理已接纳的任务；
4. 按相反顺序 detach effect、overlay、input、view/lease；
5. 清理对 Host port 的引用；
6. 返回成功后，不再投递 owner callback，不再访问 renderer、interactor 或数据快照；
7. 某一步失败时，保留足够的状态以便重试。

析构只是最后一道防线，不能替代正常的 Detach，也不能在未知线程上强行销毁 owner-thread 上的 VTK 资源。

### 7.4 OnHostTick

在 owner thread 上运行，只做有界的消费、状态推进和回调派发；不做整卷扫描、长时间算法、阻塞等待或不可控 I/O。队列有界，积压时要有拒绝、合并或取消策略。tick 失败不能破坏已发布的 active 状态。

### 7.5 异步任务

worker 数、队列容量和内存上限显式给出，不为每个请求新建线程；取消使用协作式 token/flag，在切片、块、迭代和写入的边界上检查；callback 只在批准的 owner dispatcher/timer 上消费，worker 对共享对象和线程私有候选的访问、业务期限和 Stop/Detach 等待期限按根 `AGENTS.md` 第 10.3 节执行；Detach 或 Stop 之后，迟到的结果要因 token/version/lifetime 校验而失效；等待期限到期后，不释放 worker 仍在使用的对象。

### 7.6 所有权

设计时要能回答本次涉及对象的这几个问题：谁创建、谁拥有、哪些线程能访问、什么时候释放、失败时如何回滚。常见对象的默认答案：

| 对象 | 创建者 | 拥有者 | 线程 | 释放时机 | 失败回滚 |
| --- | --- | --- | --- | --- | --- |
| Feature 入口 | 应用 | 应用（shared_ptr） | owner thread 接入 | Detach 后由应用释放 | Attach 失败不留注册 |
| Host port | Host | Host；Feature 持窄引用 | 按端口契约 | Detach 时清理 | 不清理仍在注册中的资源 |
| overlay/effect/widget | Feature | Feature | owner thread 写入 | Detach/Exit/Stop | 反序撤销 |
| worker task | Feature service | 有界 executor | worker | 完成/取消/Stop | 丢弃迟到结果 |
| snapshot/candidate | Host/Feature | 不可变 shared 或局部 | 按契约 | 请求完成 | 发布前不改 active |
| vendor runtime handle | 私有 bridge | bridge/loader | 明确线程 | Feature/runtime stop | 不暴露半加载状态 |

所有权确定不了时，暂停依赖这一关系的设计。

### 7.7 回调重入

回调里可能立即查询、再次请求、取消、Detach 或 Stop，允许哪些操作要写进接口契约。规则见根 `AGENTS.md` 第 10.3 节：先完成状态转换再调用用户代码、不持锁回调、回调返回后重新核验。已成功发布的结果不因观察者异常而倒退。

## 8. View、渲染、Overlay 与交互

### 8.1 View

通过 `FeatureViewDirectory` 按稳定的 view ID/role/targets 获取 View；DTO 只描述身份，可调用能力通过 `FeatureViewService`、`OverlayService`、input view/lease 单独获取。不缓存跨拓扑变化的 endpoint 地址。目标 View 缺失、lease 失效或 role 不匹配时，返回稳定的失败，不改用其他 renderer。

### 8.2 RenderEffect

通过 `FeatureViewService::AttachRenderEffect`/`DetachRenderEffect` 管理，并且必须成对、可回滚。effect 只接收实现所需的输入，拿不到主体的 dirty state、材质系统、LOD 或 VisualStrategy。修改渲染状态后用 `SetRenderNeeded()` 请求重绘，不自己驱动全局 Render 循环。

### 8.3 Overlay

按 Feature + View 的作用域注册，不扫描全局 renderer props。先完成数据和 View 校验，再原子替换 active overlay。Detach/Exit/Stop 时移除本 Feature 创建的全部 prop/effect/observer，不碰其他 Feature 或 Host 的对象。

### 8.4 Interaction

input binding 使用唯一稳定的 Feature ID 和明确的目标 View 集合；按键映射属于 Feature Config 或 Standalone 的外部组合，不写死进 Host；输入回调只把输入转成业务请求，不直接执行长算法；Standalone 的控制入口也必须调用 Feature 公共的 `SendRequest`。

### 8.5 明确禁止

扫描 `vtkRenderer` 的全部 props 来猜哪些属于 Feature；用 `vtkAssembly` 或全局容器代替明确的所有权；建立全局 SceneManager；修改 Host 私有的 visual strategy、window 或 App runtime；worker 越线程写入共享的渲染或交互对象（按根 `AGENTS.md` 第 10.3 节判断）；attach 失败一半后留下 observer、effect 或 overlay。

## 9. 数据、算法与性能

目标遵循根 `AGENTS.md` 第 8 节：低内存、简单、渲染快。

### 9.1 算法层

输入输出和坐标空间明确；不持有 Host、UI、renderer 或生命周期对象；尽量写成纯计算，副作用交给 service 或入口编排；公式、不变量、复杂度、误差和空输入边界可以单独测试。不为调用一个第三方函数就造一个“算法层”。

### 9.2 数据复制与资源准入

- 优先共享只读 snapshot、视图或分块读取，按需物化，复用生命周期明确的缓冲，及时释放中间结果。不做没有业务必要的整卷副本。必要的副本要说明大小、owner 和同时存活的范围。不能为了零拷贝破坏快照的不可变性或借用的有效期。
- **资源准入唯一定义**：尺寸溢出、非法范围、明确约定且可验证的工作缓冲上限或任务资源限制、实际分配失败、硬件能力上限可以拒绝操作。不可靠的全局估算、历史累计发布量、对象数量和显示容量不能作为数据加载或结果发布的硬门槛。
- 分块、降采样、LOD 必须是显式策略，不能用隐藏的降级冒充达到了目标质量。
- 临时缓冲的元素类型、体素顺序、extent、spacing、origin、方向、字节数和上限要明确；所有尺寸乘法、offset 和分配在执行前检查溢出。

### 9.3 性能改动

优化已有路径之前，按 `TESTING.md` 第 12 节记录：可复现的输入、配置和测量边界；适用的指标（耗时、帧耗时、内存/显存、尾延迟；渲染关注首个正确画面、切换和交互反馈，不只看平均 FPS）；热点证据；预期改善和不可退化项。首次新增的业务没有可比较的旧入口时，按该节建立首次记录。先证明结果正确，再比较收益；没有收益却增加复杂度时，撤回本轮改动。

禁止：没有基线就上大范围缓存、线程池或预取；把机器相关的数字放进 Host 公共 API；为“兼容低端机器”悄悄改变用户显式指定的目标；每帧重新决策长期配置；只凭 Debug 性能判断 Release。

### 9.4 两阶段提交

```text
确认当前状态和依赖版本
-> 准备候选的逻辑输入，校验数据/参数/硬约束
-> 按具体资源策略物化候选，或先释放可重建的资源
-> 在 owner thread 上检查生命周期/版本，提交绑定
-> 完成清理和结果通知
```

- 两阶段提交不要求新旧整卷数据和 GPU 资源同时驻留。
- Volume 和 Iso 各按自己的资源顺序处理，规则见根 `AGENTS.md` 第 10.5 节。
- 恢复也可能失败：这时保留必要的输入和逻辑身份，如实报告“显示不可用”或“恢复失败”。显示准备失败的结果语义见根 `AGENTS.md` 第 10.5 节。

### 9.5 渲染效率

渲染资源复用与空闲行为见根 `AGENTS.md` 第 10.5 节；刷新请求的合并语义见 md/development/core/ENGINEERING.core.md 第 10.4 节，显示质量的显式策略见本文件第 9.2 节。

## 10. CMake 与 SDK 接入

### 10.1 CMake 接入与解决方案同步

**新功能必须能被 CMake 正确构建和测试到。** 新 Feature、现有 Feature 的新业务能力和 Host 的新业务能力都适用这一条。

1. 按实际影响检查并同步修改：target 的源码列表、仓内 header surface、构建开关、`add_subdirectory`、PUBLIC/PRIVATE 依赖、测试注册、preset。需要改哪项就改哪项，不要求每项都改。
2. 确实不需要改的（例如在已有 `.cpp` 里新增能力），在报告中写明“沿用 `<受影响target>`”和覆盖它的测试入口。
3. 不做空改动，不为了制造 CMake 差异而加 target、开关或测试。只把源码放进目录、只改 IDE 工程、依赖旧缓存编译通过，都不算接入完成。
4. 按 `TESTING.md` 第 3.2 节确定并完成受影响 target 的构建和测试。
5. 风险按根 `AGENTS.md` 第 11 节判断。
6. 新增或调整测试 target 时，按 `TESTING.md` 第 7.2.1 节处理复用和 IDE 分组。

#### 10.1.1 CMake 修改的解决方案完整性

完整定义见根 `AGENTS.md` 第 11.1 节。

### 10.2 新 Feature 的仓内最小配置

1. 按现行构建策略设置 `MVVCVTK_BUILD_<FEATURE>` 开关，在 `features/CMakeLists.txt` 中按条件加入目录；
2. 新建不带项目名前缀的 Feature target，并提供 `MVVCVTK::<Feature>` alias；
3. 使用适用的 target 默认设置、私有实现目录、最小的 build-interface 入口和仓内测试；
4. PUBLIC 只传播 Feature SPI 和公开签名需要的依赖，算法、支持层和私有第三方依赖都是 PRIVATE；Host 不链接 Feature；
5. 确认 Feature 关闭时没有残留的生成头或 target，Host 和其他 Feature 不受影响。

SDK 准入及安装、导出和发布配置的限制按 `SDK_DELIVERY.md` 第 3、16 节执行。

使用现有 helper 前，读源码确认它是否改动 SDK 头映射、install/export 或 component，再按上述章节核对。

### 10.3 公开头

- 所有 Feature 都要检查：最小的 build-interface 入口、直接的 include 闭包、私有头隔离，以及关闭后生成头已清理。
- 已进入 SDK 的 Feature，还要核对 staged include 路径、`MVVCVTK/API/Features/<Feature>/...` 映射、install/export 和 package 依赖。
- 未获 SDK 准入的 Feature，预期结果是“没有新增安装头和 component”，不是把它补进 SDK 预期清单让测试通过。
- 头文件数量和公开符号契约分开审查，数量没变不代表接口没变。

### 10.4 链接可见性

出现在公开签名中的类型，其提供方 target 用 PUBLIC；只在 `.cpp` 或私有头里使用的依赖用 PRIVATE。编译失败时不要靠改成 PUBLIC 解决，先检查是不是把实现类型放进了公开签名。FeatureSupport 的使用方式见第 3.4 节。DefX 的 target、头和导入库不进入 consumer 的 CMake 图。

### 10.5 配置组合

按实际影响选择要验证的组合，无关组合不进入计划：

| 当前 Feature | 其他 Feature | Standalone | 预期 |
| --- | --- | --- | --- |
| OFF | 任意 | OFF | Host 独立配置和构建，当前 Feature 无残留 |
| ON | OFF | OFF | 当前 Feature 在仓内独立构建 |
| OFF | ON | OFF | 其他 Feature 不受影响 |
| ON | ON | ON | Standalone 显式组合成功，且不进入 SDK |

上表中 Host 始终为 ON，不存在 Host OFF 的配置。仓内开关为 ON 不代表 SDK 增加了 component。

### 10.6 SDK 准出

触及实际 SDK 的契约、安装/package 或发布脚本时，按 `SDK_DELIVERY.md` 第 15 节对应的级别验证；只有仓内构建变化时，验证仓内组合并确认没有被意外纳入 SDK。正式交付的流程和门禁见 `SDK_DELIVERY.md` 第 11 至 13 节。

## 11. 开发流程

以下阶段用来判断这次任务需要做哪些工作。局部修复不必重走完整流程；已有且未受影响的设计、契约和证据直接复用。“停下来的信号”涉及的方向选择按根 `AGENTS.md` 第 13.2 节处理。

| 阶段 | 要做什么 | 准出 | 停下来的信号 |
| --- | --- | --- | --- |
| 0 调研 | 看清工作树和代码现状，搜索同职责实现，判断归属（Host / Feature 公共 / SPI / 私有 / 三方 bridge），列出受影响的 target、公开头、线程、数据版本、runtime、测试和 CMake | 问题陈述、范围、非目标、风险级别、验证草案 | 目标文件有冲突改动；目标与现有架构互斥 |
| 1 准入 | 按第 12 节核对本次适用的条件 | 归属成立；公开面、所有权、线程、失败、回滚和验证都说得清 | 需要 Host 私有对象、跨 Feature 依赖、全局可变 manager 或尚未按风险规则决定的第三方才能实现 |
| 2 契约设计 | 由外到内：应用怎么持有 → Config/Request/Result/State → 需要哪些端口 → view/overlay/effect/input 的所有权 → snapshot 与发布事务 → worker/取消/回调 → 私有模块职责 → CMake 可见性 → 失败注入与测试 | 公开头草案和依赖图不越界；破坏性变化已按风险规则决定 | 只有靠万能类型、HostType 膨胀、双轨兼容或隐式 fallback 才能成立 |
| 3 CMake 骨架 | 按第 10 节接入 target、开关、最小入口、测试注册；新增或调整开关时验证“Feature OFF”和“只开当前 Feature”两种组合 | 公开面可编译，关闭后无残留，Host 不依赖 Feature | 接入要泄漏私有实现、引入跨 Feature 依赖或聚合 target |
| 4 私有实现 | 默认顺序：值类型与算法 → 三方 bridge → service 与状态机 → view/render/interaction → 入口编排 → 诊断与测试；异步、取消与回滚随相关实现一起落实，每完成一个纵向切片就运行相关测试 | Debug 构建通过，核心的正向和负向路径有证据 | 反复需要突破阶段 2 的契约；性能或内存明显恶化 |
| 5 集成 | 通过 `AttachFeature`/`DetachFeature` 接入；Standalone 只调用公共 API；Host 不新增具体 Feature 的 include 或分支 | 当前 Feature 的接入链路正确；按实际影响验证组合、隔离和退出 | 只有修改 Host 私有对象或绕过入口才能集成 |
| 6 验证 | 按第 13 节和 `TESTING.md` 选择验证项；独立复核按根 `AGENTS.md` 第 13.2 节执行。 | 适用的 DoD 满足，真实数据验收有证据 | 有效门禁失败未解决；试图删测试、放宽断言或隐式降级来“通过” |
| 7 提交与集成 | 本地提交、实验集成和合入主干按 `GIT_WORKFLOW.md` 执行；任务文档和长期文档分别按根 `AGENTS.md` 第 7 节维护 | 分阶段如实报告状态 | — |

已按风险规则决定且满足适用授权门禁的共享 API/SPI/CMake 契约变化需要同步改其他 Feature 的调用点时，纳入同一次影响审查和验证，不因此停下。SPI 变化只在现有端口无法满足真实需求、能力确实归 Host 所有、并且能形成窄而稳定的契约时才提出，不为单个 Feature 的私有便利扩大 SPI。

## 12. 准入（Definition of Ready）

只检查本次实际涉及的项目，未涉及的直接省略。属于本次承诺的缺口不能省略。

**业务与范围**
- [ ] 一句话说明 Feature 为调用方提供什么业务结果；
- [ ] 列出正向场景、失败场景和不处理的事项；
- [ ] 说明它为什么是独立可选的 Feature，而不是 Host 基础能力或现有 Feature 的重复；
- [ ] 明确是修改现有 Feature 还是新增 Feature；
- [ ] 涉及性能优化或规模/时延承诺时，明确真实输入规模、指标、比较条件和基线或目标。

**公共契约**
- [ ] 列出要新增或修改的全部公开头、类型和方法，说明每个字段为什么必要；
- [ ] 引入 VTK 类型时说明为什么不能用值类型；Qt、DefX 和私有类型不进入公开签名；
- [ ] 破坏性变化的判级、决定与通知按 md/development/core/LOOP.core.md 第 14.1 节完成；涉及 SDK 边界时核对具体授权；兼容与旧入口处理引用 md/development/core/ENGINEERING.core.md 第 8 节。

**依赖与构建**
- [ ] 不依赖 Host 私有 include，不依赖其他 Feature，Host 也不新增对具体 Feature 的依赖；
- [ ] 列出 PUBLIC/PRIVATE 链接传播；
- [ ] 新功能列出 CMake 接入方案（要改的文件和配置项，或者沿用哪个 target）、受影响的 target 和验证方式；
- [ ] Feature 关闭时 Host 和其他 Feature 的预期明确；
- [ ] 新 Feature 的业务名、target、alias、开关和测试方案明确，SDK 准入写明“未批准、不安装”或已批准的最小变化；
- [ ] 第三方变化的来源、配置矩阵、私有 bridge、runtime 定位和失败策略已明确；决定与通知按 md/development/core/LOOP.core.md 第 14.1 节完成，SDK 边界授权另行核对。

**生命周期与线程**
- [ ] Attach、重复 Attach、部分失败、Detach、重复 Detach、Stop、析构的语义明确；
- [ ] renderer/interactor/widget/effect/overlay 操作所在的 owner thread 明确；
- [ ] worker、队列、取消点、回调线程和“恰好一次”语义明确；
- [ ] 本次涉及对象的所有权说得清（第 7.6 节）；
- [ ] 迟到的结果如何被 version/token/lifetime 拒绝已经定义。

**数据与回滚**
- [ ] 输入 snapshot、坐标空间、extent、数据版本、工作缓冲和可验证的硬约束明确；
- [ ] 候选构建和 active 提交分离，每个可失败步骤的回滚顺序明确；
- [ ] 发布前失败不覆盖已发布的数据和逻辑身份（第 9.4 节）；
- [ ] 写入数据时的 CAS/expected version 或等价条件明确。

**验证**
- [ ] 按 `TESTING.md` 第 3.2 节选定本次的测试集合，已有覆盖直接复用；
- [ ] 按根 `AGENTS.md` 第 11 节确定风险级别；
- [ ] 当前环境能运行的构建和测试命令明确，不能运行的依赖已经识别；
- [ ] 正向验收用的真实数据、预期结果/容差和公共入口链路明确；数据暂缺时记为验收阻塞，不用合成样本冒充；
- [ ] 性能相关任务的代表性输入、指标来源和不可退化项明确。

## 13. 验证重点

按 `TESTING.md` 第 3.2 节选择本次受影响的行为，已有证据按其第 19.2 节复用。下列条目是“涉及这个能力时要覆盖什么”，不是每次都要逐条新增用例。

**CMake 接入**：CMake 修改及合入目标工作树后的解决方案完整性核对，按根 `AGENTS.md` 第 11.1 节的适用范围执行。

**行为**：默认 Config 和显式 Config；未 BuildSession 或未 Attach 时发送请求；Attach 成功、重复、缺少端口、部分注册失败；请求成功、非法 Action、字段冲突、空输入、越界；重复的 Start/Exit/Switch/Reset；Detach 成功、重复、中途失败后重试；Stop 前后的请求和回调；析构后 weak callback 不再访问对象；目标 View 缺失、role 不匹配、lease 失效；失败或退出后 overlay/effect/input 数量恢复原状；其他 Feature 不受影响。

**数据与异步**：snapshot 版本正常发布；计算期间数据 reload，旧结果被拒绝；在读取、算法、候选构建、提交之前取消；回调恰好一次；拒绝路径按第 6.2.1 节逐个接口族测试；按根 `AGENTS.md` 第 10.3 节验证 Stop/Detach 等待期限，到期时保留可重试的状态；队列满、超过工作缓冲上限、分配失败和尺寸溢出时返回稳定的失败；整卷复制符合第 9.2 节。

**渲染与交互**：每个目标 View 单独 attach/detach；多 View 部分失败时反序回滚；effect/overlay 只修改自己的对象；input 的 View 和 Feature ID 归属；owner-thread 调度失败或抛异常；KeyPress/KeyRelease 重复事件；`SetRenderNeeded` 不形成无限 render 循环；退出后 renderer 中没有本 Feature 的残留。

**私有 runtime**：见 `TESTING.md` 第 13.2 节。

**SDK 边界**（仅已进入 SDK 的 Feature）：只公开入口和最小类型；consumer 包含不到私有头；`MVVCVTK::FeatureSPI` 自足；只编译头契约时不需要链接 `MVVCVTKHost.lib`；Feature 关闭时 Host 没有 include/link 错误；FeatureSupport 按第 3.4 节核验；Qt、Standalone 和测试不安装；clean-room 下 Debug/Release 都能链接。

**性能**：按 `TESTING.md` 第 12 节。

**真实数据**：正向功能必须用真实输入，经 Feature/Host 公共入口走完加载、请求、完成回调、读取结果和退出资源的全过程；只跑私有 Algorithm、mock host，或者只判一个成功 bool，都不构成业务完成的证据。样本、参考、容差和结果状态按 `TESTING.md` 第 5、6、17 节处理。

## 14. 准出（Definition of Done）

只核验本次适用的条目；本次承诺的缺口和实际失败不能省略。

**功能**
- [ ] 正向业务结果已在可追溯的真实数据上，经公共入口复现，结果和参考/容差一致；
- [ ] 没有顺手实现非目标；非法输入、缺资源、取消、版本冲突和第三方失败都返回明确的结果；不靠 fallback 或旧状态伪报成功。

**边界**
- [ ] Host 不依赖具体 Feature；Feature 不包含 Host 私有头、不依赖其他 Feature；FeatureSupport 按第 3.4 节核验；
- [ ] 公开头只有入口和必要类型，私有实现没有泄漏，Qt/DefX 类型没有进入公共契约；
- [ ] 没有新增 SDK 聚合 target、未批准的产品库或全局 manager。

**生命周期与线程**
- [ ] Attach 是原子的，部分失败能完整回滚；Detach/Exit/Stop 幂等或可以重试；
- [ ] owner thread 管理的渲染和交互对象写入与 worker 私有候选处理已按根 `AGENTS.md` 第 10.3 节分离；业务期限和 Stop/Detach 等待期限已按该节核验，取消、迟到结果和“恰好一次”语义已验证；析构后没有悬空捕获或后台访问。

**数据与性能**
- [ ] 发布条件正确；发布前失败能保持已发布的状态（第 9.4 节）；尺寸、offset 和分配都做了溢出检查；
- [ ] 复制、缓冲和渲染刷新符合第 9.2、9.5 节；优化有同条件的基线，新动作按 `TESTING.md` 第 12 节做了首次实测。

**代码质量**
- [ ] 复用已有业务链，没有重复职责、同义类型或空壳层；公开头的 include 自足；
- [ ] 命名、Pimpl、所有权和注释符合根 `AGENTS.md` 和 `NAMING.md`；没有无关的格式化或改名，没有临时调试代码，没有新的编译警告。

**构建、测试与 SDK**
- [ ] 新功能的 CMake 接入已完成并说明，CMake 修改及合入目标工作树后的解决方案完整性已按根 `AGENTS.md` 第 11.1 节的适用范围核对；
- [ ] 按风险级别完成了要求的 Debug/Release 构建和测试；受影响的 Feature ON/OFF、Host 独立构建和组合隔离已经验证；
- [ ] 触及实际 SDK 时已通过对应的发布与 clean-room 门禁；未获准入的新 Feature 没有被意外安装；
- [ ] 第三方 runtime 的配置和负例已经通过（如果适用）；
- [ ] 新增或调整的测试有明确的契约依据和可信判据，相关的错误、重复或过时测试已按 `TESTING.md` 第 7.6、7.7 节处理；
- [ ] 必需但未执行的验证已写进完成报告。

**状态表述**：Feature 的实现和真实数据验证状态、实验组合状态、主干集成与文档状态、SDK 安装态验证状态，四者分开报告。

## 15. 代码审查要点

**公共契约**：这个类型为什么必须公开？去掉某个字段，调用方还能不能完成业务？签名有没有泄漏实现、线程、缓存或第三方？admission 和 outcome 是否分开？状态是快照还是可变对象？VTK 类型是否必要？有没有 V1/V2 或双轨状态？任何一个问题答不上来，公共契约就不能准出。

**依赖**：用 CMake target 和 include 路径验证实际依赖，不只看目录图；PUBLIC 是否真是公开签名需要的；Host/Feature/FeatureSupport/第三方的依赖方向；Feature 关闭后有没有残留的 staged header、target、宏或 component；clean-room consumer 会不会偶然从源码树或开发机路径拿到头或库；新功能的 CMake 接入是否已按第 10.1 节完成，CMake 修改及合入目标工作树后的解决方案完整性是否已按根 `AGENTS.md` 第 11.1 节的适用范围核对。

**生命周期**：每个 attach 步骤都有对应的回滚或 detach 步骤；每个 callback 都能找到 owner、触发线程、恰好一次的保护和失效保护；每个后台任务都有取消点、版本检查和 Stop 行为，期限按根 `AGENTS.md` 第 10.3 节核验；每个 VTK 对象都能找到创建、拥有、修改和销毁它的线程；每个失败返回之后 active 状态仍然一致。

**完整 diff**：只改了任务需要的文件；没有无关的格式化或换行；没有无用的 include、头、类型或 helper；没有残留的旧路径或兼容代码；测试能检出它针对的错误，没有复制被测实现来做判据，没有用 true/非空/文件存在冒充业务结果；注释和代码一致；多 Agent 合并后没有重复实现。

## 16. 立即停下重新设计的信号

- Host 开始 include 具体 Feature 的头或出现 Feature 分支；
- Feature 需要 Host 私有 include、AppRuntime、Context 或完整的窗口对象；
- Feature A 直接调用 Feature B；
- 为了修复编译把私有 include/link 改成 PUBLIC；
- 公开头开始泄漏 Algorithm/Service/Router/Widget、worker 或供应商类型（已批准的窄 SPI 除外）；
- attach/detach 不能成对，失败后说不清资源归谁；
- worker 越线程写入共享的渲染或交互对象，或者回调线程不明确；
- 旧 snapshot 的结果可能覆盖新数据；
- 性能优化需要隐藏降级、伪报成功或每帧自动重新决策；
- 需要大量新增 HostTypes、Manager、adapter 或 V2 才能继续；
- 多个 Agent 在改同一个公共文件，或者各自维护不同的契约；
- 想通过删除或放宽有效测试来掩盖产品错误。

重新设计涉及的方向选择按根 `AGENTS.md` 第 13.2 节处理；建议优先考虑缩小 Feature、收敛公开面、明确所有权和失败事务，避免再加一层抽象。
<!-- 规范文件结束：md/development/FEATURE_DEVELOPMENT.md -->
