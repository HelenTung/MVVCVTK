#include "StdRenderContext.h"
#include "TimeUpdateHandler.h"   // 新增
#include "Viewer2DHandler.h"     // 新增
#include "Viewer3DHandler.h"     // 新增
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleTrackballActor.h>

// ─────────────────────────────────────────────────────────────────────
// 构造
// ─────────────────────────────────────────────────────────────────────
StdRenderContext::StdRenderContext()
{
    m_interactor = vtkSmartPointer<vtkRenderWindowInteractor>::New();
    m_interactor->SetRenderWindow(m_renderWindow);

    m_picker = vtkSmartPointer<vtkPropPicker>::New();

    m_eventCallback = vtkSmartPointer<vtkCallbackCommand>::New();
    m_eventCallback->SetCallback(AbstractRenderContext::DispatchVTKEvent);
    m_eventCallback->SetClientData(this);

    // 监听所有需要路由的 VTK 事件
    m_interactor->AddObserver(vtkCommand::MouseWheelForwardEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::MouseWheelBackwardEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::LeftButtonPressEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::MouseMoveEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::LeftButtonReleaseEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::KeyPressEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::ExitEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::InteractionEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::RightButtonPressEvent, m_eventCallback, 0.5);
    m_interactor->AddObserver(vtkCommand::RightButtonReleaseEvent, m_eventCallback, 0.5);
}

// ─────────────────────────────────────────────────────────────────────
// InitInteractor —— 初始化定时器 + 测量 Widget
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::InitInteractor()
{
    if (m_interactor && !m_interactor->GetInitialized()) {
        m_interactor->Initialize();
    }

    if (m_interactor) {
        m_interactor->CreateRepeatingTimer(33);  // ~30 FPS
        m_interactor->AddObserver(vtkCommand::TimerEvent, m_eventCallback, 1.0);
    }

    if (!m_distanceWidget) {
        m_distanceWidget = vtkSmartPointer<vtkDistanceWidget>::New();
        m_distanceWidget->SetInteractor(m_interactor);
        m_distanceWidget->CreateDefaultRepresentation();
        m_distanceWidget->SetPriority(1.0);
    }

    if (!m_angleWidget) {
        m_angleWidget = vtkSmartPointer<vtkAngleWidget>::New();
        m_angleWidget->SetInteractor(m_interactor);
        m_angleWidget->CreateDefaultRepresentation();
        m_angleWidget->SetPriority(1.0);
    }

    // interactor 就位后才能正确构建 Router（TimeUpdateHandler 需要 renderWindow）
    BuildInteractionRouter();
}

// ─────────────────────────────────────────────────────────────────────
// BuildInteractionRouter —— 装配 Handler
//
// 顺序决定 FirstMatch 优先级（越前越优先）：
//   1. TimeUpdateHandler  → Timer 心跳，使用 Broadcast 模式单独处理
//   2. Viewer2DHandler    → SliceXxx 模式下的滚轮/十字线
//   3. Viewer3DHandler    → CompositeXxx 模式下的平面拖拽
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::BuildInteractionRouter()
{
    m_interactionRouter.Clear();

    if (!m_interactiveService) {
        return;
    }

    m_interactionRouter.Add(std::make_unique<TimeUpdateHandler>(
        m_interactiveService.get(), m_renderWindow.GetPointer()));

    m_interactionRouter.Add(std::make_unique<Viewer2DHandler>(
        m_interactiveService.get(),
        m_picker.GetPointer(),
        m_renderer.GetPointer()));

    m_interactionRouter.Add(std::make_unique<Viewer3DHandler>(
        m_interactiveService.get(),
        m_picker.GetPointer(),
        m_renderer.GetPointer()));
}

// ─────────────────────────────────────────────────────────────────────
// BindService
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::BindService(std::shared_ptr<AbstractAppService> service)
{
    AbstractRenderContext::BindService(service);
    m_interactiveService =
        std::dynamic_pointer_cast<AbstractInteractiveService>(service);

    // Service 就位后重建 Router
    BuildInteractionRouter();
}

// ─────────────────────────────────────────────────────────────────────
// Start
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::Start()
{
    if (m_renderWindow) m_renderWindow->Render();
    if (m_interactor) {
        if (!m_interactor->GetInitialized()) {
            m_interactor->Initialize();
        }
        m_interactor->Start();
    }
}

// ─────────────────────────────────────────────────────────────────────
// SetInteractionMode
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::SetInteractionMode(VizMode mode)
{
    m_currentMode = mode;

    if (mode == VizMode::SliceAxial
        || mode == VizMode::SliceCoronal
        || mode == VizMode::SliceSagittal)
    {
        auto style = vtkSmartPointer<vtkInteractorStyleImage>::New();
        style->SetInteractionModeToImage2D();
        m_interactor->SetInteractorStyle(style);
    }
    else {
        auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
        m_interactor->SetInteractorStyle(style);
    }
}

// ─────────────────────────────────────────────────────────────────────
// ToggleOrientationAxes
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::ToggleOrientationAxes(bool show)
{
    if (show) {
        if (!m_axesWidget) {
            auto axes = vtkSmartPointer<vtkAxesActor>::New();
            m_axesWidget = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
            m_axesWidget->SetOrientationMarker(axes);
            if (m_interactor) m_axesWidget->SetInteractor(m_interactor);
            m_axesWidget->SetViewport(0.0, 0.0, 0.2, 0.2);
            m_axesWidget->SetEnabled(1);
            m_axesWidget->InteractiveOff();
        }
        else {
            m_axesWidget->SetEnabled(1);
        }
    }
    else {
        if (m_axesWidget) m_axesWidget->SetEnabled(0);
    }
}

