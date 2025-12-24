#include "StdRenderContext.h"
#include <vtkCallbackCommand.h>
#include <vtkInteractorStyleImage.h>


void StdRenderContext::InitInteractor()
{
    if (m_interactor) {
        m_interactor->Initialize();
    }

    // 初始化距离测量 Widget
    if (!m_distanceWidget) {
        m_distanceWidget = vtkSmartPointer<vtkDistanceWidget>::New();
        m_distanceWidget->SetInteractor(m_interactor);
        m_distanceWidget->CreateDefaultRepresentation();
        // 设置高优先级，确保它能拦截点击事件
        m_distanceWidget->SetPriority(1.0);
    }

    // 初始化角度测量 Widget
    if (!m_angleWidget) {
        m_angleWidget = vtkSmartPointer<vtkAngleWidget>::New();
        m_angleWidget->SetInteractor(m_interactor);
        m_angleWidget->CreateDefaultRepresentation();
        m_angleWidget->SetPriority(1.0);
    }
}

StdRenderContext::StdRenderContext()
{
	// 初始化交互器
    m_interactor = vtkSmartPointer<vtkRenderWindowInteractor>::New();
    m_interactor->SetRenderWindow(m_renderWindow);

	// 初始化拾取器
    m_picker = vtkSmartPointer<vtkPropPicker>::New();
    
    // 初始化回调命令
    m_eventCallback = vtkSmartPointer<vtkCallbackCommand>::New();
    m_eventCallback->SetCallback(AbstractRenderContext::DispatchVTKEvent);
    m_eventCallback->SetClientData(this);

    // 监听滚轮事件
    m_interactor->AddObserver(vtkCommand::MouseWheelForwardEvent, m_eventCallback, 1.0);
    m_interactor->AddObserver(vtkCommand::MouseWheelBackwardEvent, m_eventCallback, 1.0);
    // 监听鼠标左键按下、移动、抬起
    m_interactor->AddObserver(vtkCommand::LeftButtonPressEvent, m_eventCallback, 1.0);
    m_interactor->AddObserver(vtkCommand::MouseMoveEvent, m_eventCallback, 1.0);
    m_interactor->AddObserver(vtkCommand::LeftButtonReleaseEvent, m_eventCallback, 1.0);
	// 监听键盘按键事件
    m_interactor->AddObserver(vtkCommand::KeyPressEvent, m_eventCallback, 1.0);
}

void StdRenderContext::Start()
{
    if (m_renderWindow) m_renderWindow->Render();
    if (m_interactor) {
        //防止还没Init就Start
        if (!m_interactor->GetInitialized()) {
            m_interactor->Initialize();
        }
        m_interactor->Start();
    }
}

void StdRenderContext::SetInteractionMode(VizMode mode)
{
    m_currentMode = mode;

    if (mode == VizMode::SliceAxial || mode == VizMode::SliceCoronal || 
        mode == VizMode::SliceSagittal) {
        // 2D 模式：使用图像交互风格 (支持窗宽窗位调整)
        auto style = vtkSmartPointer<vtkInteractorStyleImage>::New();
        style->SetInteractionModeToImage2D(); // 强制为 2D 图像模式
        m_interactor->SetInteractorStyle(style);

    }
    else {
        // 3D 模式：使用轨迹球风格 (支持旋转缩放)
        auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
        m_interactor->SetInteractorStyle(style);
    }
}

void StdRenderContext::SetToolMode(ToolMode mode)
{
    m_toolMode = mode;

    // 根据模式开关 Widget
    // Widget->On() 激活其内部的交互机制
    if (m_distanceWidget) {
        if (mode == ToolMode::DistanceMeasure) {
            m_distanceWidget->On();
        }
        else {
            m_distanceWidget->Off();
        }
    }

    if (m_angleWidget) {
        if (mode == ToolMode::AngleMeasure) {
            m_angleWidget->On();
        }
        else {
            m_angleWidget->Off();
        }
    }

    // 触发渲染刷新 UI
    this->Render();
}

