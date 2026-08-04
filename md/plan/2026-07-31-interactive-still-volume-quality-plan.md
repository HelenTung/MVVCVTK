# 交互态与静止态体渲染质量 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: 按任务逐项实施；每项先补失败测试，再做最小实现，再执行本节指定的 x64 验证。任何交互质量切换都不得改写 Qt/Host 公共请求、用户保存的 `VolumeQualityParams` 或其他功能的状态真源。

**Goal:** 在不改变 Qt/上位机公开接口和既有功能流程的前提下，为体渲染建立可验证的交互预览质量与静止最终质量两档，并确保交互结束后恢复用户原始质量。

**Architecture:** 沿用 `VTK input/style event -> InteractiveService::SetInteracting -> SharedInteractionState -> AppService` 的既有状态链。所有当前 `vtkInteractorStyle` 的 Start/End 都由 `StdRenderContext.cpp` 发布为同一个 `CameraStyle` 交互事实，不按 Volume、Slice 或 Iso 模式过滤，也不扩展公开事件枚举或类布局；style Start 回调仅在已验证的 render-owner 线程即时把 desired rate 镜像为交互值，Timer 仍负责所有来源的权威同步和结束恢复。只有 `VolumeStrategy::Mapper` 在实际 `GPURender` 前读取同一 RenderWindow 的 desired rate，对 image/ray sampling 做临时覆盖；Slice、Iso 等策略不消费该 rate 改变采样。producer、mask、denoise、TF、material、gradient、effect 和公开质量配置保持不变，静止状态由同一 mapper 从完整基线恢复。

**Tech Stack:** C++17、Qt 5.14.2、VTK 9.4、MSBuild、Debug/Release x64、现有 Interaction/QtHost 测试工程。

## Global Constraints

- `HostViewSetRequest`、`HostVolumeQuality`、`VtkAppHostSession` 公开签名、枚举值、布局和默认语义不变。
- `InteractionEventKind`、`InteractionEvent`、`StdRenderContext` 的公开头文件和对象布局不变；本任务不得用“内部事件”名义扩展公开 include。
- 不新增 `isoBaseColor` 或任何新的颜色/质量公共接口；本计划与等值面交互、normals/gradients、导出链无关。
- `VolumeQualityParams` 是用户配置真源；交互预览不得写回 `m_volumeQuality`、不得改变 Quality/Custom、`maxDimension`、用户 `sampleDistance` 或 `isJitterOn`。
- `FeatureActive` 继续只表达 OrthogonalCrop 等功能的 Quality producer 选择；不得借用它表达普通相机交互。
- `RenderRate` 与 `VolumeQuality` 仍是不同业务轴。这里只让 RenderRate 驱动 mapper 的短暂采样覆盖，不把它转换为新的 Host 质量值。
- 交互边界不重建 producer，不改变 mask/denoise/TF/material/gradient/effect/input connection。
- `TimeUpdateHandler` 仍是状态同步和 dirty Render 的统一入口；VTK interactor style 的直接 Render 只增加交互质量提示，不被本计划改为新的渲染入口。
- 普通 3D 相机交互、2D 工具、参考平面、裁剪、Load/Reload、Export、Gap overlay 和 IsoSurface 流程必须保持原行为。
- 只验证 `Debug|x64` 与 `Release|x64`，不执行 Win32。
- 不执行 `git add`、`git commit`、`git push`，不删除未跟踪文件。

## 1. 研究结论与当前代码事实

### 1.1 工业/科学体渲染的共同做法

