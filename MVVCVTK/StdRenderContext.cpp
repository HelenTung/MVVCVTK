#include "StdRenderContext.h"
#include <vtkCallbackCommand.h>
#include <vtkInteractorStyleImage.h>

void StdRenderContext::InitInteractor()
{
    if (m_interactor) {
        m_interactor->Initialize();
    }
}

StdRenderContext::StdRenderContext()
{
	// 初始化交互器
    m_interactor = vtkSmartPointer<vtkRenderWindowInteractor>::New();
    m_interactor->SetRenderWindow(m_renderWindow);

	// 初始化拾取器
    m_picker = vtkSmartPointer<vtkCellPicker>::New();
    m_picker->SetTolerance(0.005); // 设置拾取精度
    
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

    if (mode == VizMode::AxialSlice) {
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

void StdRenderContext::HandleVTKEvent(vtkObject* caller, long unsigned int eventId, void* callData)
{
    // 将基类 Service 转换为具体的 MedicalService 以访问 GetStrategy
    auto medService = std::dynamic_pointer_cast<MedicalVizService>(m_service);
    if (!medService) return;

    // 处理滚轮切片逻辑
    if (m_currentMode == VizMode::AxialSlice) 
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
    }

     if (m_currentMode == VizMode::CompositeVolume || m_currentMode == VizMode::CompositeIsoSurface) 
    {
        vtkRenderWindowInteractor* iren = static_cast<vtkRenderWindowInteractor*>(caller);
        int* eventPos = iren->GetEventPosition();

        if (eventId == vtkCommand::LeftButtonPressEvent) 
        {
            // 尝试拾取
            if (m_picker->Pick(eventPos[0], eventPos[1], 0, m_renderer)) {
                // 如果点到了东西 (切片)，开始拖拽
                // 为了体验更好，你可以检查 m_picker->GetActor() 是否是你那个切片 Actor
                // 但在这里简单的全场景拾取通常足够
                m_isDragging = true;
                
                // 此时通常需要禁止默认的旋转操作 (这也是为什么通常要自定义 InteractorStyle)
                // 这里简单粗暴地拦截事件：
                m_eventCallback->SetAbortFlag(1); 
            }
        }
        else if (eventId == vtkCommand::MouseMoveEvent) 
        {
            if (m_isDragging) {
                // 持续拾取，获取新的世界坐标
                m_picker->Pick(eventPos[0], eventPos[1], 0, m_renderer);
                double* worldPos = m_picker->GetPickPosition();

                // 将世界坐标转换为图像索引 (IJK)
                auto img = medService->GetDataManager()->GetVtkImage();
                if (img) {
                    double origin[3], spacing[3];
                    img->GetOrigin(origin);
                    img->GetSpacing(spacing);
                    
                    int i = (worldPos[0] - origin[0]) / spacing[0];
                    int j = (worldPos[1] - origin[1]) / spacing[1];
                    int k = (worldPos[2] - origin[2]) / spacing[2];

                    // 调用 Service 更新状态 -> 触发所有 2D 视图联动
                    // 注意：这会反过来调用 OnStateChanged，更新 3D 切片位置
                    // 形成闭环
                    auto state = medService->GetSharedState(); // 你需要在 Service 加个 GetState 接口
                    if(state) state->SetCursorPosition(i, j, k);
                }

                // 阻止默认的旋转行为
                m_eventCallback->SetAbortFlag(1); 
            }
        }
        else if (eventId == vtkCommand::LeftButtonReleaseEvent) 
        {
            m_isDragging = false;
        }
     }
}