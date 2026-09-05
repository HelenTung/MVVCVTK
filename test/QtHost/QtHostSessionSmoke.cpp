#include "Host/VtkAppHostSession.h"
#include "Host/HostCoreServices.h"
#include "Host/HostCommandRouter.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/Types/HostRequestTypes.h"
#include "App/AppState.h"
#include "App/AppStateEvents.h"
#include "Data/DataManager.h"
#include "Interaction/TimeUpdateHandler.h"
#include "VolumeStrategy.h"

#include <QApplication>
#include <QCoreApplication>
#include <QOpenGLWidget>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkCallbackCommand.h>
#include <vtkAutoInit.h>
#include <vtkAlgorithm.h>
#include <vtkAlgorithmOutput.h>
#include <vtkCamera.h>
#include <vtkColorTransferFunction.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkImageAnisotropicDiffusion3D.h>
#include <vtkImageSlice.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkInteractorStyle.h>
#include <vtkSmartPointer.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkWindowToImageFilter.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);

namespace {

double GetRenderRate(const bool isInteracting) noexcept
{
    return isInteracting ? 15.0 : 0.001;
}

// 默认 true 路径只验证 Host 是否调用 Render；probe 隔离真实 OpenGL context 和平台 Timer。
class RenderProbeWindow final : public vtkGenericOpenGLRenderWindow {
public:
    static RenderProbeWindow* New();
    vtkTypeMacro(RenderProbeWindow, vtkGenericOpenGLRenderWindow);

    void Render() override
    {
        ++m_renderCount;
    }

    std::size_t GetRenderCount() const
    {
        return m_renderCount;
    }

    void* GetGenericWindowId() override
    {
        return this;
    }

protected:
    RenderProbeWindow()
    {
        this->Mapped = 1;
    }
    ~RenderProbeWindow() override = default;

private:
    std::size_t m_renderCount{0};
};

vtkStandardNewMacro(RenderProbeWindow);

class RenderProbeInteractor final : public vtkRenderWindowInteractor {
public:
    static RenderProbeInteractor* New();
    vtkTypeMacro(RenderProbeInteractor, vtkRenderWindowInteractor);

    void Initialize() override
    {
        this->Initialized = 1;
        this->Enabled = 1;
    }

    void Start() override
    {
        ++m_startCount;
    }

    std::size_t GetStartCount() const
    {
        return m_startCount;
    }

    int GetCreatedTimerId() const
    {
        return m_createdTimerId;
    }

protected:
    RenderProbeInteractor() = default;
    ~RenderProbeInteractor() override = default;

    int InternalCreateTimer(
        int timerId, int, unsigned long) override
    {
        m_createdTimerId = timerId;
        return timerId;
    }

    int InternalDestroyTimer(int) override
    {
        return 1;
    }

private:
    std::size_t m_startCount{0};
    int m_createdTimerId{0};
};

vtkStandardNewMacro(RenderProbeInteractor);

bool SendTimer(
    vtkRenderWindowInteractor* interactor,
    const int idOffset = 0)
{
    if (!interactor) return false;
    int timerId = interactor->GetTimerEventId();
    if (auto* probe = RenderProbeInteractor::SafeDownCast(interactor)) {
        timerId = probe->GetCreatedTimerId();
    }
    if (timerId == 0) {
        for (int candidate = 1; candidate <= 64; ++candidate) {
            if (interactor->GetTimerDuration(candidate) != 0) {
                timerId = candidate;
                break;
            }
        }
    }
    if (timerId == 0) return false;
    timerId += idOffset;
    interactor->InvokeEvent(vtkCommand::TimerEvent, &timerId);
    return true;
}

class TimerProbePort final : public RenderUpdatePort {
public:
    bool SendUpdates() override
    {
        ++m_updateCount;
        return true;
    }

    bool SetRenderNeeded() override
    {
        m_isDirty = true;
        return true;
    }

    bool ResetRenderNeeded() override
    {
        const bool wasDirty = m_isDirty;
        m_isDirty = false;
        return wasDirty;
    }

    std::size_t GetUpdateCount() const
    {
        return m_updateCount;
    }

    void ResetCount()
    {
        m_updateCount = 0;
        m_isDirty = false;
    }

private:
    std::size_t m_updateCount{0};
    bool m_isDirty{false};
};

struct PhaseThreadProbe final {
    std::thread::id styleThread;
    std::thread::id timerThread;
    std::size_t startCount{0};
    std::size_t moveCount{0};
    std::size_t endCount{0};
    std::size_t timerCount{0};

    static void OnEvent(
        vtkObject*, unsigned long eventId,
        void* clientData, void*)
    {
        auto* probe =
            static_cast<PhaseThreadProbe*>(clientData);
        if (!probe) {
            return;
        }
        if (eventId == vtkCommand::TimerEvent) {
            probe->timerThread = std::this_thread::get_id();
            ++probe->timerCount;
        }
        else if (eventId
            == vtkCommand::StartInteractionEvent) {
            probe->styleThread = std::this_thread::get_id();
            ++probe->startCount;
        }
        else if (eventId
            == vtkCommand::InteractionEvent) {
            ++probe->moveCount;
        }
        else if (eventId
            == vtkCommand::EndInteractionEvent) {
            ++probe->endCount;
        }
    }
};

bool BuildTransferReturnTest()
{
    auto dataManager =
        std::make_shared<RawVolumeDataManager>();
    auto broadcaster =
        std::make_shared<SharedStateBroadcaster>();
    auto state =
        std::make_shared<SharedInteractionState>(
            broadcaster);
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 2, 2);
    image->AllocateScalars(VTK_FLOAT, 1);
    for (vtkIdType index = 0;
        index < image->GetNumberOfPoints();
        ++index) {
        static_cast<float*>(
            image->GetScalarPointer())[index] =
                static_cast<float>(index);
    }
    image->Modified();
    VtkImageGridSnapshot published;
    if (!dataManager->SetImageSnapshot(image)
        || !dataManager->GetLoadStage()
        || !dataManager->SetLoadCommit(
            dataManager->GetLoadStage(), published)) {
        return false;
    }

    HostCoreServices core;
    core.sharedDataMgr = dataManager;
    core.sharedStateBroadcaster = broadcaster;
    core.sharedState = state;
    auto renderWindow =
        vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);
    HostRenderViewConfig view;
    view.id = "preset-router";
    view.role = HostRenderViewRole::Primary3D;
    view.window.viewInit.viewMode =
        HostRenderMode::IsoSurface;
    view.renderWindow = renderWindow;
    HostViewRuntimeRegistry views;
    const std::vector<HostRenderViewConfig> configs{
        view
    };
    if (!views.Build(core, configs)) {
        return false;
    }
    HostCommandRouter router(views.GetViewDirectory());
    HostViewSetRequest request;
    request.targetView.viewId = "preset-router";
    HostVolumeTransferFunction function;
    function.colorNodes = {
        { 0.0, 0.0, 0.0, 0.0 },
        { 7.0, 1.0, 1.0, 1.0 }
    };
    function.opacityNodes = {
        { 0.0, 0.0 },
        { 7.0, 1.0 }
    };
    request.volumeTransferFunction = function;
    const bool isTransferAccepted =
        router.Dispatch(std::move(request));
    HostViewTarget target;
    target.viewId = "preset-router";
    const auto finalState = views.GetViewState(target);
    std::cout
        << "DIAG_TRANSFER: accepted=" << isTransferAccepted
        << " color_nodes="
        << (finalState
            ? finalState->volumeTransferFunction.colorNodes.size() : 0)
        << '\n';
    return isTransferAccepted && finalState
        && finalState->volumeTransferFunction.colorNodes.size() == 2
        && finalState->volumeTransferFunction.opacityNodes.size() == 2;
}

bool BuildRenderSourceTest()
{
    HostCoreServices core;
    core.sharedDataMgr =
        std::make_shared<RawVolumeDataManager>();
    core.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    core.sharedState =
        std::make_shared<SharedInteractionState>(
            core.sharedStateBroadcaster);

    auto setWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto setInteractor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    setWindow->SetInteractor(setInteractor);
    setInteractor->SetRenderWindow(setWindow);
    HostRenderViewConfig setView;
    setView.id = "view-set-source";
    setView.role = HostRenderViewRole::Primary3D;
    setView.renderWindow = setWindow;
    HostViewRuntimeRegistry viewSet;
    const std::vector<HostRenderViewConfig> configs{
        setView
    };
    if (!viewSet.Build(core, configs)
        || setWindow->GetRenderCount() != 0) {
        return false;
    }
    viewSet.SendRenderAll();
    const bool hasViewSetRender =
        setWindow->GetRenderCount() == 1;

    auto startWindow =
        vtkSmartPointer<RenderProbeWindow>::New();
    auto startInteractor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    startWindow->SetInteractor(startInteractor);
    startInteractor->SetRenderWindow(startWindow);
    HostRenderViewConfig startView;
    startView.id = "context-start-source";
    startView.role = HostRenderViewRole::Primary3D;
    startView.renderWindow = startWindow;
    HostViewRuntimeRegistry startViews;
    const bool isStartBuilt = startViews.Build(
        core, std::vector<HostRenderViewConfig>{ startView });
    const bool hasStartRender = isStartBuilt
        && startViews.StartStandaloneView()
        && startWindow->GetRenderCount() == 1
        && startInteractor->GetStartCount() == 1;

    auto sessionWindow =
        vtkSmartPointer<RenderProbeWindow>::New();
    auto sessionInteractor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    sessionWindow->SetInteractor(sessionInteractor);
    sessionInteractor->SetRenderWindow(sessionWindow);
    HostRenderViewConfig sessionView;
    sessionView.id = "session-start-source";
    sessionView.role = HostRenderViewRole::Primary3D;
    sessionView.renderWindow = sessionWindow;
    HostSessionConfig sessionConfig;
    sessionConfig.renderViews.push_back(
        std::move(sessionView));
    VtkAppHostSession session(
        std::move(sessionConfig));
    const bool hasSessionStart =
        session.Start()
        && sessionWindow->GetRenderCount() == 2
        && sessionInteractor->GetStartCount() == 1;

    std::cout
        << "DIAG_RENDER_SOURCE: send_all="
        << setWindow->GetRenderCount()
        << " std_start=" << startWindow->GetRenderCount()
        << " session_start="
        << sessionWindow->GetRenderCount()
        << " interactor_start="
        << sessionInteractor->GetStartCount()
        << '\n';
    return hasViewSetRender
        && hasStartRender
        && hasSessionStart;
}

