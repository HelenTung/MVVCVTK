// =====================================================================
// main.cpp
//
// 【改动总结对比】
//   改动点                    原来                        现在
//   ─────────────────────────────────────────────────────────────────
//   显示模式设置时机           ShowXxx 直接调用           PreInit_SetVizMode 登记意图
//   数据加载触发者             每个 service 独立感知       只由 serviceA 触发一次
//   回调捕获方式               [&sharedState, serviceA]   [sharedState, serviceA]（值捕获）
//   ProcessPendingUpdates      Start 前手动调用            由心跳 Timer 自动驱动
//   Show* 调用顺序依赖          必须在 BindService 后        无顺序要求（纯状态登记）
// =====================================================================
#include <vtkAutoInit.h>
#include <vtkSMPTools.h>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);

#include "DataManager.h"
#include "AppService.h"
#include "StdRenderContext.h"

int main()
{
    vtkSMPTools::Initialize();

    // =================================================================
    // 【第一阶段：前处理】创建共享资源，配置所有数据无关参数
    //
    // 【为什么在 LoadFileAsync 之前做这些】
    //   数据加载是异步的，但窗口配置（大小、位置、标题、光照参数）
    //   与数据完全无关，先配好再加载，逻辑更清晰，也不存在时序风险。
    // =================================================================

    // 共享数据管理器：5 个窗口共享同一份体数据，零拷贝
    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    // 共享交互状态：所有窗口光标/参数保持同步的核心
    auto sharedState = std::make_shared<SharedInteractionState>();
    // 分析服务（可选，与渲染解耦）
    auto imageAnalysis = std::make_shared<VolumeAnalysisService>(sharedDataMgr);

    // ── 窗口 A：等值面 + 切片平面复合视图 ───────────────────────────
    auto serviceA = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextA = std::make_shared<StdRenderContext>();
    contextA->BindService(serviceA); // 触发 Initialize，注册 Observer

    // 【前处理】光照、透明度参数登记到 SharedState
    // 此时数据未加载，但参数会被持久化，数据到达后 Strategy 会读取应用
    serviceA->PreInit_SetLuxParams(0.3, 0.6, 0.2, 15.0);
    serviceA->PreInit_SetOpacity(1.0);

    // 【前处理】登记显示模式意图（不执行 VTK 操作，只记录 m_pendingVizMode）
    serviceA->PreInit_SetVizMode(VizMode::CompositeIsoSurface);

    // 窗口外观配置（与数据无关，前处理阶段安全）
    contextA->SetWindowTitle("Window A: Composite IsoSurface");
    contextA->SetWindowSize(600, 600);
    contextA->SetWindowPosition(50, 50);
    contextA->ToggleOrientationAxes(true);
    contextA->SetInteractionMode(VizMode::CompositeIsoSurface);

    // ── 窗口 E：体渲染 + 切片平面复合视图 ──────────────────────────
    auto serviceE = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextE = std::make_shared<StdRenderContext>();
    contextE->BindService(serviceE);

    // 【前处理】体渲染传输函数（数据无关的颜色映射配置）
    std::vector<TFNode> volTF = {
        {0.0,  0.0, 0.0, 0.0, 0.0},  // 完全透明，隐藏背景
        {0.5,  0.0, 0.0, 0.5, 0.0},  // 完全透明，过滤噪声
        {0.85, 0.8, 0.0, 0.5, 0.0},  // 中间半透
        {1.0,  1.0, 0.0, 0.5, 0.0},  // 高值不透明
    };
    serviceE->PreInit_SetTransferFunction(volTF);
    serviceE->PreInit_SetVizMode(VizMode::CompositeVolume);

    contextE->SetWindowTitle("Window E: Composite Volume");
    contextE->SetWindowSize(600, 600);
    contextE->SetWindowPosition(660, 50);
    contextE->SetInteractionMode(VizMode::CompositeVolume);

    // ── 窗口 B：轴状位切片 ──────────────────────────────────────────
    auto serviceB = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextB = std::make_shared<StdRenderContext>();
    contextB->BindService(serviceB);

    serviceB->PreInit_SetVizMode(VizMode::SliceAxial);

    contextB->SetWindowTitle("Window B: Axial Slice");
    contextB->SetWindowSize(400, 400);
    contextB->SetWindowPosition(50, 660);
    contextB->SetInteractionMode(VizMode::SliceAxial);

    // ── 窗口 C：冠状位切片 ──────────────────────────────────────────
    auto serviceC = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextC = std::make_shared<StdRenderContext>();
    contextC->BindService(serviceC);

    serviceC->PreInit_SetVizMode(VizMode::SliceCoronal);

    contextC->SetWindowTitle("Window C: Coronal Slice");
    contextC->SetWindowSize(400, 400);
    contextC->SetWindowPosition(460, 660);
    contextC->SetInteractionMode(VizMode::SliceCoronal);

    // ── 窗口 D：矢状位切片 ──────────────────────────────────────────
    auto serviceD = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextD = std::make_shared<StdRenderContext>();
    contextD->BindService(serviceD);

    serviceD->PreInit_SetVizMode(VizMode::SliceSagittal);

    contextD->SetWindowTitle("Window D: Sagittal Slice");
    contextD->SetWindowSize(400, 400);
    contextD->SetWindowPosition(870, 660);
    contextD->SetInteractionMode(VizMode::SliceSagittal);

    // =================================================================
    // 【第二阶段：发起数据加载】
    //
    // 【为什么只由 serviceA 发起一次 LoadFileAsync】
    //   所有 service 共享同一个 sharedDataMgr，数据只需加载一次。
    //   加载完成后 NotifyDataReady 广播给所有已注册的 service，
    //   B/C/D/E 通过 Observer 自动响应，不需要各自调用加载接口。
    //   原代码只有 serviceA 加载没有问题，但意图不够清晰；
    //   现在通过 PreInit_SetVizMode 让所有 service 的意图都已登记，
    //   DataReady 广播后各自按自己的 m_pendingVizMode 重建管线。
    //
    // 【回调为什么用值捕获 [sharedState, serviceA] 而不是 [&sharedState]】
    //   回调在后台线程执行，此时 main 函数的栈帧可能（理论上）已经不在。
    //   引用捕获（&sharedState）是引用栈上的局部变量，后台线程访问时
    //   存在生命周期风险。
    //   值捕获（shared_ptr 拷贝）会增加引用计数，保证对象在回调执行
    //   期间一定存活，完全安全。
    // =================================================================
    serviceA->LoadFileAsync(
        "D:\\CT-1209\\data\\1536X1536X1536.raw",
        [sharedState, serviceA](bool success) {
            // !! 后台线程 !! 只允许操作 SharedState（内部有 mutex）
            if (!success) {
                std::cerr << "[Error] Failed to load volume data." << std::endl;
                return;
            }
            // 数据加载成功后，为 serviceA 设置基于实际数据范围的等值面���值
            // 其他 service（B/C/D/E）的个性化参数也可在此处设置
            auto range = sharedState->GetDataRange(); // 有锁，线程安全
            serviceA->PreInit_SetIsoThreshold(
                range[0] + (range[1] - range[0]) * 0.6
            );
        }
    );
    // =================================================================
    // 【第三阶段：初始化渲染骨架，进入消息循环】
    //
    // 【为什么不再手动调 ProcessPendingUpdates】
    //   原代码在 Start() 前手动调用，是为了在"同步加载"场景下
    //   确保首帧显示正确。现在是异步加载：数据到达时间不确定，
    //   手动调用没有意义（数据可能还没到）。
    //   改由心跳 Timer（33ms 间隔）在每帧自动轮询，数据到达后
    //   的下一个 Timer 事件会检测到 m_needsDataRefresh=true 并执行重建。
    // =================================================================

    // 渲染骨架：此时数据未到，窗口显示空白（或背景色），但窗口立即弹出
    contextA->Render();
    contextB->Render();
    contextC->Render();
    contextD->Render();
    contextE->Render();

    // 初始化交互器（内部会创建心跳定时器，驱动 ProcessPendingUpdates）
    contextA->InitInteractor();
    contextB->InitInteractor();
    contextC->InitInteractor();
    contextD->InitInteractor();
    contextE->InitInteractor();

    std::cout << "Application started. Loading data in background..." << std::endl;
    std::cout << "Use 'A'/'D' to navigate slices, 'M' to toggle model transform." << std::endl;

    // 启动消息循环（阻塞直到用户关闭所有窗口）
    // contextB 持有事件循环（其他窗口通过 Timer 驱动）
    contextB->Start();

    return 0;
}