#include "StdRenderContext.h"
#include <vtkCallbackCommand.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleTrackballActor.h>
void StdRenderContext::InitInteractor()
{
    if (m_interactor && !m_interactor->GetInitialized()) {
        m_interactor->Initialize();
    }

    if (m_interactor)
    {
        // CreateRepeatingTimer定时器 间隔 33ms (约 30FPS) 或 16ms (60FPS)
        m_interactor->CreateRepeatingTimer(33);
        m_interactor->AddObserver(vtkCommand::TimerEvent, m_eventCallback, 1.0);
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
    m_interactor->AddObserver(vtkCommand::MouseWheelForwardEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::MouseWheelBackwardEvent, m_eventCallback, 0.5);
    // 监听鼠标左键按下、移动、抬起
    m_interactor->AddObserver(vtkCommand::LeftButtonPressEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::MouseMoveEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::LeftButtonReleaseEvent, m_eventCallback, 0.5);
	// 监听键盘按键事件
    m_interactor->AddObserver(vtkCommand::KeyPressEvent, m_eventCallback, 0.5);
	// 监听exit事件
	m_interactor->AddObserver(vtkCommand::ExitEvent, m_eventCallback, 0.5);
    // 监听 InteractionEvent，用于在拖拽过程中同步矩阵
    m_interactor->AddObserver(vtkCommand::InteractionEvent, m_eventCallback, 0.5);
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
        // 2D 模式 (窗宽窗位调整)
        auto style = vtkSmartPointer<vtkInteractorStyleImage>::New();
        style->SetInteractionModeToImage2D(); // 强制为 2D 图像模式
        m_interactor->SetInteractorStyle(style);

    }
    else {
        // 3D 模式 (旋转缩放)
        auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
        m_interactor->SetInteractorStyle(style);
    }
}

void StdRenderContext::BindService(std::shared_ptr<AbstractAppService> service)
{   
    AbstractRenderContext::BindService(service);
    m_interactiveService = std::dynamic_pointer_cast<AbstractInteractiveService>(service);
}

void StdRenderContext::ToggleOrientationAxes(bool show)
{
    if (show) {
        if (!m_axesWidget) {
            // 创建坐标轴 Actor
            auto axes = vtkSmartPointer<vtkAxesActor>::New();

            // 创建 Widget
            m_axesWidget = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
            m_axesWidget->SetOrientationMarker(axes);

            // 确保 Interactor 已经存在
            if (m_interactor) {
                m_axesWidget->SetInteractor(m_interactor);
            }

            // 设置位置：左下角，占用窗口 20% 大小
            m_axesWidget->SetViewport(0.0, 0.0, 0.2, 0.2);

            m_axesWidget->SetEnabled(1);
            m_axesWidget->InteractiveOff();
        }
        else {
            // 如果 Widget 已存在但被隐藏，重新显示
            m_axesWidget->SetEnabled(1);
        }
    }
    else {
        if (m_axesWidget) {
            m_axesWidget->SetEnabled(0);
        }
    }
}

void StdRenderContext::SetToolMode(ToolMode mode)
{
    if (m_toolMode == ToolMode::ModelTransform && m_interactiveService) {
        vtkProp3D* mainProp = m_interactiveService->GetMainProp();
        if (mainProp) {
            mainProp->SetPickable(false); // 还原为不可拾取，避免干扰切片交互
        }
    }
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
    if (mode == ToolMode::ModelTransform) {
        // 切换到 ModelTransform 时，强制开启 Pickable
        if (m_interactiveService) {
            vtkProp3D* mainProp = m_interactiveService->GetMainProp();
            if (mainProp) {
                mainProp->SetPickable(true); //让 Actor 可被抓取
            }
        }

        // 切换到“操纵 Actor”的交互风格
        auto style = vtkSmartPointer<vtkInteractorStyleTrackballActor>::New();
        m_interactor->SetInteractorStyle(style);

        // TrackballActor 默认是点击哪个 Prop 动哪个。
        // 或者未来在这里强制指定 Prop。
        // 目前架构下，IsoSurfaceStrategy 和 VolumeStrategy 的坐标轴(CubeAxes)是不可拾取的，
        std::cout << "Mode: Model Transform (Rotate/Scale/Translate Model)" << std::endl;
    }
    else if (mode == ToolMode::Navigation || mode == ToolMode::DistanceMeasure || mode == ToolMode::AngleMeasure) {
        // 恢复原来的交互模式 (Camera Trackball 或 Image Slice 2D)
        // 调用既有的 SetInteractionMode 来复位 Style
        SetInteractionMode(m_currentMode);
    }

    // 标记脏状态以刷新 UI
    if (m_interactiveService) m_interactiveService->SetDirty(true);
}

