// =====================================================================
//
// 三阶段结构（保持不变）：
//   Phase 1 【前处理】  创建共享资源 + 通过 WindowConfig / PreInit_CommitConfig
//                       批量配置所有与数据无关的参数（含背景色）
//   Phase 2 【加载】    通过 IDataLoaderService 接口发起异步加载
//   Phase 3 【渲染骨架】InitInteractor + Start 进入消息循环
//
//   • PreInitConfig 增加 bgColor / hasBgColor（前处理背景色）
//   • LoadFileAsync 回调改为数据相关的后处理业务（等值面阈值推算）
//   • 回调内明确注释：在后台线程，只允许操作 SharedState
//   • 加载失败时通过 NotifyLoadFailed → PostData_HandleLoadFailed 处理
//   • IDataLoaderService 增加 GetLoadState() 可用于主线程状态查询
//   • main.cpp 中的 InitInteractor() 调用方式明确分离（接口显式）
// =====================================================================

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

#include <iostream>
#include <vector>

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

    // ── 步骤1：BindService（触发 Initialize → 注册 Observer）──────
    context->BindService(service);

    // ── 步骤2：批量提交前处理配置（一次锁 + 一次广播）────────────
    service->PreInit_CommitConfig(cfg.preInitCfg);

    // ── 步骤3：窗口属性（纯渲染上下文配置，数据无关）─────────────
    context->SetWindowTitle(cfg.title);
    context->SetWindowSize(cfg.width, cfg.height);
    context->SetWindowPosition(cfg.posX, cfg.posY);
    context->SetInteractionMode(cfg.vizMode);
    if (cfg.showAxes)
        context->ToggleOrientationAxes(true);

    // ── 步骤4：背景色（前处理：直接应用到渲染器，数据无关）───────
    if (cfg.preInitCfg.hasBgColor)
        service->PreInit_SetBackground(cfg.preInitCfg.bgColor);

    return { service, context };
}

