// =====================================================================
// main.cpp
//
// 三阶段结构：
//   Phase 1 【前处理】  创建共享资源 + 配置所有数据无关参数
//   Phase 2 【加载】    由 DataLoader（SharedState）统一发起异步加载
//   Phase 3 【渲染骨架】初始化渲染骨架 + 进入消息循环
//
// 关键改进（相比旧版本）：
//   • 彻底删除 Show* / SetLuxParams 等旧接口调用，全部使用 PreInit_*
//   • 加载由 serviceA 的 LoadFileAsync 发起，解耦加载与特定 service 的关联
//   • 回调用值捕获 [sharedState, serviceA]，避免后台线程引用悬空指针
//   • ProcessPendingUpdates 由心跳 Timer 自动驱动，不再手动调用
// =====================================================================
#include <vtkAutoInit.h>
#include <vtkSMPTools.h>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);

#include "DataManager.h"
#include "AppService.h"
#include "VolumeAnalysisService.h"
#include "StdRenderContext.h"

int main()
{
    vtkSMPTools::Initialize();

    // 共享数据管理器：5 个窗口零拷贝共享同一份体数据
    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();

    // 共享交互状态：光标、材质、传输函数、模型矩阵等全局状态中枢
    auto sharedState = std::make_shared<SharedInteractionState>();

    // 分析服务（独立于渲染，可随时按需使用）
    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);

    // ── 窗口 A：等值面 + 切片参考平面复合视图 ───────────────────────
    auto serviceA = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextA = std::make_shared<StdRenderContext>();
    contextA->BindService(serviceA);    // 触发 Initialize，注册 Observer

    serviceA->PreInit_SetLuxParams(0.3, 0.6, 0.2, 15.0);   // 光照参数
    serviceA->PreInit_SetOpacity(1.0);                       // 透明度
    serviceA->PreInit_SetVizMode(VizMode::CompositeIsoSurface);

    contextA->SetWindowTitle("Window A: Composite IsoSurface");
    contextA->SetWindowSize(600, 600);
    contextA->SetWindowPosition(50, 50);
    contextA->ToggleOrientationAxes(true);
    contextA->SetInteractionMode(VizMode::CompositeIsoSurface);

    // ── 窗口 E：体渲染 + 切片参考平面复合视图 ───────────────────────
    auto serviceE = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextE = std::make_shared<StdRenderContext>();
    contextE->BindService(serviceE);

    // 体渲染传输函数（颜色映射完全与数据无关，前处理阶段安全）
    std::vector<TFNode> volTF = {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },  // 背景完全透明
        { 0.50, 0.0, 0.0, 0.5, 0.0 },  // 低值透明（过滤噪声）
        { 0.85, 0.8, 0.0, 0.5, 0.0 },  // 中间值半透
        { 1.00, 1.0, 0.0, 0.5, 0.0 },  // 高值不透明
    };
    serviceE->PreInit_SetTransferFunction(volTF);
    serviceE->PreInit_SetVizMode(VizMode::CompositeVolume);

    contextE->SetWindowTitle("Window E: Composite Volume");
    contextE->SetWindowSize(600, 600);
    contextE->SetWindowPosition(660, 50);
    contextE->SetInteractionMode(VizMode::CompositeVolume);

    // ── 窗口 B：轴状位（Axial）切片 ─────────────────────────────────
    auto serviceB = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextB = std::make_shared<StdRenderContext>();
    contextB->BindService(serviceB);
    serviceB->PreInit_SetVizMode(VizMode::SliceAxial);
    contextB->SetWindowTitle("Window B: Axial Slice");
    contextB->SetWindowSize(400, 400);
    contextB->SetWindowPosition(50, 660);
    contextB->SetInteractionMode(VizMode::SliceAxial);

    // ── 窗口 C：冠状位（Coronal）切片 ───────────────────────────────
    auto serviceC = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextC = std::make_shared<StdRenderContext>();
    contextC->BindService(serviceC);
    serviceC->PreInit_SetVizMode(VizMode::SliceCoronal);
    contextC->SetWindowTitle("Window C: Coronal Slice");
    contextC->SetWindowSize(400, 400);
    contextC->SetWindowPosition(460, 660);
    contextC->SetInteractionMode(VizMode::SliceCoronal);

    // ── 窗口 D：矢状位（Sagittal）切片 ──────────────────────────────
    auto serviceD = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextD = std::make_shared<StdRenderContext>();
    contextD->BindService(serviceD);
    serviceD->PreInit_SetVizMode(VizMode::SliceSagittal);
    contextD->SetWindowTitle("Window D: Sagittal Slice");
    contextD->SetWindowSize(400, 400);
    contextD->SetWindowPosition(870, 660);
    contextD->SetInteractionMode(VizMode::SliceSagittal);

    serviceA->LoadFileAsync(
        "D:\\CT-1209\\data\\1536X1536X1536.raw",
        [sharedState, serviceA](bool success) {
            if (!success) {
                std::cerr << "[Error] Failed to load volume data." << std::endl;
                return;
            }
            // !! 后台线程 !! 只操作 SharedState（内部有 mutex）
            // 基于实际数据范围设置等值面阈值（60% 处）
            auto range = sharedState->GetDataRange();
            serviceA->PreInit_SetIsoThreshold(
                range[0] + (range[1] - range[0]) * 0.6
            );
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

    std::cout << "Application started. Loading data in background...\n";
    std::cout << "Controls: A/D = navigate slices | M = toggle model transform\n";

    // contextB 持有主事件循环（其他窗口通过共享 Timer 驱动）
    contextB->Start();

    return 0;
}