bool BuildStyleQualityTest()
{
    constexpr double volumeTargetFps = 20.0;
    HostCoreServices core;
    auto dataManager =
        std::make_shared<RawVolumeDataManager>();
    core.sharedDataMgr = dataManager;
    core.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    core.sharedState =
        std::make_shared<SharedInteractionState>(
            core.sharedStateBroadcaster);
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(image->GetScalarPointer()),
        image->GetNumberOfPoints(),
        static_cast<unsigned char>(128));
    image->Modified();
    VtkImageGridSnapshot published;
    if (!dataManager->SetImageSnapshot(image)
        || !dataManager->GetLoadStage()
        || !dataManager->SetLoadCommit(
            dataManager->GetLoadStage(), published)) {
        return false;
    }

    auto renderWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor = vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);

    HostRenderViewConfig view;
    view.id = "style-quality-probe";
    view.role = HostRenderViewRole::Primary3D;
    view.window.viewInit.viewMode = HostRenderMode::Volume;
    view.renderWindow = renderWindow;
    HostViewRuntimeRegistry views;
    if (!views.Build(core, { view })) {
        return false;
    }
    const HostViewTarget target{
        "style-quality-probe", false,
        HostRenderViewRole::Primary3D
    };
    const auto endpoints = views.BuildEndpoints();
    if (endpoints.size() != 1
        || !endpoints.front().renderer
        || !endpoints.front().interactor) {
        return false;
    }
    HostCommandRouter router(views.GetViewDirectory());
    const auto setMode = [&router, &target](
        const HostRenderMode mode) {
        HostViewSetRequest request;
        request.targetView = target;
        request.mode = mode;
        return router.Dispatch(std::move(request));
    };
    const auto setTool = [&router, &target](
        const HostToolMode mode) {
        HostToolSetRequest request;
        request.targetView = target;
        request.toolMode = mode;
        return router.Dispatch(std::move(request));
    };
    // 数据在 Host 组合根创建前已提交；用共享事件入口发布一次完整快照，
    // 再由窄 update port 构建策略，测试不注入具体 App runtime。
    core.sharedStateBroadcaster->SendFlags(UpdateFlags::All);
    if (!views.SendViewUpdates(target)) {
        return false;
    }
    // 交互首帧 rate 镜像只允许在已绑定 owner Timer 的 context 中启用；
    // 这里用同线程 no-op handler 建立与真实 HostTimer 相同的线程门。
    if (!views.SetTimerHandler(target, [] {})
        || !views.SetInteractorsReady()
        || !setMode(HostRenderMode::Volume)) {
        return false;
    }
    // 首次 Timer 清理初始化阶段已积累的状态，后续只观察 style source
    // 产生的 RenderRate 边界。
    (void)SendTimer(interactor);
    auto* oldStyle = vtkInteractorStyle::SafeDownCast(
        interactor->GetInteractorStyle());
    if (!oldStyle) {
        return false;
    }

    renderWindow->SetDesiredUpdateRate(GetRenderRate(false));
    const std::size_t renderCountBeforeStyle =
        renderWindow->GetRenderCount();
    oldStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    const bool isStyleRenderEnabled =
        interactor->GetEnableRender();
    interactor->Render();
    oldStyle->InvokeEvent(vtkCommand::InteractionEvent);
    const bool isDirectRenderEnabled =
        renderWindow->GetRenderCount()
            == renderCountBeforeStyle + 1;
    const bool isStartFast =
        renderWindow->GetDesiredUpdateRate() == GetRenderRate(true);
    const bool isStartActive =
        core.sharedState->GetIsInteracting();
    (void)SendTimer(interactor);
    const bool isTimerFast =
        renderWindow->GetDesiredUpdateRate() == volumeTargetFps
        && renderWindow->GetRenderCount()
            == renderCountBeforeStyle + 2;
    oldStyle->InvokeEvent(vtkCommand::EndInteractionEvent);
    const bool isStyleRenderKept =
        interactor->GetEnableRender();
    const bool isEndInactive =
        !core.sharedState->GetIsInteracting();
    (void)SendTimer(interactor);
    const bool isStyleRenderRestored =
        interactor->GetEnableRender();
    const bool isEndStill = isStyleRenderRestored
        && renderWindow->GetDesiredUpdateRate() == GetRenderRate(false);

    oldStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    oldStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    const bool isRepeatedStartStable =
        core.sharedState->GetIsInteracting()
        && interactor->GetEnableRender();
    oldStyle->InvokeEvent(vtkCommand::EndInteractionEvent);
    oldStyle->InvokeEvent(vtkCommand::EndInteractionEvent);
    const bool isRepeatedEndStable =
        isRepeatedStartStable
        && !core.sharedState->GetIsInteracting()
        && interactor->GetEnableRender();
    (void)SendTimer(interactor);
    const bool isRepeatedBoundaryStable =
        isRepeatedEndStable && interactor->GetEnableRender();

    oldStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    oldStyle->InvokeEvent(vtkCommand::EndInteractionEvent);
    oldStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    const bool isRestartPendingActive =
        core.sharedState->GetIsInteracting()
        && interactor->GetEnableRender();
    (void)SendTimer(interactor);
    const bool isRestartTimerStable =
        core.sharedState->GetIsInteracting()
        && interactor->GetEnableRender();
    oldStyle->InvokeEvent(vtkCommand::EndInteractionEvent);
    (void)SendTimer(interactor);
    const bool isRestartBoundaryStable =
        isRestartPendingActive
        && isRestartTimerStable
        && !core.sharedState->GetIsInteracting()
        && interactor->GetEnableRender();

    auto* camera = endpoints.front().renderer->GetActiveCamera();
    if (!camera) {
        return false;
    }
    camera->SetPosition(0.0, 0.0, 10.0);
    camera->SetFocalPoint(0.0, 0.0, 0.0);
    camera->SetViewUp(0.0, 1.0, 0.0);
    double cameraBefore[3] = { 0.0, 0.0, 0.0 };
    camera->GetPosition(cameraBefore);
    renderWindow->SetSize(640, 480);
    const std::size_t cameraRenderCount =
        renderWindow->GetRenderCount();
    interactor->SetEventPosition(300, 220);
    interactor->InvokeEvent(vtkCommand::LeftButtonPressEvent);
    interactor->SetEventPosition(360, 250);
    interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
    interactor->InvokeEvent(vtkCommand::LeftButtonReleaseEvent);
    double cameraAfter[3] = { 0.0, 0.0, 0.0 };
    camera->GetPosition(cameraAfter);
    const bool isDefaultCameraActive =
        std::abs(cameraAfter[0] - cameraBefore[0]) > 1e-9
        || std::abs(cameraAfter[1] - cameraBefore[1]) > 1e-9
        || std::abs(cameraAfter[2] - cameraBefore[2]) > 1e-9;
    const std::size_t cameraDirectRenderCount =
        renderWindow->GetRenderCount();
    const bool hasCameraDirectRender =
        cameraDirectRenderCount > cameraRenderCount;
    (void)SendTimer(interactor);
    const bool hasCameraFinalRender =
        renderWindow->GetRenderCount()
            == cameraDirectRenderCount + 1;

    vtkSmartPointer<vtkInteractorStyle> replacedStyle = oldStyle;
    oldStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    (void)SendTimer(interactor);
    if (!setMode(HostRenderMode::Volume)) {
        return false;
    }
    (void)SendTimer(interactor);
    const bool isReplaceStill =
        renderWindow->GetDesiredUpdateRate() == GetRenderRate(false);
    auto* newStyle = vtkInteractorStyle::SafeDownCast(
        interactor->GetInteractorStyle());
    if (!newStyle || newStyle == replacedStyle.GetPointer()) {
        return false;
    }

    replacedStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    (void)SendTimer(interactor);
    const bool isOldDetached =
        renderWindow->GetDesiredUpdateRate() == GetRenderRate(false);
    newStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    (void)SendTimer(interactor);
    const bool isNewAttached =
        renderWindow->GetDesiredUpdateRate() == volumeTargetFps;
    newStyle->InvokeEvent(vtkCommand::EndInteractionEvent);
    (void)SendTimer(interactor);
    const bool isNewEndStill =
        renderWindow->GetDesiredUpdateRate() == GetRenderRate(false);

    vtkSmartPointer<vtkInteractorStyle> cameraBeforeTool = newStyle;
    cameraBeforeTool->InvokeEvent(vtkCommand::StartInteractionEvent);
    if (!setTool(HostToolMode::ModelTransform)) {
        return false;
    }
    (void)SendTimer(interactor);
    const bool isToolReplaceStill =
        !core.sharedState->GetIsInteracting()
        && renderWindow->GetDesiredUpdateRate()
            == GetRenderRate(false);
    auto* actorStyle = vtkInteractorStyle::SafeDownCast(
        interactor->GetInteractorStyle());
    cameraBeforeTool->InvokeEvent(vtkCommand::StartInteractionEvent);
    const bool isToolOldDetached =
        !core.sharedState->GetIsInteracting();
    if (!actorStyle
        || actorStyle == cameraBeforeTool.GetPointer()) {
        return false;
    }
    actorStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    const bool isToolNewAttached =
        core.sharedState->GetIsInteracting()
        && renderWindow->GetDesiredUpdateRate()
            == GetRenderRate(true);
    actorStyle->InvokeEvent(vtkCommand::EndInteractionEvent);
    (void)SendTimer(interactor);
    const bool isToolEndStill =
        !core.sharedState->GetIsInteracting()
        && renderWindow->GetDesiredUpdateRate()
            == GetRenderRate(false);
    vtkSmartPointer<vtkInteractorStyle> replacedActor = actorStyle;
    if (!setTool(HostToolMode::Navigation)) {
        return false;
    }
    replacedActor->InvokeEvent(vtkCommand::StartInteractionEvent);
    const bool isActorDetached =
        !core.sharedState->GetIsInteracting();

    auto replacementWindow =
        vtkSmartPointer<RenderProbeWindow>::New();
    auto replacementInteractor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    replacementWindow->SetInteractor(replacementInteractor);
    replacementInteractor->SetRenderWindow(replacementWindow);
    vtkSmartPointer<vtkInteractorStyle> styleBeforeInteractor =
        vtkInteractorStyle::SafeDownCast(
            interactor->GetInteractorStyle());
    styleBeforeInteractor->InvokeEvent(
        vtkCommand::StartInteractionEvent);
    const bool isWindowRebound = views.SetViewWindow(
        "style-quality-probe", replacementWindow);
    if (!setMode(HostRenderMode::Volume)) {
        return false;
    }
    if (!views.SetInteractorsReady()) {
        return false;
    }
    const bool isInteractorReplaceClear =
        isWindowRebound
        && !core.sharedState->GetIsInteracting();
    styleBeforeInteractor->InvokeEvent(
        vtkCommand::StartInteractionEvent);
    const bool isInteractorOldDetached =
        !core.sharedState->GetIsInteracting();
    auto* replacementStyle = vtkInteractorStyle::SafeDownCast(
        replacementInteractor->GetInteractorStyle());
    if (!replacementStyle) {
        return false;
    }
    replacementStyle->InvokeEvent(
        vtkCommand::StartInteractionEvent);
    const bool isInteractorNewAttached =
        core.sharedState->GetIsInteracting()
        && replacementWindow->GetDesiredUpdateRate()
            == GetRenderRate(true);
    replacementStyle->InvokeEvent(
        vtkCommand::EndInteractionEvent);
    (void)SendTimer(replacementInteractor);
    const bool isInteractorEndStill =
        !core.sharedState->GetIsInteracting()
        && replacementWindow->GetDesiredUpdateRate()
            == GetRenderRate(false);

    // CameraStyle 描述的是相机交互事实，不属于 Volume 策略。所有使用
    // vtkInteractorStyle 的视图都必须发布同一个 source；具体是否降低
    // mapper 质量由对应 Strategy 自己决定。
    const std::array<HostRenderMode, 7> cameraModes = {
        HostRenderMode::Volume,
        HostRenderMode::IsoSurface,
        HostRenderMode::SliceTopDown,
        HostRenderMode::SliceFrontBack,
        HostRenderMode::SliceLeftRight,
        HostRenderMode::CompositeVolume,
        HostRenderMode::CompositeIsoSurface
    };
    bool isCameraSourceUnified = true;
    for (const auto mode : cameraModes) {
        if (!setMode(mode)) {
            isCameraSourceUnified = false;
            break;
        }
        auto* modeStyle = vtkInteractorStyle::SafeDownCast(
            replacementInteractor->GetInteractorStyle());
        if (!modeStyle) {
            isCameraSourceUnified = false;
            break;
        }
        replacementWindow->SetDesiredUpdateRate(
            GetRenderRate(false));
        modeStyle->InvokeEvent(
            vtkCommand::StartInteractionEvent);
        const bool isModeStarted =
            core.sharedState->GetIsInteracting()
            && replacementWindow->GetDesiredUpdateRate()
                == GetRenderRate(true);
        modeStyle->InvokeEvent(
            vtkCommand::EndInteractionEvent);
    (void)SendTimer(replacementInteractor);
        const bool isModeStopped =
            !core.sharedState->GetIsInteracting()
            && replacementWindow->GetDesiredUpdateRate()
                == GetRenderRate(false);
        if (!isModeStarted || !isModeStopped) {
            isCameraSourceUnified = false;
            break;
        }
    }

    auto* exitStyle = vtkInteractorStyle::SafeDownCast(
        replacementInteractor->GetInteractorStyle());
    if (!exitStyle) return false;
    exitStyle->InvokeEvent(vtkCommand::StartInteractionEvent);
    const bool isExitStartEnabled =
        core.sharedState->GetIsInteracting()
        && replacementInteractor->GetEnableRender();
    replacementInteractor->InvokeEvent(vtkCommand::ExitEvent);
    const bool isExitRestored = isExitStartEnabled
        && !core.sharedState->GetIsInteracting()
        && replacementInteractor->GetEnableRender();

    const bool isValid = isStartFast
        && isStartActive
        && isStyleRenderEnabled
        && isDirectRenderEnabled
        && isTimerFast
        && isEndInactive
        && isStyleRenderKept
        && isStyleRenderRestored
        && isEndStill
        && isRepeatedBoundaryStable
        && isRestartBoundaryStable
        && isDefaultCameraActive
        && hasCameraDirectRender
        && hasCameraFinalRender
        && isReplaceStill
        && isOldDetached
        && isNewAttached
        && isNewEndStill
        && isToolReplaceStill
        && isToolOldDetached
        && isToolNewAttached
        && isToolEndStill
        && isActorDetached
        && isInteractorReplaceClear
        && isInteractorOldDetached
        && isInteractorNewAttached
        && isInteractorEndStill
        && isCameraSourceUnified
        && isExitRestored;
    if (!isValid) {
        std::cerr
            << "DIAG_STYLE:"
            << " start_fast=" << isStartFast
            << " start_active=" << isStartActive
            << " style_render_enabled="
            << isStyleRenderEnabled
            << " direct_render_enabled="
            << isDirectRenderEnabled
            << " timer_fast=" << isTimerFast
            << " end_inactive=" << isEndInactive
            << " style_render_kept="
            << isStyleRenderKept
            << " style_render_restored="
            << isStyleRenderRestored
            << " end_still=" << isEndStill
            << " repeated_boundary="
            << isRepeatedBoundaryStable
            << " restart_boundary="
            << isRestartBoundaryStable
            << " default_camera_active="
            << isDefaultCameraActive
            << " camera_direct_render="
            << hasCameraDirectRender
            << " camera_final_render="
            << hasCameraFinalRender
            << " replace_still=" << isReplaceStill
            << " old_detached=" << isOldDetached
            << " new_attached=" << isNewAttached
            << " new_end_still=" << isNewEndStill
            << " tool_replace_still=" << isToolReplaceStill
            << " tool_old_detached=" << isToolOldDetached
            << " tool_new_attached=" << isToolNewAttached
            << " tool_end_still=" << isToolEndStill
            << " actor_detached=" << isActorDetached
            << " interactor_replace_clear="
            << isInteractorReplaceClear
            << " interactor_old_detached="
            << isInteractorOldDetached
            << " interactor_new_attached="
            << isInteractorNewAttached
            << " interactor_end_still="
            << isInteractorEndStill
            << " camera_source_unified="
            << isCameraSourceUnified
            << " exit_restored=" << isExitRestored
            << '\n';
    }
    return isValid;
}

