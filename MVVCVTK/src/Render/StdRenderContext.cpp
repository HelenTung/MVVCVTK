#include "StdRenderContext.h"
#include "TimeUpdateHandler.h"
#include "Viewer2DHandler.h"
#include "Viewer3DHandler.h"
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleTrackballActor.h>
#include <array>
#include <utility>

namespace {
constexpr double kDefaultObserverPriority = 0.5;
constexpr double kTimerObserverPriority = 1.0; // Timer 事件优先级更高，确保主线程后处理先于同帧普通交互事件收敛
constexpr int kTimerIntervalMs = 33;

static void ConfigureRenderWindowForOverlay(vtkRenderWindow* renderWindow)
{
    if (!renderWindow) {
        return;
    }

    // 叠加层和透明材质都依赖稳定的 alpha/depth 行为；外部 Qt window 注入时也必须走同一约束。
    renderWindow->SetAlphaBitPlanes(1);
    renderWindow->SetMultiSamples(0);
}
}

// ─────────────────────────────────────────────────────────────────────
// 构造
// ─────────────────────────────────────────────────────────────────────
StdRenderContext::StdRenderContext()
{
    ConfigureRenderWindowForOverlay(m_renderWindow);

    m_picker = vtkSmartPointer<vtkPropPicker>::New();

    m_eventCallback = vtkSmartPointer<vtkCallbackCommand>::New();
    m_eventCallback->SetCallback(AbstractRenderContext::DispatchVTKEvent);
    m_eventCallback->SetClientData(this);

    AttachInteractor(vtkSmartPointer<vtkRenderWindowInteractor>::New());
}

StdRenderContext::~StdRenderContext()
{
    RemoveTimer();
    RemoveObservers();
}

void StdRenderContext::AttachInteractor(vtkSmartPointer<vtkRenderWindowInteractor> interactor)
{
    if (!interactor) {
        return;
    }

    if (m_interactor.GetPointer() == interactor.GetPointer()) {
        if (m_renderWindow) {
            // 同一个 interactor 也可能因为外部窗口注入而换绑 window，必须保持 VTK 双向关系一致。
            m_interactor->SetRenderWindow(m_renderWindow);
        }
        EnsureObservers();
        return;
    }

    RemoveTimer();
    RemoveObservers();
    m_interactor = std::move(interactor);
    if (m_renderWindow) {
        m_interactor->SetRenderWindow(m_renderWindow);
    }
    // observer 绑定在 interactor 上；替换 interactor 后必须重新挂载，否则 Qt 注入窗口收不到业务事件。
    EnsureObservers();
    if (m_axesWidget) {
        m_axesWidget->SetInteractor(m_interactor);
    }
    BuildInteractionRouter();
}

void StdRenderContext::EnsureObservers()
{
    if (!m_interactor || !m_eventCallback || !m_observerTags.empty()) {
        return;
    }

    const std::array<unsigned long, 12> events = {
        vtkCommand::MouseWheelForwardEvent,
        vtkCommand::MouseWheelBackwardEvent,
        vtkCommand::LeftButtonPressEvent,
        vtkCommand::MouseMoveEvent,
        vtkCommand::LeftButtonReleaseEvent,
        vtkCommand::KeyPressEvent,
        vtkCommand::KeyReleaseEvent,
        vtkCommand::CharEvent,
        vtkCommand::ExitEvent,
        vtkCommand::InteractionEvent,
        vtkCommand::RightButtonPressEvent,
        vtkCommand::RightButtonReleaseEvent
    };

    // 所有业务关心的交互事件都先汇入同一个 callback，
    // 后面再由 RenderContext 按工具模式和 Router 规则细分处理路径。
    m_observerTags.reserve(events.size());
    for (const auto eventId : events) {
        m_observerTags.push_back(
            m_interactor->AddObserver(eventId, m_eventCallback, kDefaultObserverPriority));
    }
}

void StdRenderContext::RemoveObservers()
{
    if (m_interactor) {
        for (const auto tag : m_observerTags) {
            if (tag != 0) {
                m_interactor->RemoveObserver(tag);
            }
        }
    }
    m_observerTags.clear();
}

void StdRenderContext::EnsureTimer()
{
    if (!m_interactor || !m_eventCallback) {
        return;
    }

    if (m_timerId == 0) {
        m_timerId = m_interactor->CreateRepeatingTimer(kTimerIntervalMs);
    }
    if (m_timerObserverTag == 0) {
        m_timerObserverTag =
            m_interactor->AddObserver(vtkCommand::TimerEvent, m_eventCallback, kTimerObserverPriority);
    }
}