void StdRenderContext::HandleVTKEvent(vtkObject* caller, long unsigned int eventId, void* callData)
{
    // 将基类 Service 转换为具体的 MedicalService 以访问 GetStrategy
    auto medService = std::dynamic_pointer_cast<MedicalVizService>(m_service);
    if (!medService) return;

    vtkRenderWindowInteractor* iren = static_cast<vtkRenderWindowInteractor*>(caller);
    int* eventPos = iren->GetEventPosition();


    // 键盘快捷键处理
    if (eventId == vtkCommand::KeyPressEvent) {
        char key = iren->GetKeyCode();
        std::string keySym = iren->GetKeySym();

        // 按 'd' 开启距离测量，'a' 开启角度测量，'Esc' 退出测量
        if (key == 'd' || key == 'D') {
            SetToolMode(ToolMode::DistanceMeasure);
            std::cout << "Mode: Distance Measurement" << std::endl;
        }
        else if (key == 'a' || key == 'A') {
            SetToolMode(ToolMode::AngleMeasure);
            std::cout << "Mode: Angle Measurement" << std::endl;
        }
        else if (keySym == "Escape") {
            SetToolMode(ToolMode::Navigation);
            std::cout << "Mode: Navigation" << std::endl;
        }
    }

    // 如果当前处于测量模式，屏蔽原有的鼠标左键逻辑（如窗宽窗位调节、十字线拖拽）
    // 让 Widget 能够独占左键点击用于放置测量点
    if (m_toolMode != ToolMode::Navigation) {
        // 允许滚轮切片，但屏蔽左键操作
        if (eventId == vtkCommand::LeftButtonPressEvent ||
            eventId == vtkCommand::MouseMoveEvent ||
            eventId == vtkCommand::LeftButtonReleaseEvent) {
            return;
        }
    }

    // 处理滚轮切片逻辑
    if (m_currentMode == VizMode::SliceAxial ||
        m_currentMode == VizMode::SliceCoronal ||
        m_currentMode == VizMode::SliceSagittal)
    {
        if (eventId == vtkCommand::MouseWheelForwardEvent ||
            eventId == vtkCommand::MouseWheelBackwardEvent) {

            // 计算交互值
            int delta = (eventId == vtkCommand::MouseWheelForwardEvent) ? 1 : -1;
            medService->UpdateInteraction(delta);
            // 触发渲染以更新画面
            this->Render();

            // 中止后续默认处理
            m_eventCallback->SetAbortFlag(1);
        }
        else if (eventId == vtkCommand::LeftButtonPressEvent)
        {
            // 检查 Shift 键状态
            if (iren->GetShiftKey()) {
                m_enableDragCrosshair = true;
                // 设为 1 阻止 VTK 默认的 Window/Level 调整
                m_eventCallback->SetAbortFlag(1);
            }
        }
        else if (eventId == vtkCommand::MouseMoveEvent)
        {
            if (m_enableDragCrosshair)
            {
                m_picker->Pick(eventPos[0], eventPos[1], 0, m_renderer);
                double* worldPos = m_picker->GetPickPosition();

                auto img = medService->GetDataManager()->GetVtkImage();
                if (img && worldPos) {
                    double origin[3], spacing[3];
                    img->GetOrigin(origin);
                    img->GetSpacing(spacing);

                    auto state = medService->GetSharedState();
                    if (state) {
                        int* currentPos = state->GetCursorPosition();
                        int newPos[3] = { currentPos[0], currentPos[1], currentPos[2] };

                        // 计算新坐标
                        int i = (worldPos[0] - origin[0]) / spacing[0];
                        int j = (worldPos[1] - origin[1]) / spacing[1];
                        int k = (worldPos[2] - origin[2]) / spacing[2];

                        newPos[0] = i;
                        newPos[1] = j;
                        newPos[2] = k;

                        if (m_currentMode == VizMode::SliceAxial) {
                            newPos[2] = currentPos[2]; // 轴状位：锁定 Z，只更新 XY
                        }
                        else if (m_currentMode == VizMode::SliceCoronal) {
                            newPos[1] = currentPos[1]; // 冠状位：锁定 Y，只更新 XZ
                        }
                        else if (m_currentMode == VizMode::SliceSagittal) {
                            newPos[0] = currentPos[0]; // 矢状位：锁定 X，只更新 YZ
                        }
                        medService->GetSharedState()->SetCursorPosition(newPos[0], newPos[1], newPos[2]);
                        m_eventCallback->SetAbortFlag(1);
                    }
                }
            }
           
        }
        else if (eventId == vtkCommand::LeftButtonReleaseEvent)
        {
            if (m_enableDragCrosshair) {
                m_enableDragCrosshair = false;
                // 这里通常不需要 Abort，让 Interactor 恢复内部状态
            }
        }
    }

    if (m_currentMode == VizMode::CompositeVolume || m_currentMode == VizMode::CompositeIsoSurface) 
    {
        //vtkRenderWindowInteractor* iren = static_cast<vtkRenderWindowInteractor*>(caller);
        //int* eventPos = iren->GetEventPosition();

        if (eventId == vtkCommand::LeftButtonPressEvent) 
        {
            // 使用 vtkPropPicker 进行拾取
            if (m_picker->Pick(eventPos[0], eventPos[1], 0, m_renderer)) {
                // 获取拾取到的 Actor
                auto pickedActor = m_picker->GetActor();

                // 判断是否为平面
                m_dragAxis = medService->GetPlaneAxis(pickedActor);

                if (m_dragAxis != -1) {
                    // 确认拾取到的是我们的一个平面
                    m_isDragging = true;
                    m_eventCallback->SetAbortFlag(1); // 阻止相机转动
                }
                // 如果 m_dragAxis == -1，说明点到的是主模型或空白处
                // 此时 m_isDragging 保持 false，不中止事件，相机可以正常交互
            }
        }
        else if (eventId == vtkCommand::MouseMoveEvent) 
        {
            //只在拾取到平面时才会触发
            if (m_isDragging && m_dragAxis != -1) {
                // 这里我们用 Pick 来持续获取鼠标下的3D坐标
                // vtkPropPicker 同样返回准确的 PickPosition
                m_picker->Pick(eventPos[0], eventPos[1], 0, m_renderer);
                double* worldPos = m_picker->GetPickPosition();

                auto img = medService->GetDataManager()->GetVtkImage();
                if (img && worldPos) {
                    double origin[3], spacing[3];
                    img->GetOrigin(origin);
                    img->GetSpacing(spacing);

                    auto state = medService->GetSharedState();
                    if (state) {
                        int* currentPos = state->GetCursorPosition();
                        int newPos[3] = { currentPos[0], currentPos[1], currentPos[2] };

                        // 计算新坐标
                        int i = (worldPos[0] - origin[0]) / spacing[0];
                        int j = (worldPos[1] - origin[1]) / spacing[1];
                        int k = (worldPos[2] - origin[2]) / spacing[2];

                        // 只更新锁定的轴
                        if (m_dragAxis == 0) newPos[0] = i;
                        else if (m_dragAxis == 1) newPos[1] = j;
                        else if (m_dragAxis == 2) newPos[2] = k;

                        state->SetCursorPosition(newPos[0], newPos[1], newPos[2]);
                    }
                }
                m_eventCallback->SetAbortFlag(1);
            }
        }
        else if (eventId == vtkCommand::LeftButtonReleaseEvent) 
        {
            m_isDragging = false;
            m_dragAxis = -1;
        }
    }
}