void StdRenderContext::HandleVTKEvent(vtkObject* caller, long unsigned int eventId, void* callData)
{
    if (m_eventCallback)
        m_eventCallback->SetAbortFlag(0);

    vtkRenderWindowInteractor* iren = static_cast<vtkRenderWindowInteractor*>(caller);
    int* eventPos = iren->GetEventPosition();

    if (eventId == vtkCommand::ExitEvent) {
        // 销毁定时器，彻底停止心跳
        if (m_interactor) {
            m_interactor->DestroyTimer();
        }
        return;
    }

	// 心跳定时器处理
    if (eventId == vtkCommand::TimerEvent) {

        // 检查并处理挂起的逻辑更新,同步数据
        if (m_interactiveService) {
            m_interactiveService->ProcessPendingUpdates();
            
			// 检查窗口状态，确保窗口存在且已映射，否则跳过渲染
            if (!m_renderWindow || !m_renderWindow->GetMapped()) {
                return;
            }

            // 检查 Service 检查渲染脏标记
            if (m_interactiveService && m_interactiveService->IsDirty()) {
				// 触发渲染更新,窗口存在且有效时才渲染
                if (m_renderWindow && m_renderWindow->GetGenericWindowId()) {
                    m_renderWindow->Render();
                }
                // 重置标记
                m_interactiveService->SetDirty(false);
            }
        }

        // 处理完心跳直接返回，不干扰后续逻辑
        return;
    }

    // 快捷键 'M' 切换模型变换模式 
    if (eventId == vtkCommand::KeyPressEvent) {
        vtkRenderWindowInteractor* iren = static_cast<vtkRenderWindowInteractor*>(caller);
        char key = iren->GetKeyCode();
        if (key == 'm' || key == 'M') {
            if (m_toolMode == ToolMode::ModelTransform)
                SetToolMode(ToolMode::Navigation);
            else
                SetToolMode(ToolMode::ModelTransform);
            return; // 处理完毕
        }
    }

    // 在模型变换模式下，处理矩阵同步
    // InteractionEvent 在拖拽过程中持续触发，保证流畅度
    if (m_toolMode == ToolMode::ModelTransform && eventId == vtkCommand::InteractionEvent) {
        if (m_interactiveService) {
            // 获取主模型 (Actor 或 Volume)
            vtkProp3D* prop = m_interactiveService->GetMainProp();
            if (prop && prop->GetUserMatrix()) {
                // 将交互修改后的 UserMatrix 同步回 SharedState
                m_interactiveService->SyncModelMatrix(prop->GetUserMatrix());

                // 强制标记脏，触发其他窗口（如果有）同步
                m_interactiveService->MarkDirty();
            }
        }
    }

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
        if (m_toolMode != ToolMode::ModelTransform)
        {
            // 允许滚轮切片，但屏蔽左键操作
            if (eventId == vtkCommand::LeftButtonPressEvent ||
                eventId == vtkCommand::MouseMoveEvent ||
                eventId == vtkCommand::LeftButtonReleaseEvent) {
                m_eventCallback->SetAbortFlag(1);
                return;
            }
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
            m_interactiveService->UpdateInteraction(delta);
            // 触发渲染以更新画面
            // this->Render();
            m_interactiveService->SetDirty(true);

            // 中止后续默认处理
            m_eventCallback->SetAbortFlag(1);
        }
        else if (eventId == vtkCommand::LeftButtonPressEvent)
        {
            // 检查 Shift 键状态
            if (iren->GetShiftKey()) {
                m_enableDragCrosshair = true;
                // 开始拖拽，进入低性能模式
                if (m_interactiveService) {
                    m_interactiveService->SetInteracting(true);
                }
                // 设为 1 阻止 VTK 默认的 Window/Level 调整
                m_eventCallback->SetAbortFlag(1);
            }
        }
        else if (eventId == vtkCommand::MouseMoveEvent)
        {
            if (m_enableDragCrosshair && m_interactiveService)
            {
                m_picker->Pick(eventPos[0], eventPos[1], 0, m_renderer);
                double* worldPos = m_picker->GetPickPosition();
                m_interactiveService->SyncCursorToWorldPosition(worldPos);
                m_eventCallback->SetAbortFlag(1);
            }
           
        }
        else if (eventId == vtkCommand::LeftButtonReleaseEvent)
        {
            if (m_enableDragCrosshair) {
                m_enableDragCrosshair = false;
                // 这里通常不需要 Abort，让 Interactor 恢复内部状态

                // 结束拖拽，通知全局退出交互模式
                if (m_interactiveService) {
                    m_interactiveService->SetInteracting(false);
                }
            }
        }
    }

    if (m_currentMode == VizMode::CompositeVolume || m_currentMode == VizMode::CompositeIsoSurface) 
    {

        if (eventId == vtkCommand::LeftButtonPressEvent) 
        {
            // 使用 vtkPropPicker 进行拾取
            if (m_picker->Pick(eventPos[0], eventPos[1], 0, m_renderer)) {
                // 获取拾取到的 Actor
                auto pickedActor = m_picker->GetActor();

                // 判断是否为平面
                m_dragAxis = m_interactiveService->GetPlaneAxis(pickedActor);
  
                if (m_dragAxis != -1) {
                    // 确认拾取到的是我们的一个平面
                    m_isDragging = true;
                    m_eventCallback->SetAbortFlag(1); // 阻止相机转动

					// 切换不同视窗时，降低渲染更新率
                    if (m_interactiveService) {
                        m_interactiveService->SetInteracting(true);
                    }
                        
					// 告诉vtk交互器，已经开始拖拽，可以适当降低更新率
                    if (m_renderWindow) {
                        m_renderWindow->SetDesiredUpdateRate(15.0);
                    }
                }
                // 如果 m_dragAxis == -1，说明点到的是主模型或空白处
                // 此时 m_isDragging 保持 false，不中止事件，相机可以正常交互
            }
        }
        else if (eventId == vtkCommand::MouseMoveEvent) 
        {
            //只在拾取到平面时才会触发
            if (m_isDragging && m_dragAxis != -1) {
                // 用 Pick 来持续获取鼠标下的3D坐标
                // vtkPropPicker 同样返回准确的 PickPosition
                m_picker->Pick(eventPos[0], eventPos[1], 0, m_renderer);
                double* worldPos = m_picker->GetPickPosition();
				m_interactiveService->SyncCursorToWorldPosition(worldPos, m_dragAxis);
                m_eventCallback->SetAbortFlag(1);
            }
        }
        else if (eventId == vtkCommand::LeftButtonReleaseEvent) 
        {
            if (m_isDragging && m_renderWindow) {
				m_renderWindow->SetDesiredUpdateRate(0.001); // 恢复到静态高精度模式,0.001是VTK推荐的静态模式更新率

                // 强制触发一次最终的高质量渲染
                if (m_interactiveService) {
                    m_interactiveService->SetInteracting(false);
                    m_interactiveService->MarkDirty();
                }
            }
            m_isDragging = false;
            m_dragAxis = -1;
        }
    }
}