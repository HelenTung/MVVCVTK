#include "Interaction/ViewContextFactory.h"

#include "Interaction/InputCallbackHandler.h"
#include "Interaction/InteractionRouter.h"
#include "Interaction/TimeUpdateHandler.h"
#include "Interaction/Viewer2DHandler.h"
#include "Interaction/Viewer3DHandler.h"

#include <vtkAxesActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkInteractorStyle.h>
#include <vtkInteractorStyleImage.h>
#include <vtkInteractorStyleTrackballActor.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkProp3D.h>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

#include <array>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr double kObserverPriority = 0.5;
// 这里只提高同一次 TimerEvent 内的 callback 顺序，不保证跨事件顺序。
constexpr double kTimerPriority = 1.0;
constexpr int kTimerIntervalMs = 33;

} // namespace

class StdViewContext final : public AbstractViewContext {
public:
    explicit StdViewContext(InteractionPorts ports);
    ~StdViewContext() override;
    static void RemoveContext(
        AbstractViewContext* context) noexcept;

    bool SetInteractorReady() override;
    bool Start() override;
    bool StopInput() override;
    bool SetCameraStyle(VizMode mode) override;
    bool SetRenderWindow(
        vtkSmartPointer<vtkRenderWindow> renderWindow) override;
    bool SetOrientationAxesVisible(bool isVisible) override;
    bool GetOrientationAxesVisible() const override
    {
        return GetIsOwnerThread() && m_isAxesVisible;
    }
    bool SetToolMode(ToolMode mode) override;
    ToolMode GetToolMode() const override
    {
        return GetIsOwnerThread()
            ? m_toolMode : ToolMode::Navigation;
    }
    bool SetInputHandler(
        std::function<InteractionResult(const InteractionEvent&)> handler,
        std::vector<InteractionEventKind> eventKinds) override;
    bool ClearInputHandler() override;
    bool SetTimerHandler(std::function<void()> handler) override;
    bool ClearTimerHandler() override;
    vtkRenderWindowInteractor* GetInteractor() const override
    {
        return GetIsOwnerThread()
            ? m_interactor.GetPointer() : nullptr;
    }
    bool GetIsCreated() const noexcept
    {
        return m_isCreated;
    }

protected:
    void OnVTKEvent(
        vtkObject* caller,
        unsigned long eventId,
        void* callData) override;

private:
    bool AttachInteractor(
        vtkSmartPointer<vtkRenderWindowInteractor> interactor);
    bool AttachObservers();
    bool RemoveObservers();
    bool AttachTimer();
    bool RemoveTimer();
    bool BuildInteractionRouter();
    bool SetInputStyle();
    double GetRenderRate(bool isInteracting) const noexcept;
    InteractionEventKind GetEventKind(unsigned long eventId) const;
    void BuildInteractionEvent(
        InteractionEvent& event,
        vtkRenderWindowInteractor* interactor,
        InteractionEventKind eventKind) const;

    InteractionPorts m_ports;
    vtkSmartPointer<vtkRenderWindowInteractor> m_interactor;
    vtkSmartPointer<vtkCallbackCommand> m_eventCallback;
    vtkSmartPointer<vtkPropPicker> m_picker;
    VizMode m_currentMode = VizMode::Volume;
    ToolMode m_toolMode = ToolMode::Navigation;
    InteractionRouter m_interactionRouter;
    std::vector<unsigned long> m_observerTags;
    unsigned long m_timerObserverTag = 0;
    int m_timerId = 0;
    bool m_isInteractorReady = false;
    std::function<InteractionResult(const InteractionEvent&)>
        m_inputHandler;
    std::vector<InteractionEventKind> m_inputEventKinds;
    std::function<void()> m_timerHandler;
    vtkSmartPointer<vtkOrientationMarkerWidget> m_axesWidget;
    bool m_isAxesVisible = false;
    bool m_isCreated = false;
    bool m_isStyleInteracting = false;
};

std::shared_ptr<AbstractViewContext> CreateViewContext(
    InteractionPorts ports)
{
    if (!ports.update || !ports.state
        || !ports.slice || !ports.model) {
        return nullptr;
    }
    std::shared_ptr<AbstractViewContext> context(
        new StdViewContext(std::move(ports)),
        &StdViewContext::RemoveContext);
    const auto* value = static_cast<StdViewContext*>(context.get());
    return value->GetIsCreated() ? std::move(context) : nullptr;
}

