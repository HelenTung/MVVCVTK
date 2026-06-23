/*
=====================================================================
example.cpp — 当前架构对齐的接入说明示例

用途：
- 这不是一份要直接编译运行的 demo，而是一份和 main.cpp 同步维护的“活文档”。
- 重点不是罗列所有类，而是把当前主流程、关键 Set 接口、以及为什么要这样设变量讲清楚。
- 如果 example.cpp 和 main.cpp 有冲突，以 main.cpp 的当前实现为准，再回头修正这份说明。

当前架构的核心判断：
1. SharedInteractionState 是跨窗口共享的状态真源。
2. MedicalVizService 是“窗口级业务调度层”，StdRenderContext 是“窗口级渲染/事件入口”。
3. 数据加载、导出、重载都通过 service 发起，但真正的数据只保留一份，窗口之间共享。
4. OrthogonalCropInteractionBridgeService 不直接做算法，它只负责 widget、坐标换算、preview 时机和结果分发。
5. OrthogonalCropInteractionBridgeService 先在 request 中写入 dataSource/backend，OrthogonalCropBackendRouterService 只按 request 执行。
=====================================================================
*/

/*
─────────────────────────────────────────────────────────────────────
一、先认清现在这套架构里谁在负责什么
─────────────────────────────────────────────────────────────────────

1) 共享层

    RawVolumeDataManager
    - 真正持有 vtkImageData / buffer / 导出实现。
    - 多个窗口共享同一份数据，不为每个窗口复制体数据。

    SharedStateBroadcaster + SharedInteractionState
    - SharedInteractionState 是单一状态真源，保存 TF、材质、WW/WC、cursor、modelMatrix、可见性等。
    - SharedStateBroadcaster 只负责把状态变化翻译成 UpdateFlags 广播出去。

2) 窗口层

    MedicalVizService
    - 每个窗口各一份。
    - 负责：前处理配置登记、加载入口、导出入口、交互状态写回、主线程后处理。

    StdRenderContext
    - 每个窗口各一份。
    - 负责：VTK renderer/renderWindow/interactor 生命周期、事件接入、Timer 驱动、工具模式切换。

3) 裁切层

    OrthogonalCropInteractionBridgeService
    - 交互桥。
    - 负责：widget、world / active input model 坐标换算、preview 刷新时机、结果分发到多个窗口。

    OrthogonalCropBackendRouterService
    - 数据后端路由。
    - 负责：按 OrthogonalCropRequest 已指定的 dataSource/backend 校验输入并分发到算法层。

    OrthogonalCropAlgorithm
    - 纯算法层。
    - 负责：request 归一化、bounds 校验、voxel snapped、image preview、polydata preview 和 image submit 结果构造。
*/

