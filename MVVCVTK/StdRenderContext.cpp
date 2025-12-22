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
    m_interactor = vtkSmartPointer<vtkRenderWindowInteractor>::New();
    m_interactor->SetRenderWindow(m_renderWindow);

    // 初始化回调命令
    m_eventCallback = vtkSmartPointer<vtkCallbackCommand>::New();

    m_eventCallback->SetCallback(AbstractRenderContext::DispatchVTKEvent);
    m_eventCallback->SetClientData(this);

    // 监听滚轮事件
    m_interactor->AddObserver(vtkCommand::MouseWheelForwardEvent, m_eventCallback, 1.0);
    m_interactor->AddObserver(vtkCommand::MouseWheelBackwardEvent, m_eventCallback, 1.0);

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
}