// ─────────────────────────────────────────────────────────────────────
// SetToolMode —— 测量 Widget 开关 + 交互风格切换
// （与路由无关，保持原有逻辑）
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::SetToolMode(ToolMode mode)
{
    // 离开 ModelTransform 时还原 Pickable
    if (m_toolMode == ToolMode::ModelTransform && m_interactiveService) {
        vtkProp3D* mainProp = m_interactiveService->GetMainProp();
        if (mainProp) mainProp->SetPickable(false);
    }

    m_toolMode = mode;

    if (m_distanceWidget) {
        (mode == ToolMode::DistanceMeasure)
            ? m_distanceWidget->On()
            : m_distanceWidget->Off();
    }
    if (m_angleWidget) {
        (mode == ToolMode::AngleMeasure)
            ? m_angleWidget->On()
            : m_angleWidget->Off();
    }

    if (mode == ToolMode::ModelTransform) {
        if (m_interactiveService) {
            vtkProp3D* mainProp = m_interactiveService->GetMainProp();
            if (mainProp) mainProp->SetPickable(true);
        }
        auto style = vtkSmartPointer<vtkInteractorStyleTrackballActor>::New();
        m_interactor->SetInteractorStyle(style);
    }
    else {
        SetInteractionMode(m_currentMode);
    }

    if (m_interactiveService) m_interactiveService->SetDirty(true);
}

void StdRenderContext::SetElementVisible(uint32_t flagBit, bool show)
{
    if (m_interactiveService) {
        m_interactiveService->SetElementVisible(flagBit, show);
    }
}
// ─────────────────────────────────────────────────────────────────────
// HandleVTKEvent —— 统一入口，委托给 Router
//
// 这里只做三件事：
//   1. ExitEvent / 守卫性检查
//   2. 测量模式下屏蔽左键（让 Widget 独占）
//   3. 填充 InteractionEvent → Dispatch → 处理 abortVtk
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::HandleVTKEvent(vtkObject* caller,
    long unsigned int eventId,
    void* callData)
{
    (void)callData;

    // ── ExitEvent：销毁定时器，停止心跳 ─────────────────────────────
    if (eventId == vtkCommand::ExitEvent) {
        if (m_interactor) m_interactor->DestroyTimer();
        return;
    }

    // ── 键盘：ModelTransform 切换 & 测量模式切换 ─────────────────────
    // （这部分与路由解耦，StdRenderContext 自身处理快捷键）
    if (eventId == vtkCommand::KeyPressEvent) {
        auto* iren = vtkRenderWindowInteractor::SafeDownCast(caller);
        if (iren) {
            const char key = iren->GetKeyCode();
            const std::string s = iren->GetKeySym() ? iren->GetKeySym() : "";

            if (key == 'm' || key == 'M') {
                SetToolMode(m_toolMode == ToolMode::ModelTransform
                    ? ToolMode::Navigation
                    : ToolMode::ModelTransform);
                return;
            }
            if (key == 'd' || key == 'D') { SetToolMode(ToolMode::DistanceMeasure); return; }
            if (key == 'a' || key == 'A') { SetToolMode(ToolMode::AngleMeasure);    return; }
            if (s == "Escape") { SetToolMode(ToolMode::Navigation);      return; }
        }
    }

    // ── ModelTransform：InteractionEvent → 矩阵同步 ──────────────────
    if (m_toolMode == ToolMode::ModelTransform
        && eventId == vtkCommand::InteractionEvent
        && m_interactiveService)
    {
        vtkProp3D* prop = m_interactiveService->GetMainProp();
        if (prop && prop->GetUserMatrix()) {
            m_interactiveService->SyncModelMatrix(prop->GetUserMatrix());
            m_interactiveService->MarkDirty();
        }
        return;
    }

    // ── 测量模式：屏蔽左键，让 Widget 独占 ───────────────────────────
    if (m_toolMode == ToolMode::DistanceMeasure
        || m_toolMode == ToolMode::AngleMeasure)
    {
        if (eventId == vtkCommand::LeftButtonPressEvent
            || eventId == vtkCommand::LeftButtonReleaseEvent
            || eventId == vtkCommand::MouseMoveEvent)
        {
            return;  // 不中止 VTK，让 Widget 正常接收
        }
    }

    // ── 通用鼠标/Timer 事件 → 交给 Router ────────────────────────────
    auto* iren = vtkRenderWindowInteractor::SafeDownCast(caller);
    if (!iren) return;

    InteractionEvent eve;
    eve.vtkEventId = eventId;
    eve.iren = iren;

    const int* pos = iren->GetEventPosition();
    if (pos) { eve.x = pos[0]; eve.y = pos[1]; }

    eve.shift = iren->GetShiftKey() != 0;
    eve.ctrl = iren->GetControlKey() != 0;
    eve.alt = iren->GetAltKey() != 0;
    eve.keyCode = iren->GetKeyCode();
    eve.keySym = iren->GetKeySym() ? iren->GetKeySym() : "";
    eve.vizMode = m_currentMode;
    eve.toolMode = m_toolMode;

    // Timer → Broadcast（所有 Handler 均需执行）；其余 → FirstMatch
    const auto dispatchMode = (eventId == vtkCommand::TimerEvent)
        ? RouterDispatchMode::Broadcast
        : RouterDispatchMode::FirstMatch;

    const InteractionResult result =
        m_interactionRouter.Dispatch(eve, dispatchMode);

    if (result.abortVtk && m_eventCallback) {
        m_eventCallback->SetAbortFlag(1);
    }
}