/*
─────────────────────────────────────────────────────────────────────
二、当前 main.cpp 的真实调用流程
─────────────────────────────────────────────────────────────────────

下面这条顺序就是当前主程序的骨架，不要再按旧版“先建窗口再边走边改状态”的方式理解。

Step 1: 创建共享资源

    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedStateBroadcaster = std::make_shared<SharedStateBroadcaster>();
    auto sharedState = std::make_shared<SharedInteractionState>(sharedStateBroadcaster);
    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);
    auto gapAnalysis = std::make_shared<GapAnalysisService>(sharedDataMgr);
    auto orthogonalCropBridge = std::make_shared<OrthogonalCropInteractionBridgeService>();

为什么要先建这些：
- DataManager 和 SharedState 是所有窗口共享的，属于应用级单例资源，而不是窗口级资源。
- imageAnalysis / gapAnalysis 依赖同一个 DataManager，说明它们消费的是同一份体数据快照。
- orthogonalCropBridge 也只有一份，因为裁切模式本质上是“应用当前数据上的一个交互工具”，不是每个窗口各自独立一套。

Step 2: 先声明 WindowConfig，而不是立刻操作 VTK 对象

    WindowConfig cfgA;
    WindowConfig cfgB;
    WindowConfig cfgC;
    WindowConfig cfgD;
    WindowConfig cfgE;

为什么：
- WindowConfig/PreInitConfig 是纯数据，不碰 VTK，适合在真正建窗前先把意图说清楚。
- 这样可以保证“前处理配置”和“窗口生命周期”解耦。
- 尤其是 vizMode / TF / material / WW/WC 这类配置，本质上是状态，不应该写死在建窗细节里。

Step 3: 通过 GetWindowPair 批量建窗

    auto [serviceA, contextA] = GetWindowPair(cfgA, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceE, contextE] = GetWindowPair(cfgE, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceB, contextB] = GetWindowPair(cfgB, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceC, contextC] = GetWindowPair(cfgC, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceD, contextD] = GetWindowPair(cfgD, sharedDataMgr, sharedState, sharedStateBroadcaster);

为什么按这个阶段建窗：
- 到这一步，每个窗口都得到一对固定组合：MedicalVizService + StdRenderContext。
- service 是业务面，context 是渲染/交互面；后面所有 Set 基本都围绕这两个对象展开。

Step 4: 绑定 OrthogonalCrop 的坐标参考和 preview 目标

    orthogonalCropBridge->SetDataManager(sharedDataMgr);
    orthogonalCropBridge->SetReferenceRenderService(serviceA);
    orthogonalCropBridge->SetReferenceRenderer(contextA->GetRenderer());
    orthogonalCropBridge->SetPreviewRenderServices({ serviceA, serviceB, serviceC, serviceD, serviceE });
    auto orthogonalCropSubmitWorkflow = std::make_shared<OrthogonalCropSubmitWorkflow>(
        orthogonalCropBridge,
        reloadSubmitter,
        sharedDataMgr);

为什么这么设：
- SetDataManager：只作为缺 image 输入时的兜底来源，不是主输入真源。
- SetReferenceRenderService(serviceA)：A 是当前 3D 等值面主参考窗口，裁切盒的 world / active input model 坐标转换都以它为准。
- SetReferenceRenderer(contextA->GetRenderer())：只把参考窗口 renderer 作为算法内部相机快照来源，不把相机状态写进 Service 或 SharedState。
- SetPreviewRenderServices(...)：谁要跟着 overlay 刷新，就放进这个列表。当前 main 把 5 个窗口都放进来，意味着 2D 和 3D 都参与联动。
- OrthogonalCropSubmitWorkflow：统一协调 submit payload、主数据 reload、reload 完成后的 bridge 收尾；Workflow 只接收 reload 能力函数和数据管理接口，不直接依赖 MedicalVizService，也不修改 iso threshold 等视觉参数。

Step 5: 再做窗口级辅助元素策略

    serviceA->SetElementVisible(VisFlags::Planes3D, false);
    serviceE->SetElementVisible(VisFlags::Planes3D, false);
    serviceA->SetElementVisible(VisFlags::Ruler, false);
    serviceE->SetElementVisible(VisFlags::Ruler, false);
    serviceB->SetElementVisible(VisFlags::Crosshair, true);
    serviceC->SetElementVisible(VisFlags::Crosshair, true);
    serviceD->SetElementVisible(VisFlags::Crosshair, true);

为什么这些不放进 WindowConfig：
- 这些不是“窗口壳子”的固定属性，而是运行期可变的渲染元素策略。
- 它们最后会进入 SharedState 的 visibilityMask，并通过 Strategy 同步到具体渲染对象。
- 所以这类设置属于 service 级状态，而不是 context 级窗口参数。

Step 6: 只用一个 service 发起加载

    serviceA->LoadFileAsync(..., [sharedState, sharedDataMgr, serviceA, imageAnalysis, orthogonalCropBridge](bool success) {
        ...
    });

为什么由 A 来发起：
- 任何一个 service 理论上都能发起，因为底层 DataManager/SharedState 是共享的。
- 当前选择 A，是因为加载成功后会立刻推导 iso 值，并把裁切工具绑定到主 3D 参考窗口；A 最接近这条主线。

Step 7: 在加载成功回调里做“数据相关”的后处理

    auto range = sharedState->GetDataRange();
    double isoVal = range[0] + (range[1] - range[0]) * 0.55;
    serviceA->SetIsoThreshold(isoVal);

    auto histogramTable = imageAnalysis->GetHistogramData(2048);
    ...

    orthogonalCropBridge->SetInputImage(sharedDataMgr->GetVtkImage());

为什么这些动作必须等加载成功后再做：
- Iso 阈值依赖实际数据 range，加载前没有意义。
- Histogram 也必须消费已经就绪的 vtkImageData。
- OrthogonalCrop 的 image 输入只有在 DataManager 真正持有有效 image 后才能绑定，否则 bridge/routing 里看到的是空输入。

非常重要：
- LoadFileAsync / ReloadFromBufferAsync / SaveTransformedDataAsync 的业务回调，当前实现是“主线程延迟回调”，不是后台线程直接回调。
- 也就是说，回调执行时，ProcessPendingUpdates 已经先把 DataReady / LoadFailed / pipeline rebuild 收敛过一轮了。

Step 8: 全窗口先 Render，再 Initialize interactor

    contextA->Render();
    contextB->Render();
    contextC->Render();
    contextD->Render();
    contextE->Render();

    contextA->InitializeInteractor();
    contextB->InitializeInteractor();
    contextC->InitializeInteractor();
    contextD->InitializeInteractor();
    contextE->InitializeInteractor();

为什么分成两步：
- Render() 先把初始 renderer/window 内容打出来，避免第一次交互前窗口是半初始化状态。
- InitializeInteractor() 再去初始化 interactor 和 repeating timer，Timer 才能开始驱动主线程后处理链路。

Step 9: 明确谁给裁切 widget 提供 interactor

    orthogonalCropBridge->SetPrimaryInteractor(contextA->GetInteractor());

为什么一定是 A：
- 当前 widget 只挂一处 interactor，不能同时挂在 5 个窗口上。
- A 是主 3D 参考窗口，最适合作为 O/1/2/3 裁切热键与 box widget 的宿主。

Step 10: 给所有窗口挂同一份裁切热键观察器

    contextA->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, orthogonalCropHotkeyObserver);
    ...
    contextE->GetInteractor()->AddObserver(vtkCommand::CharEvent, orthogonalCropHotkeyObserver, 1.0f);

为什么既挂 KeyPress/KeyRelease，又挂 CharEvent：
- KeyPress/KeyRelease 负责我们自己的动作语义和按键防抖。
- CharEvent 必须额外拦截，因为 VTK 默认交互样式还会在 CharEvent 里处理内建快捷键；
  当前 `3` 如果不拦，会落到 stereo 默认逻辑上，触发不必要的 VTK warning。

Step 11: 只让一个窗口进入 Start()

    contextB->Start();

为什么不是所有窗口都 Start：
- 一个应用只需要一个主消息循环。
- 当前 main 选择 B 作为持有主事件循环的窗口，这只是应用层选择，不代表 B 比其他窗口更“核心”。
- 真正关键的是：在 Start() 之前，所有窗口都已经完成 Render() 和 InitializeInteractor()。
*/

