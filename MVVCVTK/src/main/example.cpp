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
