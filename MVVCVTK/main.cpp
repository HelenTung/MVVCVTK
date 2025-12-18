#include <vtkAutoInit.h>
VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);

#include <vtkRenderWindowInteractor.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include "AppService.h"
#include <iostream>

int main() {
    // 初始化 VTK 基础设施 (UI层)
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetSize(800, 600);
    renderWindow->SetWindowName("Medical Viz Architecture Demo");

    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    renderWindow->AddRenderer(renderer);

    auto interactor = vtkSmartPointer<vtkRenderWindowInteractor>::New();
    interactor->SetRenderWindow(renderWindow);

    auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
    interactor->SetInteractorStyle(style);

    // 启动服务层 (Service Layer)
    auto service = std::make_shared<MedicalVizService>();
    service->Initialize(renderWindow, renderer);

    // 模拟业务流程
    std::cout << "Loading data..." << std::endl;
    service->LoadFile("D:\\CT-1209\\data\\1000X1000X1000.raw");

    // 在 Qt/MFC 中，这里会连接到按钮的 Slot

    interactor->Initialize();

    // 添加一个简单的按键回调来测试切换
    // 按 '1': 体渲染, '2': 等值面, '3': 2D切片
    class KeyCallback : public vtkCommand {
    public:
        static KeyCallback* New() { return new KeyCallback; }
        std::shared_ptr<MedicalVizService> svc;

        void Execute(vtkObject* caller, unsigned long, void*) override {
            vtkRenderWindowInteractor* iren = static_cast<vtkRenderWindowInteractor*>(caller);
            std::string key = iren->GetKeySym();
            if (key == "1") {
                std::cout << "Switching to Volume..." << std::endl;
                svc->ShowVolume();
            }
            else if (key == "2") {
                std::cout << "Switching to IsoSurface..." << std::endl;
                svc->ShowIsoSurface();
            }
            else if (key == "3") {
                std::cout << "Switching to Axial Slice..." << std::endl;
                svc->ShowSliceAxial();
            }
        }
    };

    auto callback = vtkSmartPointer<KeyCallback>::New();
    callback->svc = service;
    interactor->AddObserver(vtkCommand::KeyPressEvent, callback);

    std::cout << "Press '1' for Volume, '2' for IsoSurface, '3' for Slice" << std::endl;
    interactor->Start();

    return 0;
}