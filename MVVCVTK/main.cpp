// =====================================================================
// main.cpp  v2
//
// 三阶段结构：
//   Phase 1 【前处理】  创建共享资源 + 通过 WindowConfig / PreInit_CommitConfig
//                       批量配置所有与数据无关的参数
//   Phase 2 【加载】    通过 IDataLoaderService 接口发起异步加载（与渲染解耦）
//   Phase 3 【渲染骨架】InitInteractor + Start 进入消息循环
//
//   • 新增 AppTypes.h：公共数据结构零依赖
//   • PreInit_CommitConfig：批量提交，一次锁 + 一次广播
//   • LoadFileAsync 通过 IDataLoaderService* 调用，与 MedicalVizService 解耦
//   • 引入 WindowConfig + BuildWindow 辅助函数消除5窗口重复样板
//   • 修复ClearStrategyCache 中 VTK Detach 延迟到主线程执行
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

// ─────────────────────────────────────────────────────────────────────
// BuildWindow：根据 WindowConfig 创建 Service + Context，完成前处理配置
// 返回 {service, context} pair
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

    // 绑定触发 Initialize（注册 Observer���
    context->BindService(service);

    // 批量提交前处理配置（一次锁 + 一次广播）
    service->PreInit_CommitConfig(cfg.preInitCfg);
    // VizMode 在 CommitConfig 内部已通过 m_pendingVizModeInt 记录

    // 窗口属性
    context->SetWindowTitle(cfg.title);
    context->SetWindowSize(cfg.width, cfg.height);
    context->SetWindowPosition(cfg.posX, cfg.posY);
    context->SetInteractionMode(cfg.vizMode);
    if (cfg.showAxes) context->ToggleOrientationAxes(true);

    return { service, context };
}

int main()
{
    vtkSMPTools::Initialize();

    // ── Phase 1：共享资源 ──────────────────────────────────────────
    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedState = std::make_shared<SharedInteractionState>();
    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);

    // ── Phase 1：传输函数（数据无关，前处理阶段安全）────────────────
    std::vector<TFNode> volTF = {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };

    // ── Phase 1：窗口配置表（前处理参数全部在此集中声明）────────────
    // 窗口 A：等值面 + 切片参考平面
    WindowConfig cfgA;
    cfgA.title = "Window A: Composite IsoSurface";
    cfgA.width = 600; cfgA.height = 600;
    cfgA.posX = 50;  cfgA.posY = 50;
    cfgA.vizMode = VizMode::CompositeIsoSurface;
    cfgA.showAxes = true;
    cfgA.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    cfgA.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 1.0, false };

    // 窗口 E：体渲染 + 切片参考平面
    WindowConfig cfgE;
    cfgE.title = "Window E: Composite Volume";
    cfgE.width = 600; cfgE.height = 600;
    cfgE.posX = 660; cfgE.posY = 50;
    cfgE.vizMode = VizMode::CompositeVolume;
    cfgE.preInitCfg.vizMode = VizMode::CompositeVolume;
    cfgE.preInitCfg.tfNodes = volTF;
    cfgE.preInitCfg.hasTF = true;

    // 窗口 B：Axial 切片
    WindowConfig cfgB;
    cfgB.title = "Window B: Axial Slice";
    cfgB.width = 400; cfgB.height = 400;
    cfgB.posX = 50;  cfgB.posY = 660;
    cfgB.vizMode = VizMode::SliceAxial;
    cfgB.preInitCfg.vizMode = VizMode::SliceAxial;

    // 窗口 C：Coronal 切片
    WindowConfig cfgC;
    cfgC.title = "Window C: Coronal Slice";
    cfgC.width = 400; cfgC.height = 400;
    cfgC.posX = 460; cfgC.posY = 660;
    cfgC.vizMode = VizMode::SliceCoronal;
    cfgC.preInitCfg.vizMode = VizMode::SliceCoronal;

    // 窗口 D：Sagittal 切片
    WindowConfig cfgD;
    cfgD.title = "Window D: Sagittal Slice";
    cfgD.width = 400; cfgD.height = 400;
    cfgD.posX = 870; cfgD.posY = 660;
    cfgD.vizMode = VizMode::SliceSagittal;
    cfgD.preInitCfg.vizMode = VizMode::SliceSagittal;

    // ── Phase 1：批量建窗 ──────────────────────────────────────────
    auto [serviceA, contextA] = BuildWindow(cfgA, sharedDataMgr, sharedState);
    auto [serviceE, contextE] = BuildWindow(cfgE, sharedDataMgr, sharedState);
    auto [serviceB, contextB] = BuildWindow(cfgB, sharedDataMgr, sharedState);
    auto [serviceC, contextC] = BuildWindow(cfgC, sharedDataMgr, sharedState);
    auto [serviceD, contextD] = BuildWindow(cfgD, sharedDataMgr, sharedState);

    // ── Phase 2：异步加载（通过 IDataLoaderService 接口，与渲染解耦）
    // 加载完成后，在后台线程基于实际数据范围设置等值面阈值
    IDataLoaderService* loader = serviceA.get();
    loader->LoadFileAsync(
        "D:\\CT-1209\\data\\1536X1536X1536.raw",
        [sharedState, serviceA](bool success) {
            if (!success) {
                std::cerr << "[Error] Failed to load volume data.\n";
                return;
            }
            // !! 后台线程 !! 只操作 SharedState（内部有 mutex）
            auto range = sharedState->GetDataRange();
            double isoVal = range[0] + (range[1] - range[0]) * 0.6;
            // 使用逐项接口设置（数据相关，后处理阶段）
            serviceA->PreInit_SetIsoThreshold(isoVal);
        }
    );

    // ── Phase 3：初始化渲染骨架 + 进入消息循环 ────────────────────
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