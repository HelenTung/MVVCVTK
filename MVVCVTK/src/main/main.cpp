// =====================================================================
//
// 三阶段结构（保持不变）：
//   Phase 1 【前处理】  创建共享资源 + 通过 WindowConfig / SetVisualConfig
//                       批量配置所有与数据无关的参数（含背景色）
//   Phase 2 【加载】    通过 IDataLoaderService 接口发起异步加载
//   Phase 3 【渲染骨架】SetInteractorInitialized + SetStarted 进入消息循环
//
//   • PreInitConfig 中统一携带 bgColor / hasBgColor（前处理背景色）
//   • SetFileLoadedAsync 回调中执行数据相关的后处理业务（等值面阈值推算）
//   • 回调内明确注释：在后台线程，只允许操作 SharedState
//   • 加载失败时通过 SetLoadFailed → PostData_HandleLoadFailed 处理
//   • IDataLoaderService 通过 GetFileLoadState / GetReloadLoadState 做主线程状态查询
//   • main.cpp 中的 SetInteractorInitialized() 调用方式明确分离（接口显式）
// =====================================================================
#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <vtkAutoInit.h>
#include <vtkSMPTools.h>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);

#include "AppTypes.h"
#include "AppInterfaces.h"
#include "AppState.h"
#include "AppService.h"
#include "DataManager.h"
#include "VolumeAnalysisService.h"
#include "StdRenderContext.h"

// 融合算法孔隙分析
#include "GapAnalysisService.h"
#include "GapAlgorithmOverlayStrategies.h"
#include <vtkCommand.h>

#include <iostream>
#include <vector>

class GapOverlayKeyTrigger : public vtkCommand {
public:
    static GapOverlayKeyTrigger* New() { return new GapOverlayKeyTrigger; }

    std::shared_ptr<GapAnalysisService> gapAnalysis;
    std::shared_ptr<MedicalVizService> srvA, srvB, srvC, srvD, srvE;
    bool isApplied = false;

    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override {
        // 1. 只响应键盘按下事件
        if (eventId != vtkCommand::KeyPressEvent) return;

        // 2. 获取交互器并提取按键
        auto* iren = vtkRenderWindowInteractor::SafeDownCast(caller);
        if (!iren) return;

        char key = iren->GetKeyCode();
        // 如果按下的不是 j 或 J，直接忽略
        if (key != 'j' && key != 'J') return;

        // 如果已经加载过图层，就不重复加载了
        if (isApplied || !gapAnalysis) return;

        auto state = gapAnalysis->GetAnalysisState();

        // 3. 状态分支判断
        if (state == GapAnalysisState::Running) {
            std::cout << "[Key Trigger] Algorithm is still running... Please wait." << std::endl;
            return;
        }
        else if (state == GapAnalysisState::Succeeded) {
            isApplied = true;
            std::cout << "\n[Main] Gap Analysis completed! Applying overlays to UI...\n";

            // 获取算法产物（此时仍在主线程）
            auto voidMesh = gapAnalysis->BuildVoidMesh();
            auto labelImg = gapAnalysis->BuildLabelImage();

            // 为 3D 窗口配置网格融合图层 (Window A, E)
            auto meshOverlayA = std::make_shared<GapMeshOverlayStrategy>();
            meshOverlayA->SetInputData(voidMesh);
            srvA->SetOverlayStrategyAdded(meshOverlayA);

            auto meshOverlayE = std::make_shared<GapMeshOverlayStrategy>();
            meshOverlayE->SetInputData(voidMesh);
            srvE->SetOverlayStrategyAdded(meshOverlayE);

            // 为 2D 窗口配置标签切片融合图层 (Window B, C, D)
            auto sliceOverlayB = std::make_shared<GapSliceOverlayStrategy>(Orientation::Top_down);
            sliceOverlayB->SetInputData(labelImg);
            srvB->SetOverlayStrategyAdded(sliceOverlayB);

            auto sliceOverlayC = std::make_shared<GapSliceOverlayStrategy>(Orientation::Front_back);
            sliceOverlayC->SetInputData(labelImg);
            srvC->SetOverlayStrategyAdded(sliceOverlayC);

            auto sliceOverlayD = std::make_shared<GapSliceOverlayStrategy>(Orientation::Left_right);
            sliceOverlayD->SetInputData(labelImg);
            srvD->SetOverlayStrategyAdded(sliceOverlayD);

            gapAnalysis->saveResults("E:\\data\\ct\\out");

            // 通知所有窗口画面标脏，触发重绘
            srvA->SetDirtyMarked(); srvB->SetDirtyMarked();
            srvC->SetDirtyMarked(); srvD->SetDirtyMarked(); srvE->SetDirtyMarked();
        }
        else if (state == GapAnalysisState::Failed) {
            isApplied = true;
            std::cerr << "\n[Main] Gap Analysis failed to process.\n";
        }
    }
};

