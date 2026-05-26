/*
=====================================================================
example.cpp — 精简接入示例（对齐当前 main.cpp）

用途：
- 只保留当前项目最常用、最接近实际可复制调用的骨架。
- 以 main.cpp 当前做法为准，不再保留 UML、模式说明、长篇职责文档。

当前主线：
1. 共享一份 DataManager
2. 共享一份 SharedStateBroadcaster + SharedInteractionState
3. 每个窗口各自持有 MedicalVizService + StdRenderContext
4. 由一个主窗口发起 SetFileLoadedAsync
5. 所有窗口 SetRendered / SetInteractorInitialized
6. 由一个窗口进入 SetStarted()
=====================================================================
*/

/*
─────────────────────────────────────────────────────────────────────
一、最小可复制骨架
─────────────────────────────────────────────────────────────────────

    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedStateBroadcaster = std::make_shared<SharedStateBroadcaster>();
    auto sharedState = std::make_shared<SharedInteractionState>(sharedStateBroadcaster);
    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);

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
    cfgE.preInitCfg.tfNodes = {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };
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

    WindowConfig cfgC;
    cfgC.title = "Window C: Front_back Slice";
    cfgC.width = 400;
    cfgC.height = 400;
    cfgC.posX = 460;
    cfgC.posY = 660;
    cfgC.preInitCfg.vizMode = VizMode::SliceFront_back;
    cfgC.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgC.preInitCfg.hasBgColor = true;
    cfgC.preInitCfg.windowLevel = { 400.0, 40.0 };
    cfgC.preInitCfg.hasWindowLevel = true;

    WindowConfig cfgD;
    cfgD.title = "Window D: Left_right Slice";
    cfgD.width = 400;
    cfgD.height = 400;
    cfgD.posX = 870;
    cfgD.posY = 660;
    cfgD.preInitCfg.vizMode = VizMode::SliceLeft_right;
    cfgD.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgD.preInitCfg.hasBgColor = true;
    cfgD.preInitCfg.windowLevel = { 400.0, 40.0 };
    cfgD.preInitCfg.hasWindowLevel = true;

    auto [serviceA, contextA] = GetWindowPair(cfgA, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceE, contextE] = GetWindowPair(cfgE, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceB, contextB] = GetWindowPair(cfgB, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceC, contextC] = GetWindowPair(cfgC, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceD, contextD] = GetWindowPair(cfgD, sharedDataMgr, sharedState, sharedStateBroadcaster);

    serviceA->SetElementVisible(VisFlags::Planes3D, true);
    serviceE->SetElementVisible(VisFlags::Planes3D, true);
    serviceA->SetElementVisible(VisFlags::Ruler, false);
    serviceE->SetElementVisible(VisFlags::Ruler, false);
    serviceB->SetElementVisible(VisFlags::Crosshair, true);
    serviceC->SetElementVisible(VisFlags::Crosshair, true);
    serviceD->SetElementVisible(VisFlags::Crosshair, true);

    serviceA->SetFileLoadedAsync(
        "E:/data/ct/700x1358x1252.raw",
        { 0.02125f, 0.02125f, 0.02125f },
        { 0.0f, 0.0f, 0.0f },
        [sharedState, serviceA, imageAnalysis](bool success)
        {
            if (!success) {
                std::cerr << "[onComplete] Volume data load failed.\n";
                return;
            }

            auto range = sharedState->GetDataRange();
            double isoVal = range[0] + (range[1] - range[0]) * 0.55;
            serviceA->SetIsoThreshold(isoVal);

            auto histogramTable = imageAnalysis->GetHistogramData(2048);
            if (histogramTable && histogramTable->GetNumberOfRows() > 0) {
                imageAnalysis->SetHistogramImageSaved("E:/data/ct/histogram.png", 2048);
            }
        });

    contextA->SetRendered();
    contextB->SetRendered();
    contextC->SetRendered();
    contextD->SetRendered();
    contextE->SetRendered();

    contextA->SetInteractorInitialized();
    contextB->SetInteractorInitialized();
    contextC->SetInteractorInitialized();
    contextD->SetInteractorInitialized();
    contextE->SetInteractorInitialized();

    // 当前 main.cpp 由 B 窗口进入消息循环
    contextB->SetStarted();

说明：
- SetFileLoadedAsync 的 onComplete 是主线程延迟回调。
- 等值面阈值、直方图导出这类后处理，统一放在加载成功回调里即可。
- 辅助元素显隐由各自窗口自己的 service 控制，不放进 WindowConfig。
*/

/*
─────────────────────────────────────────────────────────────────────
二、GetWindowPair 当前约定
─────────────────────────────────────────────────────────────────────

    static std::pair<std::shared_ptr<MedicalVizService>,
                     std::shared_ptr<StdRenderContext>>
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
        context->SetCameraStyleByVizMode(cfg.preInitCfg.vizMode);
        if (cfg.showAxes) {
            context->SetOrientationAxesVisible(true);
        }
        if (cfg.preInitCfg.hasBgColor) {
            service->SetBackground(cfg.preInitCfg.bgColor);
        }

        return { service, context };
    }
*/

