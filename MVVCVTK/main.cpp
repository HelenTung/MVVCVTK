#include <vtkAutoInit.h>
VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);

#include <vtkRenderWindowInteractor.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include "AppService.h"
#include "DataManager.h" // 引入具体的数据管理器
//
//// 用于演示分别控制左右两个Service
//class SplitScreenCallback : public vtkCommand {
//public:
//    static SplitScreenCallback* New() { return new SplitScreenCallback; }
//
//    std::shared_ptr<MedicalVizService> svcLeft;
//    std::shared_ptr<MedicalVizService> svcRight;
//
//    void Execute(vtkObject* caller, unsigned long, void*) override {
//        vtkRenderWindowInteractor* iren = static_cast<vtkRenderWindowInteractor*>(caller);
//        std::string key = iren->GetKeySym();
//
//        // 逻辑：小写字母控制左屏，大写字母(Shift)控制右屏
//
//        // --- 左屏控制 ---
//        if (key == "1") {
//            std::cout << "[Left] Show Volume" << std::endl;
//            svcLeft->ShowVolume();
//        }
//        else if (key == "2") {
//            std::cout << "[Left] Show IsoSurface" << std::endl;
//            svcLeft->ShowIsoSurface();
//        }
//        else if (key == "3") {
//            std::cout << "[Left] Show Axial Slice" << std::endl;
//            svcLeft->ShowSliceAxial();
//        }
//
//        // --- 右屏控制 ---
//        else if (key == "4") { // 4
//            std::cout << "[Right] Show Volume" << std::endl;
//            svcRight->ShowVolume();
//        }
//        else if (key == "5") { // 5
//            std::cout << "[Right] Show IsoSurface" << std::endl;
//            svcRight->ShowIsoSurface();
//        }
//        else if (key == "6") { // 6
//            std::cout << "[Right] Show Axial Slice" << std::endl;
//            svcRight->ShowSliceAxial();
//        }
//    }
//};
//
//int main() {
//    // 创建共享的数据管理器
//    auto sharedData = std::make_shared<RawVolumeDataManager>();
//
//    // 创建窗口
//    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
//    renderWindow->SetSize(1200, 600); // 宽一点，因为要分屏
//    renderWindow->SetWindowName("Split Screen Medical Viz");
//
//    // 创建两个渲染器 (Renderer)
//    auto renLeft = vtkSmartPointer<vtkRenderer>::New();
//    renLeft->SetViewport(0.0, 0.0, 0.5, 1.0); // 左半屏: x从0到0.5
//    renLeft->SetBackground(0.1, 0.1, 0.1);    // 深灰背景
//
//    auto renRight = vtkSmartPointer<vtkRenderer>::New();
//    renRight->SetViewport(0.5, 0.0, 1.0, 1.0); // 右半屏: x从0.5到1.0
//    renRight->SetBackground(0.2, 0.2, 0.2);    // 浅灰背景
//
//    renderWindow->AddRenderer(renLeft);
//    renderWindow->AddRenderer(renRight);
//
//    // 创建交互器
//    auto interactor = vtkSmartPointer<vtkRenderWindowInteractor>::New();
//    interactor->SetRenderWindow(renderWindow);
//
//    auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
//    interactor->SetInteractorStyle(style);
//
//    // 创建两个独立的 Service，但注入同一个数据对象
//    auto serviceLeft = std::make_shared<MedicalVizService>(sharedData);
//    serviceLeft->Initialize(renderWindow, renLeft);
//
//    auto serviceRight = std::make_shared<MedicalVizService>(sharedData);
//    serviceRight->Initialize(renderWindow, renRight);
//
//    // 加载数据 
//    std::cout << "Loading data..." << std::endl;
//    bool loaded = sharedData->LoadData("D:\\CT-1209\\data\\1000X1000X1000.raw");
//
//    if (loaded) {
//        serviceLeft->ShowIsoSurface();
//        serviceRight->ShowSliceAxial(); // 右侧默认显示切片，形成对比
//    }
//
//    // 设置回调
//    auto callback = vtkSmartPointer<SplitScreenCallback>::New();
//    callback->svcLeft = serviceLeft;
//    callback->svcRight = serviceRight;
//    interactor->AddObserver(vtkCommand::KeyPressEvent, callback);
//
//    std::cout << "Controls:" << std::endl;
//    std::cout << "  [Left View]  Press 1, 2, 3" << std::endl;
//    std::cout << "  [Right View] Press 4, 5, 6" << std::endl;
//
//    interactor->Initialize();
//    interactor->Start();
//
//    return 0;
//}




