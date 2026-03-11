//// =====================================================================
////
//// 三阶段结构（保持不变）：
////   Phase 1 【前处理】  创建共享资源 + 通过 WindowConfig / PreInit_CommitConfig
////                       批量配置所有与数据无关的参数（含背景色）
////   Phase 2 【加载】    通过 IDataLoaderService 接口发起异步加载
////   Phase 3 【渲染骨架】InitInteractor + Start 进入消息循环
////
////   • PreInitConfig 增加 bgColor / hasBgColor（前处理背景色）
////   • LoadFileAsync 回调改为数据相关的后处理业务（等值面阈值推算）
////   • 回调内明确注释：在后台线程，只允许操作 SharedState
////   • 加载失败时通过 NotifyLoadFailed → PostData_HandleLoadFailed 处理
////   • IDataLoaderService 增加 GetLoadState() 可用于主线程状态查询
////   • main.cpp 中的 InitInteractor() 调用方式明确分离（接口显式）
//// =====================================================================
//
//#include <vtkAutoInit.h>
//#include <vtkSMPTools.h>
//
//VTK_MODULE_INIT(vtkRenderingOpenGL2);
//VTK_MODULE_INIT(vtkInteractionStyle);
//VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
//VTK_MODULE_INIT(vtkRenderingFreeType);
//
//#include "AppTypes.h"
//#include "AppInterfaces.h"
//#include "AppState.h"
//#include "AppService.h"
//#include "DataManager.h"
//#include "VolumeAnalysisService.h"
//#include "StdRenderContext.h"
//
//#include <iostream>
//#include <vector>
//
//// ─────────────────────────────────────────────────────────────────────
//// BuildWindow：根据 WindowConfig 创建 Service + Context，完成前处理配置
////
//// 前处理职责（数据无关，Phase 1 调用）：
////   1. BindService → Initialize → 注册 Observer
////   2. PreInit_CommitConfig → 批量写 SharedState（材质/TF/等值面/背景色）
////   3. 设置窗口属性（标题/尺寸/位置/交互模式/坐标轴）
////   4. PreInit_SetBackground → 直接写渲染器背景色
////
//// 返回 {service, context} pair
//// ──────────────────────────���──────────────────────────────────────────
//static std::pair<
//    std::shared_ptr<MedicalVizService>,
//    std::shared_ptr<StdRenderContext>>
//    BuildWindow(
//        const WindowConfig& cfg,
//        std::shared_ptr<AbstractDataManager>    dataMgr,
//        std::shared_ptr<SharedInteractionState> sharedState)
//{
//    auto service = std::make_shared<MedicalVizService>(dataMgr, sharedState);
//    auto context = std::make_shared<StdRenderContext>();
//
//    // ── 步骤1：BindService（触发 Initialize → 注册 Observer）──────
//    context->BindService(service);
//
//    // ── 步骤2：批量提交前处理配置（一次锁 + 一次广播）────────────
//    service->PreInit_CommitConfig(cfg.preInitCfg);
//
//    // ── 步骤3：窗口属性（纯渲染上下文配置，数据无关）─────────────
//    context->SetWindowTitle(cfg.title);
//    context->SetWindowSize(cfg.width, cfg.height);
//    context->SetWindowPosition(cfg.posX, cfg.posY);
//    context->SetInteractionMode(cfg.vizMode);
//    if (cfg.showAxes)
//        context->ToggleOrientationAxes(true);
//
//    // ── 步骤4：背景色（前处理：直接应用到渲染器，数据无关）───────
//    if (cfg.preInitCfg.hasBgColor)
//        service->PreInit_SetBackground(cfg.preInitCfg.bgColor);
//
//    return { service, context };
//}
//
//int main()
//{
//    vtkSMPTools::Initialize();
//    // ── 共享资源 ──────────────────────────────────────────────────
//    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
//    auto sharedState = std::make_shared<SharedInteractionState>();
//    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);
//
//    // ── 传输函数（数据无关，前处理阶段安全）──────────────────────
//    std::vector<TFNode> volTF = {
//        { 0.00, 0.0, 0.0, 0.0, 0.0 },
//        { 0.50, 0.0, 0.0, 0.5, 0.0 },
//        { 0.85, 0.8, 0.0, 0.5, 0.0 },
//        { 1.00, 1.0, 0.0, 0.5, 0.0 },
//    };
//
//    // ── 窗口配置表（前处理参数全部在此集中声明）──────────────────
//
//    // 窗口 A：等值面 + 切片参考平面
//    WindowConfig cfgA;
//    cfgA.title = "Window A: Composite IsoSurface";
//    cfgA.width = 600; cfgA.height = 600;
//    cfgA.posX = 50;  cfgA.posY = 50;
//    cfgA.vizMode = VizMode::CompositeIsoSurface;
//    cfgA.showAxes = true;
//    cfgA.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
//    cfgA.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 1.0, false };
//    cfgA.preInitCfg.bgColor = { 0.05, 0.05, 0.05 }; // 深灰背景
//    cfgA.preInitCfg.hasBgColor = true;
//
//    // 窗口 E：体渲染 + 切片参考平面
//    WindowConfig cfgE;
//    cfgE.title = "Window E: Composite Volume";
//    cfgE.width = 600; cfgE.height = 600;
//    cfgE.posX = 660; cfgE.posY = 50;
//    cfgE.vizMode = VizMode::CompositeVolume;
//    cfgE.preInitCfg.vizMode = VizMode::CompositeVolume;
//    cfgE.preInitCfg.tfNodes = volTF;
//    cfgE.preInitCfg.hasTF = true;
//    cfgE.preInitCfg.bgColor = { 0.08, 0.08, 0.12 }; // 深蓝背景
//    cfgE.preInitCfg.hasBgColor = true;
//
//    // ── 窗口 B：Axial 切片（2D，默认软组织窗）────────────────────
//    WindowConfig cfgB;
//    cfgB.title = "Window B: Axial Slice";
//    cfgB.width = 400; cfgB.height = 400;
//    cfgB.posX = 50;  cfgB.posY = 660;
//    cfgB.vizMode = VizMode::SliceAxial;
//    cfgB.preInitCfg.vizMode = VizMode::SliceAxial;
//    cfgB.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
//    cfgB.preInitCfg.hasBgColor = true;
//    cfgB.preInitCfg.windowLevel = { 400.0, 40.0 };   // ★ WW=400, WC=40
//    cfgB.preInitCfg.hasWindowLevel = true;
//
//    // ── 窗口 C：Coronal 切片 ─────────────────────────────────────
//    WindowConfig cfgC;
//    cfgC.title = "Window C: Coronal Slice";
//    cfgC.width = 400; cfgC.height = 400;
//    cfgC.posX = 460; cfgC.posY = 660;
//    cfgC.vizMode = VizMode::SliceCoronal;
//    cfgC.preInitCfg.vizMode = VizMode::SliceCoronal;
//    cfgC.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
//    cfgC.preInitCfg.hasBgColor = true;
//    cfgC.preInitCfg.windowLevel = { 400.0, 40.0 };   // ★
//    cfgC.preInitCfg.hasWindowLevel = true;
//
//    // ── 窗口 D：Sagittal 切片 ────────────────────────────────────
//    WindowConfig cfgD;
//    cfgD.title = "Window D: Sagittal Slice";
//    cfgD.width = 400; cfgD.height = 400;
//    cfgD.posX = 870; cfgD.posY = 660;
//    cfgD.vizMode = VizMode::SliceSagittal;
//    cfgD.preInitCfg.vizMode = VizMode::SliceSagittal;
//    cfgD.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
//    cfgD.preInitCfg.hasBgColor = true;
//    cfgD.preInitCfg.windowLevel = { 400.0, 40.0 };   // ★
//    cfgD.preInitCfg.hasWindowLevel = true;
//
//    // ── 批量建窗（前处理完成）────────────────────────────────────
//    auto [serviceA, contextA] = BuildWindow(cfgA, sharedDataMgr, sharedState);
//    auto [serviceE, contextE] = BuildWindow(cfgE, sharedDataMgr, sharedState);
//    auto [serviceB, contextB] = BuildWindow(cfgB, sharedDataMgr, sharedState);
//    auto [serviceC, contextC] = BuildWindow(cfgC, sharedDataMgr, sharedState);
//    auto [serviceD, contextD] = BuildWindow(cfgD, sharedDataMgr, sharedState);
//
//    // 3D窗口：隐藏剪切平面（Composite 模式默认显示，等值面/体渲染模式无剪切平面）
//    contextA->SetElementVisible(VisFlags::ClipPlanes, false);
//    contextE->SetElementVisible(VisFlags::ClipPlanes, false);
//
//    // 3D 窗口：隐藏标尺
//    contextA->SetElementVisible(VisFlags::RulerAxes, false);
//    contextE->SetElementVisible(VisFlags::RulerAxes, false);
//
//    // 2D 窗口：隐藏十字测量线
//    contextB->SetElementVisible(VisFlags::Crosshair, false);
//    contextC->SetElementVisible(VisFlags::Crosshair, false);
//    contextD->SetElementVisible(VisFlags::Crosshair, false);
//
//    IDataLoaderService* loader = serviceA.get();
//    loader->LoadFileAsync(
//        "D:\\CT-1209\\data\\700x1358x1252.raw",
//        [sharedState, serviceA](bool success)
//        {
//            // !! 后台线程 !! 只操作 SharedState��内部有 mutex）
//            if (!success) {
//                // 加载失败由 NotifyLoadFailed 广播，
//                // PostData_HandleLoadFailed 在主线程处理，此处仅记录日志
//                std::cerr << "[onComplete] Volume data load failed.\n";
//                return;
//            }
//
//            // 后处理业务：数据就绪后，基于实际数据范围推算等值面阈值
//            // 此操作数据相关，必须在加载完成后执行（不能在前处理阶段）
//            auto range = sharedState->GetDataRange();
//            double isoVal = range[0] + (range[1] - range[0]) * 0.35;
//            serviceA->PreInit_SetIsoThreshold(isoVal);  // 线程安全：写 SharedState
//
//            // ── 后处理 B：★ 切片 WW/WC 自动推算（基于实际数据范围）
//                        // 取数据范围中央 60% 作为窗口宽度，中点为窗位
//            double ww = (range[1] - range[0]) * 0.6;
//            double wc = range[0] + (range[1] - range[0]) * 0.5;
//            serviceA->PreInit_SetWindowLevel(ww, wc);
//
//            std::cout << "[onComplete] Data loaded."
//                << " IsoThreshold=" << isoVal
//                << " WW=" << ww << " WC=" << wc << "\n";
//        }
//    );
//
//    contextA->Render();
//    contextB->Render();
//    contextC->Render();
//    contextD->Render();
//    contextE->Render();
//
//    contextA->InitInteractor();
//    contextB->InitInteractor();
//    contextC->InitInteractor();
//    contextD->InitInteractor();
//    contextE->InitInteractor();
//
//    std::cout << "Application started. Loading data in background...\n"
//        << "Controls: A/D = navigate slices | M = toggle model transform\n";
//
//    // contextB 持有主事件循环（其他窗口通过共享 Timer 驱动）
//    contextB->Start();
//
//    return 0;
//}