/*
─────────────────────────────────────────────────────────────────────
三、前处理配置接口
─────────────────────────────────────────────────────────────────────

1) 批量提交初始化配置

    PreInitConfig cfg;
    cfg.vizMode = VizMode::CompositeIsoSurface;
    cfg.material = { 0.3, 0.6, 0.2, 15.0, 0.4, false };
    cfg.hasBgColor = true;
    cfg.bgColor = { 0.05, 0.05, 0.05 };
    cfg.hasWindowLevel = true;
    cfg.windowLevel = { 400.0, 40.0 };
    serviceA->SetVisualConfig(cfg);

2) 切换模式

    serviceA->SetVizMode(VizMode::CompositeIsoSurface);
    serviceE->SetVizMode(VizMode::CompositeVolume);

3) 设置材质 / 透明度 / 背景 / spacing

    serviceA->SetMaterial({ 0.3, 0.6, 0.2, 15.0, 0.4, false });
    serviceA->SetOpacity(0.4);
    serviceA->SetBackground({ 0.05, 0.05, 0.05 });
    serviceA->SetSpacing(0.02125, 0.02125, 0.02125);

4) 设置传输函数

    std::vector<TFNode> tf = {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };
    serviceE->SetTransferFunction(tf);

5) 设置窗宽窗位 / 等值面阈值

    serviceB->SetWindowLevel(400.0, 40.0);
    serviceA->SetIsoThreshold(1800.0);
*/

/*
─────────────────────────────────────────────────────────────────────
四、加载与状态查询
─────────────────────────────────────────────────────────────────────

1) 从文件异步加载

    serviceA->SetFileLoadedAsync(
        "E:/data/ct/700x1358x1252.raw",
        { 0.02125f, 0.02125f, 0.02125f },
        { 0.0f, 0.0f, 0.0f },
        [](bool success) {
            std::cout << (success ? "load ok\n" : "load failed\n");
        });

2) 从上游 buffer 重载

    const float* data = nullptr;
    std::array<int, 3> dims = { 512, 512, 512 };
    std::array<float, 3> spacing = { 0.1f, 0.1f, 0.1f };
    std::array<float, 3> origin = { 0.0f, 0.0f, 0.0f };
    serviceA->SetReloadFromBufferAsync(data, dims, spacing, origin,
        [](bool success) {
            std::cout << (success ? "reload ok\n" : "reload failed\n");
        });

3) 查询加载状态 / 请求取消

    LoadState fileState = serviceA->GetFileLoadState();
    LoadState reloadState = serviceA->GetReloadLoadState();
    serviceA->SetFileLoadCanceled();
*/

/*
─────────────────────────────────────────────────────────────────────
五、交互与状态接口
─────────────────────────────────────────────────────────────────────

1) 切片滚动 / 光标联动

    serviceB->SetSliceScrolled(+1);
    double worldPos[3] = { 10.0, 20.0, 30.0 };
    serviceB->SetCursorWorldPosition(worldPos, -1);
    auto cursor = serviceB->GetCursorWorld();

2) 交互态开关

    serviceA->SetInteracting(true);
    serviceA->SetInteracting(false);

3) 控制辅助元素显隐

    serviceA->SetElementVisible(VisFlags::Planes3D, true);
    serviceA->SetElementVisible(VisFlags::Ruler, false);
    serviceB->SetElementVisible(VisFlags::Crosshair, true);

4) 拖拽式窗宽窗位调节

    auto wl = serviceB->GetWindowLevel();
    serviceB->SetWindowLevelAdjusted(
        20,
        -10,
        400,
        400,
        wl.windowWidth,
        wl.windowCenter);

5) 模型变换 / 读取矩阵

    double t[3] = { 1.0, 2.0, 3.0 };
    double r[3] = { 0.0, 0.0, 15.0 };
    double s[3] = { 1.0, 1.0, 1.0 };
    serviceA->SetModelTransform(t, r, s);
    serviceA->SetModelTransformReset();
    auto matrix = serviceA->GetModelMatrix();
*/

/*
─────────────────────────────────────────────────────────────────────
六、导出接口
─────────────────────────────────────────────────────────────────────

1) 导出当前变换后的体数据

    serviceA->SetTransformedDataSavedAsync({}, [](bool success) {
        std::cout << (success ? "export ok\n" : "export failed\n");
    });

2) 导出当前方向全部切片图

    serviceB->SetSliceImagesSavedAsync({}, 0.0, [](bool success) {
        std::cout << (success ? "slice export ok\n" : "slice export failed\n");
    });
*/

/*
─────────────────────────────────────────────────────────────────────
七、Context 常用接口
─────────────────────────────────────────────────────────────────────

1) 绑定与窗口属性

    contextA->SetServiceBound(serviceA);
    contextA->SetWindowTitle("Window A: Composite IsoSurface");
    contextA->SetWindowSize(600, 600);
    contextA->SetWindowPosition(50, 50);

2) 相机风格 / 方向轴 / 工具模式

    contextA->SetCameraStyleByVizMode(VizMode::CompositeIsoSurface);
    contextA->SetOrientationAxesVisible(true);
    contextA->SetToolMode(ToolMode::Navigation);
    contextA->SetToolMode(ToolMode::ModelTransform);

3) 渲染生命周期

    contextA->SetRendered();
    contextA->SetInteractorInitialized();
    contextB->SetStarted();
*/

/*
─────────────────────────────────────────────────────────────────────
八、实际接入时记住这几条
─────────────────────────────────────────────────────────────────────

1. SharedState 和 DataManager 是多窗口共享的。
2. MedicalVizService 和 StdRenderContext 是每个窗口各自一份。
3. SetFileLoadedAsync / SetReloadFromBufferAsync / SetTransformedDataSavedAsync 的回调，
   都按当前实现走主线程延迟分发。
4. 前端不要在鼠标事件或业务回调里直接操作底层 VTK 对象，
   优先通过 service / context 的公开接口表达意图。
5. 如果想看最完整的真实接法，以 main.cpp 当前版本为准，example.cpp 只保留常用骨架。
*/