StdViewContext::StdViewContext(InteractionPorts ports)
    : m_ports(std::move(ports))
{
    if (m_renderWindow) {
        // 叠加层和透明材质都依赖稳定的 alpha/depth 行为。
        m_renderWindow->SetAlphaBitPlanes(1);
        m_renderWindow->SetMultiSamples(0);
    }

    m_picker = vtkSmartPointer<vtkPropPicker>::New();
    m_eventCallback = vtkSmartPointer<vtkCallbackCommand>::New();
    m_eventCallback->SetCallback(
        AbstractViewContext::DispatchVTKEvent);
    m_eventCallback->SetClientData(this);

    m_isCreated = AttachInteractor(
        vtkSmartPointer<vtkRenderWindowInteractor>::New());
}

StdViewContext::~StdViewContext()
{
    if (m_eventCallback) {
        m_eventCallback->SetClientData(nullptr);
    }
}

void StdViewContext::RemoveContext(
    AbstractViewContext* context) noexcept
{
    auto* value = static_cast<StdViewContext*>(context);
    if (!value) return;
    if (!value->GetIsOwnerThread() || !value->StopInput()) {
        // 正常 Session 路径必须在 owner thread 先 StopInput；这里仅是
        // 误用诊断。析构会先清空 callback clientData，不再为保命永久泄漏。
        std::cerr
            << "[Interaction] View context destroyed without owner-thread StopInput.\n";
    }
    delete value;
}

bool StdViewContext::AttachInteractor(
    vtkSmartPointer<vtkRenderWindowInteractor> interactor)
{
    if (!interactor) return false;

    // 1. 所有 tag、timer 和 handler 都属于旧绑定，先完整卸载。
    const bool wasReady = m_isInteractorReady;
    if (!RemoveTimer()) return false;
    if (!RemoveObservers()) {
        if (wasReady) (void)AttachTimer();
        return false;
    }
    m_interactionRouter.ClearHandlers();
    m_isInteractorReady = false;

    // 2. 修复 window/interactor 双向关系，再恢复 style、pickable 与 axes。
    m_interactor = std::move(interactor);
    if (m_renderWindow) {
        m_interactor->SetRenderWindow(m_renderWindow);
    }
    if (!SetInputStyle()) return false;

    if (m_axesWidget) {
        m_axesWidget->SetEnabled(0);
        m_axesWidget->SetInteractor(m_interactor);
        if (m_isAxesVisible) {
            m_axesWidget->SetEnabled(1);
            m_axesWidget->InteractiveOff();
        }
    }

    // 3. router 和 observer 只引用当前端口与当前 VTK 对象。
    if (wasReady) return SetInteractorReady();
    return BuildInteractionRouter() && AttachObservers();
}

bool StdViewContext::AttachObservers()
{
    if (!m_interactor || !m_eventCallback) return false;

    if (m_observerTags.empty()) {
        const std::array<unsigned long, 11> events = {
            vtkCommand::MouseWheelForwardEvent,
            vtkCommand::MouseWheelBackwardEvent,
            vtkCommand::LeftButtonPressEvent,
            vtkCommand::MouseMoveEvent,
            vtkCommand::LeftButtonReleaseEvent,
            vtkCommand::KeyPressEvent,
            vtkCommand::KeyReleaseEvent,
            vtkCommand::CharEvent,
            vtkCommand::ExitEvent,
            vtkCommand::RightButtonPressEvent,
            vtkCommand::RightButtonReleaseEvent
        };

        m_observerTags.reserve(events.size());
        for (const auto eventId : events) {
            const auto tag = m_interactor->AddObserver(
                eventId, m_eventCallback, kObserverPriority);
            if (tag == 0) {
                (void)RemoveObservers();
                return false;
            }
            m_observerTags.push_back(tag);
        }
    }

    // Start/End 是当前 style 的事件；换 style 后必须重新挂载。
    auto* style = m_interactor->GetInteractorStyle();
    if (!style) return false;
    style->RemoveObservers(
        vtkCommand::StartInteractionEvent, m_eventCallback);
    style->RemoveObservers(
        vtkCommand::EndInteractionEvent, m_eventCallback);
    const auto startTag = style->AddObserver(
        vtkCommand::StartInteractionEvent,
        m_eventCallback,
        kObserverPriority);
    const auto endTag = style->AddObserver(
        vtkCommand::EndInteractionEvent,
        m_eventCallback,
        kObserverPriority);
    if (startTag != 0
        && endTag != 0) return true;

    (void)RemoveObservers();
    return false;
}