bool BuildSceneCameraTest()
{
    std::size_t vtkErrorCount = 0;
    auto errorCallback = vtkSmartPointer<vtkCallbackCommand>::New();
    errorCallback->SetClientData(&vtkErrorCount);
    errorCallback->SetCallback([](
        vtkObject*, unsigned long, void* clientData, void*) {
        auto* count = static_cast<std::size_t*>(clientData);
        if (count) ++*count;
    });

    HostCoreServices core;
    auto dataManager = std::make_shared<RawVolumeDataManager>();
    core.sharedDataMgr = dataManager;
    core.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    core.sharedState = std::make_shared<SharedInteractionState>(
        core.sharedStateBroadcaster);

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(image->GetScalarPointer()),
        image->GetNumberOfPoints(),
        static_cast<unsigned char>(128));
    image->Modified();
    VtkImageGridSnapshot published;
    if (!dataManager->SetImageSnapshot(image)
        || !dataManager->GetLoadStage()
        || !dataManager->SetLoadCommit(
            dataManager->GetLoadStage(), published)) {
        return false;
    }

    auto renderWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor = vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);
    const unsigned long sourceErrorTag = renderWindow->AddObserver(
        vtkCommand::ErrorEvent, errorCallback);

    auto peerWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto peerInteractor = vtkSmartPointer<RenderProbeInteractor>::New();
    peerWindow->SetInteractor(peerInteractor);
    peerInteractor->SetRenderWindow(peerWindow);

    HostRenderViewConfig view;
    view.id = "scene-camera";
    view.role = HostRenderViewRole::Primary3D;
    view.window.viewInit.viewMode = HostRenderMode::Volume;
    view.renderWindow = renderWindow;
    HostRenderViewConfig peerView = view;
    peerView.id = "scene-camera-peer";
    peerView.role = HostRenderViewRole::Composite3D;
    peerView.renderWindow = peerWindow;

    HostViewRuntimeRegistry views;
    if (sourceErrorTag == 0 || !views.Build(core, { view, peerView })) {
        return false;
    }
    const HostViewTarget target{
        "scene-camera", false, HostRenderViewRole::Primary3D };
    const HostViewTarget peerTarget{
        "scene-camera-peer", false, HostRenderViewRole::Composite3D };
    core.sharedStateBroadcaster->SendFlags(UpdateFlags::All);
    if (!views.SendViewUpdates(target)
        || !views.SendViewUpdates(peerTarget)
        || !views.SetFeatureViews(
            "scene-camera-feature", { "scene-camera" })) {
        return false;
    }

    const auto endpoints = views.BuildEndpoints();
    const auto endpoint = std::find_if(
        endpoints.begin(), endpoints.end(),
        [](const HostRenderViewEndpoint& current) {
            return current.id == "scene-camera";
        });
    if (endpoint == endpoints.end() || !endpoint->renderer) {
        return false;
    }

    const auto getArrayNear = [](const auto& left, const auto& right) {
        return std::equal(
            left.begin(), left.end(), right.begin(),
            [](const double first, const double second) {
                return std::abs(first - second) < 1e-12;
            });
    };
    const auto getCameraFrameEqual = [&getArrayNear](
        const HostCameraState& left,
        const HostCameraState& right) {
        return getArrayNear(left.position, right.position)
            && getArrayNear(left.focalPoint, right.focalPoint)
            && getArrayNear(left.viewUp, right.viewUp)
            && std::abs(left.parallelScale - right.parallelScale) < 1e-12
            && std::abs(left.viewAngle - right.viewAngle) < 1e-12
            && left.isParallel == right.isParallel;
    };
    const auto getCameraEqual = [
        &getArrayNear, &getCameraFrameEqual](
        const HostCameraState& left,
        const HostCameraState& right) {
        return getCameraFrameEqual(left, right)
            && getArrayNear(left.clippingRange, right.clippingRange);
    };

    const auto peerBefore = views.GetSceneViewState(peerTarget);
    auto* camera = endpoint->renderer->GetActiveCamera();
    if (!peerBefore || !peerBefore->camera || !camera) return false;

    HostCameraState expectedCamera;
    expectedCamera.position = { 3.0, 4.0, 5.0 };
    expectedCamera.focalPoint = { 0.5, 1.0, 1.5 };
    expectedCamera.viewUp = { 0.0, 1.0, 0.0 };
    expectedCamera.clippingRange = { 0.25, 500.0 };
    expectedCamera.parallelScale = 2.5;
    expectedCamera.viewAngle = 35.0;
    expectedCamera.isParallel = false;
    camera->SetPosition(expectedCamera.position.data());
    camera->SetFocalPoint(expectedCamera.focalPoint.data());
    camera->SetViewUp(expectedCamera.viewUp.data());
    camera->SetClippingRange(expectedCamera.clippingRange.data());
    camera->SetParallelScale(expectedCamera.parallelScale);
    camera->SetViewAngle(expectedCamera.viewAngle);
    camera->SetParallelProjection(expectedCamera.isParallel ? 1 : 0);

    const auto sceneBeforeRebind = views.GetSceneViewState(target);
    const auto peerAfterCamera = views.GetSceneViewState(peerTarget);
    const bool isCameraProjectionValid = sceneBeforeRebind
        && sceneBeforeRebind->camera
        && getCameraEqual(*sceneBeforeRebind->camera, expectedCamera)
        && peerAfterCamera
        && peerAfterCamera->camera
        && !getCameraEqual(*peerAfterCamera->camera, expectedCamera)
        && getCameraEqual(*peerBefore->camera, *peerAfterCamera->camera);

    auto replacementWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto replacementInteractor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    replacementWindow->SetInteractor(replacementInteractor);
    replacementInteractor->SetRenderWindow(replacementWindow);
    const unsigned long replacementErrorTag = replacementWindow->AddObserver(
        vtkCommand::ErrorEvent, errorCallback);
    const std::size_t errorsBeforeRebind = vtkErrorCount;
    const bool isWindowRebound = replacementErrorTag != 0
        && views.SetViewWindow("scene-camera", replacementWindow);
    const auto sceneAfterRebind = views.GetSceneViewState(target);
    const auto peerAfterRebind = views.GetSceneViewState(peerTarget);
    const auto reboundEndpoints = views.BuildEndpoints();
    const auto reboundEndpoint = std::find_if(
        reboundEndpoints.begin(), reboundEndpoints.end(),
        [](const HostRenderViewEndpoint& current) {
            return current.id == "scene-camera";
        });
    auto* reboundCamera = reboundEndpoint != reboundEndpoints.end()
        && reboundEndpoint->renderer
        ? reboundEndpoint->renderer->GetActiveCamera()
        : nullptr;
    std::array<double, 2> reboundClipping{};
    if (reboundCamera && reboundCamera->GetClippingRange()) {
        std::copy_n(
            reboundCamera->GetClippingRange(),
            reboundClipping.size(),
            reboundClipping.begin());
    }
    const bool isSceneIdentityStable = isWindowRebound
        && sceneBeforeRebind && sceneAfterRebind
        && sceneBeforeRebind->id == sceneAfterRebind->id
        && sceneBeforeRebind->role == sceneAfterRebind->role
        && sceneBeforeRebind->isAvailable == sceneAfterRebind->isAvailable
        && sceneBeforeRebind->presentation.has_value()
            == sceneAfterRebind->presentation.has_value()
        && sceneBeforeRebind->presentationRevision
            == sceneAfterRebind->presentationRevision
        && sceneBeforeRebind->activeFeatureIds
            == std::vector<std::string>{ "scene-camera-feature" }
        && sceneBeforeRebind->activeFeatureIds
            == sceneAfterRebind->activeFeatureIds;
    const bool isCameraRebindStable = sceneBeforeRebind
        && sceneBeforeRebind->camera
        && sceneAfterRebind
        && sceneAfterRebind->camera
        && getCameraFrameEqual(
            *sceneBeforeRebind->camera, *sceneAfterRebind->camera)
        // Rebind 后 VTK 会按新 renderer bounds 重算 clipping；快照必须反映
        // 当前 active camera，而不是保留可能裁掉新场景的旧范围。
        && std::isfinite(sceneAfterRebind->camera->clippingRange[0])
        && std::isfinite(sceneAfterRebind->camera->clippingRange[1])
        && sceneAfterRebind->camera->clippingRange[0] > 0.0
        && sceneAfterRebind->camera->clippingRange[1]
            > sceneAfterRebind->camera->clippingRange[0]
        && reboundCamera
        && getArrayNear(
            sceneAfterRebind->camera->clippingRange,
            reboundClipping);
    const bool isPeerRebindStable = peerAfterRebind
        && peerAfterRebind->camera
        && getCameraEqual(*peerBefore->camera, *peerAfterRebind->camera)
        && peerBefore->presentationRevision
            == peerAfterRebind->presentationRevision;
    const bool isEndpointRebound = reboundEndpoint != reboundEndpoints.end()
        && reboundEndpoint->renderer
        && reboundEndpoint->renderWindow == replacementWindow.GetPointer()
        && reboundEndpoint->renderWindow != renderWindow.GetPointer()
        && reboundEndpoint->interactor == replacementInteractor.GetPointer();
    const bool hasNoRebindError = vtkErrorCount == errorsBeforeRebind;
    const bool isRebindStable = isSceneIdentityStable
        && isCameraRebindStable
        && isPeerRebindStable
        && isEndpointRebound
        && hasNoRebindError;

    const bool isFeatureCleared = views.SetFeatureViews(
        "scene-camera-feature", {});
    const auto directory = views.GetViewDirectory().lock();
    const bool isPeerStopped = directory
        && directory->StopView("scene-camera-peer");
    const auto stoppedPeer = views.GetSceneViewState(peerTarget);
    const auto stoppedPeerByRole = views.GetSceneViewState(
        HostViewTarget{
            "", true, HostRenderViewRole::Composite3D });
    const auto stoppedScenes = views.GetSceneViewStates();
    const auto stoppedPeerInList = std::find_if(
        stoppedScenes.begin(), stoppedScenes.end(),
        [](const HostSceneViewState& current) {
            return current.id == "scene-camera-peer";
        });
    const bool isUnavailableTopologyValid = isPeerStopped
        && stoppedPeer
        && !stoppedPeer->isAvailable
        && !stoppedPeerByRole
        && !stoppedPeer->presentation
        && !stoppedPeer->camera
        && stoppedPeer->activeFeatureIds.empty()
        && stoppedScenes.size() == 2
        && stoppedScenes[0].id == "scene-camera"
        && stoppedScenes[1].id == "scene-camera-peer"
        && stoppedPeerInList != stoppedScenes.end()
        && !stoppedPeerInList->isAvailable
        && !stoppedPeerInList->presentation
        && !stoppedPeerInList->camera;

    const bool isValid = isCameraProjectionValid
        && isRebindStable
        && isFeatureCleared
        && isUnavailableTopologyValid;
    if (!isValid) {
        std::cerr
            << "DIAG_SCENE_CAMERA: projection="
            << isCameraProjectionValid
            << " rebind=" << isRebindStable
            << " identity=" << isSceneIdentityStable
            << " camera=" << isCameraRebindStable
            << " peer=" << isPeerRebindStable
            << " endpoint=" << isEndpointRebound
            << " no_error=" << hasNoRebindError
            << " unavailable=" << isUnavailableTopologyValid
            << " errors=" << vtkErrorCount
            << " clip_before="
            << (sceneBeforeRebind && sceneBeforeRebind->camera
                ? sceneBeforeRebind->camera->clippingRange[0] : -1.0)
            << ','
            << (sceneBeforeRebind && sceneBeforeRebind->camera
                ? sceneBeforeRebind->camera->clippingRange[1] : -1.0)
            << " clip_after="
            << (sceneAfterRebind && sceneAfterRebind->camera
                ? sceneAfterRebind->camera->clippingRange[0] : -1.0)
            << ','
            << (sceneAfterRebind && sceneAfterRebind->camera
                ? sceneAfterRebind->camera->clippingRange[1] : -1.0)
            << '\n';
    }
    return isValid;
}

bool BuildStyleDestroyTest()
{
    HostCoreServices core;
    core.sharedDataMgr =
        std::make_shared<RawVolumeDataManager>();
    core.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    core.sharedState =
        std::make_shared<SharedInteractionState>(
            core.sharedStateBroadcaster);
    vtkSmartPointer<vtkInteractorStyle> destroyedStyle;
    {
        auto renderWindow =
            vtkSmartPointer<RenderProbeWindow>::New();
        auto interactor =
            vtkSmartPointer<RenderProbeInteractor>::New();
        renderWindow->SetInteractor(interactor);
        interactor->SetRenderWindow(renderWindow);
        HostRenderViewConfig view;
        view.id = "style-destroy-probe";
        view.role = HostRenderViewRole::Primary3D;
        view.window.viewInit.viewMode =
            HostRenderMode::Volume;
        view.renderWindow = renderWindow;
        HostViewRuntimeRegistry views;
        if (!views.Build(core, { view })
            || views.BuildEndpoints().empty()) {
            return false;
        }
        destroyedStyle = vtkInteractorStyle::SafeDownCast(
            interactor->GetInteractorStyle());
        if (!destroyedStyle) {
            return false;
        }
        destroyedStyle->InvokeEvent(
            vtkCommand::StartInteractionEvent);
        if (!core.sharedState->GetIsInteracting()) {
            return false;
        }
    }

    // context 析构必须释放 source，并从被外部保留的旧 style 上卸载 callback。
    if (core.sharedState->GetIsInteracting()) {
        return false;
    }
    destroyedStyle->InvokeEvent(
        vtkCommand::StartInteractionEvent);
    destroyedStyle->InvokeEvent(
        vtkCommand::EndInteractionEvent);
    return !core.sharedState->GetIsInteracting();
}

