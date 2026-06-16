// =====================================================================
//
// 三阶段结构（保持不变）：
//   Phase 1 【前处理】  创建共享资源 + 通过 WindowConfig / SetVisualConfig
//                       批量配置所有与数据无关的参数（含背景色）
//   Phase 2 【加载】    通过 IDataLoaderService 接口发起异步加载
//   Phase 3 【渲染骨架】InitializeInteractor + Start 进入消息循环
//
//   • PreInitConfig 中统一携带 bgColor / hasBgColor（前处理背景色）
//   • LoadFileAsync 回调中执行数据相关的后处理业务（等值面阈值推算、算法启动）
//   • 加载回调由主线程 Timer 延迟执行；算法完成后的 VTK overlay 挂接也通过 Timer observer 主线程收口
//   • 加载失败时通过 SetLoadFailed → PostData_HandleLoadFailed 处理
//   • IDataLoaderService 通过 GetFileLoadState / GetReloadLoadState 做主线程状态查询
//   • main.cpp 中的 InitializeInteractor() 调用方式明确分离（接口显式）
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
#include "OrthogonalCropInteractionBridgeService.h"
#include "OrthogonalCropSubmitWorkflow.h"

// 融合算法孔隙分析
#include "GapAnalysisService.h"
#include "GapAlgorithmOverlayStrategies.h"
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkRenderWindowInteractor.h>

#include <cmath>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

class OrthogonalCropHotkeyObserver : public vtkCommand {
public:
    static OrthogonalCropHotkeyObserver* New() { return new OrthogonalCropHotkeyObserver; }

    std::shared_ptr<OrthogonalCropInteractionBridgeService> orthogonalCropBridge;
    std::shared_ptr<OrthogonalCropSubmitWorkflow> orthogonalCropSubmitWorkflow;

    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override {
        (void)callData;
        this->AbortFlagOff();
        auto* iren = vtkRenderWindowInteractor::SafeDownCast(caller);
        if (!iren) return;

        if (!orthogonalCropBridge) {
            return;
        }

        const std::string keySym = iren->GetKeySym() ? iren->GetKeySym() : "";
        const char keyCode = iren->GetKeyCode();
        const bool isControlPressed = iren->GetControlKey() != 0;
        const bool isCropToggleKey = keyCode == 'o' || keyCode == 'O' || keySym == "o" || keySym == "O";
        const bool isInsidePreviewKey = keyCode == '1' || keySym == "1";
        const bool isOutsidePreviewKey = keyCode == '2' || keySym == "2";
        const bool isPreviewArtifactModeKey = keyCode == '3' || keySym == "3";
        const bool isSubmitKey = isPreviewArtifactModeKey && isControlPressed;
        const bool isManagedCropHotkey = isCropToggleKey
            || keySym == "Escape"
            || isInsidePreviewKey
            || isOutsidePreviewKey
            || isPreviewArtifactModeKey;

        if (eventId == vtkCommand::CharEvent && isManagedCropHotkey) {
            this->AbortFlagOn();
            return;
        }

        if (eventId == vtkCommand::KeyPressEvent) {
            if (isCropToggleKey && !m_cropToggleKeyDown) {
                m_cropToggleKeyDown = true;
                orthogonalCropBridge->ToggleInteractiveCrop();
                return;
            }

            if (keySym == "Escape") {
                orthogonalCropBridge->ExitInteractiveCrop();
                return;
            }

            if (isInsidePreviewKey && !m_insidePreviewKeyDown) {
                m_insidePreviewKeyDown = true;
                orthogonalCropBridge->ToggleInsidePreview();
                return;
            }

            if (isOutsidePreviewKey && !m_outsidePreviewKeyDown) {
                m_outsidePreviewKeyDown = true;
                orthogonalCropBridge->ToggleOutsidePreview();
                return;
            }

            if (isPreviewArtifactModeKey && !m_previewModeKeyDown) {
                m_previewModeKeyDown = true;
                if (isSubmitKey) {
                    if (orthogonalCropSubmitWorkflow) {
                        orthogonalCropSubmitWorkflow->ApplySubmit();
                    }
                }
                else {
                    orthogonalCropBridge->TogglePreviewMode();
                }
                return;
            }
        }

        if (eventId == vtkCommand::KeyReleaseEvent) {
            if (isCropToggleKey) {
                m_cropToggleKeyDown = false;
                return;
            }

            if (isInsidePreviewKey) {
                m_insidePreviewKeyDown = false;
                return;
            }

            if (isOutsidePreviewKey) {
                m_outsidePreviewKeyDown = false;
                return;
            }

            if (isPreviewArtifactModeKey) {
                m_previewModeKeyDown = false;
                return;
            }
        }
    }

private:
    bool m_cropToggleKeyDown = false;
    bool m_insidePreviewKeyDown = false;
    bool m_outsidePreviewKeyDown = false;
    bool m_previewModeKeyDown = false;
};