bool StdViewContext::RemoveObservers()
{
    if (m_ports.state) {
        const InteractionSource source{
            "ViewContext", "CameraStyle"
        };
        // 业务状态通知不决定物理 observer 是否已经移除。
        try {
            (void)m_ports.state->SetInteracting(source, false);
        }
        catch (...) {
        }
    }

    if (m_interactor) {
        auto* style = m_interactor->GetInteractorStyle();
        if (style && m_eventCallback) {
            style->RemoveObservers(
                vtkCommand::StartInteractionEvent, m_eventCallback);
            style->RemoveObservers(
                vtkCommand::EndInteractionEvent, m_eventCallback);
        }
        for (const auto tag : m_observerTags) {
            if (tag != 0) m_interactor->RemoveObserver(tag);
        }
    }
    m_observerTags.clear();
    m_isStyleInteracting = false;
    if (m_renderWindow) {
        m_renderWindow->SetDesiredUpdateRate(
            GetRenderRate(false));
    }
    return true;
}

bool StdViewContext::AttachTimer()
{
    if (!m_interactor || !m_eventCallback) return false;

    const int oldTimerId = m_timerId;
    const unsigned long oldObserverTag = m_timerObserverTag;
    const int nextTimerId = oldTimerId != 0
        ? oldTimerId
        : m_interactor->CreateRepeatingTimer(kTimerIntervalMs);
    const unsigned long nextObserverTag = oldObserverTag != 0
        ? oldObserverTag
        : m_interactor->AddObserver(
            vtkCommand::TimerEvent,
            m_eventCallback,
            kTimerPriority);
    if (nextTimerId != 0 && nextObserverTag != 0) {
        m_timerId = nextTimerId;
        m_timerObserverTag = nextObserverTag;
        return true;
    }

    if (oldObserverTag == 0 && nextObserverTag != 0) {
        m_interactor->RemoveObserver(nextObserverTag);
    }
    if (oldTimerId == 0 && nextTimerId != 0
        && m_interactor->DestroyTimer(nextTimerId) == 0) {
        // 销毁失败时保留 timer id，StopInput 可以在同一 interactor 上重试。
        m_timerId = nextTimerId;
    }
    return false;
}

bool StdViewContext::RemoveTimer()
{
    // 先销毁 timer；失败时 observer/tag 均保留，原输入链仍可运行并重试。
    if (m_timerId != 0) {
        if (!m_interactor) return false;
        const int timerId = m_timerId;
        if (m_interactor->DestroyTimer(timerId) == 0) {
            // 外部已销毁 timer 时先记住物理事实、保留 observer；下一次
            // StopInput 只需完成剩余阶段。真正的销毁失败仍保留 id 重试。
            if (m_interactor->GetTimerDuration(timerId) == 0) {
                m_timerId = 0;
            }
            return false;
        }
        m_timerId = 0;
    }
    if (m_timerObserverTag != 0) {
        if (!m_interactor) return false;
        m_interactor->RemoveObserver(m_timerObserverTag);
        m_timerObserverTag = 0;
    }
    return true;
}

bool StdViewContext::SetInteractorReady()
{
    if (!GetIsOwnerThread() || !m_interactor) return false;
    if (m_renderWindow) {
        m_interactor->SetRenderWindow(m_renderWindow);
    }
    if (!m_interactor->GetInitialized()) {
        m_interactor->Initialize();
    }

    const bool isReady = BuildInteractionRouter()
        && AttachObservers()
        && AttachTimer();
    m_isInteractorReady = isReady;
    return isReady;
}