bool BuildLeaseRetryTest()
{
    HostCoreServices core;
    core.sharedDataMgr =
        std::make_shared<RawVolumeDataManager>();
    core.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    core.sharedState =
        std::make_shared<SharedInteractionState>(
            core.sharedStateBroadcaster);

    auto renderWindow =
        vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);

    HostRenderViewConfig view;
    view.id = "lease-retry-probe";
    view.role = HostRenderViewRole::Primary3D;
    view.window.viewInit.viewMode = HostRenderMode::Volume;
    view.renderWindow = renderWindow;
    HostViewRuntimeRegistry views;
    if (!views.Build(core, { view })
        || !views.SetInteractorsReady()) {
        return false;
    }

    const int timerId = interactor->GetCreatedTimerId();
    if (timerId == 0) {
        return false;
    }
    const bool isTimerRemoved =
        interactor->DestroyTimer(timerId) != 0;
    const bool isFirstRejected = isTimerRemoved
        && !views.StopLease();
    const bool isRuntimeRetained =
        views.BuildEndpoints().size() == 1;
    auto* style = vtkInteractorStyle::SafeDownCast(
        interactor->GetInteractorStyle());
    if (style) {
        style->InvokeEvent(vtkCommand::StartInteractionEvent);
    }
    const bool isInteractionRetained = style
        && core.sharedState->GetIsInteracting();
    if (style) {
        style->InvokeEvent(vtkCommand::EndInteractionEvent);
    }
    const bool isRetryAccepted = views.StopLease();
    const bool isIdempotent = views.StopLease();
    std::cout
        << "DIAG_LEASE_RETRY: first=" << isFirstRejected
        << " retained=" << isRuntimeRetained
        << " interaction=" << isInteractionRetained
        << " retry=" << isRetryAccepted
        << " empty=" << views.BuildEndpoints().empty()
        << '\n';
    return isFirstRejected
        && isRuntimeRetained
        && isInteractionRetained
        && isRetryAccepted
        && isIdempotent
        && views.BuildEndpoints().empty();
}

bool BuildDefaultRenderTest()
{
    auto renderWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor = vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);
    const std::size_t renderCount = renderWindow->GetRenderCount();

    HostRenderViewConfig view;
    view.id = "render-probe";
    view.role = HostRenderViewRole::Primary3D;
    view.renderWindow = renderWindow;

    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession session(std::move(config));
    if (!session.BuildSession()
        || renderWindow->GetRenderCount() != renderCount) {
        return false;
    }
    interactor->Initialize();
    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = {
        "render-probe", false, HostRenderViewRole::Primary3D
    };
    if (!session.AttachTimer(timer)) {
        return false;
    }

    HostVolumeTransferFunction function;
    function.colorNodes = {
        { 0.0, 0.75, 0.75, 0.75 },
        { 1.0, 0.75, 0.75, 0.75 }
    };
    function.opacityNodes = {
        { 0.0, 0.0 },
        { 1.0, 1.0 }
    };
    HostViewSetRequest firstRequest;
    firstRequest.targetView.viewId = "render-probe";
    firstRequest.mode = HostRenderMode::Volume;
    firstRequest.volumeTransferFunction = function;
    if (!session.SendRequest(std::move(firstRequest))) {
        return false;
    }
    (void)SendTimer(interactor);
    if (renderWindow->GetRenderCount() != renderCount + 1) {
        return false;
    }

    HostViewSetRequest sameRequest;
    sameRequest.targetView.viewId = "render-probe";
    sameRequest.volumeTransferFunction = function;
    if (!session.SendRequest(std::move(sameRequest))) {
        return false;
    }
    const std::size_t changedRenderCount =
        renderWindow->GetRenderCount();
    (void)SendTimer(interactor);
    (void)SendTimer(interactor);
    return renderWindow->GetRenderCount() == changedRenderCount;
}

bool BuildZoomRenderTest()
{
    auto renderWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor = vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);

    HostRenderViewConfig view;
    view.id = "zoom-probe";
    view.role = HostRenderViewRole::TopDownSlice;
    view.window.viewInit.viewMode = HostRenderMode::SliceTopDown;
    view.renderWindow = renderWindow;

    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession session(std::move(config));
    if (!session.BuildSession()) {
        return false;
    }
    interactor->Initialize();
    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = {
        "zoom-probe", false, HostRenderViewRole::TopDownSlice
    };
    if (!session.AttachTimer(timer)) {
        return false;
    }
    const auto* endpoint =
        session.GetRenderViewEndpoint("zoom-probe");
    if (!endpoint || !endpoint->renderer || !endpoint->interactor
        || !endpoint->renderer->GetActiveCamera()) {
        return false;
    }

    // 清空 BuildSession 留下的历史 dirty，再单独测右键缩放的 render 来源。
    (void)SendTimer(endpoint->interactor);
    const std::size_t renderCount = renderWindow->GetRenderCount();
    auto* camera = endpoint->renderer->GetActiveCamera();
    const double startScale = camera->GetParallelScale();
    endpoint->interactor->SetEventPosition(100, 100);
    endpoint->interactor->InvokeEvent(
        vtkCommand::RightButtonPressEvent);
    constexpr int moveCount = 100;
    for (int index = 1; index <= moveCount; ++index) {
        endpoint->interactor->SetEventPosition(100, 100 + index);
        endpoint->interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
    }
    endpoint->interactor->InvokeEvent(
        vtkCommand::RightButtonReleaseEvent);

    const double expectedScale =
        startScale * std::pow(1.01, moveCount);
    const bool isCameraUpdated =
        std::abs(camera->GetParallelScale() - expectedScale)
            < 1e-9;
    const std::size_t directRenderCount =
        renderWindow->GetRenderCount() - renderCount;
    const bool hasNoDirectRender =
        directRenderCount == 0;
    (void)SendTimer(endpoint->interactor);
    const bool hasOneTimerRender =
        renderWindow->GetRenderCount() == renderCount + 1;
    (void)SendTimer(endpoint->interactor);
    std::cout
        << "DIAG: zoom moves=" << moveCount
        << " direct_renders=" << directRenderCount
        << " camera_ok=" << isCameraUpdated
        << " timer_once=" << hasOneTimerRender
        << '\n';
    return isCameraUpdated
        && hasNoDirectRender
        && hasOneTimerRender
        && renderWindow->GetRenderCount() == renderCount + 1;
}

class SessionFixture final {
public:
    bool BuildWindow()
    {
        if (m_widget || m_renderWindow) {
            return false;
        }

        m_widget = std::make_unique<QVTKOpenGLNativeWidget>();
        m_widget->setWindowTitle(QStringLiteral("MVVCVTK Qt Host Session Smoke"));
        m_renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        if (!AttachRenderStats()) {
            return false;
        }
        m_errorCallback = vtkSmartPointer<vtkCallbackCommand>::New();
        m_errorCallback->SetClientData(this);
        m_errorCallback->SetCallback(&SessionFixture::OnVtkError);
        m_errorTag = m_renderWindow->AddObserver(
            vtkCommand::ErrorEvent, m_errorCallback);
        if (m_errorTag == 0) {
            DetachRenderStats();
            return false;
        }
        m_widget->setRenderWindow(m_renderWindow);
        m_widget->resize(640, 480);
        m_widget->show();
        return true;
    }

    bool StartHost()
    {
        if (!m_widget || !m_renderWindow || m_session) {
            return false;
        }

        HostRenderViewConfig view;
        view.id = "primary-3d";
        view.role = HostRenderViewRole::Primary3D;
        view.window.title = "Qt Host Session Smoke";
        view.window.width = 640;
        view.window.height = 480;
        view.window.viewInit.viewMode = HostRenderMode::CompositeIsoSurface;
        view.window.viewInit.background = { 0.08, 0.12, 0.16 };
        view.window.viewInit.hasBackground = true;
        view.renderWindow = m_renderWindow;

        HostSessionConfig config;
        config.renderViews.push_back(std::move(view));

        m_session = std::make_unique<VtkAppHostSession>(std::move(config));
        const std::size_t renderStartCount = m_renderStartCount;
        const bool isBuilt = m_session->BuildSession();
        HostTimerConfig timer;
        timer.isTimerEnabled = true;
        timer.targetView = { "primary-3d", false, HostRenderViewRole::Primary3D };
        const bool isTimerAttached = m_session->AttachTimer(timer);
        const bool hasInitialRender = m_renderStartCount != renderStartCount;

        const auto& endpoints = m_session->GetRenderViewEndpoints();
        const auto* endpoint = m_session->GetRenderViewEndpoint("primary-3d");
        const auto* primaryEndpoint = m_session->GetPrimaryEndpoint();
        if (m_vtkErrorCount > 0) {
            std::cerr
                << "FAIL: VTK ErrorEvent during BuildSession"
                << " count=" << m_vtkErrorCount;
            if (!m_vtkErrorText.empty()) {
                std::cerr << " message=" << m_vtkErrorText;
            }
            std::cerr << '\n';
        }
        const bool hasRenderStats =
            m_renderStartCount == m_renderEndCount
            && m_renderStarts.empty()
            && !m_renderTimesMs.empty();
        std::cout
            << "DIAG: render samples=" << m_renderTimesMs.size()
            << " p50_ms=" << GetRenderTimeMs(0.50)
            << " p95_ms=" << GetRenderTimeMs(0.95)
            << " max_ms=" << GetRenderTimeMs(1.00)
            << '\n';
        return isBuilt && isTimerAttached && !hasInitialRender
            && m_vtkErrorCount == 0
            && hasRenderStats
            && endpoints.size() == 1
            && endpoint != nullptr
            && primaryEndpoint == endpoint
            && endpoint->role == HostRenderViewRole::Primary3D
            && endpoint->renderer != nullptr
            && endpoint->interactor != nullptr
            && endpoint->renderWindow == m_renderWindow.Get()
            && m_widget->renderWindow() == m_renderWindow.Get();
    }

    bool GetBlendVisualValid()
    {
        if (!m_session) {
            return false;
        }
        const auto* endpoint =
            m_session->GetRenderViewEndpoint("primary-3d");
        if (!endpoint || !endpoint->renderer
            || !endpoint->renderWindow) {
            return false;
        }

        constexpr int sideLength = 32;
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(
            sideLength, sideLength, sideLength);
        image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        auto* scalars =
            static_cast<unsigned char*>(image->GetScalarPointer());
        if (!scalars) {
            return false;
        }
        for (int z = 0; z < sideLength; ++z) {
            for (int y = 0; y < sideLength; ++y) {
                for (int x = 0; x < sideLength; ++x) {
                    const double dx =
                        x - (sideLength - 1) * 0.5;
                    const double dy =
                        y - (sideLength - 1) * 0.5;
                    const double dz =
                        z - (sideLength - 1) * 0.5;
                    const double radius =
                        std::sqrt(dx * dx + dy * dy + dz * dz);
                    const vtkIdType index =
                        x + sideLength
                            * (y + sideLength * z);
                    scalars[index] = radius < 13.0
                        ? static_cast<unsigned char>(
                            std::max(1.0, 255.0 - radius * 16.0))
                        : 0;
                }
            }
        }
        image->Modified();

        auto mapper =
            vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
        mapper->SetInputData(image);
        mapper->AutoAdjustSampleDistancesOff();
        mapper->SetSampleDistance(0.5);
        mapper->SetImageSampleDistance(1.0);
        mapper->UseJitteringOff();
        auto color =
            vtkSmartPointer<vtkColorTransferFunction>::New();
        color->AddRGBPoint(0.0, 0.0, 0.0, 0.0);
        color->AddRGBPoint(255.0, 0.75, 0.75, 0.75);
        auto opacity =
            vtkSmartPointer<vtkPiecewiseFunction>::New();
        opacity->AddPoint(0.0, 0.0);
        opacity->AddPoint(255.0, 0.85);
        auto property =
            vtkSmartPointer<vtkVolumeProperty>::New();
        property->SetColor(color);
        property->SetScalarOpacity(opacity);
        property->ShadeOff();
        property->SetInterpolationTypeToLinear();
        auto volume = vtkSmartPointer<vtkVolume>::New();
        volume->SetMapper(mapper);
        volume->SetProperty(property);

        vtkSmartPointer<vtkRenderer> renderer =
            endpoint->renderer;
        vtkNew<vtkCamera> cameraSnapshot;
        cameraSnapshot->DeepCopy(
            renderer->GetActiveCamera());
        const int multiSamples =
            endpoint->renderWindow->GetMultiSamples();
        renderer->AddVolume(volume);
        renderer->ResetCamera();
        endpoint->renderWindow->SetMultiSamples(0);
        constexpr int warmupCount = 3;
        for (int index = 0; index < warmupCount; ++index) {
            endpoint->renderWindow->Render();
        }

        const auto getPixels = [&]() {
            endpoint->renderWindow->Render();
            vtkNew<vtkWindowToImageFilter> capture;
            capture->SetInput(endpoint->renderWindow);
            capture->SetInputBufferTypeToRGB();
            capture->ReadFrontBufferOff();
            capture->ShouldRerenderOff();
            capture->Update();
            auto* output = capture->GetOutput();
            auto* pixels = output
                ? static_cast<unsigned char*>(
                    output->GetScalarPointer())
                : nullptr;
            const vtkIdType valueCount = output
                ? output->GetNumberOfPoints()
                    * output->GetNumberOfScalarComponents()
                : 0;
            return pixels && valueCount > 0
                ? std::vector<unsigned char>(
                    pixels, pixels + valueCount)
                : std::vector<unsigned char>{};
        };

        const int defaultBlend = mapper->GetBlendMode();
        const vtkMTimeType defaultMTime = mapper->GetMTime();
        const auto defaultPixels = getPixels();
        mapper->SetBlendModeToComposite();
        const vtkMTimeType explicitMTime = mapper->GetMTime();
        const auto explicitPixels = getPixels();
        mapper->SetBlendModeToComposite();
        const vtkMTimeType repeatMTime = mapper->GetMTime();
        const auto repeatPixels = getPixels();
        renderer->RemoveVolume(volume);
        renderer->GetActiveCamera()->DeepCopy(
            cameraSnapshot);
        endpoint->renderWindow->SetMultiSamples(
            multiSamples);

        const unsigned char maxSignal =
            defaultPixels.empty()
            ? 0
            : *std::max_element(
                defaultPixels.begin(),
                defaultPixels.end());
        const auto getMaxDiff = [](
            const std::vector<unsigned char>& left,
            const std::vector<unsigned char>& right) {
            if (left.size() != right.size()) {
                return 255;
            }
            int maxDiff = 0;
            for (std::size_t index = 0;
                index < left.size();
                ++index) {
                maxDiff = std::max(
                    maxDiff,
                    std::abs(
                        static_cast<int>(left[index])
                        - static_cast<int>(right[index])));
            }
            return maxDiff;
        };
        const int explicitDiff =
            getMaxDiff(defaultPixels, explicitPixels);
        const int repeatDiff =
            getMaxDiff(explicitPixels, repeatPixels);
        const bool hasSignal = maxSignal > 64;
        const bool isVisualEqual =
            !defaultPixels.empty()
            && explicitDiff <= 1
            && repeatDiff <= 1;
        const bool isBlendStable =
            defaultBlend == vtkVolumeMapper::COMPOSITE_BLEND
            && mapper->GetBlendMode()
                == vtkVolumeMapper::COMPOSITE_BLEND
            && defaultMTime == explicitMTime
            && explicitMTime == repeatMTime;
        std::cout
            << "DIAG_BLEND: default=" << defaultBlend
            << " pixels=" << defaultPixels.size()
            << " signal=" << hasSignal
            << " explicit_max_diff=" << explicitDiff
            << " repeat_max_diff=" << repeatDiff
            << " mtime_stable=" << isBlendStable
            << '\n';
        return hasSignal && isVisualEqual && isBlendStable;
    }