void StdRenderContext::RemoveTimer()
{
    if (m_interactor && m_timerObserverTag != 0) {
        m_interactor->RemoveObserver(m_timerObserverTag);
    }
    if (m_interactor && m_timerId != 0) {
        m_interactor->DestroyTimer(m_timerId);
    }
    m_timerObserverTag = 0;
    m_timerId = 0;
}

// ─────────────────────────────────────────────────────────────────────
// InitializeInteractor —— 初始化定时器
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::InitializeInteractor()
{
    if (m_interactor && !m_interactor->GetInitialized()) {
        m_interactor->Initialize();
    }

    EnsureTimer();

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
    m_interactionRouter.ClearHandlers();

    if (!m_interactiveService) {
        return;
    }

    // 这里的装配顺序就是交互链路优先级：Timer 负责统一收口状态推进，
    // 2D/3D Handler 再分别处理各自模式下的输入命中，避免同一事件被多个处理器重复消费。
    m_interactionRouter.AddHandler(std::make_unique<TimeUpdateHandler>(
        m_interactiveService.get(), m_renderWindow.GetPointer()));

    m_interactionRouter.AddHandler(std::make_unique<Viewer2DHandler>(
        m_interactiveService.get(),
        m_picker.GetPointer(),
        m_renderer.GetPointer()));

    m_interactionRouter.AddHandler(std::make_unique<Viewer3DHandler>(
        m_interactiveService.get(),
        m_picker.GetPointer(),
        m_renderer.GetPointer()));
}

// ─────────────────────────────────────────────────────────────────────
// SetServiceBound
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::SetServiceBound(std::shared_ptr<AbstractAppService> service)
{
    AbstractRenderContext::SetServiceBound(service);
    m_interactiveService =
        std::dynamic_pointer_cast<AbstractInteractiveService>(service);

    // Router handler 持有 service / renderer / picker 等入口，service 换绑后必须重建以避免旧依赖继续收事件。
    BuildInteractionRouter();
}

void StdRenderContext::SetRenderWindow(vtkSmartPointer<vtkRenderWindow> renderWindow)
{
    vtkRenderWindow* oldRenderWindow = GetRenderWindow();
    AbstractRenderContext::SetRenderWindow(std::move(renderWindow));
    if (GetRenderWindow() == oldRenderWindow) {
        return;
    }

    ConfigureRenderWindowForOverlay(GetRenderWindow());
    // 1. 外部 window 已经有 interactor 时优先采用它，让 Qt/QVTK 生命周期继续由宿主掌控。
    // 2. 外部 window 没有 interactor 时沿用当前 interactor，保持独立 VTK 路径行为不变。
    if (GetRenderWindow() && GetRenderWindow()->GetInteractor()) {
        AttachInteractor(GetRenderWindow()->GetInteractor());
    }
    else if (m_interactor) {
        m_interactor->SetRenderWindow(m_renderWindow);
    }
    if (m_axesWidget) {
        m_axesWidget->SetInteractor(m_interactor);
    }

    // TimeUpdateHandler 持有 renderWindow 指针，替换底层窗口后必须重建路由表。
    BuildInteractionRouter();
}

// ─────────────────────────────────────────────────────────────────────
// Start
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::Start()
{
    // 先触发一次 Render，保证窗口初始内容与后续交互状态一致，
    // 然后再进入 interactor 主循环。
    if (m_renderWindow) m_renderWindow->Render();
    if (m_interactor) {
        if (!m_interactor->GetInitialized()) {
            m_interactor->Initialize();
        }
        m_interactor->Start();
    }
}

// ─────────────────────────────────────────────────────────────────────
// SetCameraStyle
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::SetCameraStyle(VizMode mode)
{
    m_currentMode = mode;

    if (mode == VizMode::SliceTop_down
        || mode == VizMode::SliceFront_back
        || mode == VizMode::SliceLeft_right)
    {
        // 切片模式要求屏幕坐标与切片平面交互稳定对应，因此使用 Image2D 风格。
        auto style = vtkSmartPointer<vtkInteractorStyleImage>::New();
        style->SetInteractionModeToImage2D();
        m_interactor->SetInteractorStyle(style);
    }
    else {
        // 3D 模式保留相机轨道球交互；ModelTransform 工具模式会再覆盖成 Actor 交互风格。
        auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
        m_interactor->SetInteractorStyle(style);
    }
}