bool StdViewContext::BuildInteractionRouter()
{
    m_interactionRouter.ClearHandlers();
    if (!m_ports.update || !m_ports.state
        || !m_ports.slice || !m_ports.model) {
        return false;
    }

    // 顺序就是 FirstMatch 优先级；Timer 使用 Broadcast 单独处理。
    m_interactionRouter.AttachHandler(
        std::make_unique<TimeUpdateHandler>(
            m_ports.update.get(), m_renderWindow.GetPointer()));

    if (m_inputHandler) {
        m_interactionRouter.AttachHandler(
            std::make_unique<InputCallbackHandler>(
                m_inputHandler, m_inputEventKinds));
    }

    m_interactionRouter.AttachHandler(
        std::make_unique<Viewer2DHandler>(
            m_ports.state.get(),
            m_ports.slice.get(),
            m_ports.model.get(),
            m_ports.update.get(),
            m_picker.GetPointer(),
            m_renderer.GetPointer()));
    m_interactionRouter.AttachHandler(
        std::make_unique<Viewer3DHandler>(
            m_ports.state.get(),
            m_ports.slice.get(),
            m_ports.model.get(),
            m_ports.update.get(),
            m_picker.GetPointer(),
            m_renderer.GetPointer()));
    return true;
}

bool StdViewContext::SetInputStyle()
{
    if (!m_interactor) return false;

    if (m_ports.model) {
        auto* mainProp = m_ports.model->GetMainProp();
        if (mainProp) {
            mainProp->SetPickable(
                m_toolMode == ToolMode::ModelTransform);
        }
    }

    if (m_toolMode == ToolMode::ModelTransform) {
        m_interactor->SetInteractorStyle(
            vtkSmartPointer<vtkInteractorStyleTrackballActor>::New());
        return true;
    }

    if (m_currentMode == VizMode::SliceTop_down
        || m_currentMode == VizMode::SliceFront_back
        || m_currentMode == VizMode::SliceLeft_right) {
        auto style = vtkSmartPointer<vtkInteractorStyleImage>::New();
        style->SetInteractionModeToImage2D();
        m_interactor->SetInteractorStyle(style);
        return true;
    }

    m_interactor->SetInteractorStyle(
        vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New());
    return true;
}

bool StdViewContext::SetRenderWindow(
    vtkSmartPointer<vtkRenderWindow> renderWindow)
{
    if (!GetIsOwnerThread() || !renderWindow) return false;
    const auto oldWindow = m_renderWindow;
    const auto oldInteractor = m_interactor;
    const bool wasReady = m_isInteractorReady;
    vtkSmartPointer<vtkRenderWindowInteractor> interactor =
        m_interactor;
    if (renderWindow->GetInteractor()) {
        interactor = renderWindow->GetInteractor();
    }

    if (!AbstractViewContext::SetRenderWindow(
            std::move(renderWindow))) {
        return false;
    }

    m_renderWindow->SetAlphaBitPlanes(1);
    m_renderWindow->SetMultiSamples(0);
    if (AttachInteractor(std::move(interactor))) return true;

    // Context 自己保证换绑失败不遗留半绑定；Host 仍负责 Render port
    // 失败时的跨端口事务回滚。
    (void)AbstractViewContext::SetRenderWindow(oldWindow);
    const bool isRolledBack = AttachInteractor(oldInteractor)
        && (!wasReady || SetInteractorReady());
    if (!isRolledBack) {
        // 回滚也失败时不再伪装成可交互 Context；Registry 会将该 View
        // 标记为 unavailable，Session StopPending 统一保留并重试。
        (void)StopInput();
        m_isInteractorReady = false;
    }
    return false;
}

bool StdViewContext::Start()
{
    if (!GetIsOwnerThread()
        || !m_renderWindow || !m_interactor) {
        return false;
    }
    if (!m_isInteractorReady && !SetInteractorReady()) return false;

    m_renderWindow->Render();
    m_interactor->Start();
    return true;
}

bool StdViewContext::StopInput()
{
    if (!GetIsOwnerThread()) return false;
    if (!RemoveTimer()) return false;
    const bool isObserverStopped = RemoveObservers();
    m_interactionRouter.ClearHandlers();
    m_isInteractorReady = false;
    return isObserverStopped;
}

bool StdViewContext::SetCameraStyle(const VizMode mode)
{
    if (!GetIsOwnerThread() || !m_interactor) return false;
    const VizMode oldMode = m_currentMode;
    const bool isObserverRemoved = RemoveObservers();
    if (!isObserverRemoved) {
        if (!AttachObservers()) (void)StopInput();
        return false;
    }

    m_currentMode = mode;
    const bool isStyleSet = SetInputStyle() && AttachObservers();
    if (isStyleSet) return true;

    // style/observer 是一个提交单元；失败时恢复旧 mode 与旧 style。
    (void)RemoveObservers();
    m_currentMode = oldMode;
    if (!SetInputStyle() || !AttachObservers()) {
        (void)StopInput();
    }
    return false;
}