class GapAnalysisOverlayCommitObserver : public vtkCommand {
public:
    static GapAnalysisOverlayCommitObserver* New() { return new GapAnalysisOverlayCommitObserver; }

    std::shared_ptr<GapAnalysisService> gapAnalysis;
    std::shared_ptr<MedicalVizService> serviceA;
    std::shared_ptr<MedicalVizService> serviceB;
    std::shared_ptr<MedicalVizService> serviceC;
    std::shared_ptr<MedicalVizService> serviceD;
    std::shared_ptr<MedicalVizService> serviceE;
    std::shared_ptr<SharedInteractionState> sharedState;

    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override {
        (void)caller;
        (void)callData;
        if (eventId != vtkCommand::TimerEvent || m_overlayCommitted || !gapAnalysis) {
            return;
        }

        const GapAnalysisState state = gapAnalysis->GetAnalysisState();
        if (state == GapAnalysisState::Idle) {
            TryStartAnalysis();
            return;
        }

        if (state == GapAnalysisState::Running) {
            return;
        }

        if (state == GapAnalysisState::Failed) {
            if (!m_failureLogged) {
                std::cerr << "[Main] Gap Analysis failed; overlay will not be attached." << std::endl;
                m_failureLogged = true;
            }
            return;
        }

        CommitOverlays();
        m_overlayCommitted = true;
    }

private:
    bool m_overlayCommitted = false;
    bool m_failureLogged = false;

    void TryStartAnalysis()
    {
        if (!sharedState || sharedState->GetFileLoadState() != LoadState::Succeeded) {
            return;
        }

        const auto range = sharedState->GetDataRange();
        const double isoValue = range[0] + (range[1] - range[0]) * 0.55;
        if (serviceA) {
            serviceA->SetIsoThreshold(isoValue);
        }

        SurfaceParams surfP;
        surfP.isoValue = static_cast<float>(isoValue);
        gapAnalysis->SetSurfaceParams(surfP);

        VoidDetectionParams voidP;
        voidP.grayMin = -0.2262536138296127f;
        voidP.grayMax = 0.15f;
        voidP.minVolumeMM3 = 0.0001;
        voidP.angleThresholdDeg = 30.0f;
        voidP.tensorWindowSize = 1;
        voidP.erosionIterations = 2;
        gapAnalysis->SetVoidParams(voidP);

        gapAnalysis->RunAsync();
    }

    static void AddMeshOverlay(
        const std::shared_ptr<MedicalVizService>& service,
        vtkSmartPointer<vtkPolyData> voidMesh)
    {
        if (!service || !voidMesh) {
            return;
        }

        auto overlay = std::make_shared<GapMeshOverlayStrategy>();
        overlay->SetInputData(voidMesh);
        service->AddOverlayStrategy(overlay);
    }

    static void AddSliceOverlay(
        const std::shared_ptr<MedicalVizService>& service,
        vtkSmartPointer<vtkImageData> labelImage,
        Orientation orientation)
    {
        if (!service || !labelImage) {
            return;
        }

        auto overlay = std::make_shared<GapSliceOverlayStrategy>(orientation);
        overlay->SetInputData(labelImage);
        service->AddOverlayStrategy(overlay);
    }

