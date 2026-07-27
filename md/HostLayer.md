# Host 层说明

## 1. 定位与事实基线

| 项目 | 当前事实 |
| --- | --- |
| 对外门面 | `VtkAppHostSession` |
| 一次性配置 | `HostSessionConfig`，只保存 `renderViews` |
| 主体命令 | `SendData`、`SendView`、`SendTool` |
| 可选 Feature | composition root 构造并持有 `CropHostFeature` / `GapHostFeature`，Session 通过 `AttachFeature` 注册 |
| Adapter 入口 | `AttachTimer`、`AttachHotkeys` |
| Standalone 入口 | `Start` |
| 主体下游 | Data、App/State、Render |
| 不负责 | Qt UI、固定窗口数量、具体算法、业务默认参数 |
| 接纳协议 | Feature 读取不可变 `ImageSnapshot`，以 expected snapshot 做 CAS 发布 |
| 本地实现快照 | 2026-07-27；HEAD `c0b657909bbe150b5b7d39443a0c37b417a27df7` |

Host 是宿主适配与组合边界。Qt、standalone 或上位机只应依赖 Session、主体 DTO、Feature 公共类型和 endpoint，不应取得 `VizService`、DataManager 或 feature service 后直接操作。Crop/Gap 不属于主体命令 variant；删去任一 Feature 目录后，主体 Host、Render、App 与 Interaction 仍可独立编译。

## 2. 当前文件与类型

| 文件 | 责任 |
| --- | --- |
| `MVVCVTK/include/Host/VtkAppHostSession.h` | 公共门面 |
| `MVVCVTK/include/Host/Types/HostValueTypes.h` | 视图角色、目标、Host 视觉值类型 |
| `MVVCVTK/include/Host/Types/HostSessionTypes.h` | 窗口拓扑与 endpoint |
| `MVVCVTK/include/Host/Types/HostRequestTypes.h` | Data/View/Tool 三个主体业务域的 request |
| `MVVCVTK/include/Host/Types/HostCommandTypes.h` | 三类主体 typed command 与 completion |
| `MVVCVTK/include/Host/Types/HostAdapterTypes.h` | Hotkey 与 Timer adapter 配置 |
| `MVVCVTK/include/Host/HostRenderViewSet.h` | 多视图 owner、选择与 endpoint |
| `MVVCVTK/include/Host/HostCommandRouter.h` | typed command 分发 |
| `MVVCVTK/include/Host/HostFeature.h` | Feature 接口、上下文 reader/writer/input/completion port |
| `MVVCVTK/include/Host/HostHotkeyRouter.h` | standalone 输入适配 |
| `MVVCVTK/features/OrthogonalCrop/include/Host/CropHostFeature.h` | Crop 可选 Feature 的请求、状态与门面 |
| `MVVCVTK/features/GapAnalysis/include/Host/GapHostFeature.h` | Gap 可选 Feature 的请求、状态与门面 |

旧路径 `include/Host/HostSessionTypes.h`、`include/Host/HostCommandTypes.h` 已不存在；公共类型均位于 `include/Host/Types/`。

## 3. 会话与构建

### 3.1 所有权

```text
VtkAppHostSession::Impl
  HostSessionConfig
  HostCoreServices
    RawVolumeDataManager
    SharedStateBroadcaster
    SharedInteractionState
  HostRenderViewSet
    N x (VizService + StdRenderContext)
  HostCommandRouter
  endpoint cache
  HostHotkeyRouter
  weak Feature registry
```

endpoint 中的 renderer、renderWindow、interactor 都是非拥有指针，只在 session 及其视图 runtime 存活期间有效。应用/Qt composition root 对 Session、Crop、Gap 各自持有强引用；Session 只保留 Feature 的 `weak_ptr`，因此不会反向延长可选模块寿命。

### 3.2 `BuildSession()`

`BuildSession()` 幂等；空 `renderViews` 返回 `false`。成功顺序为：

```text
BuildCore
  -> HostRenderViewSet::Build
  -> 创建 HostCommandRouter
  -> SetInitialVisibility
  -> SetInteractorsReady
  -> BuildEndpoints
  -> 创建 HostHotkeyRouter
  -> isBuilt = true
```

`BuildSession()` 不调用 `SendRenderAll()`。不存在 `isInitialRenderEnabled`。`Start()` 才会在构建成功后先 `SendRenderAll()`，再让 standalone start view 进入阻塞式 VTK 事件循环。因此 Qt 只调用 `BuildSession()`，绝不能调用 `Start()`。