- VGStudio MAX 的公开手册把 Preview Volume 限制为更小的最大分辨率，数据调整时对预览体做预计算和渲染，静止后再使用最终分辨率。[VGStudio MAX 手册](https://www.esrf.fr/files/live/sites/www/files/UsersAndScience/Experiments/StructMaterials/ID19/microtomography/vgstudiomax.pdf)
- 商业工业 CT 软件 Dragonfly 直接提供 `Lower quality in motion`，只在 Track/Pan 等运动时降低细节来提速；另有 `Maximize quality` 通过增加 raycasting steps 提高静止质量，并明确运动降质不影响导出的动画序列。[Dragonfly 3D View Properties](https://www.theobjects.com/dragonfly/dfhelp/2021-1/Content/Key%20Features/Scenes%20and%20Views/3D%20Scene%27s%20View%20Properties.htm)
- 3D Slicer 的 Adaptive 质量在旋转、参数变化时降低射线数量/采样距离，目标是交互 FPS；停止交互后恢复质量。[Slicer Volume Rendering](https://slicer.readthedocs.io/en/5.4/user_guide/modules/volumerendering.html)
- ParaView 将 interactive render 与 still render 分离，交互使用专属 `Image Reduction Factor` 生成较小图像再放大，静止渲染保留完整色彩精度。[ParaView Rendering](https://docs.paraview.org/en/v5.8/ReferenceManual/parallelDataVisualization.html)
- VTK 的 `vtkGPUVolumeRayCastMapper` 只有在 `AutoAdjustSampleDistances` 开启时才会根据 desired update rate 调整 image sample distance；`SampleDistance` 与 `ImageSampleDistance` 是两个独立轴。[VTK mapper](https://vtk.org/doc/nightly/html/classvtkGPUVolumeRayCastMapper.html)

工业软件的共性不是某个固定倍率，而是三层隔离：运动状态决定临时 LOD，静止状态恢复最终质量，导出/分析使用独立的确定性链。VGStudio 的预览体属于更重的第二阶段方案；Dragonfly/ParaView 的运动期 ray/image 降采样更贴合本项目当前结构。

本项目不直接照搬“重建预览体”方案。当前已有 Quality/Custom producer、mask、denoise 缓存，交互期间重建它们会引入 CPU 延迟、MTime 变化和失败回滚风险。第一阶段只在 mapper 上做可逆的采样覆盖，静止恢复时不触碰数据管线，Export 也不读取交互 LOD。

### 1.2 现有质量和交互链

1. 用户质量配置由 `VizService::Impl::SetVolumeQuality` 规范化并保存到 `m_volumeQuality`，见 `MVVCVTK/src/App/Services/AppService.cpp:982-1013`。
2. `SharedInteractionState::SetInteracting` 用 source 集合聚合交互边界，只在空集合与非空集合之间切换时发布 `UpdateFlags::RenderRate`，见 `MVVCVTK/src/App/AppState.cpp:560-595`。
3. `SetStrategyState` 当前只消费 RenderRate 并设置 `vtkRenderWindow::SetDesiredUpdateRate`，随后把 RenderRate 从 flags 中移除，见 `MVVCVTK/src/App/Services/AppService.cpp:1986-2040`。
4. `TimeUpdateHandler` 每 33 ms 先 `SendUpdates`，再 `ResetDirty`，最后执行 `Render`，见 `MVVCVTK/src/Interaction/TimeUpdateHandler.cpp:12-45`。
5. `VolumeStrategy` 构造和 `SetMapperQuality` 都关闭 auto-adjust，并固定 image sample distance/min/max 为 `1.0`，见 `MVVCVTK/src/Render/Strategies/VolumeStrategy.cpp:82-100,456-489`。
6. `VolumeStrategy::SetMapperInput` 按 `m_isFeatureActive` 在 Quality/Custom producer 间切换；`SetVisualState` 的 quality/denoise 分支可能构建 producer，见 `MVVCVTK/src/Render/Strategies/VolumeStrategy.cpp:427-567`。
7. `RenderParams::isFeatureActive` 只表达 feature 的 Quality producer 选择，见 `MVVCVTK/include/App/AppTypes.h:198-222`；没有交互质量字段。

### 1.3 计划建立时的交互识别缺口（已关闭）

计划建立时的旧 HEAD 只在 interactor 上观察鼠标、滚轮和 `InteractionEvent`，尚未把实际 style 的 Start/End 绑定到 `SetInteracting`。当前实现已在 `StdRenderContext` 的实际 `vtkInteractorStyle` 上对称绑定 `StartInteractionEvent/EndInteractionEvent`，全部七个 `VizMode` 统一发布 `StdRender/CameraStyle`；style switch、interactor replacement 和析构路径均卸载旧 callback 并释放 source。因此：

- 2D、3D、Iso 与 Composite 模式的相机 style 均进入同一个 source 聚合；
- `CameraStyle` 只表达交互事实，只有 `VolumeStrategy` 根据统一 desired rate 做 mapper preview；
- `QtHostSessionSmoke` 已用真实 style Start/End、Timer、首帧 preview 和全模式往返证明该链生效，直接 `Render()` benchmark 只保留为独立 mapper 归因证据。

VTK 文档说明 `vtkInteractorObserver` 为交互操作发出 Start/Interaction/End 三类事件，因此应在实际 style 对象上观察并在 style 更换时重新绑定；不能只假设 interactor 会转发这些事件，也不能让 mapper 在另一个线程直接读取 style 的可变 `State`。

## 2. 目标状态机与不变量

```text
Still
  └─ StartInteractionEvent ─> InteractivePreview
InteractivePreview
  └─ EndInteractionEvent ───> 等待 Timer 同步 Still
```

交互 source 仍采用已有集合语义。多个来源重叠时：

- 第一个 source 激活：进入 `InteractivePreview`；
- 中间 source 释放：仍保持 `InteractivePreview`；
- 最后一个 source 释放：恢复 `Still`；
- 重复激活/释放同一 source 不产生额外边界；
- Handler 析构仍通过 `SetInteracting(source, false)` 对称清理。

质量不变量：

```text
用户配置 base:
  auto = false
  image = 1.0
  ray = QualityStep 或 Custom.sampleDistance
  jitter = Quality=true 或 Custom.isJitterOn

交互覆盖:
  auto = false
  image = 2.0
  ray = max(baseRay, 2.0 * baseRay)
  jitter = baseJitter

静止恢复:
  精确恢复 base 的 auto/image/min/max/ray/jitter
```

`image=2.0` 和 ray 两倍只属于第一阶段内部预览候选，不写入 `VolumeQualityParams`；若固定 GPU 测量不能在同一窗口、同一数据和同一相机下改善 p95，则该候选不得产品化，保留现有固定采样。

在 FeatureActive、mask、denoise、maxDimension 固定的 G0/G1 交互往返中，交互覆盖只改变两个真正影响射线数量的采样轴。以下对象必须保持 identity、MTime 或语义不变：

- Quality/Custom resample producer；
- mask producer 和 mapper mask input；
- denoise filter；
- color/scalar opacity/gradient opacity function；
- `vtkVolumeProperty` material/shade；
- RenderEffect binding、candidate stage/commit；
- Qt/Host 请求快照、callback 和 Load/Export owner。

## 3. 推荐实现边界

### 3.1 在 StdRenderContext 接入 VTK style 生命周期

**Files:**

- Modify: `MVVCVTK/src/Render/StdRenderContext.cpp`
- Test: `test/Interaction/AppStateTests.cpp`
- Test: `test/QtHost/QtHostSessionSmoke.cpp`

**Interfaces:**

- Consumes: VTK `vtkInteractorStyle` Start/End events and existing `InteractiveService::SetInteracting`.
- Produces: 既有 `InteractionSource` 的 start/end 状态和 render-owner 线程上的首帧 rate 镜像；不增加 `InteractionEventKind`，不改变任何公开 Host/Interaction/RenderContext 类型。

Implementation:

1. 复用现有 `m_eventCallback`。`AttachObservers()` 除 interactor 原始输入外，在当前 `m_interactor->GetInteractorStyle()` 上观察 Start/End；`RemoveObservers()` 在替换前对旧 style 分别调用 VTK 的 `RemoveObservers(eventId, m_eventCallback)`。不增加 tag 成员，不改变 `StdRenderContext` 布局，也不依赖无法随旧 style 找回的 tag。
2. `SetCameraStyle`、`SetToolMode` 在 `SetInteractorStyle` 前调用既有 `RemoveObservers()`，完成替换后调用既有 `AttachObservers()`；`AttachInteractor` 和析构继续走相同对称路径。测试必须直接覆盖这三个替换入口。
3. `OnVTKEvent` 识别 caller 是否为当前 `vtkInteractorStyle`。所有 VizMode 的 Start/End 都不进入公开 `InteractionEventKind`/Router，而是用 `.cpp` 内固定的 `InteractionSource{"StdRender", "CameraStyle"}` 调用既有 `SetInteracting`；`CameraStyle` 只描述交互事实，不判断当前 Strategy，也不设置 AbortFlag。是否降低 mapper 质量由 `VolumeStrategy` 独立决定。
4. 实施首帧镜像前，先用测试探针记录 style Start callback、rate 写入、Timer 和 `GPURender` 的 thread id。只有固定 Qt/VTK 集成环境证明 style Start 与其随后触发的 `GPURender` 同属 render-owner 线程，才允许在 Start 内设置 source 并把当前 RenderWindow desired rate 镜像为 `GetRenderRate(true)`。`StdRenderContext` 不为首帧镜像另增 owner-thread 成员或全局表，而是复用 `.cpp` 内 `TimerHandler` 记录的 owner；Service 级 VTK 提交门则由第 10 节所述 `VizService::Impl` 私有 owner mutex/thread 状态负责。
5. End callback 只释放 source 并 `SetDirty()`，不直接写静止 rate；若还有 Crop/参考平面等 source，Timer 保持交互 rate，否则下一 Timer 恢复 `GetRenderRate(false)`。这样不需要在公开 service 增加 `GetIsInteracting()`。
6. 若探针证明 style callback、Timer 或 render 不在同一 owner 线程，则首帧镜像决策门失败：禁止即时 VTK 写入，退回纯 Timer 切换并把结果记录为已知一帧延迟；不得用 atomic 快照掩盖 VTK 对象跨线程访问。

验收：

- style Start/End 能使 `GetIsInteracting()` 在边界正确切换；
- `Volume`、`IsoSurface`、三个 Slice 与两个 Composite 模式均发布同一个 `CameraStyle` source，不在 `StdRenderContext` 建立体渲染模式分支；
- default camera 仍收到 press/move/release，`isPropagationStopped=false`；
- 分别经过 `SetCameraStyle`、`SetToolMode`、`AttachInteractor` 替换后，显式触发被保存的旧 style Start/End 不再改变状态，新 style 只产生一组边界事件；context 析构后旧 style callback 不得访问已销毁的 ClientData；
- Start callback、rate 写入和其触发的 `GPURender` thread id 一致；Timer 中 `SetMapperQuality` 与紧随其后的 Render thread id 一致；否则首帧镜像不得实施；
- 2D、Crop、参考平面现有 source 交互回归不变。

### 3.2 保持 Timer-owned desired rate 和线程边界

**Files:**

- Inspect only: `MVVCVTK/src/App/Services/AppService.cpp`
- Test: `test/Interaction/AppStateTests.cpp`
- Test: `test/QtHost/QtHostSessionSmoke.cpp`

`VizService::Impl::SetInteracting` 保持当前实现，只委托 `SharedInteractionState::SetInteracting`。`UpdateFlags::RenderRate` 继续由 Timer 主线程的 `SetStrategyState` 消费，并在 `AppService.cpp:2004-2016` 更新 RenderWindow desired rate。

原因是 `SetInteracting` 可由 Viewer、CropBridge 或其他交互来源调用；service setter 不是 VTK render-thread 边界。在 setter 中即时调用 `vtkRenderWindow::SetDesiredUpdateRate` 会把 VTK 对象写入扩散到未知调用线程，并破坏当前“Timer 主线程集中消费”的约束。

禁止在 `SetInteracting`、SharedState observer 或普通输入 callback 中直接调用 `Render()`、`SetVisualState()`、`BuildPipeline()`、`SendUpdates()` 或写 RenderWindow。唯一例外是 3.1 中经线程断言约束的 style Start callback，它只镜像 desired rate，不主动 Render；默认 style 随后的既有 Render 自然消费该值。

验收：

- 第一个/最后一个 source 边界发布一次 RenderRate，下一次 Timer 得到 `15.0/0.001`；
- 重复 source 和重叠 source 不重复改变 rate；
- 无 RenderWindow 时 flags 保留既有语义，不崩溃、不跨线程触碰 VTK；
- 非法 source 仍返回 false，且不发布 RenderRate。

### 3.3 在 Volume mapper 内做可逆采样覆盖

**Files:**

- Modify: `MVVCVTK/src/Render/Strategies/VolumeStrategy.cpp`
- Modify: `MVVCVTK/include/Render/Strategies/VolumeStrategy.h`（仅当基线成员需要由策略持有）
- Test: `test/QtHost/QtHostViewTests.cpp`

`VolumeStrategy::Mapper` 在 `.cpp` 内保存完整的静止质量状态，而不是只保存 ray/jitter：

```cpp
struct QualityState {
    bool isAuto = false;
    double image = 1.0;
    double minImage = 1.0;
    double maxImage = 1.0;
    double ray = 1.0;
    bool isJitter = true;
};
```

`SetMapperQuality()` 经第三轮的 thread-id 测试门证明与 Timer Render 同线程后，完成合法性检查，一次性生成完整 `QualityState` 并更新 `m_stillQuality`；不得逐字段发布半更新基线。`GPURender()` 只读取它所属 RenderWindow 的 desired rate，禁止读取 `vtkInteractorStyle::GetState()`：

```cpp
const bool isPreview =
    renderWindow
    && renderWindow->GetDesiredUpdateRate() >= GetRenderRate(true);
if (isPreview != m_isPreviewActive) {
    SetPreviewQuality(isPreview);
}
```

style 首帧由 3.1 经测试门批准的同线程 rate 镜像覆盖；其他非 style 来源继续由下一次 Timer 更新 desired rate。所有 Strategy 共享同一交互 rate 事实，但只有 Volume mapper 根据该值切换 preview。测试必须记录 `SetMapperQuality`、preview setter、父类 `GPURender` 的 thread id；若不一致，不得增加跨线程 setter，方案退回现有 Timer/render 边界。

`SetPreviewQuality(true)` 只执行：

```cpp
SetAutoAdjustSampleDistances(false);
SetImageSampleDistance(2.0);
SetMinimumImageSampleDistance(m_stillQuality.minImage);
SetMaximumImageSampleDistance(m_stillQuality.maxImage);
SetSampleDistance(std::max(m_stillQuality.ray,
    2.0 * m_stillQuality.ray));
SetUseJittering(m_stillQuality.isJitter);
```

`SetPreviewQuality(false)` 逐字段精确恢复 `m_stillQuality` 的 auto/image/min/max/ray/jitter。预览覆盖不得切换 `m_qualityResample`/`m_customResample`，不得调用 `SetMapperInput()`。

这里的“完整基线”严格指本任务和当前 `SetMapperQuality` 会改变的六个 mapper 质量字段：auto/image/min/max/ray/jitter；`MaxMemoryInBytes`、`MaxMemoryFraction` 等未被当前质量链修改的 mapper 属性既不纳入覆盖，也必须前后不变。

preview 是跨交互帧保持的 mapper 状态，不是一次 `GPURender` 内必须 finally 恢复的临时变量：交互期间持续保持 preview；desired rate 回到静止值后的下一次 `GPURender` 在调用父类前恢复六字段。preview setter 必须发生在现有 effect `OnRenderStart()` 之前，且不额外调用 `OnRenderStart/OnRenderStop`。本任务保持当前成功渲染的 effect 调用次序，不顺带改变父类 `GPURender` 抛异常时 `OnRenderStop` 可能缺失的历史语义；由于 still 恢复发生在父类调用前，即使该次父类抛异常，六字段也已恢复。测试以 callback 计数证明 preview 切换没有新增或漏掉成功帧 callback。

质量变更与交互同时发生时：

- `SetMapperQuality()` 先更新新的静止基线；
- 当前 desired rate 为交互值时，再应用一次 preview overlay；
- producer 缓存键、mask、denoise 和 input connection 继续沿现有 quality/denoise flags 运行；
- 失败仍由 `SetVisualState` 恢复旧质量真值和旧 mapper 连接。

不采用 `AutoAdjustSampleDistancesOn()` 作为产品默认，因为现有 benchmark 已显示小体场景下 desired rate 改变而 mapper sample 轴不一定变化；固定覆盖更可复现，也不改变现有 Quality/Custom 语义。

### 3.4 其他功能隔离

- `SliceStrategy`、`IsoSurfaceStrategy`、`ColoredPlanesStrategy`、Gap overlay 参与统一 `CameraStyle` 交互聚合，但继续忽略交互质量覆盖。
- `CompositeStrategy` 仍先同步 reference planes，再同步主策略；只有主策略为 Volume 时 mapper 采样发生变化。
- `SetFeatureActive` 仍只切换 Quality/Custom producer；交互 preview 不得调用它。
- TF、material、gradient、denoise、mask、reload、export 的 flags 和 callback 顺序保持不变。
- 交互质量 G0/G1 的“只改 image/ray”结论只在 FeatureActive、mask、denoise、maxDimension 固定时成立；普通 Quality/Custom/Feature 切换仍可能按现有流程更换 producer/input。
- 同一 Timer 请求同时包含 Quality/Feature 与 TF/material/gradient 时，先按现有事务恢复/提交 quality、feature、denoise、mask producer，再同步 TF/material/gradient，最后由 dirty Timer 渲染，禁止观测到 transient stale input。
- 交互期间的数据 reload 若触发 producer rebuild，重建成功后下一次 `GPURender` 仍根据当前 desired rate应用 preview；失败则保留旧管线和当前 preview 状态。
- Crop/Gap、Reload、Export 使用各自既有 owner/async 链，不与采样轴 benchmark 同时启动，也不纳入“只改两个采样轴”的归因结论。

## 4. 测试与性能验收任务

### Task 1：style 生命周期失败测试

**Files:** `test/Interaction/AppStateTests.cpp`, `test/QtHost/QtHostSessionSmoke.cpp`

- 构造真实 `vtkInteractorStyleTrackballCamera`，调用 Start/End，验证 source 边界、重复事件和重叠 source。
- 遍历全部七个 `VizMode`，验证每个当前 style 都发布同一个 `CameraStyle` source，并由 Timer 恢复静止 rate。
- 验证 style event 不被 Router 标记为传播停止，VTK 默认相机仍改变 position/focal point/parallel scale。
- 调用 `SetCameraStyle` 和 `SetToolMode` 后分别触发旧/新 style 事件，断言只有新 style 生效。

命令：

```powershell
& 'D:\vs2026\program\MSBuild\Current\Bin\MSBuild.exe' `
  'test\Interaction\InteractionRouterTests.vcxproj' /m `
  /p:Configuration=Debug /p:Platform=x64
& 'test\Interaction\x64\Debug\InteractionRouterTests.exe'
```

预期：退出码 0；非法 source fail count 为 0；旧 style 不再发布边界。

### Task 2：mapper 预览/恢复属性测试

**Files:** `test/QtHost/QtHostViewTests.cpp`

对 Quality、Custom(1000, 1.0, jitter=false)、mask on、denoise on、TF、gradient、Soft/Dense/Glossy 各建立一次基线，记录：

- auto/image/min/max/ray/jitter；
- producer、mask、denoise、input connection 地址和 MTime；
- CTF/OTF/gradient function 节点与 range；
- material/shade；
- effect state。

然后按下列顺序执行：

1. 设 desired rate `0.001`，Render，保存静止基线；
2. 触发真实 style Start（经 3.1 镜像 rate）或直接在 Timer fixture 设 desired rate `15.0`，Render，断言 image=2、ray=2×base、auto=false，其他记录不变；
3. 触发真实 style End 并让下一次 Timer 把 desired rate 恢复为 `0.001`，Render，断言所有采样字段精确回到基线；
4. 在 FeatureActive、mask、denoise、maxDimension 固定并记录的前提下切换 Quality↔Custom、TF、material、gradient，各重复 1-3；
5. 另做 FeatureActive active↔inactive、mask、denoise 往返，断言 producer/input identity 按既有语义切换并在返回原状态后恢复；预览覆盖不参与 producer 选择；
6. 失败的 producer 构建或质量校验后，断言旧 mapper/input/cache 和 preview 状态保持。

不得使用截图全黑/非全黑作为唯一通过条件；固定 GPU 视觉健康检查只能作为附加证据。

### Task 3：Timer 提交链与直接 style Render 分层 benchmark

**Files:** `test/QtHost/QtHostSessionSmoke.cpp`

拆分两个 fixture：

- `TimerPhase`: 每个样本只 `SetDirty`，直接调用真实 `TimeUpdateHandler::Send(timerEvent)`，覆盖 `SendUpdates -> ResetDirty -> Render` 合帧提交链；该 fixture 不经 interactor `InvokeEvent(TimerEvent)`，因此只证明 Timer handler 链，不冒充 VTK interactor 事件投递；
- `StylePhase`: 只触发真实 style Start/Interaction/End，让 VTK 默认 style 自己 Render，记录 style callback/rate 写入/GPURender 的线程 id 和 mapper preview 状态。

每个阶段至少记录 120 个稳态样本的 p50/p95/max；初始化、candidate prewarm、shader/TF 首帧单列。不得把直接 `renderWindow->Render()` 的样本冒充 Timer 交互链。

验收目标不是绝对毫秒，而是同机相对结果：

- Preview 阶段 image/ray 采样确实低于静止基线；
- Style 阶段第一帧已经使用 preview；
- End 后由下一次 Timer 同步 static rate，首个 Timer 可见帧恢复静止基线；
- Timer 合并窗口每周期最多一次可见 Render；
- 没有新增 direct Render 来源；
- 其他功能的 Render 次数不增加。

### Task 4：跨功能回归

**Files:** `test/Interaction/HostCommandRouterTests.cpp`, `test/QtHost/QtHostViewTests.cpp`, `test/OrthogonalCrop/AppTaskServiceTests.cpp`

- 公开请求结构、enum 值、callback 次数和失败传播保持现有静态断言；
- Volume preview 往返后执行 Slice cursor/window-level、Iso threshold、Crop commit、Gap overlay、ExportData/ExportSlices，断言 snapshot/version、owner、strategy mode 不变；
- CompositeVolume reference planes 仍在同一帧同步，不因 preview 变更位置、颜色或可见性；
- FeatureActive 与 Interaction source 重叠时，最后一个 source 释放后仍恢复 FeatureActive 原有 Quality 语义。

### Task 5：x64 构建与图谱

Debug 通过后以同一顺序验证 Release：

```powershell
& 'D:\vs2026\program\MSBuild\Current\Bin\MSBuild.exe' `
  'test\QtHost\QtHostMethodTests.vcxproj' /m:1 `
  /p:Configuration=Debug /p:Platform=x64
& 'test\QtHost\x64\Debug\QtHostMethodTests.exe'
& 'D:\vs2026\program\MSBuild\Current\Bin\MSBuild.exe' `
  'test\QtHost\QtHostSessionSmoke.vcxproj' /m:1 `
  /p:Configuration=Debug /p:Platform=x64
& 'test\QtHost\x64\Debug\QtHostSessionSmoke.exe'
```

若修改了代码，执行 `npx graphify hook-rebuild`，随后 `graphify portable-check .graphify`。计划文档本身不触发 graph rebuild。

## 5. 三轮 Review 计划

### Review 1：代码链与 VTK 事件归属

逐行复核 `StdRenderContext` style observer 的绑定/解绑、`OnVTKEvent` caller 解析、`.cpp` 内 CameraStyle source 与 `Viewer3DHandler` 既有参考平面 source 的对称性、AppService 的 Timer-owned RenderRate、`VolumeStrategy::Mapper::GPURender` 基线保存和恢复。必须确认除经线程门批准的 style Start rate 镜像外没有在 service/input callback 中触碰 VTK、没有重复 source 泄漏、没有把 style 事件错误地当成 interactor 事件。

### Review 2：公共 API 与其他功能隔离

逐项比较 Qt/Host public include、`HostViewSetRequest` 布局、enum 值和现有 setter。追踪 Composite/Volume/Slice/Iso/Crop/Gap/Load/Reload/Export 的 flags、snapshot、owner 和回调链，确认固定 producer 的 G0/G1 交互往返只改变 Volume mapper 两个采样轴。

### Review 3：实验可归因性与恢复证明

复核 TimerPhase 是否覆盖真实 `TimeUpdateHandler::Send` 提交链、StylePhase 是否覆盖 VTK style 默认 Render；检查 120 样本、首帧/稳态分层、p50/p95/max、异常帧政策；逐字段证明 preview→still 恢复。若任何证据只能证明“desired rate 改了”而不能证明 mapper image/ray 改了，则计划不得宣称性能已解决。

## 6. 三轮 Review 记录

### 6.1 Review 1：状态链、mapper 与 effect

结论：发现 3 项 important，均已回修。

- 原方案曾考虑从任意 `SetInteracting` 调用即时写 RenderWindow，违反现有 Timer 集中写 VTK 的线程边界；现已限定只有经 thread-id 测试门证明同线程的 style Start callback 可镜像 rate，其他来源仍由 Timer 消费。
- 原方案只保存 ray/jitter，却要求恢复六字段；现已定义 `.cpp` 内 `QualityState`，一次性保存并恢复 auto/image/min/max/ray/jitter。
- preview 与 effect 的先后和异常语义不清；现已明确 preview setter 位于 `OnRenderStart` 前，preview 跨交互帧保持，不增加 effect callback，也不在本任务改动父类异常时的历史 effect 语义。

### 6.2 Review 2：API/ABI 与跨功能隔离

结论：发现 3 项 important、2 项中等级和 2 项低等级问题，均已回修。

- 删除对 `InteractionTypes.h`、`StdRenderContext.h` 和 `Viewer3DHandler` 的计划修改；style 观察、source 投递和解绑全部收敛在 `StdRenderContext.cpp`，公开枚举、类布局和 Host API 不变。
- 删除 mapper 跨线程读取 `vtkInteractorStyle::State` 的方案；mapper 只读所属 RenderWindow desired rate，style 首帧使用同线程 rate 镜像并设置失败决策门。
- 明确 `SetCameraStyle`、`SetToolMode`、`AttachInteractor` 三个替换入口的旧/new style 对称解绑测试。
- 将 FeatureActive 纳入矩阵，区分“固定 producer 时 preview 只改 image/ray”与 Quality/Feature 本身可能重绑 producer；补 active↔inactive 往返和恢复顺序。
- Crop/Gap、Reload、Export 保持各自 owner/async 链，与采样性能实验隔离。

### 6.3 Review 3：实验归因、线程证据与恢复证明

结论：无 blocking；3 项 important 已澄清并形成实施门。

- 现有直接 `Render()` benchmark 不能替代 TimerPhase/StylePhase；两阶段须分别清空计数、分离首帧/预热/稳态并报告 p50/p95/max。
- style callback、rate 写入、Timer、`SetMapperQuality` 和 `GPURender` 必须由测试探针给出 thread-id 证据；证据不一致时禁用首帧镜像，不新增跨线程状态。
- 明确 preview 是交互状态而非单帧 RAII 覆盖；still 六字段在父类 render 前恢复。其他 mapper 内存上限等非质量字段不进入覆盖，前后保持不变。

三轮 review 均以当前代码入口和 `file:line` 为依据；“尚未实现”不作为 finding，finding 只针对计划可执行性、边界和验证缺口。

## 7. 风险与决策门

| 风险 | 控制 |
|---|---|
| VTK style 事件绑定到错误对象 | 只观察实际 `vtkInteractorStyle`，style 替换时对称重绑，并用旧/新 style 测试 |
| 交互开始第一帧仍使用静止质量 | style Start 同线程镜像 rate；StylePhase 同时记录 callback/rate/GPURender thread id 和首帧 mapper 属性，线程门失败则明确接受 Timer 一帧延迟 |
| 预览采样覆盖写回用户配置 | mapper 单独保存 base sample/jitter；`m_volumeQuality` 前后字节/字段比较 |
| 质量切换触发 producer 重建 | preview 只在 `GPURender` 设置 image/ray，不调用 `SetMapperInput` |
| FeatureActive 被交互覆盖 | 不读写 `m_isFeatureActive`，只使用 RenderWindow desired rate |
| 2D/Crop/Gap/Export 流程被改动 | RenderRate 只在 Volume mapper 内消费；跨功能回归逐条验证 |
| 2 倍采样仍不降低真实 p95 | 不改变默认；保留固定采样并记录证据，不用复杂策略掩盖结果 |
| 预览画面过糊或闪烁 | 固定 GPU 对照 CTF/OTF/visual hash；只允许采样轴变化，材质和 TF 不混改 |

## 8. 完成标准

- 新文档经过三轮代码结合 review，所有 blocking/important finding 已修订或明确保留为决策门。
- 3D 默认相机旋转/缩放、2D 工具和 Crop style 的开始/结束均能正确聚合交互 source。
- Volume Preview 的 image/ray sampling 在真实 style Render 和 Timer Render 中都可观察到，Still 帧逐字段恢复。
- producer、mask、denoise、TF、material、gradient、effect、Host 请求和其他功能流程不变。
- Debug/Release x64 构建和相关测试退出码均为 0；性能报告区分 style 默认 Render、Timer handler Render、预热和稳态。
- 文档 UTF-8、无 BOM、LF；不新增 `isoBaseColor`，不改 IsoSurfaceStrategy。

## 9. 本轮按计划落地与最终验证

### 9.1 实际代码落点

- `MVVCVTK/src/Render/StdRenderContext.cpp`
  - 在实际 `vtkInteractorStyle` 上绑定/解绑 Start、End observer；`SetCameraStyle`、`SetToolMode`、`AttachInteractor` 和析构均走对称生命周期。
  - 所有 VizMode 统一发布 `StdRender/CameraStyle`，不在 RenderContext 判断 Volume/Slice/Iso；策略差异只保留在下游 Strategy。
  - style Start 只在 render-owner 线程镜像交互 `desired update rate`，不调用 `Render()`、不设置 AbortFlag；End 只释放既有 `StdRender/CameraStyle` source 并置脏。
  - 旧 style、替换 style、context 析构后的回调均有真实测试。
- `MVVCVTK/src/Render/Strategies/VolumeStrategy.cpp`
  - `.cpp` 内 `QualityState` 保存 auto/image/min/max/ray/jitter 六个静止基线字段。
  - `GPURender()` 只读取所属 RenderWindow 的 desired rate；preview 在 effect `OnRenderStart()` 前切换，静止帧在父类 render 前逐字段恢复。
  - producer、mask、denoise、TF、material、gradient、effect 和公开 `VolumeQualityParams` 均未被 preview 写回。
  - 质量/TF/gradient/material 同帧非法请求在任何视觉提交前返回；管线失败回滚质量真值、feature、denoise，并显式检查 mapper input 恢复结果。
  - 质量 setter、preview setter 与 render 的 thread id 在 mapper 内部记录并在 benchmark 输出中比对；不一致时立即撤销 preview。
- `MVVCVTK/src/Interaction/TimeUpdateHandler.cpp`
  - 对 `vtkGenericOpenGLRenderWindow::GetReadyForRendering()` 增加真实 QVTK 窗口就绪门；每个 Timer 周期保持 `SendUpdates -> ResetDirty -> 至多一次 Render`。
- 测试覆盖扩展至 `QtHostViewTests.cpp`、`QtHostCropTests.cpp`、`CropShaderPreviewTests.cpp`、`QtHostSessionSmoke.cpp`；既有 `AppStateTests.cpp`、`HostCommandRouterTests.cpp` 的 source 聚合、公开请求/enum/callback 静态断言保持不变。

### 9.2 性能实验与决策门

- TimerPhase 使用真实 `TimeUpdateHandler::Send` 合帧链，每轮 120 个稳态样本；StylePhase 使用真实 VTK press/move/release 与 interactor `TimerEvent`，随后循环 120 次 MouseMove。两者分别报告，未把直接 Render 样本冒充 TimerPhase。
- 窗口、相机、数据固定到 1280×960、128³ 体；Render EndEvent 后调用 `vtkRenderWindow::WaitForCompletion()`，避免只测 OpenGL 命令提交时间。
- 预热样本单列；每个交互档执行 3 轮 before/during/after，报告 p50/p95/max，并以配对轮次的中位 p95 gain 做决策。
- Debug 复查后最终基准：`I_fixed_interaction` 配对中位 gain 3.04%（诊断档）；代表性 `J_custom_interaction` static p95 1.6029 ms、preview 1.3424 ms（gain 16.25%，required=1）。
- Release 复查后最终基准：`I_fixed_interaction` 配对中位 gain 4.77%（诊断档）；`J_custom_interaction` static p95 0.7927 ms、preview 0.5378 ms（gain 33.35%，required=1）。
- `VolumeQualityThread`、StylePhase、Timer 和 GPURender thread id 全部一致；首帧 `first_preview=1`、Timer `timer_preview=1`、End 后 `restored=1`。
- 跨模式检查输出 `BENCH_CROSS: slice=1 iso=1 export=1 stale_preview=0`。Crop、Gap、Load/Reload、Export 的 owner/version/失败传播由完整 QtHostMethod、QtHostCrop 和 Gap 测试套件继续覆盖。

### 9.3 三轮最终 review 结论

1. 状态链 review：确认 style caller 归属、旧/new style 对称解绑、Timer-owned rate、mapper 六字段恢复和 effect 顺序；补齐了质量失败的原子早退与回滚检查。
2. 公共 API/隔离 review：确认 Qt/Host public include、`HostViewSetRequest` 布局、enum 值及 `InteractionEvent` 均无变更；Slice、Iso、Composite reference props 与 Export 回归通过，未引入 iso 颜色或 normals/gradients/导出链改动。
3. 实验归因 review：补齐 GPU completion、固定窗口/相机、3×120 样本和 StylePhase 120 样本；代表性 Custom 档在 Debug/Release 均通过 p95 决策门，固定档仅保留为同机诊断对照。

### 9.4 构建、运行与图谱

以下条目是实施当次终端运行结果的记录，不是仓库内持久化的日志或 CI manifest；复核新的 `HEAD` 时必须重新执行对应命令，不能仅凭本节证明当前二进制仍通过。当前同步审计锚点为 `83f9160`。

- `test/MVVCVTK.Tests.sln Debug|x64`、`Release|x64`：成功，0 warning/0 error。
- QtHostMethodTests、QtHostSessionSmoke Debug/Release：成功，0 warning/0 error；InteractionRouter、PlanarCrop、GapAnalysis、普通会话、style-only、volume benchmark 全部退出码 0。
- 2026-08-01 新增全部七个 `VizMode` 的 `CameraStyle` Start/End 往返断言；修改前旧过滤以 `camera_source_unified=0` 失败，删除过滤后 Debug/Release `--style-quality-only` 与普通 SessionSmoke 均退出码 0。
- 2026-08-01 owner 门失败测试在旧实现上分别以“未绑定 service 修改 VolumeProperty”和“非 owner 重绑 RenderContext 后 owner 被转移”失败；修正后 Debug/Release `PlanarCropTests`、`QtHostMethodTests` 和普通 `QtHostSessionSmoke` 均退出码 0。
- 代码修改后执行 `npx graphify hook-rebuild`；随后 `graphify portable-check .graphify` 返回 `Portable artifacts OK`。
- 本轮未修改任何 public header，未新增 `isoBaseColor`，未修改 `IsoSurfaceStrategy`，未触及 normals/gradients 交互关闭或导出链。

## 10. 现状复查补丁

本轮复查发现并已修订两个线程边界缺口：

- `VizService::Impl` 在 Pimpl 内记录 RenderContext owner thread。未绑定 RenderContext 时不存在合法 VTK 提交者；状态意图可以累积，但 `SendUpdates()` 不消费 pending，`BuildPipeline()`/`SendReloadUpdate()` 返回失败。绑定后只有 owner thread 可以调用这三个提交入口；非 owner 不能借 `SetRenderContext()` 替换绑定或转移 owner，`SendReloadUpdate()` 也只做纯拒绝，不再出现“返回 false 但仍排队”的双重语义。异步状态发布必须经既有 state event/pending 邮箱，由 owner Timer 提交。
- `m_ownerMutex`、`m_ownerThread`、`m_hasOwnerThread` 和 `GetIsOwnerThread()` 位于生产文件 `AppService.cpp` 的私有 Pimpl 中，是 VTK 提交线程契约的实现，不是测试代码，也不是为测试开放的接口。
- `AppTaskServiceTests` 先证明未绑定 service 的 `SendUpdates()` 不修改 `VolumeProperty`，再显式绑定 RenderContext 并证明同一 pending 状态由 owner 正常提交；另覆盖非 owner reload 与非 owner 重新绑定均不触碰当前 VTK 管线。生产代码不为离线测试开放线程门，测试必须建立真实生产前置条件。
- `StdRenderContext` 的 style 首帧 rate 镜像增加运行时门：只有在当前线程安装了 Timer handler 且其记录的 owner thread 与 callback 线程一致时才写 RenderWindow；没有 owner Timer 的离线 context 退回 Timer-only，不再无条件跨线程写入。`QtHostSessionSmoke --style-quality-only` 已覆盖真实 HostTimer 绑定场景。

复查同时移除 Volume mapper 正常路径的 stdout 诊断噪声，仅在线程不一致时输出错误；保留内部线程比较和 preview 失败回退。当前 Debug 重建和 style-only 已重新通过。

## 11. 2026-08-01 数据导出保真增补计划

本节是后续独立收敛任务，不改变本计划第 1-10 节的交互质量结论。当前代码事实为：

- `HostDataExportFormat` 仅支持 `Raw/Ply/Stl/Obj`，仓库没有 PDF、VTP 或 CSV 导出实现；
- PLY/STL/OBJ 都由冻结的 `ImageSnapshot`、iso、validity mask 和 model-to-world 生成同一份三角网格；
- 审查起点的 PLY 只写几何和 `vtkFlyingEdges3D` 生成的法线，没有显式 RGB 数组或颜色回读测试；本节实施后改为写入由真实网格点标量与冻结 TF 映射得到的 `RGB`；
- Volume transfer function、Iso actor 光照与屏幕截图不是同一个文件颜色契约，不能从 framebuffer 反推网格 RGB；
- STL 标准链不承诺颜色，当前 `vtkOBJWriter` 也不写点 RGB。不得为了“看起来一致”向这些格式写非标准私有字段。

收敛原则：

1. 唯一业务链：继续使用 `HostDataExportRequest -> HostCommandRouter -> VizService -> AppDataExportTaskService -> BaseDataManager`，不增加旁路 writer；
2. 单一批次：任务接纳时一次冻结 image/mask、iso、model-to-world、scalar range 和 TF 节点，worker 不读取后续状态；
3. 数据保真：几何来自冻结 image 的等值面，坐标烘焙同一 model-to-world；PLY RGB 只由网格点标量与同一批 TF 映射产生；
4. 格式诚实：PLY 验证 points/faces/normals/RGB，OBJ 验证 geometry/normals，STL 只验证 geometry；不宣称格式本身无法表示的语义；
5. 回读证明：写出成功不以“文件非空”为充分条件，测试必须用独立 reader 回读并比较点数、单元数、坐标容差、法线和 RGB 数组；
6. 扩展克制：PDF 属于未来报告/切片分页能力，若实施仍进入同一异步导出链，不在 Host/App 中直接生成文件。

### 11.1 PDF 审查结论与后续优先级

当前仓库没有 PDF Request、App task、writer、依赖或回归样本，因此 PDF 是能力缺口，不是待调优的既有功能。后续实现应复用 Session/Router/worker/completion 的异步骨架，但不要把报告语义硬塞进 `HostDataExportFormat`，也不要让 `BaseDataManager` 同时承担数据序列化和页面排版。建议按以下顺序推进：

1. **P0 契约与可靠性**：新增独立 typed report request；一次冻结图像版本、切片方向、cursor、WW/WC、scalar range/TF、iso、model-to-world 与报告元数据；定义覆盖策略、临时文件到目标文件的原子替换、取消/进度和稳定失败原因；
2. **P1 可读性与真实性**：支持多页正交切片、等值面/体渲染快照、尺寸/spacing/坐标系/数据版本、TF 图例和测量标尺；文本与线条优先矢量，栅格图明确 DPI 与 sRGB，不把屏幕像素尺寸冒充物理尺寸；
3. **P1 国际化与版式**：嵌入可授权的中文字体，处理长路径/长标题、页眉页脚、分页、空数据和极端宽高比；模板只描述版式，不持有业务状态；
4. **P2 归档与交换**：按实际需求再评估 PDF/A、书签、附件或结构化测量表，避免第一版过早扩大协议；
5. **验收**：生成后同时做结构回读与逐页渲染，检查页数、文本、元数据、裁切、字体、图像分辨率和视觉一致性；失败测试覆盖 Unicode、只读目录、磁盘写入失败、取消与同名文件竞争。

### 11.2 厂商 PDF 功能结论与产品收敛

本轮只把厂商手册作为需求证据，不把宣传语或设备控制能力当成项目现状。页码均为 PDF 页码：

| 证据 | 厂商能力 | 对本项目的结论 |
| --- | --- | --- |
| `pdf/Hexagon/海克斯康_VGSTUDIO_MAX_VG软件功能概览_英文.pdf` p6；旧版参考手册 p8、p10、p14、p16 | PLY/mesh/CAD 导入、亚体素表面、孔隙/夹杂、距离/角度/卡尺、ROI 统计与连通域、内建 PDF/HTML 报告 | 报告应消费结构化测量/分析结果，不能只拼截图；旧版目录证据只作为方向，实施前仍需重新核对现代版本 |
| `pdf/Lumafield/Lumafield_Neptune_用户手册_简体中文_2026.pdf` p31-38 | 云端持久数据、权限协作、书签、虚拟切片、孔隙率、CAD/扫描对比偏差场 | 优先补本地书签/标注、ROI 与缺陷结果模型；云协作与权限不进入当前桌面核心 |
| `pdf/Nikon/尼康_Inspect-X_CT控制软件用户手册_英文.pdf` p180-205、p254-265、p561-568 | tone/histogram、ROI 测量与 void 结果表、operator/serial/status、HTML 报告副本 | 报告需携带操作者、数据版本、时间、单位与结果表；图像调节值和测量数据必须分栏，不能混为一个视觉状态 |
| `pdf/ZEISS/蔡司_METROTOM_OS_CT扫描软件操作说明书_英文.pdf` p27-44、p115、p147-177 | 事件日志、错误包、重建参数/坐标头、结果管理；brightness/contrast 只影响显示 | P0 增加稳定 failure reason、日志与来源元数据；屏幕亮度/对比度不得改写真实标量、网格 RGB 或测量结果 |
| `pdf/Bruker/布鲁克_SkyScan_1272_用户手册_英文.pdf` p24、p31、p34、p49-56；`pdf/Shimadzu/岛津_inspeXio_SMX-225CT_FPD_HR基础操作说明书_英文.pdf` p91-99 | 三正交/斜切/双斜切 MPR、2D/3D 距离、STL/PLY、3D/AVI、DICOM | 当前正交切片与 PLY/STL 已有基础；下一步先补测量与斜切 MPR，动画/DICOM 按真实交换需求后置 |
| `pdf/RXSolutions/RX_Solutions_UltraTom_技术规格表_英文.pdf` p2、`pdf/TESCAN/TESCAN_UniTOM_HR_2_产品技术规格表_英文.pdf` p3、`pdf/WENZEL/温泽_exaCT_S75HRE_技术规格表_德文.pdf` p1、`pdf/ThermoFisher/赛默飞_HeliScan_Mk2_标准操作规程_英文.pdf` p2/p7 | 宏/批处理、4D、VOI、CAD/壁厚/缺陷、sample ID/filter/存储检查 | P1 建立可组合分析任务和 CAD/缺陷结果；P0 先把样本 ID、算法/参数版本与数据来源纳入可追溯元数据 |

综合当前代码与以上证据，功能升级顺序收敛为：

1. **P0 可信结果底座**：建立 measurement/annotation/bookmark/result 的稳定数据模型，先支持距离、角度、面积、体积与 ROI 统计；所有结果带单位、坐标空间、数据版本和算法参数，供界面与未来 PDF/HTML 共用；
2. **P0 报告任务链**：独立 typed report request、单批冻结、进度/取消/稳定失败原因、临时文件原子替换；首版多页报告包含三正交切片、3D/iso 快照、TF 图例、物理标尺、数据/变换/操作者元数据和结构化结果表；
3. **P1 分析能力**：斜切/双斜切 MPR、ROI histogram/min/max/mean/deviation、阈值与连通域、孔隙/夹杂统计；随后再做 CAD 配准、偏差场、壁厚和扫描对比；
4. **P2 工作流扩展**：可复用分析模板、宏/批处理、时间序列/4D、DICOM/HTML/PDF-A、书签附件与协作发布，均在对应需求和互操作测试成立后进入；
5. **明确不做**：X 射线源、运动轴、air/offset/detector/geometric calibration 属于设备控制；本项目只接收并展示上游提供的校准状态/重建参数，不模拟或声称完成设备校准。

### Task 6：冻结完整导出参数

**Files:** `MVVCVTK/include/App/AppTypes.h`、`MVVCVTK/include/App/AppInterfaces.h`、`MVVCVTK/include/Data/DataManager.h`、`MVVCVTK/src/App/Tasks/AppDataExportTaskService.cpp`、`test/OrthogonalCrop/AppTaskServiceTests.cpp`

- [x] 定义 `DataExportParams`，收敛规范后缀、iso、model-to-world、scalar range 和 TF 节点，避免继续扩张 positional 参数。
- [x] `BuildDataTask` 在调用线程填充完整参数并按值捕获；测试在构造任务后修改共享状态，证明 worker 仍使用接纳时快照。
- [x] 保持 Host 公共 `HostDataExportRequest` 与 enum 布局不变。

### Task 7：PLY 点 RGB 与几何保真

**Files:** `MVVCVTK/src/Data/DataManager.cpp`、`test/OrthogonalCrop/AppTaskServiceTests.cpp`

- [x] 在 `BaseDataManager::Impl` 内根据输出网格点标量、冻结 scalar range 与 TF 节点构建三分量 `VTK_UNSIGNED_CHAR`、名称为 `RGB` 的 point-data 数组。
- [x] PLY writer 显式选择 `RGB`；TF 为空时使用由 scalar range 推导的灰阶 fallback，不从 renderer、actor 或截图读取颜色。
- [x] 使用绿色到青色的固定 TF fixture 回读 PLY，断言 `RGB` 为 active point scalars、三分量、tuple 数等于 point 数，并逐点符合冻结映射。
- [x] 回读 PLY/OBJ/STL，分别验证格式可表达的点、三角单元、world 坐标和法线不变量；STL/OBJ 不伪造 RGB 成功。

### Task 8：文档、x64 与图谱验收

**Files:** `md/HostLayer.md`、`md/MVVCVTKArchitectureReview.md`、`md/QtIntegrationGuide.md`

- [x] 同步写明 PLY 的 scalar-TF RGB 契约、OBJ/STL 边界，以及 PDF 尚未实现的事实。
- [x] 构建并运行 `PlanarCropTests.vcxproj` 的 Debug/Release x64；回读测试退出码必须为 0，构建不得新增 warning/error。
- [x] 运行 `npx graphify hook-rebuild` 与 `graphify portable-check .graphify`，确认 TypeScript runtime 和可移植性。

## 12. 2026-08-01 数据导出保真实施记录与三轮复审计划

> 本节记录第 11 节已经落地的实际改动，并把复审拆成三个可独立否决的门。它不是新的功能分支，也不改变 `HostDataExportRequest` 公共协议。事实基线为提交 `83f9160ab34e2328e56cb65b34d7b797a6a5562c` 加当前未提交差异；远端 `origin/master` 与该提交 ahead/behind 为 `0/0`。

### 12.1 目标、架构与全局约束

**目标：** 让 RAW/PLY/STL/OBJ 只消费同一次接纳时冻结的数据状态，并使 PLY 的点 RGB、三种网格格式的几何与真实输出三角网格一致。

**架构：** 保持 `HostDataExportRequest -> HostCommandRouter -> VizService -> AppDataExportTaskService -> BaseDataManager` 唯一业务链。App task 负责冻结值对象，Data 层负责 affine 校验、网格生成和格式序列化，测试通过独立 reader 回读证明文件语义。

**技术栈：** C++17、VTK 9.4、MSBuild、Qt 5.14.2 宿主测试、graphify TypeScript runtime。

全局约束：

- 只验证 `x64`，构建不得新增 warning/error；
- 不新增或改变 `HostDataExportFormat` 枚举值，不增加旁路 writer；
- PLY RGB 只来自最终网格点标量与冻结 scalar range/TF，不读取 renderer、actor、灯光或 framebuffer；
- OBJ 只承诺 geometry/normals，STL 只承诺 geometry，不写非标准点 RGB；
- 所有 model-to-world 必须 finite、数值可逆且满足 affine 最后一行 `[0,0,0,1]`；可逆性按 `abs(det) / (三个线性行范数乘积)` 的相对尺度判定，不能误拒绝合法的小尺度矩阵；
- 不执行 `git add/commit/push`，不删除未跟踪文件；修改文件保持 UTF-8 无 BOM 与 LF。

### 12.2 文件职责与已落地接口

| 文件 | 已落地职责 |
| --- | --- |
| `MVVCVTK/include/App/AppTypes.h` | 定义接纳时冻结的 `DataExportParams` 值对象 |
| `MVVCVTK/include/App/AppInterfaces.h`、`MVVCVTK/include/Data/DataManager.h` | 把 DataManager 导出接口从多个 positional 参数收敛为单一参数对象 |
| `MVVCVTK/include/App/Tasks/AppDataExportTaskService.h`、`MVVCVTK/src/App/Tasks/AppDataExportTaskService.cpp` | 在调用线程冻结 image/mask、iso、矩阵、range 与 TF，并按值捕获到 worker |
| `MVVCVTK/src/Data/DataManager.cpp` | 统一校验 extension/affine，生成共享三角网格，并仅为 PLY 构建/选择 `RGB` point-data 数组 |
| `test/OrthogonalCrop/AppTaskServiceTests.cpp` | 冻结快照、PLY RGB/灰阶、三格式坐标/三角几何/法线、完整/部分/错配 mask、无效 range/TF 与非法矩阵回读测试 |
| `md/HostLayer.md`、`md/MVVCVTKArchitectureReview.md`、`md/QtIntegrationGuide.md` | 同步导出契约、格式边界、PDF 缺口和 View setter 无运行时回滚事实 |
| `.graphify/` | 代码修改后的当前知识图谱与 TypeScript runtime 证明；目录仍遵循仓库 ignore/lifecycle 规则 |

精确接口为：

```cpp
struct DataExportParams final {
    std::string extension;
    double isoValue = 0.0;
    std::array<double, 16> modelToWorld = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    std::array<double, 2> scalarRange = { 0.0, 0.0 };
    std::vector<TFNode> tfNodes;
};

virtual bool ExportData(
    const ImageSnapshot& imageSnapshot,
    const std::string& outputDir,
    const DataExportParams& params) = 0;
```

### 12.3 已执行的 TDD 与实现步骤

- [x] **Step 1：先写 PLY RGB 红测。** 使用 `vtkPLYReader` 要求 active scalars 名为 `RGB`、类型为 `VTK_UNSIGNED_CHAR`、三分量且 tuple 数等于点数。
- [x] **Step 2：确认旧实现失败。** Debug x64 首次运行在 `PLY should round-trip one RGB tuple per mesh point` 失败，证明测试能捕获缺失颜色契约；首轮复审后新增的小尺度 affine 用例又先在 `data export should accept small affine scales and reject invalid transforms` 失败，证明绝对 epsilon determinant 判定存在误拒绝。
- [x] **Step 3：冻结完整参数。** `BuildDataTask` 在 worker 启动前取得 image snapshot、iso、model-to-world、scalar range 与 TF；测试在任务构造后修改共享状态，worker 仍使用旧值。
- [x] **Step 4：实现真实网格 RGB。** `BuildMeshColors` 校验 range/TF，并强制逐点读取最终输出网格现存 point scalar；缺失或 tuple 数不一致时拒绝导出，不再静默回退统一 iso。TF 为空时使用 range 灰阶，常量 range 使用白色。
- [x] **Step 5：收紧几何契约。** PLY/OBJ/STL 均从同一 FlyingEdges + mask clip + model-to-world + triangle filter 输出写出；公共入口在创建目录前拒绝非 finite、projective、数值 singular 矩阵和几何元数据不匹配的 validity mask。
- [x] **Step 6：增加格式回读。** PLY/OBJ/STL 的三角顶点使用量化后的世界坐标多重集与独立生成的预期等值面比较，不依赖 writer 的 point/cell id 顺序；PLY/OBJ 另验证单位 point normals，PLY 验证冻结 TF 与灰阶 fallback。
- [x] **Step 7：同步文档和 PDF 结论。** 文档只宣称当前已实现的 RAW/PLY/STL/OBJ；PDF 厂商资料用于产品优先级，不被误写为本项目 PDF 导出能力。
- [x] **Step 8：补齐复审边界。** 新增部分 mask 能缩减网格、错配 mask 被拒绝、逆序 range/TF 被拒绝测试；明确等值面 point scalar 数值等于冻结 iso 是当前算法不变量，因此 RGB 用例验证的是“真实 point scalar + 冻结 TF”链路，不宣称同一等值面存在任意逐点辅助色变化。

### 12.4 可复现验收命令

```powershell
& 'D:\vs2026\program\MSBuild\Current\Bin\MSBuild.exe' `
  'test\OrthogonalCrop\PlanarCropTests.vcxproj' `
  /m:1 /p:Configuration=Debug /p:Platform=x64
& 'test\OrthogonalCrop\x64\Debug\PlanarCropTests.exe'

& 'D:\vs2026\program\MSBuild\Current\Bin\MSBuild.exe' `
  'test\OrthogonalCrop\PlanarCropTests.vcxproj' `
  /m:1 /p:Configuration=Release /p:Platform=x64
& 'test\OrthogonalCrop\x64\Release\PlanarCropTests.exe'

& 'D:\vs2026\program\MSBuild\Current\Bin\MSBuild.exe' `
  'MVVCVTK\MVVCVTK.vcxproj' `
  /m:1 /p:Configuration=Debug /p:Platform=x64
& 'D:\vs2026\program\MSBuild\Current\Bin\MSBuild.exe' `
  'MVVCVTK\MVVCVTK.vcxproj' `
  /m:1 /p:Configuration=Release /p:Platform=x64

& 'D:\vs2026\program\MSBuild\Current\Bin\MSBuild.exe' `
  'test\MVVCVTK.Tests.sln' `
  /m:1 /p:Configuration=Debug /p:Platform=x64
& 'test\x64\Debug\PlanarCropTests.exe'
& 'test\x64\Debug\GapAnalysisAlgorithmTests.exe'
& 'test\Interaction\x64\Debug\InteractionRouterTests.exe'

& 'D:\vs2026\program\MSBuild\Current\Bin\MSBuild.exe' `
  'test\QtHost\QtHostMethodTests.vcxproj' `
  /m:1 /p:Configuration=Debug /p:Platform=x64
& 'test\QtHost\x64\Debug\QtHostMethodTests.exe'

npx graphify hook-rebuild
graphify portable-check .graphify
git diff --check
```

预期结果：PlanarCropTests 分别在 Debug/Release x64 构建与运行通过；主工程分别在 Debug/Release x64 构建通过；完整测试解决方案和 QtHostMethodTests 只在 Debug x64 构建与运行，所有 MSBuild 调用均为 `0 个警告 / 0 个错误`；四个 Debug 测试入口以及额外的 Release PlanarCropTests 退出码均为 `0`；graphify runtime 为 `typescript`，portable-check 返回 `Portable artifacts OK`，`git diff --check` 无内容错误。

### 12.5 三轮复审门

#### 第一轮：代码与架构契约

- [x] 核对唯一 Host/App/Task/Data 链、依赖方向和公共 Host ABI 未变；
- [x] 核对 `DataExportParams` 的值语义、worker 捕获生命周期、参数数量与命名规范；
- [x] 核对 Pimpl/Impl 边界、VTK 对象生命周期、异常与失败返回无资源泄漏；
- [x] 核对 affine/extension/TF 校验发生在写文件前，不产生已知非法输出。

#### 第二轮：数据保真、测试与失败边界

- [x] 核对 PLY RGB 确实来自最终网格 point scalar 与冻结 TF，而非显示像素；
- [x] 核对 mask、world coordinates、triangle topology、normals 与格式边界；
- [x] 核对红测证据、Debug/Release、灰阶 fallback、Unicode path 和非法矩阵覆盖；
- [x] 查找只验“文件非空”、索引顺序偶然一致、容差过宽或 writer/reader 同源自证等假阳性。

#### 第三轮：文档、计划、PDF 与交付边界

- [x] 逐项对照三个业务文档、计划、公共类型和实现，清除过时片段；
- [x] 核对 PDF 厂商页码、证据强度、P0/P1/P2 与设备控制排除边界；
- [x] 核对计划中的文件、接口、命令和实际结果一致，无 TBD/TODO/占位描述；
- [x] 核对 UTF-8/LF、ignore 状态、graphify 可移植性和未执行 add/commit/push。

### 12.6 三轮复审结论

| 轮次 | 状态 | 阻塞项 | 修正记录 |
| --- | --- | --- | --- |
| 第一轮：代码与架构契约 | 通过 | 无；初审发现 absolute epsilon 误拒绝小尺度 affine | 提取统一相对尺度 affine 判定，移除 RAW 重复绝对判定，并新增先红后绿的小尺度用例；定点复核未发现 blocking/important/minor |
| 第二轮：数据保真与测试 | 通过 | 无；初审发现 RGB 用例易被误读、拓扑比较依赖 point id 偶然顺序 | 缺失 point scalar 直接拒绝；显式验证等值面 scalar 不变量；以世界坐标三角形多重集比较；补 partial/mismatch mask 和无效 range/TF；定点复核通过 |
| 第三轮：文档与交付边界 | 通过 | 无；初审发现 `HostLayer.md` 错称干净工作树、计划未区分 Debug/Release 范围 | 改为事实基线 + 当前未提交差异；明确只有 Planar 和主工程覆盖 Release，完整套件与 Qt 覆盖 Debug；定点复核通过 |

### 12.7 已知非阻塞边界与后续升级入口

- 当前 PLY/STL/OBJ 由 VTK writer 写出、再由对应 VTK reader 回读，足以验证仓库内序列化契约，但不等同于第三方解析器互操作认证；后续可加入 MeshLab/Assimp/trimesh 等外部夹具。
- writer 当前直接写目标文件，并以 error code 与最终文件大小判定成功；写入中断时仍可能留下部分文件。后续升级应采用同目录临时文件、完整校验、原子替换，并单独设计 Windows 已存在目标文件的恢复语义。
- 当前法线测试验证 PLY/OBJ 回读后的存在性与单位长度，尚未用非均匀缩放夹具验证 inverse-transpose 后的方向；该测试应作为下一轮几何保真增强。
- 当前 RGB 业务链是体数据等值面导出；任意导入 `vtkPolyData` 的既有 RGB/RGBA 属性保留不属于本接口。若未来支持通用网格重导出，应建立独立的数据模型与属性映射契约。

### 12.8 实际验收结果

| 验收项 | 结果 |
| --- | --- |
| `PlanarCropTests` Debug x64 | 构建 `0 warning / 0 error`，测试通过 |
| `PlanarCropTests` Release x64 | 构建 `0 warning / 0 error`，测试通过 |
| `MVVCVTK.vcxproj` Debug/Release x64 | 两个配置均 `0 warning / 0 error` |
| `MVVCVTK.Tests.sln` Debug x64 | 构建 `0 warning / 0 error`；Planar、GapAnalysis、Interaction 三个入口均通过 |
| `QtHostMethodTests` Debug x64 | 构建 `0 warning / 0 error`，测试通过 |
| graphify | `1218 nodes / 1534 edges / 140 communities`；TypeScript runtime；`Portable artifacts OK` |
| 内容与交付边界 | `git diff --check` 通过；四份本轮业务/计划文档均 UTF-8 无 BOM、LF；未执行 `git add/commit/push` |