/*
─────────────────────────────────────────────────────────────────────
九、OrthogonalCrop 独立插件最小接法
─────────────────────────────────────────────────────────────────────

定位：
- 这是一个独立算法插件，不挂到 MedicalVizService / SharedState / Interaction 主链上。
- 前端只负责参数编辑、按钮流程和错误提示；插件只处理边界校验、体素吸附、RAM 估算与结果生成。
- 如果只做预览，就取 VirtualCrop；如果确认执行硬裁切，再切到 PhysicalCrop。

1) 最小调用骨架

    auto cropBackend = std::make_shared<OrthogonalCropBackendRouterService>();
    cropBackend->CropPreInit_SetInputImage(sharedDataMgr->GetVtkImage());
    cropBackend->CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource::ImageData);

    auto request = cropBackend->GetDefaultRequest();

    CropStateModel cropState;
    cropState.SetCropEnabled(true);
    cropState.SetInsideOpacity(0.20);
    cropState.SetOutsideVisibility(false);
    cropState.SetInteractionPhase(CropInteractionPhase::Released);
    request.SetCropStateModel(cropState);

    request.SetBoundsMode(CropBoundsMode::CenterAndDimensions);
    request.SetCenter({ 12.0, 15.0, 18.0 });
    request.SetDimensions({ 8.0, 10.0, 12.0 });
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetExecutionMode(CropExecutionMode::VirtualCrop);

    auto stats = cropBackend->GetStatistics(request);
    auto result = cropBackend->GetResult(request);
    if (!result.GetSucceeded()) {
        std::cerr << "crop failed: " << result.GetMessage() << std::endl;
        return;
    }

    auto previewMask = result.GetVirtualMaskImage();
    auto previewImage = result.GetDerivedImage();
    auto outline = result.GetOutlinePolyData();

2) 切到局部坐标系裁切盒

    auto request = cropBackend->GetDefaultRequest();
    request.SetBoundsMode(CropBoundsMode::LocalCenterAndDimensions);
    request.SetLocalAlignmentMatrix({
        1.0, 0.0, 0.0, 20.0,
        0.0, 1.0, 0.0, 30.0,
        0.0, 0.0, 1.0, 40.0,
        0.0, 0.0, 0.0, 1.0
    });
    request.SetLocalCenter({ 0.0, 0.0, 0.0 });
    request.SetLocalDimensions({ 6.0, 8.0, 10.0 });

3) 先预估，再执行物理硬裁切

    auto hardRequest = request;
    hardRequest.SetExecutionMode(CropExecutionMode::PhysicalCrop);
    auto hardStats = cropBackend->GetStatistics(hardRequest);
    if (!hardStats.GetCanExecutePhysicalCrop()) {
        std::cerr << hardStats.GetValidationMessage() << std::endl;
        return;
    }

    auto hardResult = cropBackend->GetResult(hardRequest);
    if (!hardResult.GetSucceeded()) {
        std::cerr << hardResult.GetMessage() << std::endl;
        return;
    }

    auto derivedImage = hardResult.GetDerivedImage();
    auto offsetMatrix = hardResult.GetCropDataModel().GetGlobalOffsetMatrix();

4) 如果输入改成 vtkPolyData

    cropBackend->CropPreInit_SetInputPolyData(inputSurface);
    cropBackend->CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource::PolyData);

    auto polyRequest = cropBackend->GetDefaultRequest();
    polyRequest.SetBoundsMode(CropBoundsMode::MinMaxCoordinates);
    polyRequest.SetRasBounds({ 10.0, 40.0, 5.0, 30.0, 8.0, 22.0 });
    auto polyResult = cropBackend->GetResult(polyRequest);
    auto clippedSurface = polyResult.GetDerivedPolyData();

5) UI / 视图层桥接

    auto cropInteractionBridge = std::make_shared<OrthogonalCropInteractionBridgeService>();
    cropInteractionBridge->SetDataManager(sharedDataMgr);
    cropInteractionBridge->SetReferenceRenderService(serviceA);
    cropInteractionBridge->SetPreviewRenderServices({ serviceB, serviceC, serviceD, serviceE });
    cropInteractionBridge->SetPrimaryInteractor(contextA->GetInteractor());
    cropInteractionBridge->CropPreInit_SetInputImage(sharedDataMgr->GetVtkImage());

    // InteractionBridgeService 内部：
    // - vtkBoxWidget2 / vtkBoxRepresentation 只管理 3D 裁切盒 UI 状态；
    // - bounds 变化后交给 OrthogonalCropBackendRouterService 选 image/polydata 后端；
    // - 参考坐标系只依赖 reference service；
    // - 只有 preview 目标服务列表会被置脏联动。

6) 调用约定

1. DataBackend 和 BoxWidgetController 是独立层；前者只处理数据，后者只处理 UI 状态。
2. request 仍然是一次性快照；前端或 ViewModel 每次确认参数后重新组装即可。
3. vtkImageData 路径默认走 image backend；vtkPolyData 路径默认走 vtkBox + vtkTableBasedClipDataSet。
4. VirtualCrop 时，image 路径可返回 mask / extracted preview image；polydata 路径返回 clipped polydata。
5. 物理裁切的 RemoveInside 在 image 路径默认阻断，因为那会打破“连续派生体、不插值、不重采样”的约束。
*/