static std::pair<
    std::shared_ptr<MedicalVizService>,
    std::shared_ptr<StdRenderContext>>
    GetWindowPair(
        const WindowConfig& cfg,
        std::shared_ptr<AbstractDataManager>    dataMgr,
        std::shared_ptr<SharedInteractionState> sharedState,
        std::shared_ptr<IStateEventSource> stateEventSource)
{
    auto service = std::make_shared<MedicalVizService>(dataMgr, sharedState, stateEventSource);
    auto context = std::make_shared<StdRenderContext>();

    // ── 步骤1：SetServiceBound（触发 SetRenderContext → 注册 Observer）──────
    context->SetServiceBound(service);

    // ── 步骤2：通过 SetVisualConfig 批量提交前处理配置（一次锁 + 一次广播）────
    service->SetVisualConfig(cfg.preInitCfg);

    // ── 步骤3：窗口属性（纯渲染上下文配置，数据无关）─────────────
    context->SetWindowTitle(cfg.title);
    context->SetWindowSize(cfg.width, cfg.height);
    context->SetWindowPosition(cfg.posX, cfg.posY);
    context->SetCameraStyleByVizMode(cfg.preInitCfg.vizMode);
    if (cfg.showAxes)
        context->SetOrientationAxesVisible(true);

    // ── 步骤4：背景色（前处理：直接应用到渲染器，数据无关）───────
    if (cfg.preInitCfg.hasBgColor)
        service->SetBackground(cfg.preInitCfg.bgColor);

    return { service, context };
}