// =====================================================================
// 三阶段结构（保持不变）
// GapAnalysis 增量：
//   窗口 F — 空洞 3D 等值面（IsoSurfaceStrategy，接受 vtkPolyData）
//   窗口 G — 空洞 Axial 切片（SliceStrategy，接受 vtkImageData 标签体）
//   按 G 键主动触发分析，Timer 心跳在主线程消费结果并推送两个窗口
// =====================================================================
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
#include "IsoSurfaceStrategy.h"
#include "SliceStrategy.h"
#include "GapAnalysisService.h"

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkInteractorStyleImage.h>

#include <iostream>
#include <vector>

// ─────────────────────────────────────────────────────────────────────
// BuildWindow（原有，不变）
// ─────────────────────────────────────────────────────────────────────
static std::pair<
    std::shared_ptr<MedicalVizService>,
    std::shared_ptr<StdRenderContext>>
    BuildWindow(
        const WindowConfig& cfg,
        std::shared_ptr<AbstractDataManager>    dataMgr,
        std::shared_ptr<SharedInteractionState> sharedState)
{
    auto service = std::make_shared<MedicalVizService>(dataMgr, sharedState);
    auto context = std::make_shared<StdRenderContext>();

    context->BindService(service);
    service->PreInit_CommitConfig(cfg.preInitCfg);
    context->SetWindowTitle(cfg.title);
    context->SetWindowSize(cfg.width, cfg.height);
    context->SetWindowPosition(cfg.posX, cfg.posY);
    context->SetInteractionMode(cfg.vizMode);
    if (cfg.showAxes)
        context->ToggleOrientationAxes(true);
    if (cfg.preInitCfg.hasBgColor)
        service->PreInit_SetBackground(cfg.preInitCfg.bgColor);

    return { service, context };
}