/*
─────────────────────────────────────────────────────────────────────
三、当前架构下的最小可复制骨架
─────────────────────────────────────────────────────────────────────

下面这段示意代码只保留当前 main 的主干，不再写旧版分支。

    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedStateBroadcaster = std::make_shared<SharedStateBroadcaster>();
    auto sharedState = std::make_shared<SharedInteractionState>(sharedStateBroadcaster);
    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);
    auto gapAnalysis = std::make_shared<GapAnalysisService>(sharedDataMgr);
    auto orthogonalCropBridge = std::make_shared<OrthogonalCropInteractionBridgeService>();

    std::vector<TFNode> volTF = {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };

    WindowConfig cfgA;
    cfgA.title = "Window A: Composite IsoSurface";
    cfgA.width = 600;
    cfgA.height = 600;
    cfgA.posX = 50;
    cfgA.posY = 50;
    cfgA.showAxes = true;
    cfgA.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    cfgA.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 0.4, false };
    cfgA.preInitCfg.bgColor = { 0.05, 0.05, 0.05 };
    cfgA.preInitCfg.hasBgColor = true;

    WindowConfig cfgE;
    cfgE.title = "Window E: Composite Volume";
    cfgE.width = 600;
    cfgE.height = 600;
    cfgE.posX = 660;
    cfgE.posY = 50;
    cfgE.preInitCfg.vizMode = VizMode::CompositeVolume;
    cfgE.preInitCfg.tfNodes = volTF;
    cfgE.preInitCfg.hasTF = true;
    cfgE.preInitCfg.bgColor = { 0.08, 0.08, 0.12 };
    cfgE.preInitCfg.hasBgColor = true;

    WindowConfig cfgB;
    cfgB.title = "Window B: Top_down Slice";
    cfgB.width = 400;
    cfgB.height = 400;
    cfgB.posX = 50;
    cfgB.posY = 660;
    cfgB.preInitCfg.vizMode = VizMode::SliceTop_down;
    cfgB.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgB.preInitCfg.hasBgColor = true;
    cfgB.preInitCfg.windowLevel = { 400.0, 40.0 };
    cfgB.preInitCfg.hasWindowLevel = true;

    WindowConfig cfgC = cfgB;
    cfgC.title = "Window C: Front_back Slice";
    cfgC.posX = 460;
    cfgC.preInitCfg.vizMode = VizMode::SliceFront_back;

    WindowConfig cfgD = cfgB;
    cfgD.title = "Window D: Left_right Slice";
    cfgD.posX = 870;
    cfgD.preInitCfg.vizMode = VizMode::SliceLeft_right;

    auto [serviceA, contextA] = GetWindowPair(cfgA, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceE, contextE] = GetWindowPair(cfgE, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceB, contextB] = GetWindowPair(cfgB, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceC, contextC] = GetWindowPair(cfgC, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceD, contextD] = GetWindowPair(cfgD, sharedDataMgr, sharedState, sharedStateBroadcaster);

    orthogonalCropBridge->SetDataManager(sharedDataMgr);
    orthogonalCropBridge->SetReferenceRenderService(serviceA);
    orthogonalCropBridge->SetPreviewRenderServices({ serviceA, serviceB, serviceC, serviceD, serviceE });
    auto orthogonalCropSubmitWorkflow = std::make_shared<OrthogonalCropSubmitWorkflow>(
        orthogonalCropBridge,
        reloadSubmitter,
        sharedDataMgr);

    serviceA->SetElementVisible(VisFlags::Planes3D, false);
    serviceE->SetElementVisible(VisFlags::Planes3D, false);
    serviceA->SetElementVisible(VisFlags::Ruler, false);
    serviceE->SetElementVisible(VisFlags::Ruler, false);
    serviceB->SetElementVisible(VisFlags::Crosshair, true);
    serviceC->SetElementVisible(VisFlags::Crosshair, true);
    serviceD->SetElementVisible(VisFlags::Crosshair, true);

    serviceA->LoadFileAsync(
        "E:/data/1000x1000x1000.raw",
        { 0.02125f, 0.02125f, 0.02125f },
        { 0.0f, 0.0f, 0.0f },
        [sharedState, sharedDataMgr, serviceA, imageAnalysis, orthogonalCropBridge](bool success)
        {
            if (!success) {
                std::cerr << "[onComplete] Volume data load failed.\n";
                return;
            }

            const auto range = sharedState->GetDataRange();
            const double isoVal = range[0] + (range[1] - range[0]) * 0.55;
            serviceA->SetIsoThreshold(isoVal);

            const int histogramBinCount = 2048;
            auto histogramTable = imageAnalysis->GetHistogramData(histogramBinCount);
            if (histogramTable && histogramTable->GetNumberOfRows() > 0) {
                imageAnalysis->SaveHistogramImage("E:/data/ct/histogram.png", histogramBinCount);
            }

            orthogonalCropBridge->SetInputImage(sharedDataMgr->GetVtkImage());
        });

    contextA->Render();
    contextB->Render();
    contextC->Render();
    contextD->Render();
    contextE->Render();

    contextA->InitializeInteractor();
    contextB->InitializeInteractor();
    contextC->InitializeInteractor();
    contextD->InitializeInteractor();
    contextE->InitializeInteractor();

    orthogonalCropBridge->SetPrimaryInteractor(contextA->GetInteractor());

    // 这里挂同一份热键观察器到五个 interactor；
    // KeyPress/KeyRelease 处理业务动作，CharEvent 负责拦截 VTK 默认快捷键。

    contextB->Start();

最关键的时序不要改：
1. 先 Render。
2. 再 InitializeInteractor。
3. 再 SetPrimaryInteractor。
4. 最后只让一个 context 调 Start。
*/