    bool StopHost()
    {
        m_session.reset();
        if (m_widget) {
            m_widget->setRenderWindow(
                static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
        }
        if (m_renderWindow && m_errorTag != 0) {
            m_renderWindow->RemoveObserver(m_errorTag);
        }
        m_errorTag = 0;
        if (m_errorCallback) {
            m_errorCallback->SetClientData(nullptr);
        }
        DetachRenderStats();
        m_renderWindow = nullptr;
        return true;
    }

    bool StartVolumeBench(int sampleCount)
    {
        if (!m_session || sampleCount < 1) {
            return false;
        }
        const auto* endpoint =
            m_session->GetRenderViewEndpoint("primary-3d");
        if (!endpoint || !endpoint->renderer
            || !endpoint->renderWindow) {
            return false;
        }

        auto image = vtkSmartPointer<vtkImageData>::New();
        constexpr int sideLength = 128;
        image->SetDimensions(sideLength, sideLength, sideLength);
        image->SetSpacing(1.0, 1.0, 1.0);
        image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        auto* scalars =
            static_cast<unsigned char*>(image->GetScalarPointer());
        if (!scalars) {
            return false;
        }
        const vtkIdType voxelCount = image->GetNumberOfPoints();
        for (vtkIdType index = 0; index < voxelCount; ++index) {
            scalars[index] = static_cast<unsigned char>(index % 256);
        }
        image->Modified();

        HostReloadRequest reload;
        reload.voxels.resize(
            static_cast<std::size_t>(voxelCount));
        for (vtkIdType index = 0;
            index < voxelCount;
            ++index) {
            reload.voxels[
                static_cast<std::size_t>(index)] =
                    static_cast<float>(scalars[index]);
        }
        reload.geometry.dimensions = {
            sideLength, sideLength, sideLength
        };
        reload.geometry.spacing = {
            1.0f, 1.0f, 1.0f
        };
        reload.geometry.origin = {
            0.0f, 0.0f, 0.0f
        };
        bool isReloadComplete = false;
        bool isReloadSucceeded = false;
        if (!m_session->SendRequest(
                std::move(reload),
                [&isReloadComplete,
                    &isReloadSucceeded](bool isSucceeded) {
                    isReloadSucceeded = isSucceeded;
                    isReloadComplete = true;
                })) {
            return false;
        }
        constexpr int reloadPollCount = 1000;
        for (int poll = 0;
            !isReloadComplete && poll < reloadPollCount;
            ++poll) {
            (void)SendTimer(endpoint->interactor);
            endpoint->renderWindow->Render();
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        if (!isReloadComplete || !isReloadSucceeded) {
            return false;
        }

        VolumeStrategy strategy;
        strategy.SetInputData(image);
        RenderParams params;
        params.scalarRange[0] = 0.0;
        params.scalarRange[1] = 255.0;
        VolumeTransferFunction currentFunction;
        currentFunction.colorNodes = {
            { 0.00, 0.00, 0.00, 0.00 },
            { 89.25, 0.75, 0.75, 0.75 },
            { 153.0, 0.85, 0.85, 0.85 },
            { 255.0, 0.95, 0.95, 0.95 }
        };
        currentFunction.opacityNodes = {
            { 0.00, 0.0 },
            { 89.25, 0.0 },
            { 153.0, 0.6 },
            { 255.0, 1.0 }
        };
        VolumeTransferFunction grayFunction;
        grayFunction.colorNodes = {
            { 0.0, 0.75, 0.75, 0.75 },
            { 127.5, 0.75, 0.75, 0.75 },
            { 255.0, 0.75, 0.75, 0.75 }
        };
        grayFunction.opacityNodes = {
            { 0.0, 0.0 },
            { 127.5, 0.1 },
            { 255.0, 0.8 }
        };
        params.volumeTransferFunction = grayFunction;
        strategy.SetVisualState(params, UpdateFlags::VolumeTransfer);
        vtkSmartPointer<vtkRenderer> renderer = endpoint->renderer;
        strategy.AttachRenderer(renderer);
        renderer->ResetCamera();
        auto* volume = vtkVolume::SafeDownCast(
            strategy.GetMainProp());
        auto* mapper = volume
            ? vtkGPUVolumeRayCastMapper::SafeDownCast(
                volume->GetMapper())
            : nullptr;
        if (!mapper || !volume->GetProperty()) {
            strategy.DetachRenderer(renderer);
            return false;
        }
        TimerProbePort timerProbe;
        TimeUpdateHandler timerHandler(
            &timerProbe, endpoint->renderWindow);
        InteractionEvent timerEvent;
        timerEvent.eventKind = InteractionEventKind::Timer;

        const auto startSamples =
            [&](const char* caseName,
                bool isMaskExpected = false,
                bool isDenoiseExpected = false) {
            if (!ResetRenderStats()) {
                return false;
            }
            constexpr int warmupCount = 3;
            for (int index = 0; index < warmupCount; ++index) {
                endpoint->renderWindow->Render();
            }
            const bool isWarmupValid =
                m_renderTimesMs.size()
                    == static_cast<std::size_t>(warmupCount)
                && m_renderStartCount == m_renderEndCount
                && m_renderStarts.empty();
            const double warmupMaxMs = GetRenderTimeMs(1.00);
            if (!ResetRenderStats()) {
                return false;
            }
            for (int index = 0; index < sampleCount; ++index) {
                endpoint->renderWindow->Render();
            }

            const bool isSampleValid =
                m_renderTimesMs.size()
                    == static_cast<std::size_t>(sampleCount)
                && m_renderStartCount == m_renderEndCount
                && m_renderStarts.empty();
            auto* property = volume->GetProperty();
            auto* color = property
                ? property->GetRGBTransferFunction()
                : nullptr;
            auto* opacity = property
                ? property->GetScalarOpacity()
                : nullptr;
            const bool hasCustomGradient =
                property
                && property->HasGradientOpacity() != 0;
            auto* gradient = property
                ? property->GetGradientOpacity()
                : nullptr;
            double colorRange[2] = {};
            double opacityRange[2] = {};
            double gradientRange[2] = {};
            if (color) {
                color->GetRange(colorRange);
            }
            if (opacity) {
                opacity->GetRange(opacityRange);
            }
            if (gradient) {
                gradient->GetRange(gradientRange);
            }
            const auto& material = params.material;
            const bool hasGradient =
                !params.gradientOpacity.empty();
            bool areTfNodesValid =
                color
                && opacity
                && color->GetSize()
                    == static_cast<int>(
                        params.volumeTransferFunction.colorNodes.size())
                && opacity->GetSize()
                    == static_cast<int>(
                        params.volumeTransferFunction.opacityNodes.size());
            for (std::size_t index = 0;
                areTfNodesValid
                    && index
                        < params.volumeTransferFunction.colorNodes.size();
                ++index) {
                double colorNode[6] = {};
                double opacityNode[4] = {};
                color->GetNodeValue(
                    static_cast<int>(index), colorNode);
                opacity->GetNodeValue(
                    static_cast<int>(index), opacityNode);
                const auto& expectedColor =
                    params.volumeTransferFunction.colorNodes[index];
                const auto& expectedOpacity =
                    params.volumeTransferFunction.opacityNodes[index];
                areTfNodesValid =
                    std::abs(
                        colorNode[0] - expectedColor.scalar) < 1e-12
                    && std::abs(
                        colorNode[1] - expectedColor.r) < 1e-12
                    && std::abs(
                        colorNode[2] - expectedColor.g) < 1e-12
                    && std::abs(
                        colorNode[3] - expectedColor.b) < 1e-12
                    && std::abs(
                        opacityNode[0] - expectedOpacity.scalar) < 1e-12
                    && std::abs(
                        opacityNode[1]
                            - expectedOpacity.opacity
                                * material.opacity) < 1e-12;
            }
            bool areGradientNodesValid =
                !hasGradient
                || (gradient
                    && gradient->GetSize()
                        == static_cast<int>(
                            params.gradientOpacity.size()));
            for (const auto& node : params.gradientOpacity) {
                areGradientNodesValid =
                    areGradientNodesValid
                    && gradient
                    && std::abs(
                        gradient->GetValue(node.gradient)
                            - node.opacity) < 1e-12;
            }
            vtkImageData* maskInput =
                mapper ? mapper->GetMaskInput() : nullptr;
            double maskRange[2] = {};
            if (maskInput) {
                maskInput->GetScalarRange(maskRange);
            }
            const auto inputConnection =
                mapper ? mapper->GetInputConnection(0, 0) : nullptr;
            vtkAlgorithm* inputProducer =
                inputConnection ? inputConnection->GetProducer()
                    : nullptr;
            const auto producerInputConnection =
                inputProducer
                ? inputProducer->GetInputConnection(0, 0)
                : nullptr;
            vtkAlgorithm* producerInput =
                producerInputConnection
                ? producerInputConnection->GetProducer()
                : nullptr;
            const bool isDenoiseApplied =
                producerInput
                && producerInput->IsA(
                    "vtkImageAnisotropicDiffusion3D");
            const bool isMaskApplied =
                maskInput
                && maskInput->GetScalarType()
                    == VTK_UNSIGNED_CHAR
                && maskInput->GetNumberOfScalarComponents() == 1
                && maskRange[0] <= 1.0
                && maskRange[1] >= 254.0;
            vtkNew<vtkWindowToImageFilter> visualCapture;
            visualCapture->SetInput(endpoint->renderWindow);
            visualCapture->SetInputBufferTypeToRGB();
            visualCapture->ReadFrontBufferOff();
            visualCapture->ShouldRerenderOff();
            visualCapture->Update();
            auto* visualOutput = visualCapture->GetOutput();
            auto* visualPixels = visualOutput
                ? static_cast<unsigned char*>(
                    visualOutput->GetScalarPointer())
                : nullptr;
            const vtkIdType visualValueCount = visualOutput
                ? visualOutput->GetNumberOfPoints()
                    * visualOutput->GetNumberOfScalarComponents()
                : 0;
            unsigned char visualSignal = 0;
            std::uint64_t visualHash = 1469598103934665603ULL;
            if (visualPixels && visualValueCount > 0) {
                for (vtkIdType index = 0;
                    index < visualValueCount;
                    ++index) {
                    visualSignal = std::max(
                        visualSignal, visualPixels[index]);
                    visualHash ^= visualPixels[index];
                    visualHash *= 1099511628211ULL;
                }
            }
            const bool isVisualValid =
                visualPixels && visualValueCount > 0
                && visualSignal > 0;
            const bool isAutoSampling =
                params.volumeQuality == VolumeQuality::Auto;
            double maximumImageDistance = 2.5;
            switch (params.volumeQuality) {
            case VolumeQuality::Low:
                maximumImageDistance = 3.0;
                break;
            case VolumeQuality::XHigh:
                maximumImageDistance = 2.0;
                break;
            case VolumeQuality::Ultra:
                maximumImageDistance = 1.5;
                break;
            case VolumeQuality::Auto:
            case VolumeQuality::High:
                break;
            }
            const bool isStateValid =
                property
                && color
                && opacity
                && areTfNodesValid
                && areGradientNodesValid
                && std::abs(colorRange[0]
                    - params.scalarRange[0]) < 1e-12
                && std::abs(colorRange[1]
                    - params.scalarRange[1]) < 1e-12
                && std::abs(opacityRange[0]
                    - params.scalarRange[0]) < 1e-12
                && std::abs(opacityRange[1]
                    - params.scalarRange[1]) < 1e-12
                && property->GetShade()
                    == static_cast<int>(material.isShadeOn)
                && std::abs(
                    property->GetAmbient()
                        - material.ambient) < 1e-12
                && std::abs(
                    property->GetDiffuse()
                        - material.diffuse) < 1e-12
                && std::abs(
                    property->GetSpecular()
                        - material.specular) < 1e-12
                && std::abs(
                    property->GetSpecularPower()
                        - material.specularPower) < 1e-12
                && hasCustomGradient == hasGradient
                && (!hasGradient
                    || (gradient
                        && gradient->GetSize()
                            == static_cast<int>(
                                params.gradientOpacity.size())
                        && std::abs(
                            gradientRange[0]
                                - params.gradientOpacity.front().gradient)
                            < 1e-12
                        && std::abs(
                            gradientRange[1]
                                - params.gradientOpacity.back().gradient)
                            < 1e-12))
                && mapper->GetGradientOpacityRangeType()
                    == vtkGPUVolumeRayCastMapper::SCALAR
                && std::abs(image->GetScalarRange()[0]) < 1e-12
                && std::abs(
                    image->GetScalarRange()[1] - 255.0) < 1e-12
                && std::abs(image->GetSpacing()[0] - 1.0) < 1e-12
                && std::abs(image->GetSpacing()[1] - 1.0) < 1e-12
                && std::abs(image->GetSpacing()[2] - 1.0) < 1e-12
                && std::abs(
                    mapper->GetImageSampleDistance() - 1.0) < 1e-12
                && std::abs(
                    mapper->GetMinimumImageSampleDistance() - 1.0)
                    < 1e-12
                && std::abs(
                    mapper->GetMaximumImageSampleDistance()
                        - maximumImageDistance)
                    < 1e-12
                && mapper->GetAutoAdjustSampleDistances()
                    == static_cast<int>(isAutoSampling)
                && mapper->GetUseJittering() != 0;
            const bool isAuxiliaryStateValid =
                isMaskApplied == isMaskExpected
                && isDenoiseApplied == isDenoiseExpected
                && (!isMaskExpected || mapper->GetMaskInput())
                && (!isDenoiseExpected || producerInput);
            std::cout
                << "BENCH: case=" << caseName
                << " volume_dims=" << sideLength
                << "^3 samples=" << m_renderTimesMs.size()
                << " warmup_max_ms=" << warmupMaxMs
                << " p50_ms=" << GetRenderTimeMs(0.50)
                << " p95_ms=" << GetRenderTimeMs(0.95)
                << " max_ms=" << GetRenderTimeMs(1.00)
                << " auto="
                << mapper->GetAutoAdjustSampleDistances()
                << " ray_step=" << mapper->GetSampleDistance()
                << " image_step="
                << mapper->GetImageSampleDistance()
                << " image_min="
                << mapper->GetMinimumImageSampleDistance()
                << " image_max="
                << mapper->GetMaximumImageSampleDistance()
                << " jitter=" << mapper->GetUseJittering()
                << " quality="
                << static_cast<int>(params.volumeQuality)
                << " shade=" << property->GetShade()
                << " ambient=" << property->GetAmbient()
                << " diffuse=" << property->GetDiffuse()
                << " specular=" << property->GetSpecular()
                << " specular_power="
                << property->GetSpecularPower()
                << " opacity=" << material.opacity
                << " gradient="
                << hasCustomGradient
                << " gradient_type="
                << mapper->GetGradientOpacityRangeType()
                << " gradient_size="
                << (gradient ? gradient->GetSize() : 0)
                << " gradient_range="
                << gradientRange[0] << ',' << gradientRange[1]
                << " mask_applied=" << isMaskApplied
                << " mask_range="
                << maskRange[0] << ',' << maskRange[1]
                << " denoise_applied=" << isDenoiseApplied
                << " visual_pixels=" << visualValueCount
                << " visual_signal="
                << static_cast<int>(visualSignal)
                << " visual_hash=" << visualHash
                << " scalar_range="
                << image->GetScalarRange()[0] << ','
                << image->GetScalarRange()[1]
                << " spacing="
                << image->GetSpacing()[0] << ','
                << image->GetSpacing()[1] << ','
                << image->GetSpacing()[2]
                << " ctf_size=" << color->GetSize()
                << " ctf_range="
                << colorRange[0] << ',' << colorRange[1]
                << " otf_size=" << opacity->GetSize()
                << " otf_range="
                << opacityRange[0] << ',' << opacityRange[1]
                << " props="
                << renderer->GetViewProps()->GetNumberOfItems()
                << " state_ok="
                << (isStateValid
                    && isAuxiliaryStateValid
                    && isVisualValid)
                << '\n';
            return isWarmupValid
                && isSampleValid
                && isStateValid
                && isAuxiliaryStateValid
                && isVisualValid;
        };

        // 所有场景共用同一真实 Qt/OpenGL 上下文、相机和体数据，以减少平台上下文噪声；
        // 每次计时前完整恢复策略基线，使 case 只保留名称所指的单一变量。
        const auto setBaseState = [&]() {
            strategy.SetInputMask(nullptr);
            params.volumeTransferFunction = grayFunction;
            params.material = {
                0.1, 0.7, 0.2, 10.0, 1.0, false
            };
            params.gradientOpacity.clear();
            params.volumeQuality = VolumeQuality::Auto;
            params.isDenoiseOn = false;
            params.isInteracting = false;
            strategy.SetVisualState(
                params,
                UpdateFlags::VolumeTransfer
                    | UpdateFlags::Material
                    | UpdateFlags::GradientOpacity
                    | UpdateFlags::Quality
                    | UpdateFlags::Denoise
                    | UpdateFlags::RenderRate);
        };

        setBaseState();
        params.volumeTransferFunction = currentFunction;
        strategy.SetVisualState(params, UpdateFlags::VolumeTransfer);
        bool areSamplesValid =
            startSamples("A_current_tf");
        setBaseState();
        areSamplesValid =
            startSamples("B_gray_tf") && areSamplesValid;

        setBaseState();
        params.material.isShadeOn = true;
        strategy.SetVisualState(
            params, UpdateFlags::Material);
        areSamplesValid =
            startSamples("C_gray_shade") && areSamplesValid;

        setBaseState();
        params.gradientOpacity = {
            { 0.0, 0.0 }, { 32.0, 0.3 }, { 255.0, 1.0 }
        };
        strategy.SetVisualState(
            params,
            UpdateFlags::GradientOpacity);
        areSamplesValid =
            startSamples("D_gray_gradient")
                && areSamplesValid;

        setBaseState();
        params.material.isShadeOn = true;
        params.gradientOpacity = {
            { 0.0, 0.0 }, { 32.0, 0.3 }, { 255.0, 1.0 }
        };
        strategy.SetVisualState(
            params,
            UpdateFlags::Material
                | UpdateFlags::GradientOpacity);
        areSamplesValid =
            startSamples("E_gray_shade_gradient")
                && areSamplesValid;

        setBaseState();
        mapper->SetAutoAdjustSampleDistances(true);
        areSamplesValid =
            startSamples("F_gray_auto") && areSamplesValid;

        setBaseState();
        params.volumeQuality = VolumeQuality::XHigh;
        strategy.SetVisualState(
            params, UpdateFlags::Quality);
        areSamplesValid =
            startSamples("G0_gray_xhigh")
                && areSamplesValid;

        setBaseState();
        params.volumeQuality = VolumeQuality::Low;
        strategy.SetVisualState(
            params, UpdateFlags::Quality);
        areSamplesValid =
            startSamples("G1_gray_low")
                && areSamplesValid;

        setBaseState();
        params.material = {
            0.25, 0.65, 0.10, 8.0, 0.80, false
        };
        strategy.SetVisualState(
            params, UpdateFlags::Material);
        areSamplesValid =
            startSamples("M_soft") && areSamplesValid;

        setBaseState();
        params.material = {
            0.10, 0.85, 0.25, 20.0, 1.0, false
        };
        strategy.SetVisualState(
            params, UpdateFlags::Material);
        areSamplesValid =
            startSamples("M_dense") && areSamplesValid;

        setBaseState();
        params.material = {
            0.08, 0.65, 0.65, 40.0, 1.0, true
        };
        strategy.SetVisualState(
            params, UpdateFlags::Material);
        areSamplesValid =
            startSamples("M_glossy") && areSamplesValid;

        setBaseState();
        auto mask = vtkSmartPointer<vtkImageData>::New();
        mask->CopyStructure(image);
        mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        auto* maskScalars =
            static_cast<unsigned char*>(mask->GetScalarPointer());
        if (!maskScalars) {
            strategy.DetachRenderer(renderer);
            return false;
        }
        for (vtkIdType index = 0; index < voxelCount; ++index) {
            maskScalars[index] = index % 3 == 0 ? 0 : 255;
        }
        mask->Modified();
        const auto maskStart = std::chrono::steady_clock::now();
        strategy.SetInputMask(mask);
        const double maskBuildMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - maskStart).count();
        std::cout
            << "BENCH_BUILD: case=mask build_ms="
            << maskBuildMs << '\n';
        areSamplesValid =
            startSamples("mask", true, false) && areSamplesValid;

        setBaseState();
        params.isDenoiseOn = true;
        const auto denoiseStart =
            std::chrono::steady_clock::now();
        strategy.SetVisualState(
            params, UpdateFlags::Denoise);
        const double denoiseBuildMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now()
                    - denoiseStart).count();
        std::cout
            << "BENCH_BUILD: case=denoise build_ms="
            << denoiseBuildMs << '\n';
        areSamplesValid =
            startSamples("denoise", false, true) && areSamplesValid;

        const auto setVolumeMode = [&]() {
            HostViewSetRequest request;
            request.targetView.viewId = "primary-3d";
            request.mode = HostRenderMode::Volume;
            if (!m_session->SendRequest(std::move(request))) {
                return false;
            }
            (void)SendTimer(endpoint->interactor);
            return std::abs(
                endpoint->renderWindow->GetDesiredUpdateRate()
                    - 0.001) < 1e-12;
        };
        const auto samplePhase =
            [&](const char* caseName,
                const char* phaseName,
                double expectedRate,
                bool expectedAuto,
                double expectedRay,
                double expectedImage,
                double expectedMaxImage,
                bool isPreview,
                bool isWarmupNeeded,
                double* phaseP95) {
            params.isInteracting = isPreview;
            if (!strategy.SetVisualState(
                    params, UpdateFlags::RenderRate)) {
                return false;
            }
            endpoint->renderWindow->SetDesiredUpdateRate(
                expectedRate);
            const auto sendTimerFrame = [&]() {
                // 重复置脏必须由一次 Timer 心跳合并成一次可见 Render。
                timerProbe.SetRenderNeeded();
                timerProbe.SetRenderNeeded();
                (void)timerHandler.Send(timerEvent);
            };
            timerProbe.ResetCount();
            if (!ResetRenderStats()) {
                return false;
            }
            if (isWarmupNeeded) {
                constexpr int warmupCount = 3;
                for (int index = 0;
                    index < warmupCount;
                    ++index) {
                    sendTimerFrame();
                }
                const bool isWarmupValid =
                    m_renderTimesMs.size()
                        == static_cast<std::size_t>(
                            warmupCount)
                    && timerProbe.GetUpdateCount()
                        == static_cast<std::size_t>(
                            warmupCount)
                    && m_renderStartCount == m_renderEndCount
                    && m_renderStarts.empty();
                std::cout
                    << "BENCH_PHASE_WARMUP: case="
                    << caseName
                    << " phase=" << phaseName
                    << " samples=" << m_renderTimesMs.size()
                    << " max_ms=" << GetRenderTimeMs(1.00)
                    << '\n';
                if (!isWarmupValid
                    || !ResetRenderStats()) {
                    return false;
                }
                timerProbe.ResetCount();
            }
            for (int index = 0; index < sampleCount; ++index) {
                sendTimerFrame();
                if (m_renderTimesMs.size()
                        != static_cast<std::size_t>(index + 1)
                    || mapper->GetAutoAdjustSampleDistances()
                        != static_cast<int>(expectedAuto)
                    || std::abs(
                        mapper->GetSampleDistance() - expectedRay)
                        >= 1e-12
                    || std::abs(
                        mapper->GetImageSampleDistance()
                            - expectedImage)
                        >= 1e-12
                    || mapper->GetUseJittering()
                        != static_cast<int>(!isPreview)
                    || isPreview
                        != (endpoint->renderWindow
                                ->GetDesiredUpdateRate()
                            >= 15.0)) {
                    return false;
                }
            }
            const bool isPhaseValid =
                m_renderStartCount == m_renderEndCount
                && m_renderStarts.empty()
                && timerProbe.GetUpdateCount()
                    == static_cast<std::size_t>(sampleCount)
                && std::abs(
                    endpoint->renderWindow
                        ->GetDesiredUpdateRate()
                        - expectedRate) < 1e-12
                && mapper->GetAutoAdjustSampleDistances()
                    == static_cast<int>(expectedAuto)
                && std::abs(
                    mapper->GetSampleDistance() - expectedRay)
                    < 1e-12
                && std::abs(
                    mapper->GetImageSampleDistance()
                        - expectedImage)
                    < 1e-12
                && std::abs(
                    mapper->GetMinimumImageSampleDistance()
                        - (isPreview ? expectedImage : 1.0))
                    < 1e-12
                && std::abs(
                    mapper->GetMaximumImageSampleDistance()
                        - (isPreview
                            ? expectedImage : expectedMaxImage))
                    < 1e-12
                && mapper->GetUseJittering()
                    == static_cast<int>(!isPreview);
            const double p95Ms = GetRenderTimeMs(0.95);
            if (phaseP95) {
                *phaseP95 = p95Ms;
            }
            std::cout
                << "BENCH_PHASE: case=" << caseName
                << " phase=" << phaseName
                << " source=TimerPhase"
                << " samples=" << m_renderTimesMs.size()
                << " p50_ms=" << GetRenderTimeMs(0.50)
                << " p95_ms=" << p95Ms
                << " max_ms=" << GetRenderTimeMs(1.00)
                << " desired_rate="
                << endpoint->renderWindow
                    ->GetDesiredUpdateRate()
                << " auto="
                << mapper->GetAutoAdjustSampleDistances()
                << " ray_step="
                << mapper->GetSampleDistance()
                << " image_step="
                << mapper->GetImageSampleDistance()
                << " image_min="
                << mapper->GetMinimumImageSampleDistance()
                << " image_max="
                << mapper->GetMaximumImageSampleDistance()
                << " state_ok=" << isPhaseValid
                << '\n';
            return isPhaseValid;
        };
        const auto runInteractionPhases =
            [&](const char* caseName,
                const VolumeQuality quality,
                const double previewImage,
                const double previewRayFactor,
                const double maximumImage,
                bool isGainRequired) {
            setBaseState();
            params.volumeQuality = quality;
            strategy.SetVisualState(
                params, UpdateFlags::Quality);
            // QualityState 只在 GPURender 的 OpenGL owner thread 提交；
            // 先渲染一帧，再读取该档静止 ray 基线。
            endpoint->renderWindow->Render();
            const double stillRay = mapper->GetSampleDistance();
            const double previewRay =
                stillRay * previewRayFactor;
            constexpr std::size_t phaseRounds = 3;
            std::array<double, phaseRounds> staticP95s{};
            std::array<double, phaseRounds> previewP95s{};
            std::array<double, phaseRounds> gainRatios{};
            bool arePhasesValid = true;
            for (std::size_t round = 0;
                round < phaseRounds;
                ++round) {
                double beforeP95 = 0.0;
                double duringP95 = 0.0;
                double afterP95 = 0.0;
                const bool isBefore = samplePhase(
                    caseName, "before", 0.001,
                    false, stillRay, 1.0, maximumImage, false,
                    round == 0, &beforeP95);
                const bool isDuring = samplePhase(
                    caseName, "during", 15.0,
                    false, previewRay, previewImage,
                    maximumImage, true,
                    false, &duringP95);
                const bool isAfter = samplePhase(
                    caseName, "after", 0.001,
                    false, stillRay, 1.0, maximumImage, false,
                    false, &afterP95);
                const double staticP95 =
                    std::max(beforeP95, afterP95);
                staticP95s[round] = staticP95;
                previewP95s[round] = duringP95;
                gainRatios[round] = staticP95 > 0.0
                    ? (staticP95 - duringP95) / staticP95
                    : 0.0;
                arePhasesValid =
                    isBefore && isDuring && isAfter
                    && arePhasesValid;
            }
            std::sort(
                staticP95s.begin(), staticP95s.end());
            std::sort(
                previewP95s.begin(), previewP95s.end());
            std::sort(
                gainRatios.begin(), gainRatios.end());
            const double staticP95 = staticP95s[1];
            const double duringP95 = previewP95s[1];
            const double medianGain = gainRatios[1];
            const bool isP95Improved = medianGain > 0.0;
            std::cout
                << "BENCH_GATE: case=" << caseName
                << " static_p95_ms=" << staticP95
                << " preview_p95_ms=" << duringP95
                << " gain_ratio=" << medianGain
                << " accepted=" << isP95Improved
                << " required=" << isGainRequired
                << '\n';
            return arePhasesValid
                && (!isGainRequired || isP95Improved);
        };
        // 640x480 下 128^3 体的 GPU 帧低于 1 ms，Timer/驱动提交噪声会
        // 淹没采样轴差异。性能决策门固定到常见工业查看器尺寸；功能测试
        // 仍使用原窗口尺寸。
        m_widget->resize(1280, 960);
        QApplication::processEvents();
        endpoint->renderWindow->SetSize(1280, 960);
        renderer->ResetCamera();
        if (!setVolumeMode()) {
            strategy.DetachRenderer(renderer);
            return false;
        }
        std::vector<std::pair<vtkProp*, int>>
            hiddenProps;
        auto* interactionProps = renderer->GetViewProps();
        if (interactionProps) {
            interactionProps->InitTraversal();
            while (auto* prop =
                interactionProps->GetNextProp()) {
                if (prop != volume) {
                    hiddenProps.emplace_back(
                        prop, prop->GetVisibility());
                    prop->VisibilityOff();
                }
            }
        }
        areSamplesValid =
            runInteractionPhases(
                "I_low_interaction",
                VolumeQuality::Low,
                2.0, 2.0, 3.0, false)
            && areSamplesValid;
        areSamplesValid =
            runInteractionPhases(
                "J_high_interaction",
                VolumeQuality::High,
                3.0, 3.0, 2.5, true)
            && areSamplesValid;

        const auto runStylePhase = [&]() {
            setBaseState();
            params.volumeQuality = VolumeQuality::Low;
            const std::thread::id qualityThread =
                std::this_thread::get_id();
            strategy.SetVisualState(
                params, UpdateFlags::Quality);
            if (!setVolumeMode()) {
                return false;
            }
            endpoint->renderWindow->Render();
            const double styleStillRay =
                mapper->GetSampleDistance();

            const auto getVisualPixels = [&]() {
                endpoint->renderWindow->Render();
                endpoint->renderWindow->WaitForCompletion();
                vtkNew<vtkWindowToImageFilter> capture;
                capture->SetInput(endpoint->renderWindow);
                capture->SetInputBufferTypeToRGB();
                capture->ReadFrontBufferOff();
                capture->ShouldRerenderOff();
                capture->Update();
                auto* output = capture->GetOutput();
                auto* pixels = output
                    ? static_cast<unsigned char*>(
                        output->GetScalarPointer())
                    : nullptr;
                const vtkIdType valueCount = output
                    ? output->GetNumberOfPoints()
                        * output->GetNumberOfScalarComponents()
                    : 0;
                return pixels && valueCount > 0
                    ? std::vector<unsigned char>(
                        pixels, pixels + valueCount)
                    : std::vector<unsigned char>{};
            };
            const int volumeVisibility = volume->GetVisibility();
            volume->VisibilityOff();
            const auto backgroundPixels = getVisualPixels();
            volume->SetVisibility(volumeVisibility);
            const auto beforePixels = getVisualPixels();
            const auto getVolumeVisible = [&](
                const std::vector<unsigned char>& pixels) {
                if (pixels.size() != backgroundPixels.size()
                    || pixels.size() < 3
                    || pixels.size() % 3 != 0) {
                    return false;
                }
                constexpr int signalThreshold = 8;
                std::size_t signalPixelCount = 0;
                for (std::size_t index = 0;
                    index < pixels.size(); index += 3) {
                    int pixelDifference = 0;
                    for (std::size_t component = 0;
                        component < 3; ++component) {
                        pixelDifference = std::max(
                            pixelDifference,
                            std::abs(
                                static_cast<int>(
                                    pixels[index + component])
                                - static_cast<int>(
                                    backgroundPixels[
                                        index + component])));
                    }
                    if (pixelDifference > signalThreshold) {
                        ++signalPixelCount;
                    }
                }
                const std::size_t pixelCount = pixels.size() / 3;
                // 至少 0.5% 像素必须来自体数据，排除纯背景和单点噪声。
                return signalPixelCount * 200 > pixelCount;
            };
            const bool isBeforeVisible =
                getVolumeVisible(beforePixels);
            const std::size_t vtkErrorCount = m_vtkErrorCount;
            auto* style = vtkInteractorStyle::SafeDownCast(
                endpoint->interactor->GetInteractorStyle());
            if (!style || !ResetRenderStats()) {
                return false;
            }
            PhaseThreadProbe threadProbe;
            auto threadCallback =
                vtkSmartPointer<vtkCallbackCommand>::New();
            threadCallback->SetClientData(&threadProbe);
            threadCallback->SetCallback(
                &PhaseThreadProbe::OnEvent);
            const unsigned long startTag = style->AddObserver(
                vtkCommand::StartInteractionEvent,
                threadCallback, -1.0);
            const unsigned long moveTag = style->AddObserver(
                vtkCommand::InteractionEvent,
                threadCallback, -1.0);
            const unsigned long endTag = style->AddObserver(
                vtkCommand::EndInteractionEvent,
                threadCallback, -1.0);
            const unsigned long timerTag =
                endpoint->interactor->AddObserver(
                    vtkCommand::TimerEvent,
                    threadCallback, -1.0);
            const auto removeThreadProbe = [&]() {
                style->RemoveObserver(startTag);
                style->RemoveObserver(moveTag);
                style->RemoveObserver(endTag);
                endpoint->interactor->RemoveObserver(
                    timerTag);
                threadCallback->SetClientData(nullptr);
            };

            endpoint->interactor->SetEventPosition(320, 240);
            endpoint->interactor->InvokeEvent(
                vtkCommand::RightButtonPressEvent);
            endpoint->interactor->SetEventPosition(321, 241);
            endpoint->interactor->InvokeEvent(
                vtkCommand::MouseMoveEvent);
            const std::thread::id firstRenderThread =
                m_lastRenderThread;
            const bool isFirstFramePreview =
                std::abs(
                    mapper->GetImageSampleDistance() - 2.0)
                    < 1e-12
                && std::abs(
                    mapper->GetSampleDistance()
                        - styleStillRay * 2.0)
                    < 1e-12
                && mapper->GetUseJittering() == 0;
            const auto duringPixels = getVisualPixels();
            const bool isDuringVisible =
                getVolumeVisible(duringPixels);

            // Start callback 只镜像 rate；默认 style 的首帧 Render 必须已消费
            // preview。Timer 随后继续以共享 source 为权威状态。
            (void)SendTimer(endpoint->interactor);
            const std::thread::id timerRenderThread =
                m_lastRenderThread;
            const bool isTimerPreview =
                std::abs(
                    mapper->GetImageSampleDistance() - 2.0)
                    < 1e-12
                && std::abs(
                    mapper->GetSampleDistance()
                        - styleStillRay * 2.0)
                    < 1e-12
                && mapper->GetUseJittering() == 0;
            bool areStyleSamplesValid = ResetRenderStats();

            for (int index = 0;
                areStyleSamplesValid && index < sampleCount;
                ++index) {
                endpoint->interactor->SetEventPosition(
                    322 + index % 7,
                    242 + index % 5);
                endpoint->interactor->InvokeEvent(
                    vtkCommand::MouseMoveEvent);
                if (m_renderTimesMs.size()
                    != static_cast<std::size_t>(index + 1)) {
                    areStyleSamplesValid = false;
                }
            }
            const bool isStyleSampleValid =
                areStyleSamplesValid
                && m_renderTimesMs.size()
                    == static_cast<std::size_t>(sampleCount)
                && m_renderStartCount == m_renderEndCount
                && m_renderStarts.empty()
                && std::abs(
                    mapper->GetImageSampleDistance() - 2.0)
                    < 1e-12
                && std::abs(
                    mapper->GetSampleDistance()
                        - styleStillRay * 2.0)
                    < 1e-12
                && mapper->GetUseJittering() == 0;
            const double p50Ms = GetRenderTimeMs(0.50);
            const double p95Ms = GetRenderTimeMs(0.95);
            const double maxMs = GetRenderTimeMs(1.00);

            endpoint->interactor->InvokeEvent(
                vtkCommand::RightButtonReleaseEvent);
            (void)SendTimer(endpoint->interactor);
            const bool isStyleRestored =
                std::abs(
                    mapper->GetImageSampleDistance() - 1.0)
                    < 1e-12
                && std::abs(
                    mapper->GetSampleDistance() - styleStillRay)
                    < 1e-12
                && mapper->GetUseJittering() != 0;
            const auto afterPixels = getVisualPixels();
            const bool isAfterVisible =
                getVolumeVisible(afterPixels);
            const bool isVisualValid = isBeforeVisible
                && isDuringVisible
                && isAfterVisible
                && m_vtkErrorCount == vtkErrorCount;
            const bool isThreadValid =
                threadProbe.startCount == 1
                && threadProbe.moveCount
                    >= static_cast<std::size_t>(sampleCount)
                && threadProbe.endCount == 1
                && threadProbe.timerCount >= 1
                && threadProbe.styleThread == qualityThread
                && threadProbe.styleThread
                    == firstRenderThread
                && threadProbe.timerThread == qualityThread
                && threadProbe.timerThread
                    == timerRenderThread;
            const auto qualityThreadId =
                std::hash<std::thread::id>{}(qualityThread);
            const auto styleThreadId =
                std::hash<std::thread::id>{}(
                    threadProbe.styleThread);
            const auto timerThreadId =
                std::hash<std::thread::id>{}(
                    threadProbe.timerThread);
            const auto renderThreadId =
                std::hash<std::thread::id>{}(
                    timerRenderThread);
            removeThreadProbe();
            std::cout
                << "BENCH_PHASE: case=K_style_interaction"
                << " phase=during source=StylePhase"
                << " samples=" << sampleCount
                << " first_preview=" << isFirstFramePreview
                << " timer_preview=" << isTimerPreview
                << " p50_ms=" << p50Ms
                << " p95_ms=" << p95Ms
                << " max_ms=" << maxMs
                << " restored=" << isStyleRestored
                << " visible=" << isBeforeVisible << ','
                << isDuringVisible << ',' << isAfterVisible
                << " visual_ok=" << isVisualValid
                << " style_events="
                << threadProbe.startCount << ','
                << threadProbe.moveCount << ','
                << threadProbe.endCount
                << " timer_events="
                << threadProbe.timerCount
                << " quality_thread=" << qualityThreadId
                << " style_thread=" << styleThreadId
                << " timer_thread=" << timerThreadId
                << " render_thread=" << renderThreadId
                << " thread_ok=" << isThreadValid
                << '\n';
            return isFirstFramePreview
                && isTimerPreview
                && isStyleSampleValid
                && isStyleRestored
                && isVisualValid
                && isThreadValid;
        };
        areSamplesValid =
            runStylePhase() && areSamplesValid;
        for (const auto& hiddenProp : hiddenProps) {
            if (hiddenProp.first) {
                hiddenProp.first->SetVisibility(
                    hiddenProp.second);
            }
        }
        strategy.DetachRenderer(renderer);

        HostViewSetRequest sliceRequest;
        sliceRequest.targetView.viewId = "primary-3d";
        sliceRequest.mode = HostRenderMode::SliceTopDown;
        sliceRequest.windowLevel =
            HostWindowLevelParams{ 180.0, 90.0 };
        HostSessionSetRequest cursorRequest;
        cursorRequest.cursor = HostCursorParams{
            { 24.0, 25.0, 26.0 }, -1
        };
        const bool isSliceSent =
            m_session->SendRequest(std::move(cursorRequest))
            && m_session->SendRequest(std::move(sliceRequest));
            (void)SendTimer(endpoint->interactor);
        bool hasSliceProp = false;
        auto* sliceProps = renderer->GetViewProps();
        if (sliceProps) {
            sliceProps->InitTraversal();
            while (auto* prop = sliceProps->GetNextProp()) {
                hasSliceProp =
                    vtkImageSlice::SafeDownCast(prop)
                    || hasSliceProp;
            }
        }
        const bool isSliceStable =
            isSliceSent
            && hasSliceProp
            && std::abs(
                endpoint->renderWindow
                    ->GetDesiredUpdateRate()
                    - GetRenderRate(false)) < 1e-12;

        HostViewSetRequest isoRequest;
        isoRequest.targetView.viewId = "primary-3d";
        isoRequest.mode = HostRenderMode::IsoSurface;
        isoRequest.iso = 64.0;
        isoRequest.volumeQuality = HostVolumeQuality::High;
        const bool isIsoSent =
            m_session->SendRequest(
                std::move(isoRequest));
        (void)SendTimer(endpoint->interactor);
        HostViewTarget isoTarget;
        isoTarget.viewId = "primary-3d";
        const auto isoState =
            m_session->GetRenderViewState(isoTarget);
        bool hasIsoActor = false;
        auto* isoProps = renderer->GetViewProps();
        if (isoProps) {
            isoProps->InitTraversal();
            while (auto* prop = isoProps->GetNextProp()) {
                hasIsoActor =
                    vtkActor::SafeDownCast(prop)
                    || hasIsoActor;
            }
        }
        const bool isIsoStable =
            isIsoSent
            && isoState
            && isoState->viewMode == HostRenderMode::IsoSurface
            && isoState->volumeQuality == HostVolumeQuality::High
            && hasIsoActor
            && std::abs(
                endpoint->renderWindow
                    ->GetDesiredUpdateRate()
                    - GetRenderRate(false)) < 1e-12;
        HostDataExportRequest exportRequest;
        exportRequest.outputPath = "volume-preview-cross";
        exportRequest.format = HostDataExportFormat::Raw;
        const bool isExportStable =
            m_session->SendRequest(
                std::move(exportRequest))
            && std::abs(
                endpoint->renderWindow
                    ->GetDesiredUpdateRate()
                    - GetRenderRate(false)) < 1e-12;
        std::cout
            << "BENCH_CROSS: slice=" << isSliceStable
            << " iso=" << isIsoStable
            << " export=" << isExportStable
            << " stale_preview=0\n";
        areSamplesValid =
            isSliceStable && isIsoStable && isExportStable
            && areSamplesValid;

        HostViewSetRequest restoreRequest;
        restoreRequest.targetView.viewId = "primary-3d";
        restoreRequest.mode =
            HostRenderMode::CompositeIsoSurface;
        restoreRequest.volumeQuality = HostVolumeQuality::Ultra;
        const bool isRestoreSent =
            m_session->SendRequest(
                std::move(restoreRequest));
        (void)SendTimer(endpoint->interactor);
        const auto restoreState =
            m_session->GetRenderViewState(isoTarget);
        areSamplesValid =
            isRestoreSent
            && restoreState
            && restoreState->viewMode
                == HostRenderMode::CompositeIsoSurface
            && restoreState->volumeQuality
                == HostVolumeQuality::Ultra
            && std::abs(
                endpoint->renderWindow->GetDesiredUpdateRate()
                    - 0.001) < 1e-12
            && areSamplesValid;
        return areSamplesValid;
    }