// ─────────────────────────────────────────────────────────────────────
// GapResultWindow — 空洞分析结果展示窗口
//
// 持有两个独立的 renderer（viewport 分割），一个窗口内同时展示：
//   Left  (viewport 0~0.5)  — Axial 切片（SliceStrategy + vtkImageData 标签体）
//   Right (viewport 0.5~1)  — 3D 等值面（IsoSurfaceStrategy + vtkPolyData）
//
// 不接入 SharedInteractionState，与主数据管线完全独立。
// ─────────────────────────────────────────────────────────────────────
class GapResultWindow {
public:
    GapResultWindow(int posX, int posY, int width, int height)
    {
        // ── 共用 RenderWindow ─────────────────────────────────────
        m_renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
        m_renderWindow->SetSize(width, height);
        m_renderWindow->SetPosition(posX, posY);
        m_renderWindow->SetWindowName("Window F: Gap Analysis — Slice (left) | 3D (right)");

        // ── 左：切片 renderer（viewport 左半）────────────────────
        m_rendererSlice = vtkSmartPointer<vtkRenderer>::New();
        m_rendererSlice->SetViewport(0.0, 0.0, 0.5, 1.0);
        m_rendererSlice->SetBackground(0.0, 0.0, 0.0);
        m_renderWindow->AddRenderer(m_rendererSlice);

        // ── 右：3D renderer（viewport 右半）──────────────────────
        m_renderer3D = vtkSmartPointer<vtkRenderer>::New();
        m_renderer3D->SetViewport(0.5, 0.0, 1.0, 1.0);
        m_renderer3D->SetBackground(0.05, 0.08, 0.12);
        m_renderWindow->AddRenderer(m_renderer3D);

        // ── 策略层（与主窗口完全相同的策略类）────────────────────
        m_sliceStrategy = std::make_shared<SliceStrategy>(Orientation::AXIAL);
        m_sliceStrategy->Attach(m_rendererSlice);

        m_isoStrategy = std::make_shared<IsoSurfaceStrategy>();
        m_isoStrategy->Attach(m_renderer3D);
        m_isoStrategy->SetupCamera(m_renderer3D);

        // ── Interactor ────────────────────────────────────────────
        m_interactor = vtkSmartPointer<vtkRenderWindowInteractor>::New();
        m_interactor->SetRenderWindow(m_renderWindow);

        // 3D 侧默认 Trackball，切片侧点击后切换（此处简化为统一 Trackball）
        auto style3D = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
        m_interactor->SetInteractorStyle(style3D);
    }