    void CommitOverlays()
    {
        auto voidMesh = gapAnalysis->BuildVoidMesh();
        auto labelImage = gapAnalysis->BuildLabelImage();

        const vtkIdType meshPoints = voidMesh ? voidMesh->GetNumberOfPoints() : 0;
        const vtkIdType meshCells = voidMesh ? voidMesh->GetNumberOfCells() : 0;
        if (voidMesh && meshPoints > 0 && meshCells > 0) {
            AddMeshOverlay(serviceA, voidMesh);
            AddMeshOverlay(serviceE, voidMesh);
        }
        else {
            std::cerr << "[Main] Gap Analysis produced no 3D void mesh overlay." << std::endl;
        }

        int labelDims[3] = { 0, 0, 0 };
        if (labelImage) {
            labelImage->GetDimensions(labelDims);
        }
        if (labelImage && labelDims[0] > 0 && labelDims[1] > 0 && labelDims[2] > 0) {
            AddSliceOverlay(serviceB, labelImage, Orientation::Top_down);
            AddSliceOverlay(serviceC, labelImage, Orientation::Front_back);
            AddSliceOverlay(serviceD, labelImage, Orientation::Left_right);
        }
        else {
            std::cerr << "[Main] Gap Analysis produced no 2D label overlay." << std::endl;
        }

        std::cout << "[Main] Gap Analysis overlay committed: mesh points = "
            << meshPoints << ", mesh cells = " << meshCells
            << ", label dims = " << labelDims[0] << "x" << labelDims[1] << "x" << labelDims[2]
            << std::endl;
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
    context->ApplyCameraStyle(cfg.preInitCfg.vizMode);
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
    auto orthogonalCropBridge = std::make_shared<OrthogonalCropInteractionBridgeService>();

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

    orthogonalCropBridge->SetDataManager(sharedDataMgr);
    orthogonalCropBridge->SetReferenceRenderService(serviceA);
    orthogonalCropBridge->SetReferenceRenderer(contextA->GetRenderer());
    orthogonalCropBridge->SetPreviewRenderServices({ serviceA, serviceB, serviceC, serviceD, serviceE });
    auto orthogonalCropSubmitWorkflow = std::make_shared<OrthogonalCropSubmitWorkflow>(
        orthogonalCropBridge,
        [serviceA](
            const float* data,
            const std::array<int, 3>& dims,
            const std::array<float, 3>& spacing,
            const std::array<float, 3>& origin,
            std::function<void(bool success)> onComplete) {
            return serviceA->ReloadFromBufferAsync(
                data,
                dims,
                spacing,
                origin,
                std::move(onComplete));
        },
        sharedDataMgr);

    // 3D窗口：设置参考切面可见（Composite 模式默认显示，纯 3D 模式无参考切面）
    serviceA->SetElementVisible(VisFlags::Planes3D, false);
    serviceE->SetElementVisible(VisFlags::Planes3D, false);

    // 3D 窗口：隐藏标尺
    serviceA->SetElementVisible(VisFlags::Ruler, false);
    serviceE->SetElementVisible(VisFlags::Ruler, false);

    // 2D 窗口：显示十字线
    serviceB->SetElementVisible(VisFlags::Crosshair, true);
    serviceC->SetElementVisible(VisFlags::Crosshair, true);
    serviceD->SetElementVisible(VisFlags::Crosshair, true);

    serviceA->LoadFileAsync(
        "F:\\data\\1000x1000x1000.raw",
        {0.02125, 0.02125, 0.02125},
        {0.0, 0.0, 0.0},
        [sharedState, sharedDataMgr, serviceA, imageAnalysis, orthogonalCropBridge](bool success)
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
                imageAnalysis->SaveHistogramImage(histogramFilePath, histogramBinCount);
            }
            else {
                std::cerr << "[Main] Histogram generation failed.\n";
            }

            orthogonalCropBridge->SetInputImage(sharedDataMgr->GetVtkImage());
            if (!orthogonalCropBridge->GetInputImage()) {
                std::cerr << "[Main] Orthogonal crop init failed: input image missing." << std::endl;
            }
            else {
                std::cout << "[Main] Orthogonal crop armed. Press O to toggle crop box, Esc to exit crop mode, press 1 toggle inside preview, press 2 toggle outside preview, press 3 toggle 3D outline/full preview, press Ctrl+3 to apply submit." << std::endl;
            }

            // GapAnalysis 的启动由 contextB 上的 Timer observer 统一处理，
            // 避免文件加载回调被其他窗口 service 先消费时漏启动算法。
        }
    );

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

    //// 只等待共享加载状态成功后按固定节奏持续调节等值面阈值并打印。
    //std::thread([sharedState, serviceA]() {
    //    using namespace std::chrono_literals;

    //    while (true) {
    //        const LoadState fileLoadState = sharedState->GetFileLoadState();
    //        if (fileLoadState == LoadState::Succeeded) {
    //            break;
    //        }
    //        if (fileLoadState == LoadState::Failed) {
    //            std::cerr << "[Iso Test] Volume data load failed; skip repeated iso threshold test." << std::endl;
    //            return;
    //        }
    //        std::this_thread::sleep_for(100ms);
    //    }

    //    const auto range = sharedState->GetDataRange();
    //    const double minIsoValue = range[0];
    //    const double maxIsoValue = range[1];
    //    const double isoSpan = maxIsoValue - minIsoValue;
    //    if (isoSpan <= 0.0) {
    //        std::cerr << "[Iso Test] Invalid scalar range; skip repeated iso threshold test." << std::endl;
    //        return;
    //    }

    //    int currentNormalizedStep = 0;
    //    double normalizedIsoValue = 0.0;
    //    double actualIsoValue = minIsoValue + isoSpan * normalizedIsoValue;
    //    serviceA->SetInteracting(true);
    //    serviceA->SetIsoThreshold(actualIsoValue);

    //    std::cout << "[Iso Test] Armed repeated iso threshold test: update about every 792 ms, normalized value steps by +0.1 and wraps after 1.0.\n"
    //        << "[Iso Test] Timer update: normalized iso = 0, actual iso = " << actualIsoValue
    //        << std::endl;


    //    while (true) {
    //        std::this_thread::sleep_for(std::chrono::milliseconds(792));
    //        currentNormalizedStep = (currentNormalizedStep + 1) % 11;
    //        normalizedIsoValue = static_cast<double>(currentNormalizedStep) * 0.05;
    //        actualIsoValue = minIsoValue + isoSpan * normalizedIsoValue;
    //        serviceA->SetInteracting(true);
    //        serviceA->SetIsoThreshold(actualIsoValue);
    //        std::cout << "[Iso Test] Timer update: normalized iso = " << normalizedIsoValue
    //            << ", actual iso = " << actualIsoValue << std::endl;
    //    }
    //}).detach();

    auto orthogonalCropHotkeyObserver = vtkSmartPointer<OrthogonalCropHotkeyObserver>::New();
    orthogonalCropHotkeyObserver->orthogonalCropBridge = orthogonalCropBridge;
    orthogonalCropHotkeyObserver->orthogonalCropSubmitWorkflow = orthogonalCropSubmitWorkflow;

    contextA->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, orthogonalCropHotkeyObserver);
    contextA->GetInteractor()->AddObserver(vtkCommand::KeyReleaseEvent, orthogonalCropHotkeyObserver);
    contextA->GetInteractor()->AddObserver(vtkCommand::CharEvent, orthogonalCropHotkeyObserver, 1.0f);
    contextB->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, orthogonalCropHotkeyObserver);
    contextB->GetInteractor()->AddObserver(vtkCommand::KeyReleaseEvent, orthogonalCropHotkeyObserver);
    contextB->GetInteractor()->AddObserver(vtkCommand::CharEvent, orthogonalCropHotkeyObserver, 1.0f);
    contextC->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, orthogonalCropHotkeyObserver);
    contextC->GetInteractor()->AddObserver(vtkCommand::KeyReleaseEvent, orthogonalCropHotkeyObserver);
    contextC->GetInteractor()->AddObserver(vtkCommand::CharEvent, orthogonalCropHotkeyObserver, 1.0f);
    contextD->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, orthogonalCropHotkeyObserver);
    contextD->GetInteractor()->AddObserver(vtkCommand::KeyReleaseEvent, orthogonalCropHotkeyObserver);
    contextD->GetInteractor()->AddObserver(vtkCommand::CharEvent, orthogonalCropHotkeyObserver, 1.0f);
    contextE->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, orthogonalCropHotkeyObserver);
    contextE->GetInteractor()->AddObserver(vtkCommand::KeyReleaseEvent, orthogonalCropHotkeyObserver);
    contextE->GetInteractor()->AddObserver(vtkCommand::CharEvent, orthogonalCropHotkeyObserver, 1.0f);

    //auto gapAnalysisOverlayObserver = vtkSmartPointer<GapAnalysisOverlayCommitObserver>::New();
    //gapAnalysisOverlayObserver->gapAnalysis = gapAnalysis;
    //gapAnalysisOverlayObserver->serviceA = serviceA;
    //gapAnalysisOverlayObserver->serviceB = serviceB;
    //gapAnalysisOverlayObserver->serviceC = serviceC;
    //gapAnalysisOverlayObserver->serviceD = serviceD;
    //gapAnalysisOverlayObserver->serviceE = serviceE;
    //gapAnalysisOverlayObserver->sharedState = sharedState;
    //contextB->GetInteractor()->AddObserver(vtkCommand::TimerEvent, gapAnalysisOverlayObserver, 0.2f);

    std::cout << "Application started. Loading data in background...\n"
        << "Controls: A/D = navigate slices | M = toggle model transform | D = distance measure | A = angle measure | O = toggle orthogonal crop box | Esc = exit crop mode | 1 = toggle inside preview | 2 = toggle outside preview | 3 = toggle 3D outline/full preview | Ctrl+3 = apply submit\n"
        << "Iso test: after load succeeds, main.cpp will update normalized iso by +0.1 every 24 timer ticks and print each change.\n";

    // contextB 持有主事件循环（其他窗口通过共享 Timer 驱动）
    contextB->Start();

    return 0;
}