// ─────────────────────────────────────────────────────────────────────
// SetOrientationAxesVisible
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::SetOrientationAxesVisible(bool show)
{
    if (show) {
        if (!m_axesWidget) {
            // 坐标轴 widget 延迟初始化，避免在不需要时创建额外的 VTK 组件。
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
// SetToolMode —— 工具模式切换 + 交互风格切换
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::SetToolMode(ToolMode mode)
{
    // 离开 ModelTransform 时还原 Pickable
    if (m_toolMode == ToolMode::ModelTransform && m_interactiveService) {
        vtkProp3D* mainProp = m_interactiveService->GetMainProp();
        if (mainProp) mainProp->SetPickable(false);
    }

    m_toolMode = mode;
    if (mode == ToolMode::ModelTransform) {
        if (m_interactiveService) {
            vtkProp3D* mainProp = m_interactiveService->GetMainProp();
            if (mainProp) mainProp->SetPickable(true);
        }
        auto style = vtkSmartPointer<vtkInteractorStyleTrackballActor>::New();
        m_interactor->SetInteractorStyle(style);
    }
    else {
        SetCameraStyle(m_currentMode);
    }

    if (m_interactiveService) m_interactiveService->MarkDirty();
}

void StdRenderContext::SetKeyHandler(
    std::function<InteractionResult(const InteractionEvent&)> handler)
{
    m_keyHandler = std::move(handler);
}

void StdRenderContext::ClearKeyHandler()
{
    m_keyHandler = nullptr;
}

void StdRenderContext::SetTimerHandler(std::function<void()> handler)
{
    m_timerHandler = std::move(handler);
}

void StdRenderContext::ClearTimerHandler()
{
    m_timerHandler = nullptr;
}

void StdRenderContext::BuildInteractionEvent(
    InteractionEvent& eve,
    vtkRenderWindowInteractor* interactor,
    long unsigned int eventId) const
{
    eve.vtkEventId = eventId;
    eve.iren = interactor;

    const int* pos = interactor->GetEventPosition();
    if (pos) {
        eve.x = pos[0];
        eve.y = pos[1];
    }

    eve.shift = interactor->GetShiftKey() != 0;
    eve.ctrl = interactor->GetControlKey() != 0;
    eve.alt = interactor->GetAltKey() != 0;
    eve.keyCode = interactor->GetKeyCode();
    eve.keySym = interactor->GetKeySym() ? interactor->GetKeySym() : "";
    eve.vizMode = m_currentMode;
    eve.toolMode = m_toolMode;
    // 到这里 InteractionEvent 就成为跨 Handler 共享的统一输入模型，
    // 后续 2D/3D/Timer 处理器都不再直接依赖 VTK interactor 原始查询接口。
}

// ─────────────────────────────────────────────────────────────────────
// OnVTKEvent —— 统一入口，委托给 Router
//
// 这里只做三件事：
//   1. ExitEvent / 守卫性检查
//   2. 填充 InteractionEvent → Dispatch → 处理 abortVtk
// ─────────────────────────────────────────────────────────────────────
void StdRenderContext::OnVTKEvent(vtkObject* caller,
    long unsigned int eventId,
    void* callData)
{
    (void)callData;

    if (m_eventCallback) {
        m_eventCallback->AbortFlagOff();
    }

    // ── ExitEvent：销毁定时器，停止心跳 ─────────────────────────────
    if (eventId == vtkCommand::ExitEvent) {
        RemoveTimer();
        return;
    }

    // ── 通用鼠标/Timer 事件 → 交给 Router ────────────────────────────
    auto* iren = vtkRenderWindowInteractor::SafeDownCast(caller);
    if (!iren) return;

    InteractionEvent eve;
    BuildInteractionEvent(eve, iren, eventId);

    // Timer → Broadcast（所有 Handler 均需执行）；其余 → FirstMatch
    const auto dispatchMode = (eventId == vtkCommand::TimerEvent)
        ? RouterDispatchMode::Broadcast
        : RouterDispatchMode::FirstMatch;

    InteractionResult result;
    if (m_keyHandler
        && (eventId == vtkCommand::KeyPressEvent
            || eventId == vtkCommand::KeyReleaseEvent
            || eventId == vtkCommand::CharEvent)) {
        result = m_keyHandler(eve);
    }

    if (!result.handled) {
        const InteractionResult routerResult =
            m_interactionRouter.Dispatch(eve, dispatchMode);
        result.handled = result.handled || routerResult.handled;
        result.abortVtk = result.abortVtk || routerResult.abortVtk;
    }

    if (eventId == vtkCommand::TimerEvent && m_timerHandler) {
        m_timerHandler();
    }

    if (result.abortVtk && m_eventCallback) {
        // 业务层已经完整消费该事件时，阻止 VTK 默认相机/窗口行为继续处理，避免双重响应。
        m_eventCallback->SetAbortFlag(1);
    }
}