bool StdViewContext::SetOrientationAxesVisible(
    const bool isVisible)
{
    if (!GetIsOwnerThread()
        || (isVisible && !m_interactor)) {
        return false;
    }

    m_isAxesVisible = isVisible;
    if (!isVisible) {
        if (m_axesWidget) m_axesWidget->SetEnabled(0);
        return true;
    }

    if (!m_axesWidget) {
        auto axes = vtkSmartPointer<vtkAxesActor>::New();
        m_axesWidget =
            vtkSmartPointer<vtkOrientationMarkerWidget>::New();
        m_axesWidget->SetOrientationMarker(axes);
        m_axesWidget->SetViewport(0.0, 0.0, 0.2, 0.2);
    }
    m_axesWidget->SetInteractor(m_interactor);
    m_axesWidget->SetEnabled(1);
    m_axesWidget->InteractiveOff();
    return true;
}

bool StdViewContext::SetToolMode(const ToolMode mode)
{
    if (!GetIsOwnerThread()
        || !m_interactor || !m_ports.update) {
        return false;
    }

    const ToolMode oldMode = m_toolMode;
    const bool isObserverRemoved = RemoveObservers();
    if (!isObserverRemoved) {
        if (!AttachObservers()) (void)StopInput();
        return false;
    }

    m_toolMode = mode;
    if (SetInputStyle()
        && AttachObservers()
        && m_ports.update->SetRenderNeeded()) {
        return true;
    }

    // dirty 拒绝也属于失败；恢复旧 tool/style，避免返回 false 后遗留新模式。
    (void)RemoveObservers();
    m_toolMode = oldMode;
    if (!SetInputStyle() || !AttachObservers()) {
        (void)StopInput();
    }
    return false;
}

bool StdViewContext::SetInputHandler(
    std::function<InteractionResult(const InteractionEvent&)> handler,
    std::vector<InteractionEventKind> eventKinds)
{
    if (!GetIsOwnerThread() || !handler) return false;
    m_inputHandler = std::move(handler);
    m_inputEventKinds = std::move(eventKinds);
    return BuildInteractionRouter();
}

bool StdViewContext::ClearInputHandler()
{
    if (!GetIsOwnerThread()) return false;
    m_inputHandler = nullptr;
    m_inputEventKinds.clear();
    return BuildInteractionRouter();
}

bool StdViewContext::SetTimerHandler(std::function<void()> handler)
{
    if (!GetIsOwnerThread() || !handler) return false;
    m_timerHandler = std::move(handler);
    return true;
}

bool StdViewContext::ClearTimerHandler()
{
    if (!GetIsOwnerThread()) return false;
    m_timerHandler = nullptr;
    return true;
}

InteractionEventKind StdViewContext::GetEventKind(
    const unsigned long eventId) const
{
    switch (eventId) {
    case vtkCommand::TimerEvent:
        return InteractionEventKind::Timer;
    case vtkCommand::MouseWheelForwardEvent:
        return InteractionEventKind::WheelForward;
    case vtkCommand::MouseWheelBackwardEvent:
        return InteractionEventKind::WheelBackward;
    case vtkCommand::LeftButtonPressEvent:
        return InteractionEventKind::PrimaryPress;
    case vtkCommand::LeftButtonReleaseEvent:
        return InteractionEventKind::PrimaryRelease;
    case vtkCommand::RightButtonPressEvent:
        return InteractionEventKind::SecondaryPress;
    case vtkCommand::RightButtonReleaseEvent:
        return InteractionEventKind::SecondaryRelease;
    case vtkCommand::MouseMoveEvent:
        return InteractionEventKind::PointerMove;
    case vtkCommand::KeyPressEvent:
        return InteractionEventKind::KeyPress;
    case vtkCommand::KeyReleaseEvent:
        return InteractionEventKind::KeyRelease;
    case vtkCommand::CharEvent:
        return InteractionEventKind::TextInput;
    case vtkCommand::InteractionEvent:
        return InteractionEventKind::ViewInteraction;
    case vtkCommand::ExitEvent:
        return InteractionEventKind::Exit;
    default:
        return InteractionEventKind::None;
    }
}

