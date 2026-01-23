#include <vtkAutoInit.h>

// 注册核心渲染模块
VTK_MODULE_INIT(vtkRenderingOpenGL2);
// 注册交互模块
VTK_MODULE_INIT(vtkInteractionStyle);
// 注册体渲染模块
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);

#include "DataManager.h"
#include "AppService.h"
#include "StdRenderContext.h"

int main() {
    // 创建共享资源 (仅作为依赖注入传递，不直接操作)
    //auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedDataMgr = std::make_shared<TiffVolumeDataManager>(); // 使用 Tiff
    auto sharedState = std::make_shared<SharedInteractionState>();
    auto image = std::make_shared<VolumeAnalysisService>(sharedDataMgr);

    // --- 窗口 A: 复合视图 (等值面 + 切片平面) ---
    // Service 是操作数据的唯一入口
    auto serviceA = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextA = std::make_shared<StdRenderContext>();
    contextA->BindService(serviceA);

    // 初始化光照 (Lux)
    serviceA->SetLuxParams(0.3, 0.6, 0.2, 15.0);
    // 初始化全局透明度
    serviceA->SetOpacity(1.0);

    // 窗口设置
    contextA->SetWindowTitle("Window A: Composite IsoSurface");
    contextA->SetWindowSize(600, 600);
    contextA->SetWindowPosition(50, 50);

    // 加载数据
    serviceA->LoadFile("D:\\CT-1209\\data\\1440");

    // 设置模式
    auto range = sharedState->GetDataRange();
    auto val = (-range[0]  + range[1]) * 0.6;
    serviceA->SetIsoThreshold(val + range[0]);
    //serviceA->SetIsoThreshold(0.1);
    //image->SaveHistogramImage("1.png");
    contextA->SetInteractionMode(VizMode::CompositeIsoSurface);
    serviceA->Show3DPlanes(VizMode::CompositeIsoSurface);
    

    // --- 窗口 E: 复合视图 (体渲染 + 切片平面) ---
    auto serviceE = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextE = std::make_shared<StdRenderContext>();
    contextE->BindService(serviceE);

    contextE->SetWindowTitle("Window E: Composite Volume");
    contextE->SetWindowSize(600, 600);
    contextE->SetWindowPosition(660, 50);

    // 因为 State 是共享的，这里设置会影响所有使用该 State 的体渲染窗口
    std::vector<TFNode> volTF = {
        {0.0, 0.0, 0,0,0},
        {0.1, 0.0, 0,0,0},
        {0.8, 0, 0,1,0}, 
        {1.0, 1.0, 0,0,1}  
    };
    serviceE->SetTransferFunction(volTF);
    contextE->SetInteractionMode(VizMode::CompositeVolume);
    serviceE->Show3DPlanes(VizMode::CompositeVolume);

    // --- 窗口 B: 轴状位切片 ---
    auto serviceB = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextB = std::make_shared<StdRenderContext>();
    contextB->BindService(serviceB);

    contextB->SetWindowTitle("Window B: Axial Slice");
    contextB->SetWindowSize(400, 400);
    contextB->SetWindowPosition(50, 660);

    serviceB->ShowSlice(VizMode::SliceAxial);
    contextB->SetInteractionMode(VizMode::SliceAxial);

    // --- 窗口 C: 冠状位切片 ---
    auto serviceC = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextC = std::make_shared<StdRenderContext>();
    contextC->BindService(serviceC);

    contextC->SetWindowTitle("Window C: Coronal Slice");
    contextC->SetWindowSize(400, 400);
    contextC->SetWindowPosition(460, 660);

    serviceC->ShowSlice(VizMode::SliceCoronal);
    contextC->SetInteractionMode(VizMode::SliceCoronal);

    // --- 窗口 D: 矢状位切片 ---
    auto serviceD = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextD = std::make_shared<StdRenderContext>();
    contextD->BindService(serviceD);

    contextD->SetWindowTitle("Window D: Sagittal Slice");
    contextD->SetWindowSize(400, 400);
    contextD->SetWindowPosition(870, 660);

    serviceD->ShowSlice(VizMode::SliceSagittal);
    contextD->SetInteractionMode(VizMode::SliceSagittal);

    // 此时所有参数都已经通过 Service 同步到了 State 中
    // 各个 Context Render 时会从 State 拉取最新参数

    serviceA->ProcessPendingUpdates();
    serviceB->ProcessPendingUpdates();
    serviceC->ProcessPendingUpdates();
    serviceD->ProcessPendingUpdates();
    serviceE->ProcessPendingUpdates();

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

    std::cout << "Starting Main Loop..." << std::endl;
    std::cout << "Use 'A'/'D' for measurements." << std::endl;

    // 启动消息循环
    contextB->Start();

    return 0;
}