/*
─────────────────────────────────────────────────────────────────────
十、OrthogonalCrop 详细设计说明
─────────────────────────────────────────────────────────────────────

这一节不再讲“怎么调用”，而是讲“为什么这样设计”。
如果前一节是接线图，这一节就是把裁切从输入、状态、坐标、后端选择到结果回传的整条链路拆开。

1) 核心原理：先把所有裁切定义归一化，再决定如何执行

这个插件真正处理的不是“按钮”或“widget”，而是一份一次性请求快照 OrthogonalCropRequest。
请求里虽然允许四种 bounds 表达方式：

- InputVolumeBounds：直接吃输入整体 bounds；
- MinMaxCoordinates：直接给六个 min/max；
- CenterAndDimensions：给中心点和尺寸；
- LocalCenterAndDimensions：给局部坐标系下的中心点和尺寸；

但算法层第一件事并不是执行裁切，而是把这些表达全部收敛成同一种客观几何语义：CropDataModel。

统一后的 CropDataModel 里，最关键的是三件东西：

- 一份最终可校验的 RAS min/max bounds；
- 一份 global offset matrix，用来表达派生结果相对原输入的平移补偿；
- 一份 local alignment matrix + local center/dimensions，用来保留“这个盒子原本是在局部坐标系里定义的”这一事实。

也就是说，裁切的本质不是“拖了一个框”，而是：
先把 UI 或上层输入归一成可验证、可投影、可复算的几何快照，
后面的 voxel 吸附、mask 生成、VOI 提取、polydata clip 都只吃这份快照。

2) 为什么拆成四层，而不是一个类全做

当前实现不是一个大而全的 CropService，而是故意拆成四个层次：

- OrthogonalCropAlgorithm：纯算法层，只处理 request -> cropData -> statistics/result；
- OrthogonalCropPluginService：image-only 的轻封装，只负责给算法层补齐输入和 RAM 查询；
- OrthogonalCropBackendRouterService：在 image 与 polydata 路径之间选后端；
- OrthogonalCropWidgetStateController：只维护 vtkBoxWidget2 的启停、样式、bounds、事件转发；
- OrthogonalCropInteractionBridgeService：热键、世界/模型坐标转换、何时触发 preview、哪些窗口要设脏。

这样拆的原因不是“好看”，而是为了切掉几种常见耦合：

- 算法层不依赖 MedicalVizService，不知道窗口、交互器、SharedState；
- widget controller 不知道 image/polydata，也不知道 preview 该怎么做；
- bridge 不直接操作具体 mapper，只负责把交互状态翻译成请求，并决定何时刷新；
- router 不知道热键，也不关心拖拽过程，只根据当前输入类型决定走哪条数据路径。

所以这个裁切模块的真正边界是：
UI 可以换，窗口数量可以换，甚至数据源从 image 换成 polydata，
只要 request/result 语义不变，裁切主逻辑就不用跟着重写。

3) 算法层到底做了什么

3.1 request 归一化

OrthogonalCropAlgorithm::GetCropDataModel 先把 request 的 boundsMode 展开：

- InputVolumeBounds：直接把输入 bounds 作为裁切盒；
- MinMaxCoordinates：直接写入 rasBounds；
- CenterAndDimensions：换算成 ras min/max；
- LocalCenterAndDimensions：先在局部坐标系中生成 8 个角点，再乘 localAlignmentMatrix，最后重新包成世界轴对齐 bounds。

这里有一个很重要的点：
局部裁切盒不是直接把中心点平移一下就完事，
而是要把八个角点全部变换到世界坐标后，再重新求一个 axis-aligned bounding box。
这样在模型有旋转或局部对齐矩阵不是单位阵时，最终 bounds 才不会错。

3.2 bounds 校验

统一完 cropData 后，算法层会立刻做两类校验：

- 几何合法性：min 必须小于 max；
- 范围合法性：裁切盒不能超出输入数据的整体 bounds。

这一步失败时，直接返回 InvalidBounds 或 BoundsOutOfRange，后面不会再进入 voxel 计算。

3.3 image 路径的体素吸附

对于 vtkImageData，真正执行前还要做一次世界坐标到 IJK 体素索引的吸附：

- 先拿 image 的 origin、spacing、dimensions；
- 按 `(coord - origin) / spacing` 把 RAS bounds 投影成索引；
- 用 llround 做近邻体素吸附；
- 再 clamp 到合法维度范围内。

这一步的结果是 snappedIJKBounds。
后面的 inside voxel 统计、mask 生成、VOI 提取，全都依赖这份吸附后的整数边界。

3.4 统计与执行为什么分两步

GetStatistics 不是可有可无，它是裁切链路里的前置判定器：

- totalVoxelCount：原体数据总体素数；
- insideVoxelCount：裁切盒内部体素数；
- outputVoxelCount：输出体素数；
- estimatedRamUsageBytes：预估内存消耗；
- canExecutePhysicalCrop：是否允许继续做硬裁切；
- failureReason / validationMessage：失败原因与提示。

这样做的目的，是把“能不能做”与“真正去做”分开。
前端可以先拿 statistics 决定是否给用户亮红字、是否需要二次确认，
而不用先真的分配一块派生体内存再说。

3.5 VirtualCrop 与 PhysicalCrop 的本质差别

VirtualCrop 不是“裁小一点的数据”，而是“保留原数据拓扑，只改变裁切语义”。
在 image 路径里，它会返回：

- 一张和原图同尺寸的 virtualMaskImage；
- 一份 outline polydata；
- 一组 statistics；
- 一份原始 cropState 快照。

这个 mask 的 inside / outside 值会根据 removalMode 决定：

- KeepInside：inside=255，outside=0；
- RemoveInside：inside=0，outside=255。

PhysicalCrop 则是真的要得到一个可脱离原图独立使用的派生体。
在 image 路径里，它会：

- 按 snappedIJKBounds 提取一个连续子块；
- 复制 spacing；
- 按新子块起点重算 origin；
- 计算新旧 origin 之间的平移差，并写回 globalOffsetMatrix；
- 把结果的 cropEnabled 置为 false，表示这份输出已经不是“待继续交互的裁切态”，而是“已生成的派生数据”。

这也是为什么 PhysicalCrop + RemoveInside 默认被拦住。
因为“移除内部、保留外部”在体数据里通常对应一个中空体，
它不再是一个简单、连续、无重采样的 VOI 子块；
如果硬做，就要引入额外的稀疏表示、重采样或多块拼接，已经超出了当前插件的语义边界。

4) router 层解决的不是算法，而是“到底走哪条数据链”

OrthogonalCropBackendRouterService 的职责很明确：
它不创造新的裁切语义，只负责把同一份 request 映射到 image 或 polydata 后端。

它的选择顺序是：

- 如果外部显式要求 PolyData，而且真的有 polydata 输入，就走 polydata；
- 如果外部显式要求 ImageData，而且真的有 image 输入，就走 image；
- 如果是 Auto，则优先 image，没有 image 再回退到 polydata；
- 两边都没有输入时，返回缺失输入错误。

这样设计的意义在于：
UI 不需要自己维护两套按钮逻辑，
也不需要写“如果当前是体数据就这样、如果当前是表面数据就那样”的分支散落在界面层。

对于 polydata，当前实现走的是：

- 用 cropData 里的 bounds 构造 vtkBox；
- 交给 vtkTableBasedClipDataSet；
- 最后过一层 vtkGeometryFilter 得到稳定的 vtkPolyData 输出。

因此，router 这一层本质上是在做“统一调用面 + 后端分发”，而不是把算法和交互揉在一起。

5) 交互桥接层真正管理的状态有哪些

如果只看 UI，感觉像是“一个盒子在拖”。
但真正参与状态切换的状态，实际上分成四类：

5.1 客观几何状态

- m_currentBounds：当前 widget 世界坐标盒，是 preview 的唯一几何真源；
- boundsInitialized：当前是否已经有一份有效盒子；
- GetActiveWorldBounds()：当前活跃输入在世界坐标下的整体包围盒；
- GetModelBoundsFromWorldBounds()：把世界盒子反推回模型坐标后，交给算法层。

5.2 交互控制状态

- m_cropInteractionEnabled：当前是否处于裁切模式；
- m_lastInteractionPhase：最近一次交互阶段，Idle / Dragging / Released；
- m_keepInsideShortcutDown / m_keepOutsideShortcutDown：1/2 是否正在按住；
- m_defaultRemovalMode：快捷键释放后的回退 removal mode；
- m_currentRemovalMode：当前真正写进 previewRequest 的 removal mode。

5.3 坐标参考状态

- referenceRenderService：只负责世界坐标和模型坐标互转；
- previewRenderServices：只负责接收“设脏刷新”通知；
- dataMgr：只作为 Auto 模式下、尚未显式绑定输入时的 image 兜底来源。

这三者故意拆开，避免一个 service 同时承担“坐标换算 + 数据输入 + 多窗口刷新列表”三种职责。

5.4 结果状态

- OrthogonalCropStatistics：一次执行的校验/估算结果；
- OrthogonalCropResult：一次执行的完整输出；
- CropStateModel：把交互态快照塞回 request/result，保证上层知道这份结果产生时处于什么交互阶段。

这里最关键的一点是：
bridge 并不把自己当作最终结果存储器。
它会在 UpdatePreviewFromCurrentBounds 里调用一次 GetResult 做校验和日志，
但它自己的职责停在“判定成功与否 + 置脏 preview 服务”这一层，
不会在内部长期缓存一份 previewResult 作为全局单例状态。

这意味着：
如果后续要把 mask、outline 或 derived image 真正接到 mapper / overlay，
应该由上层在适合的位置显式消费 result，而不是继续把 bridge 做成一个状态黑盒。

6) 为什么 widget 用世界坐标，而算法却吃模型坐标

这是当前实现里最容易被忽略，但其实最关键的耦合点。

vtkBoxWidget2 是挂在 3D 参考窗口上的，用户看到和拖拽的盒子天然属于世界坐标语义。
但数据本身的 image bounds 或 polydata bounds，很多时候更接近模型坐标或数据本地坐标。

如果直接把 widget 的 min/max 当成算法输入，就会在模型发生旋转、平移或缩放时出现错位。
所以 bridge 做了两步转换：

- GetActiveWorldBounds：把输入数据的模型 bounds 升到世界坐标，供 widget 摆放；
- GetModelBoundsFromWorldBounds：把用户拖完后的世界 bounds 再降回模型坐标，供 request.SetRasBounds 使用。

而且这里不是直接变换 min/max，
而是把 8 个角点逐个做 world/model 互转，再重新求一个轴对齐 bounds。
这样即使模型矩阵里带有旋转，最终给算法层的包围盒也仍然是正确的。

7) 完整流程：从按下 O 到 preview 更新

可以把当前交互链路理解成下面这条顺序：

7.1 进入模式前

- 外部先把 image 或 polydata 输入绑定给 backend/router；
- 如需交互，还要设置 primary interactor；
- referenceRenderService 决定坐标系换算基准；
- previewRenderServices 决定哪些窗口会跟随刷新。

7.2 按下 O

ExecuteDemo() 会做以下几步：

- EnsureInputReady：若当前是 Auto 且还没显式绑定 image，就尝试从 dataMgr 抓当前 volume；
- 若已经处于激活状态，则 O 直接走退出逻辑；
- 若第一次进入且还没初始化过 bounds，就取输入整体 bounds 的中心子盒作为默认盒；
- 把参考 bounds 和当前 bounds 同步给 widget；
- 真正启用 vtkBoxWidget2；
- 把交互状态切成 enabled=true、phase=Released；
- 主动触发一次 preview，保证初始盒子就有结果。

注意这里的默认盒子不是全幅包围盒，
而是整体 bounds 中的一个中心子盒，
目的是让用户一进来就看到一个可拖、且视觉上明显区别于整幅包围的裁切框。

7.3 拖拽过程中

widget controller 只监听三类 VTK 事件：

- StartInteractionEvent；
- InteractionEvent；
- EndInteractionEvent。

它会把这三类事件统一翻译成 CropInteractionPhase：

- StartInteraction / Interaction -> Dragging；
- EndInteraction -> Released；
- 其他 -> Idle。

bridge 收到回调后，并不会在每一帧拖拽都重新做裁切，
而是只更新：

- m_currentBounds；
- boundsInitialized；
- m_lastInteractionPhase。

只有 phase == Released 时，才真正调用 UpdatePreviewFromCurrentBounds。

这是一个非常明确的性能取舍：
拖拽中只同步轻量 UI 状态，重型 preview 只在松手时执行，
避免把鼠标移动频率直接变成体数据重算频率。

7.4 预览请求构造

BuildPreviewRequest() 会固定做几件事：

- 从 router 的 GetDefaultRequest() 拿一份基础 request；
- 强制覆盖成 MinMaxCoordinates；
- 把 m_currentBounds 从世界坐标转换回模型坐标；
- 强制 executionMode = VirtualCrop；
- 写入当前 removalMode；
- 把 cropEnabled 和 interactionPhase 打进 cropStateModel。

也就是说，交互层的 preview 永远走 VirtualCrop。
bridge 不负责在拖拽 UI 里直接触发 PhysicalCrop，
因为硬裁切意味着真正派生数据、内存分配和偏移矩阵更新，
这应该是“确认执行”的业务动作，而不是“鼠标拖一下就改底层数据”。

7.5 preview 结果消费

UpdatePreviewFromCurrentBounds() 的顺序是：

- 先 GetResult(previewRequest)；
- 失败则按 failureReason / message 打日志并立即返回；
- 成功则只通知 preview 服务列表 SetDirtyMarked；
- 最后输出本次 preview 的 source / backend / inside voxel / output voxel / bounds 日志。

所以这里的耦合边界非常清楚：
bridge 负责“什么时候算”和“哪些窗口该刷新”，
但不在这里直接写死某个 renderer 应该怎么画 mask 或 outline。

8) removal mode 为什么设计成“默认态 + 瞬时快捷键态”

当前 removal mode 不是一个完全静态的业务配置，而是一个带有瞬时覆盖语义的交互态：

- 默认值存在 m_defaultRemovalMode；
- 当前实际下发值存在 m_currentRemovalMode；
- 按住 1 时临时切回 KeepInside；
- 按住 2 时临时切成 RemoveInside；
- 松手后回退到默认态。

这意味着 1/2 更像“查看 inside/outside 两种解释的快捷观察键”，
而不是“永久改配置”的设置项。

同时它还和交互阶段耦合：

- 如果当前正在 Dragging，切换 removal mode 只更新状态，不立刻重算；
- 等到 Released，bounds changed 的 released 分支会统一刷新 preview；
- 如果当前本来就不在拖拽中，才允许快捷键即时刷新 preview。

这样做是为了避免两个高频事件源叠加：
鼠标拖拽已经很频繁，如果再叠加按键变化时立刻重算，就很容易把 preview 触发频率抬高到不可控。

9) 状态切换可以直接理解成这张文字状态机

9.1 未激活态

- m_cropInteractionEnabled = false；
- widget 关闭；
- 当前可能没有有效 bounds，也可能保留着上次退出时的 bounds。

9.2 按 O 成功进入

- 若输入有效且 interactor 就绪，widget 被启用；
- 若之前没初始化过 bounds，则创建默认中心子盒；
- phase 置为 Released；
- 立即触发一次 preview。

9.3 拖拽中

- widget 事件进入 Dragging；
- m_currentBounds 持续更新；
- 不做重 preview；
- removal mode 变化只改状态，不重算。

9.4 释放鼠标

- phase 切为 Released；
- 以当前 bounds 组装 previewRequest；
- 调一次 GetResult；
- 成功后设脏 preview 窗口。

9.5 按住 / 松开 1、2

- 修改当前 removal mode；
- 若不在拖拽中，立刻刷新 preview；
- 若在拖拽中，延后到 Released 再统一刷新。

9.6 按 Esc 或再次按 O

- widget 关闭；
- m_cropInteractionEnabled = false；
- 快捷键状态清零；
- removal mode 回退到默认值；
- 保留最后一份 bounds；
- 如已有有效 bounds，则以默认 removal mode 再稳定一次 preview 语义。

这里的关键点是：退出并不会把 bounds 清空。
也就是说，当前设计把“是否处于交互模式”和“最后一次裁切盒几何”分离开了。
这样再次进入时可以延续上次盒子，而不是每次都从头摆一个新框。

10) 这个设计当前最重要的边界与取舍

如果用一句话总结当前思路，就是：
把裁切当作“独立可复算的几何请求”，而不是“挂死在某个窗口里的特殊 UI 功能”。

因此它有几个非常明确的取舍：

- request/result 是真边界，UI、算法、后端都围绕它展开；
- 交互层只管理 widget 状态、触发时机和设脏，不抢算法层职责；
- preview 默认只在 Released 执行，优先保证交互稳定性；
- image 路径允许虚拟裁切和硬裁切，polydata 路径优先保证 clip 输出一致性；
- RemoveInside 的物理裁切被有意阻断，避免为了“看起来都支持”而引入不受控的派生语义；
- bridge 不缓存全局 preview 结果，后续若要真正接到渲染链，应由上层明确消费 result。

理解了这几点，再回头看第九节的最小接法，就会更容易明白：
example 里那份 request 不是参数堆砌，
而是在给这个“独立裁切请求机”提供一次完整、可复算、可验证的状态快照。
*/

