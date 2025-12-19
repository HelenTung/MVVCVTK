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
    // 数据是唯一的，加载一次内存
    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    if (!sharedDataMgr->LoadData("D:\\CT-1209\\data\\465x428x452_clod-negative-electrode.raw")) {
        return -1;
    }

    // ================= 窗口 A：显示 3D 体渲染 =================
    auto serviceA = std::make_shared<MedicalVizService>(sharedDataMgr);
    auto contextA = std::make_shared<StdRenderContext>();

    contextA->BindService(serviceA);
    serviceA->ShowIsoSurface(); // 业务操作
    contextA->SetInteractionMode(VizMode::IsoSurface); // 交互操作

    // contextA->GetRenderWindow()->SetWindowName("Window A - 3D Volume");

    // ================= 窗口 B：显示 2D 轴状位切片 =================
    auto serviceB = std::make_shared<MedicalVizService>(sharedDataMgr);
    auto contextB = std::make_shared<StdRenderContext>();

    contextB->BindService(serviceB);
    serviceB->ShowSliceAxial(); // 业务操作
	serviceB->UpdateSliceOrientation(Orientation::CORONAL); // 设置轴状位
    contextB->SetInteractionMode(VizMode::AxialSlice); // 交互操作
    // contextB->GetRenderWindow()->SetWindowName("Window B - Axial Slice");

    // ================= 窗口 c：显示 2D 轴状位切片 =================
    auto serviceC = std::make_shared<MedicalVizService>(sharedDataMgr);
    auto contextC = std::make_shared<StdRenderContext>();

    contextC->BindService(serviceC);
    serviceC->ShowSliceAxial(); // 业务操作
    serviceC->UpdateSliceOrientation(Orientation::SAGITTAL); // 设置轴状位
    contextC->SetInteractionMode(VizMode::AxialSlice); // 交互操作
    // contextB->GetRenderWindow()->SetWindowName("Window B - Axial Slice");


    // ================= 窗口 D：显示 2D AxialSlice =================
    auto serviceD = std::make_shared<MedicalVizService>(sharedDataMgr);
    auto contextD = std::make_shared<StdRenderContext>();

    contextD->BindService(serviceD);
    serviceD->ShowSliceAxial(); // 业务操作
    serviceD->UpdateSliceOrientation(Orientation::AXIAL); // 设置轴状位
    contextD->SetInteractionMode(VizMode::AxialSlice); // 交互操作
    // contextB->GetRenderWindow()->SetWindowName("Window B - Axial Slice");


    // ================= 启动 =================
    contextA->Render(); // 现刷一下
    contextC->Render();
    contextD->Render();
    contextB->Start();  // 阻塞在这里，开启事件循环

    return 0;
}