主体 `Send*`、endpoint getter、`AttachTimer()` 与 `Start()` 都可能触发首次懒构建。`AttachTimer()` 会先调用 `BuildSession()`；`AttachFeature()` 不负责构建，要求 Session 已构建且调用发生在 owner thread。为让失败点和生命周期顺序清晰，应用仍应显式先 `BuildSession()`，再注册 Feature、绑定 Timer。

### 3.3 视图配置

`HostRenderViewConfig` 保存 id、role、`HostWindowConfig`、可选外部 render window 和 standalone loop 标记。视图有独立 `VizService`/context/mode/camera style，但共享 DataManager 与 `SharedInteractionState`。构建期多个 view 写入共享视觉字段时，后构建者覆盖先构建者。

`HostViewTarget` 的选择规则是确定性的：非空 `viewId` 优先且唯一生效；若 id 未命中，不再退回 role。只有 `viewId` 为空且 `isViewRoleUsed=true` 时才按 role 取第一个视图。Feature 的多目标请求必须解析出至少一个精确视图；空目标集合不表示“全部视图”。

`HostWindowConfig::isAxesVisible` 默认 `false`，在 `BuildViewPair()` 时决定该窗口左下角
`vtkOrientationMarkerWidget` 的初值。运行期由 `HostViewSetRequest::isAxesVisible` 修改目标
context，不进入会话共享状态。

`SetInitialVisibility()` 还会按 role 写会话共享的 `VisFlags`：3D role 关闭 `Planes3D` 与 `Ruler`，slice role 打开 `Crosshair`。注意这三个 flag 分别表示 3D 彩色参考平面、3D cube axes 标尺和 2D 十字线，都不是 orientation marker。由于 visibility mask 是共享状态，构建多个不同 role 时这些写入不是 per-view 配置。

## 4. 当前命令协议

主体 `HostCommand` 是：

```cpp
std::variant<std::monostate,
    HostDataCommand,
    HostViewCommand,
    HostToolCommand>
```

主体命令 variant 不包含任何 Feature 命令。主体 action 与 payload variant 必须严格匹配；Feature 使用自己的 typed request，并直接调用各自 `SendRequest()`。

| Session API | Action | Payload |
| --- | --- | --- |
| `SendData` | `LoadFile` | `HostLoadRequest` |
| `SendData` | `ReloadBuffer` | `HostReloadRequest` |
| `SendData` | `ExportVolume` | `HostVolumeExportRequest` |
| `SendData` | `ExportSlices` | `HostSliceExportRequest` |
| `SendView` | `Set` | `HostViewSetRequest` |
| `SendView` | `ResetCamera` | `HostViewResetRequest` |
| `SendTool` | `Set` / `Switch` | `HostToolSetRequest` / `HostToolSwitchRequest` |

Data 异步动作可携带完成回调。Crop Export 与 Gap Start 的回调分别属于 `CropHostFeature` 和 `GapHostFeature`。

## 5. 数据链

### 5.1 File Load

```text
SendData(LoadFile)
  -> HostCommandRouter::LoadFile
  -> VolumeLayout::Create
     dimensions 全正；或仅 .raw + {0,0,0} 时从文件名末尾 NxMxK 推断
  -> VizService::LoadFileAsync
  -> worker 读取并产生 pending image
  -> GUI/VTK Timer: VizService::SendTasks
  -> SetCurrentFromPending
  -> 发布 FileDataReady / FileLoadFailed
  -> SendUpdates 重建 pipeline、释放 admission
  -> completion
```

RAW 文件名解析从最后两个 `x/X` 分隔符读取三个正整数，不使用正则。文件字节数必须精确等于 layout byte count；不会补零或截断。

Host/Data 的所有 `std::string` 路径统一为 UTF-8。Router 不改写 DTO 字节；filesystem/Win32 边界通过 `PlatformPath::GetNativePath` 转平台 native path，传给 VTK 9.4.2 KWSys 的窄字符串通过 `GetUtf8Path` 转回 UTF-8。Windows 文件映射只调用 `CreateFileW/CreateFileMappingW`，不提供 ACP 兼容分支。`SystemTools::Stat` 的 Windows 超长路径限制不在当前契约内。

### 5.2 Reload Buffer

`HostReloadRequest` 自有 `std::vector<float>`。Router 通过 `VolumeLayout::Create` 与 `VolumeBuffer::Create` 校验正 dimensions、体素数、字节计算和 `ptrdiff_t` 上限，再把 owning buffer 移入异步层。worker 只产生 pending，owner 在消费线程提交 current。