/*
─────────────────────────────────────────────────────────────────────
四、GetWindowPair 里每个 Set 接口到底在做什么
─────────────────────────────────────────────────────────────────────

当前 main 的建窗 helper 等价于：

    static std::pair<std::shared_ptr<MedicalVizService>, std::shared_ptr<StdRenderContext>>
    GetWindowPair(const WindowConfig& cfg,
                  std::shared_ptr<AbstractDataManager> dataMgr,
                  std::shared_ptr<SharedInteractionState> sharedState,
                  std::shared_ptr<IStateEventSource> stateEventSource)
    {
        auto service = std::make_shared<MedicalVizService>(dataMgr, sharedState, stateEventSource);
        auto context = std::make_shared<StdRenderContext>();

        context->SetServiceBound(service);
        service->SetVisualConfig(cfg.preInitCfg);
        context->SetWindowTitle(cfg.title);
        context->SetWindowSize(cfg.width, cfg.height);
        context->SetWindowPosition(cfg.posX, cfg.posY);
        context->ApplyCameraStyle(cfg.preInitCfg.vizMode);
        if (cfg.showAxes)
            context->SetOrientationAxesVisible(true);
        if (cfg.preInitCfg.hasBgColor)
            service->SetBackground(cfg.preInitCfg.bgColor);

        return { service, context };
    }

逐条解释：

1) context->SetServiceBound(service)
- 作用：把 renderWindow / renderer 交给 service，并注册状态事件观察链。
- 为什么必须最早做：后面的 service 配置要有合法 renderer/renderWindow 归属，观察回调也要在这里接入。

2) service->SetVisualConfig(cfg.preInitCfg)
- 作用：批量写入 PreInitConfig，对 SharedState 做一次性状态登记。
- 为什么放在建窗阶段：这些配置与数据内容无关，属于“先说明应该怎么显示”。
- 为什么是批量接口：减少多次加锁和多次 UpdateFlags 广播。

3) context->SetWindowTitle / SetWindowSize / SetWindowPosition
- 作用：设置窗口壳属性，只影响 renderWindow 外壳，不参与 SharedState。
- 为什么走 context：这是渲染上下文的职责，不是业务状态。

4) context->ApplyCameraStyle(cfg.preInitCfg.vizMode)
- 作用：根据 VizMode 选择 2D image style 或 3D trackball camera style。
- 为什么这里做：交互风格要和窗口目标模式同步，但这仍然是 context 层职责，不应该放进 service 的状态同步里。

5) context->SetOrientationAxesVisible(true)
- 作用：只控制方向轴 widget 的启停。
- 为什么不是全局配置：这是窗口装饰级能力，不是共享渲染状态。

6) service->SetBackground(cfg.preInitCfg.bgColor)
- 作用：显式写背景色到 SharedState。
- 说明：如果 `cfg.preInitCfg.hasBgColor = true`，`SetVisualConfig` 本身已经会把 bgColor 写进去；
  当前 main 额外再写一次，是为了在调用点上把“背景色属于窗口前处理显示意图”表达得更直白。
- 结论：这行在语义上更接近“显式重复说明”，不是另一个不可缺少的阶段。
*/