    QVTKOpenGLNativeWidget* GetWidget() const
    {
        return m_widget.get();
    }

private:
    bool AttachRenderStats()
    {
        if (!m_renderWindow || m_renderTimingCallback
            || m_renderStartTag != 0 || m_renderEndTag != 0) {
            return false;
        }
        m_renderTimingCallback =
            vtkSmartPointer<vtkCallbackCommand>::New();
        m_renderTimingCallback->SetClientData(this);
        m_renderTimingCallback->SetCallback(
            &SessionFixture::OnRenderTiming);
        m_renderStartTag = m_renderWindow->AddObserver(
            vtkCommand::StartEvent, m_renderTimingCallback);
        m_renderEndTag = m_renderWindow->AddObserver(
            vtkCommand::EndEvent, m_renderTimingCallback);
        if (m_renderStartTag != 0 && m_renderEndTag != 0) {
            return true;
        }
        DetachRenderStats();
        return false;
    }

    bool DetachRenderStats()
    {
        if (m_renderWindow && m_renderStartTag != 0) {
            m_renderWindow->RemoveObserver(m_renderStartTag);
        }
        if (m_renderWindow && m_renderEndTag != 0) {
            m_renderWindow->RemoveObserver(m_renderEndTag);
        }
        m_renderStartTag = 0;
        m_renderEndTag = 0;
        if (m_renderTimingCallback) {
            m_renderTimingCallback->SetClientData(nullptr);
        }
        m_renderTimingCallback = nullptr;
        m_renderStarts.clear();
        return true;
    }