同步 `false` 可能是 Host 前置拒绝，也可能是 service 接纳失败；后者仍可能排队 completion(false)。关闭阶段必须用 UI generation/QPointer 门禁防止迟到访问。

### 5.3 View Set

`HostViewSetRequest` 命中目标后，即使所有 optional 字段都缺省也会返回 `true` 并保持现状。以下字段在 Router 写入前有校验：

- material 浮点均 finite，`ambient/diffuse/specular/opacity` 在 `[0,1]`，
  `specularPower >= 0`；
- opacity 在 `[0,1]` 且 finite；
- transfer node 的 `position/opacity/r/g/b` 均要求 finite 且位于 `[0,1]`，position 非降序且显式数组非空；position 是当前 scalar range 内的归一化坐标；
- iso 要求 finite；background 三通道要求 finite 且位于 `[0,1]`；
- spacing 三轴 finite 且 `> 0`；
- window width finite 且 `> 0`，center finite；
- cursor 三个 world 分量 finite，axis 只允许 `-1/0/1/2`；
- Volume 画质只接受 `Quality` 与 `Custom`：`Quality` 强制 `766 / 1.0 / jitter on`，`Custom.maxDimension` 位于 `[1,16384]`，`sampleDistance` finite 且 `> 0`；
- gradient 节点的梯度 finite、非负、非降序，不透明度 finite 且位于 `[0,1]`；
- `volumeQuality`、`gradientOpacity`、`isDenoiseOn` 只允许用于 `Volume` / `CompositeVolume` 的有效候选模式。

Router 先解析 target、service 和 context，再把全部 optional 转入局部候选并完成所有校验；只有候选完整合法才按固定顺序调用 setter。因此任一晚字段非法都返回 `false`，不会留下 mode、material、TF 等部分更新；合法多字段请求只形成一次统一 view-set 提交。optional 全缺省仍是成功 no-op。

`HostViewAction::ResetCamera` 必须严格搭配 `HostViewResetRequest`，只调用目标 context 的
`ResetCamera()`；它不创建 camera 状态副本，也不进入 `SharedInteractionState`。方向轴
切换与相机复位都会把目标 service 标脏，由既有 Qt/VTK Timer 渲染下一帧。

交互刷新率和采样质量是两条独立状态轴。`Quality/Custom` 决定 Volume producer 与 mapper 参数；`RenderRate` 只在 interaction source 的空/非空边界切换静态/交互刷新频率，不再选择低分辨率 producer。Feature 激活时，Host 通过 `setActiveViews` 上报精确参与视图，App 临时把这些视图派生为固定 `Quality(766)`；退出后恢复进入前保存的 `Quality` 或 `Custom` 配置。拖动只改变刷新调度，不改变 producer 分辨率。

### 5.4 窗宽窗位状态链

`HostWindowLevelParams` 使用 double，默认 WW=400、WC=40。运行期 `SendView(Set)` 要求 WW finite 且 `> 0`、WC finite；请求缺省 `windowLevel` 时保持现状。

```text
HostViewSetRequest.windowLevel
  -> Router 校验
  -> target VizService::SetWindowLevel
  -> 会话唯一 SharedInteractionState
  -> UpdateFlags::WindowLevel 广播所有 view service
  -> SliceStrategy::SetColorWindow / SetColorLevel
```

target 只选择由哪个 service 发起写入，不产生 per-view 窗宽窗位。三个 slice 读取同一个共享值并同步刷新；3D Volume/IsoSurface strategy 不消费 WindowLevel flag，所以把请求发给 3D target 仍会改变共享值，但当前 3D 画面不变。

| 事件 | WW/WC 结果 |
| --- | --- |
| 运行期合法 Set | 覆盖共享值；same-value 不广播 |
| 2D 普通左键拖动 | 写共享值；WW clamp 到至少 0.01 |
| File/Reload 成功 | 重置为新数据范围：`WW=max-min`、`WC=(min+max)/2` |
| File/Reload 失败 | 保留旧值 |
| 纯 mode/pipeline rebuild | 保留共享值 |

[风险] `HostViewInitConfig::windowLevel` 只有 `hasWindowLevel=true` 时写入，但构建期 `BuildAppInit()` 当前没有执行运行期 Router 的 finite/WW>0 校验；非法初始化值可能进入共享状态。现有测试也没有覆盖 Host 正向 WW/WC、SharedState flags 或 SliceStrategy 映射。