/*
─────────────────────────────────────────────────────────────────────
五、为什么 PreInitConfig 里有一堆 hasXxx 标志位
─────────────────────────────────────────────────────────────────────

当前 PreInitConfig 不是“所有字段都必须有值”的配置对象，而是“可部分提交的前处理快照”。

    struct PreInitConfig {
        VizMode vizMode;
        MaterialParams material;
        std::vector<TFNode> tfNodes;
        double isoThreshold;
        BackgroundColor bgColor;
        std::array<double, 3> spacing;
        WindowLevelParams windowLevel;
        bool hasTF;
        bool hasIso;
        bool hasBgColor;
        bool hasSpacing;
        bool hasWindowLevel;
    };

这些 hasXxx 的原因必须明确：

1) 默认值本身可能是合法业务值
- 例如 bgColor = {0,0,0} 可能就是想要纯黑，而不是“没设置背景”。
- 例如 isoThreshold = 0.0 也可能就是合法阈值，而不是“没有设置”。

2) SetVisualConfig 需要支持“只覆盖一部分配置”
- 如果没有 hasXxx，就分不清当前是“想写默认值”，还是“根本不想覆盖这个字段”。

3) AppState::SetPreInitConfig 需要精确生成 UpdateFlags
- 只有 hasXxx 为 true 时，才应该尝试比较并触发 TF / Background / Spacing / WindowLevel 等变更位。

一句话概括：
- hasXxx 不是啰嗦，而是在避免“合法零值”和“未设置”混淆。
*/

/*
─────────────────────────────────────────────────────────────────────
六、MedicalVizService 常用 Set 接口说明
─────────────────────────────────────────────────────────────────────

这一节不只说“叫什么”，而是说“写到哪里、什么时候生效、为什么这样设计”。

1) SetVizMode(VizMode mode)
- 做什么：只写 `m_pendingVizModeInt`，不立即重建 Strategy。
- 为什么：模式切换是结构性变化，真正重建要等主线程 `ProcessPendingUpdates -> RebuildPipeline`。
- 否则如果在任意线程或任意回调里直接切 Strategy，会破坏 VTK 渲染线程边界。

2) SetVisualConfig(const PreInitConfig& cfg)
- 做什么：把 cfg 一次性提交到 SharedState。
- 为什么：前处理配置的本质是“显示意图”，不应该在这里直接碰 mapper/actor。

3) SetMaterial / SetOpacity / SetTransferFunction / SetIsoThreshold / SetBackground / SetSpacing / SetWindowLevel
- 做什么：全都只改 SharedState。
- 什么时候真正落到渲染对象：下一次 Timer 心跳，`ProcessPendingUpdates -> SyncStrategyState`。
- 为什么：保持“状态写回”和“VTK 渲染对象更新”分离，避免任意线程直接碰渲染对象。

4) LoadFileAsync(path, spacing, origin, onComplete)
- 做什么：后台线程做 I/O 和数据准备；成功后通过 SharedState 发布 DataReady；onComplete 在主线程延迟执行。
- 为什么：加载要异步，VTK 管线重建要主线程，二者不能混在同一层里直接做。

5) ReloadFromBufferAsync(...)
- 做什么：后台线程准备待提交镜像，真正消费要等主线程 `ProcessPendingUpdates`。
- 为什么：重载数据和文件流加载一样，也必须遵守主线程收敛策略。

6) SetElementVisible(flagBit, show)
- 做什么：改 SharedState 里的 visibilityMask。
- 为什么：Planes3D / Crosshair / Ruler 是策略级显示元素，不是窗口壳属性。

7) SetInteracting(bool val)
- 做什么：写 SharedState 的交互态。
- 为什么：Strategy 可以用这个状态切换轻量渲染参数，renderWindow 也会跟着调整 desired update rate。

8) ScrollSlice(delta)
- 做什么：根据当前 VizMode 计算当前应推进哪个切片轴，然后更新 cursor。
- 为什么：切片滚动本质上是“状态计算 + cursor 写回”，不是直接命令 slice actor 自己跳。

9) SetCursorWorldPosition(worldPos, axis)
- 做什么：更新共享 cursor；axis = -1 表示直接写三维点，否则只在非导航轴上跟随更新。
- 为什么：2D/3D 联动都要依赖同一份 cursor 真源。

10) AdjustWindowLevel(totalDx, totalDy, viewWidth, viewHeight, startWW, startWC)
- 做什么：把拖拽增量折算成新的 WW/WC，再写回 SharedState。
- 为什么：把交互增量转业务值的逻辑固定在服务层，不让 UI 侧重复算。

11) SetModelTransform / SetModelTransformReset / SyncModelMatrix
- SetModelTransform：基于当前矩阵叠加一轮 TRS。
- SetModelTransformReset：重置成单位矩阵。
- SyncModelMatrix：把 TrackballActor 拖拽出的 VTK 矩阵回写到 SharedState。
- 为什么：模型矩阵必须始终只有 SharedState 这一份真源，world / model 互转都以它为准。

12) SaveTransformedDataAsync / SaveSliceImagesAsync
- 做什么：后台导出，主线程延迟回调结果。
- 为什么：导出和渲染都可能重，必须异步；回调必须回主线程，避免上层 UI 状态错线程。
*/