void StdViewContext::BuildInteractionEvent(
    InteractionEvent& event,
    vtkRenderWindowInteractor* interactor,
    const InteractionEventKind eventKind) const
{
    event.eventKind = eventKind;
    const int* position = interactor->GetEventPosition();
    if (position) {
        event.x = position[0];
        event.y = position[1];
    }

    event.isShiftDown = interactor->GetShiftKey() != 0;
    event.isCtrlDown = interactor->GetControlKey() != 0;
    event.isAltDown = interactor->GetAltKey() != 0;
    event.keyCode = interactor->GetKeyCode();
    event.keySym = interactor->GetKeySym()
        ? interactor->GetKeySym()
        : "";
    event.vizMode = m_currentMode;
    event.toolMode = m_toolMode;
}

void StdViewContext::OnVTKEvent(
    vtkObject* caller,
    const unsigned long eventId,
    void* callData)
{
    if (!GetIsOwnerThread()) return;
    if (m_eventCallback) m_eventCallback->AbortFlagOff();

    if (eventId == vtkCommand::TimerEvent) {
        if (!callData || m_timerId == 0
            || *static_cast<const int*>(callData) != m_timerId) {
            return;
        }
    }

    auto* style = vtkInteractorStyle::SafeDownCast(caller);
    const bool isStyleBoundary = style
        && (eventId == vtkCommand::StartInteractionEvent
            || eventId == vtkCommand::EndInteractionEvent);
    if (isStyleBoundary) {
        if (!m_ports.state || !m_ports.update || !m_interactor
            || style != m_interactor->GetInteractorStyle()) {
            return;
        }

        const InteractionSource source{
            "ViewContext", "CameraStyle"
        };
        if (eventId == vtkCommand::StartInteractionEvent) {
            // VTK style 自己负责每次相机变换后的即时 Render。这里只发布
            // 交互边界并置一次 dirty，避免把整段拖动改造成 Timer 补帧。
            if (m_isStyleInteracting) return;
            if (!m_ports.state->SetInteracting(source, true)) return;
            m_isStyleInteracting = true;
            (void)m_ports.update->SetInteractionPhase();
            if (m_renderWindow) {
                m_renderWindow->SetDesiredUpdateRate(
                    GetRenderRate(true));
            }
            (void)m_ports.update->SetRenderNeeded();
            return;
        }
        if (eventId == vtkCommand::EndInteractionEvent) {
            if (!m_isStyleInteracting) return;
            if (m_ports.state->SetInteracting(source, false)) {
                m_isStyleInteracting = false;
                if (m_renderWindow) {
                    m_renderWindow->SetDesiredUpdateRate(
                        GetRenderRate(false));
                }
                // 最终静止质量仍由一次 heartbeat 收口；拖动过程不再按
                // MouseMove 重复置脏或等待 Timer 才显示。
                (void)m_ports.update->SetRenderNeeded();
            }
            return;
        }
    }

    const auto eventKind = GetEventKind(eventId);
    if (eventKind == InteractionEventKind::Exit) {
        (void)StopInput();
        return;
    }
    if (eventKind == InteractionEventKind::None) return;

    auto* interactor =
        vtkRenderWindowInteractor::SafeDownCast(caller);
    if (!interactor) return;

    InteractionEvent event;
    BuildInteractionEvent(event, interactor, eventKind);
    const auto dispatchMode =
        eventKind == InteractionEventKind::Timer
        ? RouterDispatchMode::Broadcast
        : RouterDispatchMode::FirstMatch;
    const auto result =
        m_interactionRouter.Dispatch(event, dispatchMode);

    if (eventKind == InteractionEventKind::Timer
        && m_timerHandler) {
        m_timerHandler();
    }
    if (result.isPropagationStopped && m_eventCallback) {
        m_eventCallback->SetAbortFlag(1);
    }
}

double StdViewContext::GetRenderRate(
    const bool isInteracting) const noexcept
{
    constexpr double staticRate = 0.001;
    constexpr double fastRate = 15.0;
    return isInteracting ? fastRate : staticRate;
}