### 5.5 Cursor、十字线与可见元素

`SharedInteractionState` 还保存全会话共享的 world cursor、cursor axis 和 visibility mask。
2D 滚轮沿当前 slice 轴推进 cursor；Shift+左键拖十字线会按 Top/Front/Left 写 Z/Y/X
轴约束位置；三张切片通过同一个 broadcaster 联动。Qt 可用
`HostViewSetRequest::cursor` 写绝对 world cursor；数据尚未就绪时内部 service 保持现状。
每次成功 `BuildPipeline()` 都把 cursor 重置到当前体数据 world 中心，因此 File/Reload、
mode switch、spacing 引发的结构重建仍会覆盖此前 cursor。

| 可见对象 | 内部状态/API | 消费者 | 当前 Host 运行期请求 |
| --- | --- | --- | --- |
| 2D 十字线 | `VisFlags::Crosshair` / `SetElementVisible` | `SliceStrategy` 两条 line actor | `visibility.isCrosshairVisible` |
| 3D 彩色参考切平面 | `VisFlags::Planes3D` | `ColoredPlanesStrategy` | `visibility.isPlanes3DVisible` |
| 3D cube axes 标尺 | `VisFlags::Ruler` | Volume/Iso strategy | `visibility.isRulerVisible` |
| 世界方向轴 marker | `StdRenderContext::SetOrientationAxesVisible` | orientation marker widget | `isAxesVisible` |

Crosshair 是显式 visibility bit，不由焦点或 cursor 有效性隐式决定。前三个业务元素
仍是会话共享 bit：target 只选择由哪个 service 发起写入，不把它们变成 per-view 状态。
方向轴属于目标 context。Qt 只构造 Host DTO 并调用 `SendView`，不取得内部
`VizService`/`StdRenderContext`。

[风险] mode switch 不像 reload 一样显式重放 `UpdateFlags::All`。若内部调用方先隐藏 Crosshair，再切到此前未构建的新 Slice strategy，新 actor 是否正确继承共享 visibility mask 当前缺少测试，不能文档化为已保证。

## 6. Crop

Crop 是可选 Feature。composition root 构造 `shared_ptr<CropHostFeature>`、对 Session 调用 `AttachFeature()`，随后直接使用 `CropHostFeature::SendRequest()` 与 `GetState()`。主体只提供 view/context、输入端口、snapshot reader、expected-snapshot writer、活动视图上报和 owner-thread completion port；history、draft、公式编译、发布任务和 widget 均留在 `features/OrthogonalCrop`。

```text
CropHostRequest
  -> CropHostFeature（先解析并校验全部 target/input）
  -> CropViewRequest
  -> CropBridge::StartView
  -> CropAlgorithm::BuildPredicateTable（每个新公式只编译一次）
  -> immutable CropShaderPayload
  -> N x VizService / Strategy-local CropShaderController

CropHostAction::Export
  -> root ImageSnapshot + allHistory[0, absoluteNode)
  -> immutable predicate plan
  -> vtkSMPTools::For（一次扫描，只生成最终 UCHAR mask）
  -> current expected ImageSnapshot CAS
```

目标语义是严格事务：

- `isTargetViewsUsed=true` 时只使用请求显式 `targetViews`；空集合或未知目标拒绝，不回退配置；
- `isTargetViewsUsed=false` 时只使用 `CropHostConfig::defaultTarget.targetViews`；
- reference view、所有 target view、input view 与输入 snapshot 必须在调用 `StartView()` 前全部解析成功；
- 任何前置失败都不改变 binding、history、widget 或 shader。

| Host action | 当前行为 |
| --- | --- |
| Start | 绑定完整 Image 或 RegisteredPolyData snapshot、reference view 和 target services，不启用 widget |
| Box / Plane | 选择 widget 类型；Released 产生或替换当前 draft，并 staged 新 payload |
| Mode | 设置后续默认模式；若当前 draft 已提交，同时更新对应 history 节点并 staged 新 revision，画面不等待下一次 Released |
| Previous / Next | 只改变物化基线后的相对前缀 `nodeCount`，复用同一 immutable table；不会越过基线回放已写入 mask 的节点 |
| Node | 将有效节点数设为请求值 |
| Export | 从 root snapshot 与绝对历史前缀构造一次性 CPU task；owner thread 以当前 expected snapshot 做 CAS，结果通过 typed callback 回传 |
| SetPolyData / ClearPolyData | 注册或清除 immutable PolyData 输入 |
| RestoreOriginal | 以当前绑定 snapshot 为 expected 值恢复原始 image 状态 |
| Exit | 关闭 widget、清 history/payload 与所有 target shader |