    bool ResetRenderStats()
    {
        if (!m_renderStarts.empty()) {
            return false;
        }
        m_renderStartCount = 0;
        m_renderEndCount = 0;
        m_renderTimesMs.clear();
        return true;
    }

    double GetRenderTimeMs(double percentile) const
    {
        if (m_renderTimesMs.empty()) {
            return 0.0;
        }
        auto values = m_renderTimesMs;
        std::sort(values.begin(), values.end());
        const auto rank = static_cast<std::size_t>(
            std::ceil(percentile * static_cast<double>(values.size())));
        const std::size_t index = std::min(
            values.size() - 1,
            rank > 0 ? rank - 1 : std::size_t{0});
        return values[index];
    }

    static void OnRenderTiming(
        vtkObject* caller,
        unsigned long eventId,
        void* clientData,
        void*)
    {
        auto* fixture = static_cast<SessionFixture*>(clientData);
        if (!fixture) {
            return;
        }
        if (eventId == vtkCommand::StartEvent) {
            fixture->m_lastRenderThread =
                std::this_thread::get_id();
            ++fixture->m_renderStartCount;
            fixture->m_renderStarts.push_back(
                std::chrono::steady_clock::now());
            return;
        }
        if (eventId != vtkCommand::EndEvent) {
            return;
        }

        ++fixture->m_renderEndCount;
        if (fixture->m_renderStarts.empty()) {
            return;
        }
        auto* renderWindow = vtkRenderWindow::SafeDownCast(caller);
        if (renderWindow) {
            // Render EndEvent 可能只表示 OpenGL 命令已提交。等待 GPU 完成，
            // 才能把 ray/image 采样量变化归因到本帧耗时。
            renderWindow->WaitForCompletion();
        }
        const auto startTime = fixture->m_renderStarts.back();
        fixture->m_renderStarts.pop_back();
        const auto duration = std::chrono::steady_clock::now()
            - startTime;
        fixture->m_renderTimesMs.push_back(
            std::chrono::duration<double, std::milli>(
                duration).count());
    }

