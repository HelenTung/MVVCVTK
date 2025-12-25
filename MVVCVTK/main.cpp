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
    auto sharedState = std::make_shared<SharedInteractionState>();
    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();

    // --- 窗口 A ---
    auto serviceA = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    serviceA->LoadFile("D:\\CT-1209\\data\\1000X1000X1000.raw");
    auto contextA = std::make_shared<StdRenderContext>();
    contextA->BindService(serviceA);
    contextA->SetInteractionMode(VizMode::CompositeIsoSurface);
    serviceA->Show3DPlanes(VizMode::CompositeIsoSurface);
    std::weak_ptr<MedicalVizService> weakServiceA = serviceA;
    sharedState->AddObserver([weakServiceA]() {
        if (auto ptr = weakServiceA.lock()) {
            ptr->OnStateChanged();
        }
    });


    // --- 窗口E  ---
    auto serviceE = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextE = std::make_shared<StdRenderContext>();
    contextE->BindService(serviceE);
    contextE->SetInteractionMode(VizMode::CompositeVolume);
    serviceE->Show3DPlanes(VizMode::CompositeVolume);
    std::weak_ptr<MedicalVizService> weakServiceE = serviceE;
    sharedState->AddObserver([weakServiceE]() {
        if (auto ptr = weakServiceE.lock()) {
            ptr->OnStateChanged();
        }
    });

	// 保存直方图图片
	// serviceA->SaveHistogramImage("D:\\CT-1209\\data\\histogram.png", 2048);

    // --- 窗口 B ---
    auto serviceB = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextB = std::make_shared<StdRenderContext>();
    contextB->BindService(serviceB);
    serviceB->ShowSlice(VizMode::SliceAxial);
    contextB->SetInteractionMode(VizMode::SliceAxial);
    std::weak_ptr<MedicalVizService> weakServiceB = serviceB;
    sharedState->AddObserver([weakServiceB]() {
        if (auto ptr = weakServiceB.lock()) {
            ptr->OnStateChanged();
        }
    });

    // --- 窗口 C ---
    auto serviceC = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextC = std::make_shared<StdRenderContext>();
    contextC->BindService(serviceC);
    serviceC->ShowSlice(VizMode::SliceCoronal);
    contextC->SetInteractionMode(VizMode::SliceCoronal);
    std::weak_ptr<MedicalVizService> weakServiceC = serviceC;
    sharedState->AddObserver([weakServiceC]() {
        if (auto ptr = weakServiceC.lock()) {
            ptr->OnStateChanged();
        }
    });

    // --- 窗口 D ---
    auto serviceD = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextD = std::make_shared<StdRenderContext>();
    contextD->BindService(serviceD);
    serviceD->ShowSlice(VizMode::SliceSagittal);
    contextD->SetInteractionMode(VizMode::SliceSagittal);
    std::weak_ptr<MedicalVizService> weakServiceD = serviceD;
    sharedState->AddObserver([weakServiceD]() {
        if (auto ptr = weakServiceD.lock()) {
            ptr->OnStateChanged();
        }
    });

    // 先把所有窗口都渲染一次
    contextA->Render();
    contextB->Render();
    contextC->Render();
    contextD->Render();
	contextE->Render();

    // 初始化所有的交互器
    contextA->InitInteractor();
    contextB->InitInteractor();
    contextC->InitInteractor();
    contextD->InitInteractor();
    contextE->InitInteractor();

    // 只启动一个主循环 (通常选主窗口)
    // 在 Windows 环境下，同一个线程的 Initialize 过的 Interactor 通常能共享消息泵
    // 所以滚动 B，C 和 D 应该也能被动刷新。
    // 但是滚动 C，可能无法触发事件
    std::cout << "Starting Main Loop..." << std::endl;
    contextB->Start();

    return 0;
}