    // ── 主线程调用：推送空洞数据，同时刷新切片和 3D 窗口 ─────────
    // labelImg → SliceStrategy（空洞标签体，按灰度显示空洞区域）
    // voidMesh → IsoSurfaceStrategy（空洞等值面）
    void Show(vtkSmartPointer<vtkImageData>  labelImg,
        vtkSmartPointer<vtkPolyData>   voidMesh)
    {
        // ── 切片侧：标签体作为灰度图显示（label>0 的区域呈亮色）───
        if (labelImg && labelImg->GetNumberOfPoints() > 0) {
            m_sliceStrategy->SetInputData(labelImg);
            m_sliceStrategy->SetupCamera(m_rendererSlice);

            // 构造 RenderParams 给 SliceStrategy 初始化 LUT
            // WW/WC 设为 [0, maxLabel]，让不同编号的空洞呈不同灰度
            double scalarRange[2];
            labelImg->GetScalarRange(scalarRange);
            RenderParams rp;
            rp.windowLevel.windowWidth = scalarRange[1] - scalarRange[0] + 1.0;
            rp.windowLevel.windowCenter = (scalarRange[0] + scalarRange[1]) * 0.5;
            rp.scalarRange[0] = scalarRange[0];
            rp.scalarRange[0] = scalarRange[1];
            rp.material.opacity = 1.0;
            m_sliceStrategy->UpdateVisuals(rp, UpdateFlags::WindowLevel);
        }

        // ── 3D 侧：空洞等值面 Mesh ────────────────────────────────
        if (voidMesh && voidMesh->GetNumberOfPoints() > 0) {
            m_isoStrategy->SetInputData(voidMesh);
            m_renderer3D->ResetCamera();
        }

        m_renderWindow->Render();

        std::cout << "[GapResultWindow] Displayed."
            << " VoidMesh points=" << (voidMesh ? voidMesh->GetNumberOfPoints() : 0)
            << " LabelImg dims=";
        if (labelImg) {
            int d[3]; labelImg->GetDimensions(d);
            std::cout << d[0] << "x" << d[1] << "x" << d[2];
        }
        else { std::cout << "none"; }
        std::cout << "\n";
    }