int main()
{
    vtkSMPTools::Initialize();
    // ── 共享资源 ──────────────────────────────────────────────────
    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedState = std::make_shared<SharedInteractionState>();
    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);

    // ── 传输函数（数据无关，前处理阶段安全）──────────────────────
    std::vector<TFNode> volTF = {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };

    // ── 窗口配置表（前处理参数全部在此集中声明）──────────────────

    // 窗口 A：等值面 + 切片参考平面
    WindowConfig cfgA;
    cfgA.title = "Window A: Composite IsoSurface";
    cfgA.width = 600; cfgA.height = 600;
    cfgA.posX = 50;  cfgA.posY = 50;
    cfgA.vizMode = VizMode::CompositeIsoSurface;
    cfgA.showAxes = true;
    cfgA.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    cfgA.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 1.0, false };
    cfgA.preInitCfg.bgColor = { 0.05, 0.05, 0.05 }; // 深灰背景
    cfgA.preInitCfg.hasBgColor = true;

    // 窗口 E：体渲染 + 切片参考平面
    WindowConfig cfgE;
    cfgE.title = "Window E: Composite Volume";
    cfgE.width = 600; cfgE.height = 600;
    cfgE.posX = 660; cfgE.posY = 50;
    cfgE.vizMode = VizMode::CompositeVolume;
    cfgE.preInitCfg.vizMode = VizMode::CompositeVolume;
    cfgE.preInitCfg.tfNodes = volTF;
    cfgE.preInitCfg.hasTF = true;
    cfgE.preInitCfg.bgColor = { 0.08, 0.08, 0.12 }; // 深蓝背景
    cfgE.preInitCfg.hasBgColor = true;

    // ── 窗口 B：Axial 切片（2D，默认软组织窗）────────────────────
    WindowConfig cfgB;
    cfgB.title = "Window B: Axial Slice";
    cfgB.width = 400; cfgB.height = 400;
    cfgB.posX = 50;  cfgB.posY = 660;
    cfgB.vizMode = VizMode::SliceAxial;
    cfgB.preInitCfg.vizMode = VizMode::SliceAxial;
    cfgB.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgB.preInitCfg.hasBgColor = true;
    cfgB.preInitCfg.windowLevel = { 400.0, 40.0 };   // ★ WW=400, WC=40
    cfgB.preInitCfg.hasWindowLevel = true;

    // ── 窗口 C：Coronal 切片 ─────────────────────────────────────
    WindowConfig cfgC;
    cfgC.title = "Window C: Coronal Slice";
    cfgC.width = 400; cfgC.height = 400;
    cfgC.posX = 460; cfgC.posY = 660;
    cfgC.vizMode = VizMode::SliceCoronal;
    cfgC.preInitCfg.vizMode = VizMode::SliceCoronal;
    cfgC.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgC.preInitCfg.hasBgColor = true;
    cfgC.preInitCfg.windowLevel = { 400.0, 40.0 };   // ★
    cfgC.preInitCfg.hasWindowLevel = true;

    // ── 窗口 D：Sagittal 切片 ────────────────────────────────────
    WindowConfig cfgD;
    cfgD.title = "Window D: Sagittal Slice";
    cfgD.width = 400; cfgD.height = 400;
    cfgD.posX = 870; cfgD.posY = 660;
    cfgD.vizMode = VizMode::SliceSagittal;
    cfgD.preInitCfg.vizMode = VizMode::SliceSagittal;
    cfgD.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgD.preInitCfg.hasBgColor = true;
    cfgD.preInitCfg.windowLevel = { 400.0, 40.0 };   // ★
    cfgD.preInitCfg.hasWindowLevel = true;

    // ── 批量建窗（前处理完成）────────────────────────────────────
    auto [serviceA, contextA] = BuildWindow(cfgA, sharedDataMgr, sharedState);
    auto [serviceE, contextE] = BuildWindow(cfgE, sharedDataMgr, sharedState);
    auto [serviceB, contextB] = BuildWindow(cfgB, sharedDataMgr, sharedState);
    auto [serviceC, contextC] = BuildWindow(cfgC, sharedDataMgr, sharedState);
    auto [serviceD, contextD] = BuildWindow(cfgD, sharedDataMgr, sharedState);

    // 3D窗口：隐藏剪切平面（Composite 模式默认显示，等值面/体渲染模式无剪切平面）
    contextA->SetElementVisible(VisFlags::ClipPlanes, true);
    contextE->SetElementVisible(VisFlags::ClipPlanes, true);

    // 3D 窗口：隐藏标尺
    contextA->SetElementVisible(VisFlags::RulerAxes, true);
    contextE->SetElementVisible(VisFlags::RulerAxes, true);

    // 2D 窗口：隐藏十字测量线
    contextB->SetElementVisible(VisFlags::Crosshair, true);
    contextC->SetElementVisible(VisFlags::Crosshair, true);
    contextD->SetElementVisible(VisFlags::Crosshair, true);

    IDataLoaderService* loader = serviceA.get();
    loader->LoadFileAsync(
        "D:\\CT-1209\\data\\1000x1000x1000.raw",
        [sharedState, serviceA](bool success)
        {
            // !! 后台线程 !! 只操作 SharedState��内部有 mutex）
            if (!success) {
                // 加载失败由 NotifyLoadFailed 广播，
                // PostData_HandleLoadFailed 在主线程处理，此处仅记录日志
                std::cerr << "[onComplete] Volume data load failed.\n";
                return;
            }

            // 后处理业务：数据就绪后，基于实际数据范围推算等值面阈值
            // 此操作数据相关，必须在加载完成后执行（不能在前处理阶段）
            auto range = sharedState->GetDataRange();
            double isoVal = range[0] + (range[1] - range[0]) * 0.35;
            serviceA->PreInit_SetIsoThreshold(isoVal);  // 线程安全：写 SharedState

            // ── 后处理 B：★ 切片 WW/WC 自动推算（基于实际数据范围）
                        // 取数据范围中央 60% 作为窗口宽度，中点为窗位
            double ww = (range[1] - range[0]) * 0.6;
            double wc = range[0] + (range[1] - range[0]) * 0.5;
            serviceA->PreInit_SetWindowLevel(ww, wc);

            std::cout << "[onComplete] Data loaded."
                << " IsoThreshold=" << isoVal
                << " WW=" << ww << " WC=" << wc << "\n";
        }
    );

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

    std::cout << "Application started. Loading data in background...\n"
        << "Controls: A/D = navigate slices | M = toggle model transform\n";

    // contextB 持有主事件循环（其他窗口通过共享 Timer 驱动）
    contextB->Start();

    return 0;
}