    static void OnVtkError(
        vtkObject*, unsigned long, void* clientData, void* callData)
    {
        auto* fixture = static_cast<SessionFixture*>(clientData);
        if (!fixture) {
            return;
        }
        ++fixture->m_vtkErrorCount;
        if (callData) {
            fixture->m_vtkErrorText = static_cast<const char*>(callData);
        }
    }

    std::unique_ptr<QVTKOpenGLNativeWidget> m_widget;
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> m_renderWindow;
    vtkSmartPointer<vtkCallbackCommand> m_renderTimingCallback;
    vtkSmartPointer<vtkCallbackCommand> m_errorCallback;
    std::unique_ptr<VtkAppHostSession> m_session;
    std::vector<std::chrono::steady_clock::time_point> m_renderStarts;
    std::vector<double> m_renderTimesMs;
    unsigned long m_renderStartTag{0};
    unsigned long m_renderEndTag{0};
    unsigned long m_errorTag{0};
    std::size_t m_renderStartCount{0};
    std::size_t m_renderEndCount{0};
    std::size_t m_vtkErrorCount{0};
    std::thread::id m_lastRenderThread;
    std::string m_vtkErrorText;
};

} // namespace

int main(int argc, char* argv[])
{
    // QVTK surface 格式必须早于 QApplication，和接入指南的构建链保持一致。
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);

    if (QCoreApplication::arguments().contains(
            QStringLiteral("--style-quality-only"))) {
        const bool isStyleQualityValid =
            BuildStyleQualityTest()
            && BuildSceneCameraTest()
            && BuildStyleDestroyTest()
            && BuildLeaseRetryTest();
        std::cout
            << (isStyleQualityValid
                ? "PASS: Style interaction quality lifecycle\n"
                : "FAIL: Style interaction quality lifecycle\n");
        return isStyleQualityValid ? 0 : 7;
    }

    if (!BuildTransferReturnTest()) {
        std::cerr
            << "FAIL: Complete transfer state was not propagated\n";
        return 3;
    }
    if (!BuildRenderSourceTest()) {
        std::cerr
            << "FAIL: Render sources were not isolated\n";
        return 4;
    }
    if (!BuildStyleQualityTest()) {
        std::cerr
            << "FAIL: Style interaction quality lifecycle was not isolated\n";
        return 7;
    }
    if (!BuildSceneCameraTest()) {
        std::cerr << "FAIL: Scene Camera snapshot or rebind changed\n";
        return 10;
    }
    if (!BuildStyleDestroyTest()) {
        std::cerr
            << "FAIL: Destroyed style retained a live context callback\n";
        return 8;
    }
    if (!BuildLeaseRetryTest()) {
        std::cerr
            << "FAIL: Failed input stop released the Host runtime\n";
        return 9;
    }
    if (!BuildDefaultRenderTest()) {
        std::cerr << "FAIL: BuildSession unexpectedly rendered the Qt-owned window\n";
        return 5;
    }
    if (!BuildZoomRenderTest()) {
        std::cerr << "FAIL: Viewer2D zoom did not converge through one Timer render\n";
        return 6;
    }

    SessionFixture smoke;
    if (!smoke.BuildWindow()) {
        std::cerr << "FAIL: Qt Host window build\n";
        return 1;
    }

    const bool isInteractive =
        QCoreApplication::arguments().contains(QStringLiteral("--interactive"));
    const bool isVolumeBench =
        QCoreApplication::arguments().contains(
            QStringLiteral("--volume-benchmark"));
    int result{2};
    bool hasStarted{false};
    QObject::connect(smoke.GetWidget(), &QOpenGLWidget::frameSwapped, &app, [&]() {
        if (hasStarted) {
            return;
        }
        hasStarted = true;
        const bool isHostPassed = smoke.StartHost();
        const bool isBlendPassed =
            isHostPassed && smoke.GetBlendVisualValid();
        const bool isBenchPassed =
            !isVolumeBench
            || (isHostPassed && smoke.StartVolumeBench(120));
        const bool isPassed =
            isHostPassed && isBlendPassed && isBenchPassed;
        result = isPassed ? 0 : 3;
        if (!isHostPassed) {
            std::cerr << "FAIL: Qt Host endpoint setup\n";
        }
        else if (!isBlendPassed) {
            std::cerr
                << "FAIL: Composite visual baseline changed\n";
        }
        else if (!isBenchPassed) {
            std::cerr << "FAIL: Volume benchmark sample collection\n";
        }
        std::cout << (isPassed
            ? "PASS: Qt Host session endpoint binding\n"
            : "FAIL: Qt Host session endpoint binding\n");

        if (!isInteractive || !isPassed) {
            QTimer::singleShot(0, &app, [&]() {
                smoke.StopHost();
                QApplication::exit(result);
            });
        }
    });
    QTimer::singleShot(5000, &app, [&]() {
        if (!hasStarted) {
            std::cerr << "FAIL: QVTK first frame timeout\n";
            result = 4;
            smoke.StopHost();
            QApplication::exit(result);
        }
    });

    const int appResult = app.exec();
    smoke.StopHost();
    return appResult;
}