    void InitInteractor() {
        if (!m_interactor->GetInitialized())
            m_interactor->Initialize();
    }

    vtkRenderWindowInteractor* GetInteractor() const {
        return m_interactor.GetPointer();
    }

private:
    vtkSmartPointer<vtkRenderWindow>              m_renderWindow;
    vtkSmartPointer<vtkRenderer>                  m_rendererSlice;
    vtkSmartPointer<vtkRenderer>                  m_renderer3D;
    vtkSmartPointer<vtkRenderWindowInteractor>    m_interactor;
    std::shared_ptr<SliceStrategy>                m_sliceStrategy;
    std::shared_ptr<IsoSurfaceStrategy>           m_isoStrategy;
};

// ─────────────────────────────────────────────────────────────────────
// Timer 轮询用的 clientData
// ─────────────────────────────────────────────────────────────────────
struct GapTimerClientData {
    GapAnalysisService* gap;
    GapResultWindow* resultWin;
    bool                consumed;
};

// G 键用的 clientData（同时持有 gap 和 timerData，用于重置 consumed）
struct GapKeyClientData {
    GapAnalysisService* gap;
    GapTimerClientData* timerData;
};

int main()
{
    vtkSMPTools::Initialize();

    // ─────────────────────────────────────────────────────────────────
    // Phase 1 【前处理】
    // ─────────────────────────────────────────────────────────────────
    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedState = std::make_shared<SharedInteractionState>();
    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);

    // ── GapAnalysisService（Phase 1）─────────────────────────────
    auto gapAnalysis = std::make_shared<GapAnalysisService>(sharedDataMgr);
    {
        SurfaceParams surfP;
        surfP.isoValue = 1200.0f;
        surfP.background = 200.0f;
        surfP.material = 2000.0f;

        VoidDetectionParams voidP;
        voidP.grayMin = 0.0f;
        voidP.grayMax = 800.0f;
        voidP.minVolumeMM3 = 0.5;
        voidP.angleThresholdDeg = 30.0f;
        voidP.tensorWindowSize = 1;

        gapAnalysis->GapPreInit_SetSurfaceParams(surfP);
        gapAnalysis->GapPreInit_SetVoidParams(voidP);
    }

    // ── 传输函数 ──────────────────────────────────────────────────
    std::vector<TFNode> volTF = {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };

    // 窗口 A
    WindowConfig cfgA;
    cfgA.title = "Window A: Composite IsoSurface";
    cfgA.width = 600; cfgA.height = 600;
    cfgA.posX = 50;  cfgA.posY = 50;
    cfgA.vizMode = VizMode::CompositeIsoSurface;
    cfgA.showAxes = true;
    cfgA.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    cfgA.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 1.0, false };
    cfgA.preInitCfg.bgColor = { 0.05, 0.05, 0.05 };
    cfgA.preInitCfg.hasBgColor = true;

    // 窗口 E
    WindowConfig cfgE;
    cfgE.title = "Window E: Composite Volume";
    cfgE.width = 600; cfgE.height = 600;
    cfgE.posX = 660; cfgE.posY = 50;
    cfgE.vizMode = VizMode::CompositeVolume;
    cfgE.preInitCfg.vizMode = VizMode::CompositeVolume;
    cfgE.preInitCfg.tfNodes = volTF;
    cfgE.preInitCfg.hasTF = true;
    cfgE.preInitCfg.bgColor = { 0.08, 0.08, 0.12 };
    cfgE.preInitCfg.hasBgColor = true;

    // 窗口 B
    WindowConfig cfgB;
    cfgB.title = "Window B: Axial Slice";
    cfgB.width = 400; cfgB.height = 400;
    cfgB.posX = 50;  cfgB.posY = 660;
    cfgB.vizMode = VizMode::SliceAxial;
    cfgB.preInitCfg.vizMode = VizMode::SliceAxial;
    cfgB.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgB.preInitCfg.hasBgColor = true;
    cfgB.preInitCfg.windowLevel = { 400.0, 40.0 };
    cfgB.preInitCfg.hasWindowLevel = true;

    // 窗口 C
    WindowConfig cfgC;
    cfgC.title = "Window C: Coronal Slice";
    cfgC.width = 400; cfgC.height = 400;
    cfgC.posX = 460; cfgC.posY = 660;
    cfgC.vizMode = VizMode::SliceCoronal;
    cfgC.preInitCfg.vizMode = VizMode::SliceCoronal;
    cfgC.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgC.preInitCfg.hasBgColor = true;
    cfgC.preInitCfg.windowLevel = { 400.0, 40.0 };
    cfgC.preInitCfg.hasWindowLevel = true;

    // 窗口 D
    WindowConfig cfgD;
    cfgD.title = "Window D: Sagittal Slice";
    cfgD.width = 400; cfgD.height = 400;
    cfgD.posX = 870; cfgD.posY = 660;
    cfgD.vizMode = VizMode::SliceSagittal;
    cfgD.preInitCfg.vizMode = VizMode::SliceSagittal;
    cfgD.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgD.preInitCfg.hasBgColor = true;
    cfgD.preInitCfg.windowLevel = { 400.0, 40.0 };
    cfgD.preInitCfg.hasWindowLevel = true;

    auto [serviceA, contextA] = BuildWindow(cfgA, sharedDataMgr, sharedState);
    auto [serviceE, contextE] = BuildWindow(cfgE, sharedDataMgr, sharedState);
    auto [serviceB, contextB] = BuildWindow(cfgB, sharedDataMgr, sharedState);
    auto [serviceC, contextC] = BuildWindow(cfgC, sharedDataMgr, sharedState);
    auto [serviceD, contextD] = BuildWindow(cfgD, sharedDataMgr, sharedState);

    contextA->SetElementVisible(VisFlags::ClipPlanes, false);
    contextE->SetElementVisible(VisFlags::ClipPlanes, false);
    contextA->SetElementVisible(VisFlags::RulerAxes, false);
    contextE->SetElementVisible(VisFlags::RulerAxes, false);
    contextB->SetElementVisible(VisFlags::Crosshair, false);
    contextC->SetElementVisible(VisFlags::Crosshair, false);
    contextD->SetElementVisible(VisFlags::Crosshair, false);

    // ── 窗口 F：间隙分析结果（左=Axial切片，右=3D等值面）──────────
    // 位置：E 右侧，宽度 1200 以容纳左右两个 viewport
    auto gapResultWin = std::make_shared<GapResultWindow>(
        1270, 50,    // posX, posY
        1200, 600    // width, height（左右各 600）
    );

    // ─────────────────────────────────────────────────────────────────
    // Phase 2 【加载】
    // ─────────────────────────────────────────────────────────────────
    IDataLoaderService* loader = serviceA.get();
    loader->LoadFileAsync(
        "D:\\CT-1209\\data\\700x1358x1252.raw",
        [sharedState, serviceA, gapAnalysis](bool success)
        {
            // !! 后台线程 !! 只操作 SharedState
            if (!success) {
                std::cerr << "[onComplete] Volume data load failed.\n";
                return;
            }

            auto range = sharedState->GetDataRange();
            double isoVal = range[0] + (range[1] - range[0]) * 0.35;
            serviceA->PreInit_SetIsoThreshold(isoVal);

            double ww = (range[1] - range[0]) * 0.6;
            double wc = range[0] + (range[1] - range[0]) * 0.5;
            sharedState->SetWindowLevel(ww, wc);

            std::cout << "[onComplete] Data loaded."
                << " IsoThreshold=" << isoVal
                << " WW=" << ww << " WC=" << wc << "\n";

            // GapAnalysis：修正 isoValue 参数（线程安全前处理写操作）
            SurfaceParams updatedSurf;
            updatedSurf.isoValue = static_cast<float>(isoVal);
            updatedSurf.background = static_cast<float>(range[0]);
            updatedSurf.material = static_cast<float>(range[1]);
            gapAnalysis->GapPreInit_SetSurfaceParams(updatedSurf);

            std::cout << "[onComplete] Gap analysis ready."
                << " Press G to run gap analysis.\n";
        }
    );

    // ─────────────────────────────────────────────────────────────────
    // Phase 3 【渲染骨架】
    // ─────────────────────────────────────────────────────────────────
    contextA->Render();
    contextB->Render();
    contextC->Render();
    contextD->Render();
    contextE->Render();

    contextA->InitInteractor();
    contextB->InitInteractor();
    contextC->InitInteractor();
    contextD->InitInteractor();
    contextE->InitInteractor();
    gapResultWin->InitInteractor();

    // ── Timer 数据（在 Start 之前初始化，生命周期覆盖事件循环）────
    auto gapTimerData = std::make_unique<GapTimerClientData>(
        GapTimerClientData{ gapAnalysis.get(), gapResultWin.get(), false });

    // ── G 键：触发分析 + 重置消费标记 ───────────────────────────────
    auto gapKeyData = std::make_unique<GapKeyClientData>(
        GapKeyClientData{ gapAnalysis.get(), gapTimerData.get() });

    auto gapKeyCmd = vtkSmartPointer<vtkCallbackCommand>::New();
    gapKeyCmd->SetClientData(gapKeyData.get());
    gapKeyCmd->SetCallback(
        [](vtkObject* caller, long unsigned int eventId,
            void* clientData, void* /*callData*/)
        {
            if (eventId != vtkCommand::KeyPressEvent) return;
            auto* iren = vtkRenderWindowInteractor::SafeDownCast(caller);
            auto* d = static_cast<GapKeyClientData*>(clientData);
            if (!iren || !d) return;

            const char key = iren->GetKeyCode();
            if (key != 'g' && key != 'G') return;

            if (d->gap->GetAnalysisState() == GapAnalysisState::Running) {
                std::cout << "[GapAnalysis] Already running, please wait...\n";
                return;
            }

            // 重置消费标记，允许新结果推送到窗口 F
            d->timerData->consumed = false;

            std::cout << "[GapAnalysis] Starting...\n" << std::flush;
            d->gap->RunAsync([](bool ok) {
                // !! 后台线程 !! 只写 std::cout，不碰 VTK 对象
                if (ok)
                    std::cout << "[GapAnalysis] Done."
                    << " Window F will update automatically.\n"
                    << std::flush;
                else
                    std::cerr << "[GapAnalysis] Failed.\n" << std::flush;
                });
        });

    // 注册到所有窗口，任意窗口有焦点时均可按 G
    contextA->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, gapKeyCmd, 1.0f);
    contextB->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, gapKeyCmd, 1.0f);
    contextC->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, gapKeyCmd, 1.0f);
    contextD->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, gapKeyCmd, 1.0f);
    contextE->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, gapKeyCmd, 1.0f);

    // ── Timer 轮询：分析完成 → 主线程构建 labelImg + voidMesh → Show
    //
    // 对齐 PostData_RebuildPipeline 的心跳驱动模式：
    //   • BuildVoidMesh() / BuildLabelImage() 必须在主线程调用（VTK 非线程安全）
    //   • consumed 防止重复推送同一次结果
    //   • 优先级 0.4，低于现有 TimerHandler（1.0），不干扰主渲染心跳
    auto gapTimerCmd = vtkSmartPointer<vtkCallbackCommand>::New();
    gapTimerCmd->SetClientData(gapTimerData.get());
    gapTimerCmd->SetCallback(
        [](vtkObject* /*caller*/, long unsigned int eventId,
            void* clientData, void* /*callData*/)
        {
            if (eventId != vtkCommand::TimerEvent) return;
            auto* d = static_cast<GapTimerClientData*>(clientData);
            if (!d || d->consumed) return;
            if (d->gap->GetAnalysisState() != GapAnalysisState::Succeeded) return;

            // ── 主线程消费：同时构建切片数据和 3D Mesh ───────────
            auto labelImg = d->gap->BuildLabelImage();   // → 切片窗
            auto voidMesh = d->gap->BuildVoidMesh();     // → 3D 窗

            d->resultWin->Show(labelImg, voidMesh);
            d->consumed = true;
        });

    contextB->GetInteractor()->AddObserver(
        vtkCommand::TimerEvent, gapTimerCmd, 0.4f);

    std::cout << "Application started. Loading data in background...\n"
        << "Controls: A/D = navigate slices | M = toggle model transform\n"
        << "          G   = run gap analysis (after data loaded)\n"
        << "          Result: Window F left=Axial slice, right=3D voids\n";

    // contextB 持有主事件循环
    contextB->Start();

    return 0;
}