/*
─────────────────────────────────────────────────────────────────────
七、StdRenderContext 常用 Set 接口说明
─────────────────────────────────────────────────────────────────────

1) SetServiceBound(service)
- 做什么：把 renderer/renderWindow 绑定给 service。
- 为什么：service 后续所有 Strategy 附着和状态同步，都以这对 VTK 对象为落点。

2) ApplyCameraStyle(mode)
- 做什么：2D 窗口用 vtkInteractorStyleImage，3D 窗口用 vtkInteractorStyleTrackballCamera。
- 为什么：相机交互风格属于 context，不属于业务状态。

3) Render()
- 做什么：触发一次初始 Render。
- 为什么：先把窗口内容打出来，再进入更复杂的 interactor/timer 阶段。

4) InitializeInteractor()
- 做什么：Initialize interactor，创建 repeating timer，挂 Timer observer。
- 为什么：Timer 是当前主线程后处理链的心跳源。

5) Start()
- 做什么：先 Render，再 Start interactor 主循环。
- 为什么：应用只需要一个真正的消息循环宿主。

6) SetToolMode(mode)
- 做什么：在 Navigation 和 ModelTransform 之间切换交互风格。
- 为什么：ModelTransform 会改成 vtkInteractorStyleTrackballActor，并允许 main prop 被 pick。
*/

/*
─────────────────────────────────────────────────────────────────────
八、OrthogonalCrop 相关 Set 接口说明
─────────────────────────────────────────────────────────────────────

1) OrthogonalCropBackendRouterService::SetInputImage / SetInputPolyData
- 做什么：绑定当前后端输入。
- 为什么：router 必须显式知道自己在裁谁，不能从窗口状态里猜。

2) OrthogonalCropBackendRouterService::SetPreferredDataSource
- 做什么：设置优先走 image / polydata。
- 为什么：UI 层不该自己散落 if/else；由 router 统一决策数据后端。

3) OrthogonalCropInteractionBridgeService::SetDataManager
- 做什么：为缺 image 输入时准备兜底来源。
- 为什么：bridge 进入交互时如果还没显式 SetInputImage，可以尽力从 DataManager 拿当前体数据。

4) OrthogonalCropInteractionBridgeService::SetReferenceRenderService
- 做什么：提供 world 和 active input model 坐标互转基准。
- 为什么：widget 在 world 坐标里拖，算法输入使用 active input model 坐标，必须有一份权威变换参考。

5) OrthogonalCropInteractionBridgeService::SetPreviewRenderServices
- 做什么：决定哪些窗口接收 overlay 和设脏刷新。
- 为什么：坐标参考和结果分发是两种不同职责，不能混成一个“主窗口服务”。

6) OrthogonalCropInteractionBridgeService::SetPrimaryInteractor
- 做什么：指定 vtkBoxWidget2 只挂在哪一个 interactor 上。
- 为什么：widget 只能有一个宿主 interactor；当前架构用 A 作为 3D 主交互窗口。

7) OrthogonalCropInteractionBridgeService::SetInputImage
- 做什么：把加载成功后的 vtkImageData 真正注入裁切后端。
- 为什么：裁切输入必须等数据成功加载后才能绑定，不能在启动阶段盲设空 image。

8) ToggleInteractiveCrop / ExitInteractiveCrop / TogglePreview / ApplySubmit
- 做什么：这些是 bridge 暴露给 UI/热键层的动作接口。
- 为什么：bridge 只认动作，不认具体键位；键盘映射应放在 main.cpp 观察器，而不是塞进 bridge 内部。
*/