int main()
{
    vtkSMPTools::Initialize();
    // ── 共享资源 ──────────────────────────────────────────────────
    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedStateBroadcaster = std::make_shared<SharedStateBroadcaster>();
    auto sharedState = std::make_shared<SharedInteractionState>(sharedStateBroadcaster);
    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);
    auto gapAnalysis = std::make_shared<GapAnalysisService>(sharedDataMgr);

    // ── 传输函数（数据无关，前处理阶段安全）──────────────────────
    std::vector<TFNode> volTF = {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };

    // ── 窗口配置表（前处理参数全部在此集中声明）──────────────────

    // 窗口 E：体渲染 + 切片参考平面
    WindowConfig cfgE;
    cfgE.title = "Window E: Composite Volume";
    cfgE.width = 600; cfgE.height = 600;
    cfgE.posX = 660; cfgE.posY = 50;
    cfgE.preInitCfg.vizMode = VizMode::CompositeVolume;
    cfgE.preInitCfg.tfNodes = volTF;
    cfgE.preInitCfg.hasTF = true;
    cfgE.preInitCfg.bgColor = { 0.08, 0.08, 0.12 }; // 深蓝背景
    cfgE.preInitCfg.hasBgColor = true;

    // ── 窗口 B：Top_down 切片（2D，默认窗）────────────────────
    WindowConfig cfgB;
    cfgB.title = "Window B: Top_down Slice";
    cfgB.width = 400; cfgB.height = 400;
    cfgB.posX = 50;  cfgB.posY = 660;
    cfgB.preInitCfg.vizMode = VizMode::SliceTop_down;
    cfgB.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgB.preInitCfg.hasBgColor = true;
    cfgB.preInitCfg.windowLevel = { 400.0, 40.0 };   // ★ WW=400, WC=40
    cfgB.preInitCfg.hasWindowLevel = true;

    // ── 窗口 C：Front_back 切片 ─────────────────────────────────────
    WindowConfig cfgC;
    cfgC.title = "Window C: Front_back Slice";
    cfgC.width = 400; cfgC.height = 400;
    cfgC.posX = 460; cfgC.posY = 660;
    cfgC.preInitCfg.vizMode = VizMode::SliceFront_back;
    cfgC.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgC.preInitCfg.hasBgColor = true;
    cfgC.preInitCfg.windowLevel = { 400.0, 40.0 };   // ★
    cfgC.preInitCfg.hasWindowLevel = true;

    // ── 窗口 D：Left_right 切片 ────────────────────────────────────
    WindowConfig cfgD;
    cfgD.title = "Window D: Left_right Slice";
    cfgD.width = 400; cfgD.height = 400;
    cfgD.posX = 870; cfgD.posY = 660;
    cfgD.preInitCfg.vizMode = VizMode::SliceLeft_right;
    cfgD.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgD.preInitCfg.hasBgColor = true;
    cfgD.preInitCfg.windowLevel = { 400.0, 40.0 };   // ★
    cfgD.preInitCfg.hasWindowLevel = true;

    // 窗口 A：等值面 + 切片参考平面
    WindowConfig cfgA;
    cfgA.title = "Window A: Composite IsoSurface";
    cfgA.width = 600; cfgA.height = 600;
    cfgA.posX = 50;  cfgA.posY = 50;
        cfgA.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    cfgA.showAxes = true;
    cfgA.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    cfgA.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 0.4, false };
    cfgA.preInitCfg.bgColor = { 0.05, 0.05, 0.05 }; // 深灰背景
    cfgA.preInitCfg.hasBgColor = true;

    // ── 批量建窗（前处理完成）────────────────────────────────────
    auto [serviceA, contextA] = GetWindowPair(cfgA, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceE, contextE] = GetWindowPair(cfgE, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceB, contextB] = GetWindowPair(cfgB, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceC, contextC] = GetWindowPair(cfgC, sharedDataMgr, sharedState, sharedStateBroadcaster);
    auto [serviceD, contextD] = GetWindowPair(cfgD, sharedDataMgr, sharedState, sharedStateBroadcaster);

    // 3D窗口：设置参考切面可见（Composite 模式默认显示，纯 3D 模式无参考切面）
    serviceA->SetElementVisible(VisFlags::Planes3D, true);
    serviceE->SetElementVisible(VisFlags::Planes3D, true);

    // 3D 窗口：隐藏标尺
    serviceA->SetElementVisible(VisFlags::Ruler, false);
    serviceE->SetElementVisible(VisFlags::Ruler, false);

    // 2D 窗口：显示十字线
    serviceB->SetElementVisible(VisFlags::Crosshair, true);
    serviceC->SetElementVisible(VisFlags::Crosshair, true);
    serviceD->SetElementVisible(VisFlags::Crosshair, true);

    serviceA->SetFileLoadedAsync(
        "E:\\data\\ct\\700x1358x1252.raw",
        {0.02125, 0.02125, 0.02125},
        {0.0, 0.0, 0.0},
        [sharedState, serviceA, imageAnalysis, gapAnalysis](bool success)
        {
            if (!success) {
                std::cerr << "[onComplete] Volume data load failed.\n";
                return;
            }

            // 设置等值面等常规数据
            auto range = sharedState->GetDataRange();
            double isoVal = range[0] + (range[1] - range[0]) * 0.55;
            serviceA->SetIsoThreshold(isoVal);

            const int histogramBinCount = 2048; // 直方图统计使用的 bin 数量
            const std::string histogramFilePath = "E:\\data\\ct\\histogram.png"; // 直方图导出图片路径
            auto histogramTable = imageAnalysis->GetHistogramData(histogramBinCount);
            if (histogramTable && histogramTable->GetNumberOfRows() > 0) {
                imageAnalysis->SetHistogramImageSaved(histogramFilePath, histogramBinCount);
            }
            else {
                std::cerr << "[Main] Histogram generation failed.\n";
            }

            //// 触发孔隙分析算法
            //SurfaceParams surfP;
            //surfP.isoValue = static_cast<float>(isoVal); // 使用自动推算的阈值
            //gapAnalysis->GapPreInit_SetSurfaceParams(surfP);

            //VoidDetectionParams voidP;
            //voidP.grayMin = -0.2262536138296127f;
            //voidP.grayMax = 0.15f;
            ////voidP.minVolumeMM3 = 3;
            //// 0.000009595703125
            //voidP.minVolumeMM3 = 0.0001;
            //voidP.angleThresholdDeg = 30.0f;
            //voidP.tensorWindowSize = 1;
            //voidP.erosionIterations = 2;
            //gapAnalysis->GapPreInit_SetVoidParams(voidP);

            //std::cout << "[Main] Triggering background Gap Analysis...\n";
            //// 发起后台计算，不阻塞当前 UI
            //gapAnalysis->RunAsync();
        }
    );

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

    //auto trigger = vtkSmartPointer<GapOverlayKeyTrigger>::New();
    //trigger->gapAnalysis = gapAnalysis;
    //trigger->srvA = serviceA; trigger->srvB = serviceB;
    //trigger->srvC = serviceC; trigger->srvD = serviceD; trigger->srvE = serviceE;

    //contextA->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, trigger);
    //contextB->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, trigger);
    //contextC->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, trigger);
    //contextD->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, trigger);
    //contextE->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, trigger);

    std::cout << "Application started. Loading data in background...\n"
        << "Controls: A/D = navigate slices | M = toggle model transform | D = distance measure | A = angle measure\n";

    // contextB 持有主事件循环（其他窗口通过共享 Timer 驱动）
    contextB->SetStarted();

    return 0;
}