`GetState()` 返回 `CropHostState{history,isActive,isPublishing}`。它要求 owner thread 且 Feature 仍处于 attached 状态；否则返回零状态。Previous/Next/Node 等 history 动作只在活动 binding 上接纳。

Export 同时冻结两个不同职责的 snapshot：计算始终读取 lineage 的 root image/root mask，并把 `allHistory[0, baseNodeCount + nodeCount)` 编译为一个不可变谓词计划；发布只把开始时的 current snapshot 作为 expected CAS 令牌。图像物化按 z 分片并行，一次遍历直接写出一张最终 UCHAR mask，不生成或缓存节点级中间 mask。发布仅在 DataManager current 仍与 expected snapshot 同一身份/version 时成功；成功原子提交裁切 image、有效域 mask 和新 version。若 Reload 先提交，Crop CAS 返回 VersionMismatch，不覆盖新数据；若 Crop 先提交，稍后的 Reload 仍可按自己的合法 admission 覆盖。两条链不通过互斥锁或跨 Feature 查询决定胜负。

物化绝对节点 N 后，N 成为新数据基线：`baseNodeCount=N`、相对 `nodeCount=0`，物化对象不作为新的谓词节点插入历史。此时 Previous 在基线处拒绝；若 N 后仍有 redo 尾部，Next 从相对节点 0 继续。RestoreOriginal 通过同一 CAS writer 恢复 root，并把基线重新设为 0。

RegisteredPolyData 遵循 immutable replacement contract：`CropHostAction::SetPolyData` 只接受严格递增的非零 `sourceVersion`，并要求新 `vtkPolyData` 指针与当前对象不同。调用方若需更新内容，必须先构造新对象或 `DeepCopy` 到新对象后再注册；禁止原地修改已注册对象再只递增 version。这样 shader、history 与异步 Export 读取的是同一不可变快照。

裁切 widget、shader discard、3D 参考切平面和世界方向轴是独立对象。Crop 预览不写主体数据链，只有 Export/RestoreOriginal 经过 expected-snapshot writer 发布。

## 7. Gap 与 Timer

Gap 同样由 composition root 持有 `shared_ptr<GapHostFeature>` 并注册到 Session。启动前必须成功调用 `AttachTimer(HostTimerConfig)`；未绑定 Timer context 时，Feature 无法得到 `OnHostTick()` 驱动。

```text
AttachTimer
  -> 选定一个 HostViewTarget
  -> StdRenderContext::SetTimerHandler

GapHostFeature::SendRequest(Start)
  -> 校验目标、参数与同一 ImageSnapshot 的 image+mask
  -> GapAnalysisService::StartView
  -> worker 接纳并立即启动
  -> 保存 activeVersion

后续 TimerEvent
  -> VtkAppHostSession::OnHostTimer
  -> Feature registry: GapHostFeature::OnHostTick
  -> GapAnalysisService::OnDisplayTick
  -> 轮询/消费 worker 终态
  -> version gate 通过后挂载 overlay、保存 statistics、交付 completion
```

Start 不主动调用 `OnDisplayTick`，但 worker 已在接纳事务中启动；后续 tick 只轮询终态、挂载显示并收口退出。worker 只处理开始时捕获的 immutable snapshot；其结果回到 owner thread 后，Feature 再核对 current version。若 Crop/Reload/其他 writer 已发布新 current，旧结果被退休，禁止回调成功、overlay 或 statistics 泄漏到新数据。

参数校验会检查 surface ratio/absolute、gray min/max、min volume、angle 的 finite；ratio 需位于 `[0,1]`，grayMin 不大于 grayMax，tensor window 为正，erosion 非负。

Overlay 只切换当前版本已有显示；Exit 设置 pending，后续 tick 会 Stop/join worker、移除 overlay、清结果并收口到 Idle。`GapHostFeature::GetState()` 返回 `analysisState`、`isViewActive`、`isExitPending` 与同一成功批次的 `GapStatistics`；未 attached、非 owner thread、退出完成或版本失配时返回零统计。