/*
─────────────────────────────────────────────────────────────────────
九、为什么 OrthogonalCrop preview 要这样设变量
─────────────────────────────────────────────────────────────────────

1) 为什么 preview request 固定走 boxToInputModelMatrix
- 因为 widget 盒子是在 world 坐标中拖出来的。
- 直接把 world min/max 当成算法 input model bounds，会在模型发生旋转/平移后失真。
- 所以 bridge 会把 widget 有向盒归一化成：标准盒 [-1,1]^3 + boxToInputModelMatrix。

2) 为什么 referenceRenderService 和 previewRenderServices 必须分开
- referenceRenderService 只负责坐标系问题。
- previewRenderServices 只负责谁跟着刷新。
- 如果把两者混成一个字段，后面一旦要“用 A 当坐标参考，但让 A/B/C/D/E 一起刷新”，语义就会打架。

3) 为什么 primary interactor 是 A，但 Start() 是 B
- primary interactor 决定 widget 挂在哪。
- Start() 决定谁持有应用主消息循环。
- 这两件事不是同一个概念，所以当前 main 才会出现“A 持有裁切 widget，B 持有主事件循环”的组合。

4) 为什么 preview request 必须写入 dataSource 和 backend
- 因为 bridge 才知道这次交互面对的是 image 还是 polydata，也知道 UI 动作是 preview 还是 submit。
- router 只应该执行 request 已经说明的后端目标，不能再根据 active source 和枚举组合自行猜测。
- 所以 preview 固定写成 ImageData + MaskPreview 或 PolyData + ClipPreview；submit 固定写成 ImageData + SubmitExtractVOI。

5) 为什么拖拽中不实时重算 preview，只在 Released 或显式 toggle 时重算
- 因为重型 preview 本身可能包含 mask 生成、polydata clip、overlay 同步。
- 如果把鼠标移动频率直接映射为结果重算频率，会明显卡顿。
- 所以当前桥接层只在拖拽中更新 bounds/phase，松手后才真正刷新结果。
*/

/*
─────────────────────────────────────────────────────────────────────
十、OrthogonalCrop 当前完整调用链
─────────────────────────────────────────────────────────────────────

从用户视角看是按 O/1/2/Ctrl+3，底层实际上是下面这条链：

1. O 键
- main.cpp 的热键观察器调用 bridge->ToggleInteractiveCrop()。
- bridge 激活 widget，设置默认 bounds，但此时还不自动开启 preview。

2. 拖拽 widget
- OrthogonalCropWidgetStateController 只把 VTK 事件翻译成 bounds + phase。
- bridge 记录 m_currentWorldBounds 和 m_lastInteractionPhase。

3. 按 1 或 2
- bridge->TogglePreview(CropRemovalMode::KeepInside / RemoveInside, true)。
- bridge 组装 preview request，并写入当前 dataSource 与 preview backend。

4. BuildPreviewRequest()
- 从 router->GetDefaultRequest() 开始。
- 覆盖为当前 widget world 有向盒对应的 boxToInputModelMatrix。
- 写入当前 removalMode。
- 写入当前 dataSource：ImageData 或 PolyData。
- 写入当前 backend：ImageData 对应 MaskPreview，PolyData 对应 ClipPreview。
- 写入 cropStateModel。

5. UpdatePreviewFromCurrentBounds()
- bridge 先按 request 构造 resultContext，固定本次结果的数据源、后端和交互态。
- 统一调用 router->GetResult(previewRequest, resultContext)。
- router 读取 request.dataSource 和 request.backend，只校验输入并分发到 OrthogonalCropAlgorithm。
- algorithm 按 ImageData + MaskPreview 生成 image mask，按 PolyData + ClipPreview 生成 polydata clip。

6. DispatchPreviewResult(previewResult)
- 2D 窗口消费 overlay mask/outline。
- 3D 主窗口先尝试主显示管道 preview；volume 只接管 KeepInside，actor/polydata 走 clip preview。

7. 按 Ctrl+3
- orthogonalCropSubmitWorkflow->ApplySubmit()。
- 校验当前是否为 KeepInside、非 dragging、widget 已激活，并且当前 active source 是 ImageData。
- OrthogonalCropCameraStateController 先保存 reference renderer 当前相机快照；同一算法内只保留最近一次，下一次保存会覆盖。
- Bridge 的 BuildSubmitReloadPayload() 把当前 widget 有向盒改成 ImageData + SubmitExtractVOI request，并生成 reload buffer / origin 翻转后的 payload。
- Workflow 持有 reload buffer 生命周期，调用 serviceA->ReloadFromBufferAsync(...) 把 image submit image 提交回主数据通道。
- 主线程后处理在 RebuildPipeline -> SyncStrategyState 之后执行 reload 回调。
- Workflow 收到 reload 完成回调后，通知 bridge 恢复相机、清理 submit 状态并关闭裁切。

注意：image submit 入口必须与 preview 日志/刷新开关分离，不能再用 bool logStats=false 这类标志位隐式表示提交模式。
*/