/*
─────────────────────────────────────────────────────────────────────
十一、OrthogonalCrop 流程图与类名速查
─────────────────────────────────────────────────────────────────────

如果前一节是在讲“原理”，这一节就是把它直接画出来。
读这一节时，建议先看 11.1 的总装图，再看 11.2 的一次交互时间线，
最后用 11.3 的类名速查表把每个类的职责对上。

11.1 总装图：谁驱动谁

    [前端 / ViewModel / main 示例]
              |
              | 组装 request、设置输入、绑定窗口
              v
    +----------------------------------------------+
    | OrthogonalCropInteractionBridgeService       |
    | 作用：交互总调度                             |
    | - 处理 O / Esc / 1 / 2 热键                  |
    | - 世界坐标 <-> 模型坐标转换                  |
    | - 决定何时触发 preview                       |
    | - 决定哪些窗口需要 SetDirtyMarked()          |
    +----------------------------------------------+
          |                            |
          | 挂接 UI 盒子               | 提交裁切请求
          v                            v
    +------------------------+   +----------------------------------+
    | OrthogonalCropWidget   |   | OrthogonalCropBackendRouter      |
    | StateController        |   | Service                          |
    | 作用：只管盒子 UI      |   | 作用：决定走 image 还是 polydata |
    | - vtkBoxWidget2 开关    |   | - Auto / Image / PolyData 选择   |
    | - bounds 同步           |   | - 统一 GetDefaultRequest         |
    | - 交互事件转 phase      |   | - 统一 GetStatistics / GetResult |
    +------------------------+   +----------------------------------+
                                               |
                        +----------------------+----------------------+
                        |                                             |
                        | image 路径                                  | polydata 路径
                        v                                             v
         +----------------------------------+         +----------------------------------+
         | OrthogonalCropPluginService      |         | vtkBox + vtkTableBasedClipDataSet |
         | 作用：image-only 轻封装          |         | + vtkGeometryFilter              |
         | - 持有 vtkImageData              |         | 作用：裁表面 / 网格              |
         | - 查询系统 RAM                   |         +----------------------------------+
         | - 转给纯算法层                   |
         +----------------------------------+
                          |
                          v
         +----------------------------------+
         | OrthogonalCropAlgorithm          |
         | 作用：真正做裁切语义计算         |
         | - request -> cropData            |
         | - bounds 校验                    |
         | - voxel 吸附                     |
         | - statistics/result 生成         |
         +----------------------------------+
                          |
                          v
         +----------------------------------+
         | OrthogonalCropResult             |
         | - virtualMaskImage               |
         | - derivedImage / derivedPolyData |
         | - outlinePolyData                |
         | - statistics                     |
         | - cropDataModel / cropStateModel |
         +----------------------------------+

一句话记忆：
- Bridge 像“导演”，决定什么时候演；
- WidgetController 像“舞台机械”，只负责把盒子摆出来；
- Router 像“分诊台”，决定病人走哪科；
- Algorithm 才是“主刀医生”，真正下刀算裁切。

11.2 一次交互时间线：从按 O 到松手刷新

    [用户按 O]
        |
        v
    ExecuteDemo()
        |
        +--> EnsureInputReady()
        |      |
        |      +--> 没有显式 image 输入时，尝试从 dataMgr 兜底
        |
        +--> 如果之前没 bounds，就创建默认中心子盒
        |
        +--> widget.SetReferenceBounds(...)
        +--> widget.SetWidgetBounds(...)
        +--> widget.SetEnabled(true)
        |
        +--> m_cropInteractionEnabled = true
        +--> m_lastInteractionPhase = Released
        |
        v
    UpdatePreviewFromCurrentBounds(true)
        |
        +--> BuildPreviewRequest()
        |      |
        |      +--> 世界 bounds -> 模型 bounds
        |      +--> executionMode 强制 VirtualCrop
        |      +--> 写入 currentRemovalMode
        |      +--> 写入 cropStateModel
        |
        +--> router.GetResult(request)
               |
               +--> image? 走 Algorithm::GetResult(...)
               |       |
               |       +--> 归一化 request
               |       +--> 校验 bounds
               |       +--> 体素吸附 snappedIJKBounds
               |       +--> 生成 statistics
               |       +--> 生成 virtualMask 或 derivedImage
               |
               +--> polydata? 走 box clip
        |
        +--> 成功: preview services SetDirtyMarked()
        +--> 失败: 打日志，不广播坏结果

拖拽过程中的节流逻辑是：

    [StartInteractionEvent / InteractionEvent]
        |
        v
    phase = Dragging
        |
        +--> 只更新 m_currentBounds
        +--> 不执行 preview 重算

    [EndInteractionEvent]
        |
        v
    phase = Released
        |
        +--> UpdatePreviewFromCurrentBounds(true)

所以当前交互不是“边拖边重算体数据”，而是“边拖边更新盒子，松手后统一重算一次”。

11.3 快捷键状态切换图

    默认态
      |
      | 按 O
      v
    进入裁切模式
      |
      | 拖拽盒子
      v
    Dragging -----------------------------+
      |                                   |
      | 松开鼠标                          | 按 1 / 2
      v                                   |
    Released                              |
      |                                   |
      | 触发 preview                      |
      +-----------------------------------+
      |
      | Esc 或再次按 O
      v
    退出裁切模式

其中 1 / 2 不是永久配置，而是瞬时覆盖：

    KeepInside(默认)
         |
         | 按住 2
         v
    RemoveInside(临时)
         |
         | 松开 2
         v
    KeepInside(回退)

如果此时正在 Dragging：
- 只切换 removal mode 状态；
- 不立刻重算 preview；
- 等 Released 再统一重算。

11.4 类名拆词：每个名字到底想表达什么

1) OrthogonalCropAlgorithm

- Orthogonal：正交的、按轴对齐的；
- Crop：裁切；
- Algorithm：真正负责算结果的算法核心。

这个类名字的重点不是“服务”，而是“算法”。
它说明这里的输入输出应该是干净的 request/result，
而不是窗口、按钮、交互器这些 UI 物件。

2) OrthogonalCropPluginService

- Plugin：独立插件；
- Service：对外暴露一套更好调用的入口。

它本质上是 image 路径的轻门面。
真正的计算仍然在 Algorithm，
它只是负责“持有输入 image + 查系统 RAM + 调算法”。

如果把 Algorithm 理解成发动机，
那 PluginService 更像“发动机外面那层可直接拧钥匙的壳”。

3) OrthogonalCropBackendRouterService

- Backend：后端实现；
- Router：路由器、分发器。

这个名字已经明说了它的重点不是算，
而是“同一个裁切请求，最后交给谁执行”。

你可以把它理解成：
- 有 image，就优先走 image backend；
- 有 polydata，就能切到 polydata backend；
- 上层不需要自己写一堆 if/else。

4) OrthogonalCropWidgetStateController

- Widget：VTK 里的裁切盒控件；
- State：这里维护的是控件状态，不是裁切结果；
- Controller：控制器，不做业务运算。

这个类最容易误会成“裁切控制器”，
但实际上它只管一件事：
让 vtkBoxWidget2 的当前 bounds、启停状态和交互事件保持稳定。

它不应该知道：
- 当前是 image 还是 polydata；
- 当前 preview 要不要生成 mask；
- 哪些窗口要刷新。

5) OrthogonalCropInteractionBridgeService

- Interaction：交互；
- Bridge：桥接；
- Service：对外可直接用的一层总控。

这个名字最准确地说明了它的真实身份：
它不是“算法执行器”，也不是“UI 控件本体”，
而是把“热键/拖拽/坐标系/preview 触发”桥接到后端裁切能力上的那层胶水。

它干的是把两边语言互相翻译：
- UI 这边说的是世界坐标、按键、拖拽阶段；
- 后端那边说的是 request、boundsMode、result、failureReason。

6) CropDataModel

- Data：客观数据；
- Model：稳定的数据模型。

它表示的是“这个裁切盒在几何上到底是什么”。
里面放的是：
- rasBounds；
- globalOffsetMatrix；
- localAlignmentMatrix；
- localCenter / localDimensions。

它不表示“用户现在是否按着鼠标”，
只表示“裁切盒这件事在客观空间里是什么样”。

7) CropStateModel

- State：瞬时状态；
- Model：状态快照。

它和 CropDataModel 刚好互补。
CropDataModel 说“盒子在哪、大小多少”，
CropStateModel 说“当前是不是启用、正在拖没拖、inside 透明度多少、当前哪个 handle 激活”。

所以可以用一句话区分：
- DataModel = 盒子本体事实；
- StateModel = 盒子当前表现状态。

8) OrthogonalCropRequest

- Request：一次请求单。

这个名字的关键在“一次性”。
它不是长期全局状态对象，
而是“当前这次要怎么裁”的完整参数包。

前端或 ViewModel 每次确认参数后，
重新组一份 request 再发下去，就是当前设计的推荐用法。

9) OrthogonalCropStatistics

- Statistics：统计与预估。

它不是结果图像，
而是“执行前/执行中给上层看的体检单”：
- inside 有多少；
- output 有多少；
- 估计要多少内存；
- 能不能做 physical crop；
- 失败的话为什么失败。

10) OrthogonalCropResult

- Result：一次执行的最终交付物。

它可以带的东西很多：
- virtualMaskImage；
- derivedImage；
- derivedPolyData；
- outlinePolyData；
- statistics；
- cropDataModel；
- cropStateModel。

所以它不是“只有图像结果”，
而是“这一刀裁下去之后，算法愿意完整交回来的所有东西”。

11.5 一眼分清这些类最简单的办法

如果你只想要一句话版本，可以这样记：

- Request：这次想怎么裁；
- DataModel：这个盒子客观上长什么样；
- StateModel：这个盒子现在处于什么交互态；
- Statistics：现在能不能安全裁；
- Result：裁完后拿到了什么；
- Algorithm：真正算；
- PluginService：给 image backend 套一层易用入口；
- BackendRouterService：决定走哪条后端路；
- WidgetStateController：只管 UI 盒子；
- InteractionBridgeService：把 UI 事件翻译成后端请求。

如果再压缩成一句：
“Bridge 收集用户意图，WidgetController 保持盒子稳定，Router 选择后端，Algorithm 产出 Result。”
*/