统计由 worker 对最终有效域一次性计算：空 mask 时 `validVoxelCount` 为全部体素数，非空 mask 时为非零 mask 体素数；`voidVoxelCount` 是成功 label volume 中的正标签体素数，`objectVoxelCount = validVoxelCount - voidVoxelCount`。若正标签超过有效域则整批失败。体积使用三轴 spacing 的体素体积，`porosityRatio = validVoxelCount == 0 ? 0 : voidVoxelCount / validVoxelCount`。

Gap 始终读取 DataManager 同一个 current snapshot 中的 image 与 mask，不读取 Crop preview、render producer 或显示参数。Crop 与 Gap 没有互相 include、指针、状态查询或同步互斥；并发正确性完全由 snapshot identity/version、CAS writer 和结果 version gate 保证。

## 8. Hotkey 与 Qt 边界

`AttachHotkeys(HostHotkeyConfig, HostHotkeyTemplates)` 只属于 standalone 主体输入适配。Feature 通过 `HostInputPort` 以自己的 feature id 注册输入 binding；过期 weak Feature 会在 Timer 清理输入。Qt 不模拟 Ctrl/Escape，主体直接调用三组 `Send*`，Feature 直接调用各自 `SendRequest()`。

Qt 适配顺序：

1. 创建 QVTK widget 和 `vtkGenericOpenGLRenderWindow`；
2. 把同一 window smart pointer 写入 `HostRenderViewConfig`；
3. 构造 `HostSessionConfig` 与 session；
4. `BuildSession()` 并校验 endpoint；
5. 持有并 `AttachFeature()` 所需的可选 Feature；
6. 需要周期驱动时 `AttachTimer()`；
7. 只运行 `QApplication::exec()`。

同一个 VTK TimerEvent 中，App 的 `TimeUpdate` handler 先消费 Data/Render 更新，随后 Host timer hook 才调用各 Feature 的 `OnHostTick()`。因此 Reload 的 current 提交先于 Feature 的版本检查，Feature 不依赖 Load completion 的手工通知。

## 9. 生命周期

Session 析构会先清 Hotkey、Timer 与所有仍存活 Feature 的 attachment，再释放 router/views/core。外部宿主关闭顺序为：

1. 禁止新命令和 UI callback；
2. 尽力对 Crop/Gap Feature 发送 Exit；
3. `DetachFeature()`，再释放 Feature 强引用；
4. 销毁 session；
5. QVTK widget 解绑 render window；
6. 释放外部 VTK window；
7. 销毁 Qt widgets。

## 10. 测试入口

| 范围 | 入口 |
| --- | --- |
| Router/Hotkey | `test/Interaction` |
| Session facade、Feature CAS/version gate 与生命周期 | `test/QtHost/QtHost*Tests.cpp` |
| QVTK 基础 | `test/QtHost/QtHostSmoke.vcxproj` |
| QVTK + Host endpoint | `test/QtHost/QtHostSessionSmoke.vcxproj` |
| Crop | `test/OrthogonalCrop` |
| Gap | `test/GapAnalysis` |

`test/MVVCVTK.Tests.sln` 不包含四个 QtHost 工程。四个工程的编译中间目录按项目名隔离，但共享输出目录且依赖 `QtHostTestCore.lib`；为避免并行重建同一依赖产物，验收脚本应串行构建。

QtHost case 覆盖 View 全请求原子拒绝、Crop/Reload 两种提交顺序、Gap 旧 worker/callback/statistics/overlay 退休，以及 mask 与 image 同版本输入。模块物理删除验收在两个独立副本中分别删除 `features/OrthogonalCrop`、`features/GapAnalysis`，再构建主体 Interaction 与 QtHost core/smoke 工程。

## 11. 扩展纪律

- 新主体业务动作先选择现有 Data/View/Tool 领域；可选业务能力优先实现独立 `HostFeature`，不得把 Feature 命令扩张进主体 variant。
- 新窗口通过 `HostRenderViewConfig` 加入，不在 session 写死数量。
- 新 adapter 参数放 `HostAdapterTypes`，不混入 `HostSessionConfig`。
- 新 Feature 内部实现不进入主体 Host DTO；Feature 只能通过 `HostFeatureContext` 获取 generic view、snapshot、CAS writer、输入、活动 view 作用域与 owner completion。活动作用域只表达 Feature 生命周期，不暴露质量配置或渲染策略。
- composition root 可以根据 UI 流程同时持有多个 Feature 并限制按钮，但这只属于产品策略，不得成为数据正确性的必要条件。
- 异步 worker 不操作 renderer/props；VTK 提交继续在消费线程收口。