/*
─────────────────────────────────────────────────────────────────────
十一、OrthogonalCrop 独立后端最小调用示例
─────────────────────────────────────────────────────────────────────

如果你暂时不接 widget，只想直接调后端，最小骨架是：

    auto cropBackend = std::make_shared<OrthogonalCropBackendRouterService>();
    cropBackend->SetInputImage(sharedDataMgr->GetVtkImage());
    cropBackend->SetPreferredDataSource(OrthogonalCropDataSource::ImageData);

    auto request = cropBackend->GetDefaultRequest();
    request.SetBoxToInputModelMatrixFromBounds({
        8.0, 16.0,
        10.0, 20.0,
        12.0, 24.0
    });
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetDataSource(OrthogonalCropDataSource::ImageData);
    request.SetBackend(OrthogonalCropBackend::MaskPreview);

    CropStateModel cropState;
    cropState.SetCropEnabled(true);
    cropState.SetInsideOpacity(0.20);
    cropState.SetOutsideVisibility(false);
    cropState.SetInteractionPhase(CropInteractionPhase::Released);
    request.SetCropStateModel(cropState);

    auto resultContext = OrthogonalCropResult();
    resultContext.SetResolvedDataSource(request.GetDataSource());
    resultContext.SetResolvedBackend(request.GetBackend());
    resultContext.SetCropStateModel(request.GetCropStateModel());

    auto stats = cropBackend->GetStatistics(request);
    if (stats.GetFailureReason() != OrthogonalCropFailureReason::None) {
        std::cerr << stats.GetValidationMessage() << std::endl;
        return;
    }

    auto result = cropBackend->GetResult(request, resultContext);
    if (!result.GetSucceeded()) {
        std::cerr << result.GetMessage() << std::endl;
        return;
    }

    auto maskPreview = result.GetMaskImage();
    auto outline = result.GetOutlinePolyData();

如果改走主数据 submit：

    auto hardRequest = request;
    hardRequest.SetDataSource(OrthogonalCropDataSource::ImageData);
    hardRequest.SetBackend(OrthogonalCropBackend::SubmitExtractVOI);
    auto hardResultContext = resultContext;
    hardResultContext.SetResolvedDataSource(hardRequest.GetDataSource());
    hardResultContext.SetResolvedBackend(hardRequest.GetBackend());
    auto hardStats = cropBackend->GetStatistics(hardRequest);
    if (hardStats.GetFailureReason() != OrthogonalCropFailureReason::None) {
        std::cerr << hardStats.GetValidationMessage() << std::endl;
        return;
    }

    auto hardResult = cropBackend->GetResult(hardRequest, hardResultContext);
    auto submitImage = hardResult.GetSubmitImage();

如果改走 polydata：

    cropBackend->SetInputPolyData(inputSurface);
    cropBackend->SetPreferredDataSource(OrthogonalCropDataSource::PolyData);

    auto polyRequest = cropBackend->GetDefaultRequest();
    polyRequest.SetBoxToInputModelMatrixFromBounds({ 10.0, 40.0, 5.0, 30.0, 8.0, 22.0 });
    polyRequest.SetDataSource(OrthogonalCropDataSource::PolyData);
    polyRequest.SetBackend(OrthogonalCropBackend::ClipPreview);

    auto polyResultContext = OrthogonalCropResult();
    polyResultContext.SetResolvedDataSource(polyRequest.GetDataSource());
    polyResultContext.SetResolvedBackend(polyRequest.GetBackend());
    polyResultContext.SetCropStateModel(polyRequest.GetCropStateModel());

    auto polyResult = cropBackend->GetResult(polyRequest, polyResultContext);
    auto clippedSurface = polyResult.GetClipPolyData();
*/

/*
─────────────────────────────────────────────────────────────────────
十二、最后记住这几条，不容易用错
─────────────────────────────────────────────────────────────────────

1. SharedState 和 DataManager 是应用级共享资源，不要给每个窗口各建一份。
2. MedicalVizService 和 StdRenderContext 是窗口级对象，一窗一对。
3. 所有 SetXxx 大多先改 SharedState，再由 Timer 心跳把状态同步到 Strategy/VTK 对象。
4. 回调里能安全假设“主线程状态已经收敛过一轮”，但不要因此绕过 service/context 直接改底层 VTK 管线。
5. OrthogonalCrop bridge 负责交互，不负责算法；router 负责路由，不负责键位；algorithm 负责结果，不负责窗口。
6. preview / submit 的业务目标写在 request.dataSource 和 request.backend；router 只执行目标，不再自行猜后端。
*/