// 简单的回调，用于将按键绑定到特定的 Service
class SingleWindowCallback : public vtkCommand {
public:
    static SingleWindowCallback* New() { return new SingleWindowCallback; }
    std::shared_ptr<MedicalVizService> service; // 每个回调只持有一个 Service

    void Execute(vtkObject* caller, unsigned long, void*) override {
        vtkRenderWindowInteractor* iren = static_cast<vtkRenderWindowInteractor*>(caller);
        std::string key = iren->GetKeySym();

        if (key == "1") service->ShowVolume();
        else if (key == "2") service->ShowIsoSurface();
        else if (key == "3") service->ShowSliceAxial();
    }
};

int main() {
    // 创建共享数据源
    auto sharedData = std::make_shared<RawVolumeDataManager>();

    // ================== 窗口 A (主屏) ==================
    auto winA = vtkSmartPointer<vtkRenderWindow>::New();
    winA->SetSize(600, 600);
    winA->SetWindowName("Monitor 1 - Axial Slice");
    winA->SetPosition(0, 0); // 屏幕左上角

    auto renA = vtkSmartPointer<vtkRenderer>::New();
    winA->AddRenderer(renA);

    auto irenA = vtkSmartPointer<vtkRenderWindowInteractor>::New();
    irenA->SetRenderWindow(winA);
    irenA->SetInteractorStyle(vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New());

    // Service A 绑定到 窗口 A
    auto serviceA = std::make_shared<MedicalVizService>(sharedData);
    serviceA->Initialize(winA, renA);


    // ================== 窗口 B (副屏) ==================
    auto winB = vtkSmartPointer<vtkRenderWindow>::New();
    winB->SetSize(600, 600);
    winB->SetWindowName("Monitor 2 - 3D Volume");
    winB->SetPosition(700, 0); // 错开位置，在另一个区域

    auto renB = vtkSmartPointer<vtkRenderer>::New();
    winB->AddRenderer(renB);

    auto irenB = vtkSmartPointer<vtkRenderWindowInteractor>::New();
    irenB->SetRenderWindow(winB);
    irenB->SetInteractorStyle(vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New());

    // Service B 绑定到 窗口 B
    auto serviceB = std::make_shared<MedicalVizService>(sharedData);
    serviceB->Initialize(winB, renB);


    // 加载数据 (一份内存，两处显示)
    std::cout << "Loading shared data..." << std::endl;
    if (sharedData->LoadData("D:\\CT-1209\\data\\1000X1000X1000.raw")) {
        // 设置不同的默认视图
        serviceA->ShowSliceAxial(); // 窗口A 看切片
        serviceB->ShowVolume();     // 窗口B 看3D
    }

    // 绑定各自的交互逻辑
    auto callbackA = vtkSmartPointer<SingleWindowCallback>::New();
    callbackA->service = serviceA;
    irenA->AddObserver(vtkCommand::KeyPressEvent, callbackA);

    auto callbackB = vtkSmartPointer<SingleWindowCallback>::New();
    callbackB->service = serviceB;
    irenB->AddObserver(vtkCommand::KeyPressEvent, callbackB);

    irenA->Initialize();
    irenB->Initialize();

    std::cout << "Running... Focus on a window and press 1/2/3 to switch its view." << std::endl;

    // 只需在一个交互器上启动阻塞循环即可驱动整个程序
    irenA->Start();

    return 0;
}
