#include "QtHostMethodCases.h"

#include "AppState.h"
#include "AppStateEvents.h"
#include "DataManager.h"
#include "Host/CropHostFeature.h"
#include "Host/GapHostFeature.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeature.h"
#include "Host/VtkAppHostSession.h"
#include "VolumeTypes.h"

#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

class ContextProbeFeature final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override
    {
        return "crop.context.probe";
    }

    bool AttachHost(
        const HostFeatureContext& context) override
    {
        if (!context.getImageSnapshot
            || !context.setImageState) {
            return false;
        }
        m_getImageSnapshot = context.getImageSnapshot;
        m_setImageState = context.setImageState;
        return true;
    }

    bool DetachHost() override
    {
        m_getImageSnapshot = {};
        m_setImageState = {};
        return true;
    }

    bool OnHostTick() override
    {
        return true;
    }

    std::function<ImageSnapshot()> m_getImageSnapshot;
    std::function<bool(
        ImageState,
        const ImageSnapshot&,
        ImageSnapshot&)> m_setImageState;
};

class ThrowingStateSink final : public IStateEventSink {
public:
    void SendFlags(UpdateFlags) override
    {
        throw 1;
    }
};

ImageState GetStateCopy(
    const ImageSnapshot& snapshot)
{
    ImageState state = snapshot
        ? *snapshot : ImageState{};
    if (state.image) {
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->DeepCopy(state.image);
        state.image = std::move(image);
    }
    if (state.validityMask) {
        auto mask = vtkSmartPointer<vtkImageData>::New();
        mask->DeepCopy(state.validityMask);
        state.validityMask = std::move(mask);
    }
    return state;
}

bool GetCoreWriterContract()
{
    std::function<ImageSnapshot()> reader;
    std::function<bool(
        ImageState,
        const ImageSnapshot&,
        ImageSnapshot&)> writer;
    ImageSnapshot retainedSnapshot;
    bool isWriterValid = false;
    {
        HostCoreServices core;
        core.sharedDataMgr =
            std::make_shared<RawVolumeDataManager>();
        core.sharedState =
            std::make_shared<SharedInteractionState>(
                std::make_shared<ThrowingStateSink>());

        const auto layout = VolumeLayout::Create(
            { 2, 2, 2 },
            { 1.0f, 1.0f, 1.0f },
            { 0.0f, 0.0f, 0.0f });
        const auto buffer = layout
            ? VolumeBuffer::Create(
                std::vector<float>(8, 1.0f),
                *layout)
            : std::nullopt;
        bool hasPending = false;
        if (!buffer
            || !core.sharedDataMgr->SetFromBuffer(*buffer)
            || !core.sharedDataMgr->SetCurrentFromPending(
                hasPending)
            || !hasPending) {
            return false;
        }

        reader = core.GetImageReader();
        writer = core.GetImageWriter();
        const auto expectedSnapshot = reader();
        ImageSnapshot publishedSnapshot;
        isWriterValid = writer(
            GetStateCopy(expectedSnapshot),
            expectedSnapshot,
            publishedSnapshot);
        ImageSnapshot stalePublished = publishedSnapshot;
        const bool isStaleRejected = !writer(
            GetStateCopy(expectedSnapshot),
            expectedSnapshot,
            stalePublished);
        retainedSnapshot = publishedSnapshot;
        isWriterValid = isWriterValid
            && isStaleRejected
            && !stalePublished
            && publishedSnapshot
            && publishedSnapshot->version
                == expectedSnapshot->version + 1
            && reader() == publishedSnapshot;
    }

    ImageSnapshot expiredPublished = retainedSnapshot;
    const bool isExpiredRejected = !writer(
        GetStateCopy(retainedSnapshot),
        retainedSnapshot,
        expiredPublished);
    return isWriterValid
        && !reader()
        && isExpiredRejected
        && !expiredPublished;
}

HostSessionConfig GetCropSessionConfig()
{
    HostRenderViewConfig view;
    view.id = "crop-primary";
    view.role = HostRenderViewRole::Primary3D;
    view.window.viewInit.viewMode =
        HostRenderMode::CompositeIsoSurface;
    view.window.viewInit.hasIso = true;
    view.window.viewInit.isoThreshold = 0.5;
    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    HostRenderViewConfig timerView;
    timerView.id = "crop-timer";
    timerView.role = HostRenderViewRole::Auxiliary;
    config.renderViews.push_back(std::move(timerView));
    return config;
}

CropHostConfig GetCropConfig()
{
    CropHostConfig config;
    config.defaultTarget.referenceView = {
        "crop-primary", false,
        HostRenderViewRole::Primary3D };
    config.defaultTarget.targetViews.viewIds = {
        "crop-primary" };
    config.defaultTarget.isTargetViewsUsed = true;
    config.defaultTarget.isStatusVisible = true;
    config.inputViews.viewIds = { "crop-primary" };
    config.keys.restoreOriginal.keyCode = '6';
    return config;
}

CropHostTarget GetCropTarget()
{
    return GetCropConfig().defaultTarget;
}

GapHostConfig GetGapConfig()
{
    GapHostConfig config;
    config.defaultStart.targetViews.viewIds = {
        "crop-primary" };
    config.defaultStart.surface.isoMode =
        GapIsoMode::DataRangeRatio;
    config.defaultStart.surface.dataRangeRatio = 0.5;
    config.defaultStart.voidParams.grayMin = 0.0f;
    config.defaultStart.voidParams.grayMax = 0.25f;
    config.defaultStart.voidParams.minVolumeMM3 = 0.0;
    config.defaultStart.voidParams.angleThresholdDeg = 40.0f;
    config.defaultStart.voidParams.tensorWindowSize = 1;
    config.defaultStart.voidParams.erosionIterations = 0;
    config.inputViews.viewIds = { "crop-primary" };
    config.keys.switchOverlay.keyCode = 'j';
    config.keys.exit.keySym = "Escape";
    return config;
}

bool SendReload(
    VtkAppHostSession& session,
    bool& isComplete,
    bool& isSucceeded)
{
    HostReloadRequest reload;
    reload.voxels.resize(4 * 4 * 4);
    for (std::size_t index = 0;
        index < reload.voxels.size(); ++index) {
        reload.voxels[index] =
            static_cast<float>(index % 3) * 0.5f;
    }
    reload.geometry.dimensions = { 4, 4, 4 };
    reload.geometry.spacing = { 1.0f, 1.0f, 1.0f };
    reload.geometry.origin = { 0.0f, 0.0f, 0.0f };
    HostDataRequest request;
    request.action = HostDataAction::ReloadBuffer;
    request.payload = std::move(reload);
    return session.SendData(
        std::move(request),
        [&isComplete, &isSucceeded](const bool value) {
            isSucceeded = value;
            isComplete = true;
        });
}

void SendTicks(
    const HostRenderViewEndpoint& endpoint,
    const int tickCount)
{
    for (int tick = 0; tick < tickCount; ++tick) {
        endpoint.interactor->InvokeEvent(vtkCommand::TimerEvent);
        endpoint.renderWindow->Render();
    }
}

void SendHostTick(
    const HostRenderViewEndpoint& primary,
    const HostRenderViewEndpoint& timer)
{
    primary.renderWindow->Render();
    timer.interactor->InvokeEvent(vtkCommand::TimerEvent);
}

bool SendWidgetInput(
    const HostRenderViewEndpoint& endpoint,
    const std::array<double, 3>& worldPoint)
{
    if (!endpoint.renderer
        || !endpoint.interactor) {
        return false;
    }
    endpoint.renderer->SetWorldPoint(
        worldPoint[0], worldPoint[1],
        worldPoint[2], 1.0);
    endpoint.renderer->WorldToDisplay();
    const auto* displayPoint =
        endpoint.renderer->GetDisplayPoint();
    const int x = static_cast<int>(
        displayPoint[0]);
    const int y = static_cast<int>(
        displayPoint[1]);
    endpoint.interactor->SetEventPosition(x, y);
    endpoint.interactor->InvokeEvent(
        vtkCommand::LeftButtonPressEvent);
    endpoint.interactor->SetEventPosition(x + 8, y);
    endpoint.interactor->InvokeEvent(
        vtkCommand::MouseMoveEvent);
    endpoint.interactor->InvokeEvent(
        vtkCommand::LeftButtonReleaseEvent);
    return true;
}

bool SendCropInput(
    CropHostFeature& feature,
    const HostRenderViewEndpoint& primary,
    const HostRenderViewEndpoint& timer,
    const double* imageBounds)
{
    const std::array<std::array<double, 3>, 10> points = {
        std::array<double, 3>{ 3.0, 1.5, 1.5 },
        std::array<double, 3>{ 0.0, 1.5, 1.5 },
        std::array<double, 3>{ 1.5, 3.0, 1.5 },
        std::array<double, 3>{ 1.5, 0.0, 1.5 },
        std::array<double, 3>{ 1.5, 1.5, 3.0 },
        std::array<double, 3>{ 1.5, 1.5, 0.0 },
        std::array<double, 3>{ 3.0, 3.0, 3.0 },
        std::array<double, 3>{ 0.0, 0.0, 0.0 },
        std::array<double, 3>{ 3.0, 0.0, 3.0 },
        std::array<double, 3>{ 0.0, 3.0, 0.0 }
    };
    for (const auto& point : points) {
        primary.renderer->ResetCamera(imageBounds);
        primary.renderWindow->Render();
        if (!SendWidgetInput(primary, point)) {
            return false;
        }
        SendHostTick(primary, timer);
        if (feature.GetState().history.nodeCount != 0) {
            return true;
        }
    }
    return false;
}

}

int GetCropFailCount()
{
    int failureCount = 0;
    VtkAppHostSession session(GetCropSessionConfig());
    auto feature = std::make_shared<CropHostFeature>(
        GetCropConfig());
    const auto initialState = feature->GetState();
    auto contextProbe =
        std::make_shared<ContextProbeFeature>();
    const bool isBuilt = session.BuildSession();
    const bool isAttached =
        session.AttachFeature(feature);
    const bool isProbeAttached =
        session.AttachFeature(contextProbe);
    const bool isInputAttached =
        session.AttachHotkeys({}, {});
    const auto* endpoint = session.GetPrimaryEndpoint();
    const auto* timerEndpoint =
        session.GetRenderViewEndpoint("crop-timer");
    if (!isBuilt || !isAttached || !isProbeAttached
        || !isInputAttached
        || !endpoint
        || !timerEndpoint
        || !endpoint->interactor
        || !endpoint->renderWindow
        || !endpoint->renderer
        || !timerEndpoint->interactor
        || !timerEndpoint->renderWindow) {
        GetCaseResult(false,
            "Crop fixture builds the public Session/Feature chain");
        return 1;
    }
    endpoint->renderWindow->SetOffScreenRendering(1);
    endpoint->renderWindow->SetSize(200, 200);

    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = {
        "crop-timer", false,
        HostRenderViewRole::Auxiliary };
    const bool isTimerAttached = session.AttachTimer(timer);
    bool isReloadComplete = false;
    bool isReloadSucceeded = false;
    const bool isReloadSent = SendReload(
        session, isReloadComplete, isReloadSucceeded);
    for (int poll = 0;
        isReloadSent && !isReloadComplete && poll < 500;
        ++poll) {
        SendTicks(*endpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    failureCount += GetCaseResult(
        !initialState.isActive
            && !initialState.isPublishing
            && initialState.history.operationCount == 0
            && isTimerAttached
            && isReloadSent
            && isReloadComplete
            && isReloadSucceeded,
        "Crop fixture publishes an image through Session data API") ? 0 : 1;

    const auto expectedSnapshot =
        contextProbe->m_getImageSnapshot();
    ImageSnapshot publishedSnapshot;
    const bool isPublished =
        contextProbe->m_setImageState(
            GetStateCopy(expectedSnapshot),
            expectedSnapshot,
            publishedSnapshot);
    ImageSnapshot stalePublished = publishedSnapshot;
    const bool isStaleRejected =
        !contextProbe->m_setImageState(
            GetStateCopy(expectedSnapshot),
            expectedSnapshot,
            stalePublished);
    failureCount += GetCaseResult(
        isPublished
            && isStaleRejected
            && !stalePublished
            && publishedSnapshot
            && publishedSnapshot->version
                == expectedSnapshot->version + 1
            && contextProbe->m_getImageSnapshot()
                == publishedSnapshot,
        "Feature context writer enforces snapshot identity and version CAS") ? 0 : 1;
    failureCount += GetCaseResult(
        GetCoreWriterContract(),
        "Core writer publishes before notification and weak closures expire safely") ? 0 : 1;
    // 让主视图消费 probe 发布的新 snapshot；主视图不承载 Host timer，
    // 因此不会提前推进后续 Feature worker。
    SendTicks(*endpoint, 2);

    endpoint->interactor->SetKeyEventInformation(
        0, 0, '6', 0, "6");
    const bool isResetKeyHandled =
        endpoint->interactor->InvokeEvent(
            vtkCommand::KeyPressEvent) != 0;
    const bool isResetReleaseHandled =
        endpoint->interactor->InvokeEvent(
            vtkCommand::KeyReleaseEvent) != 0;
    failureCount += GetCaseResult(
        isResetKeyHandled
            && isResetReleaseHandled,
        "6 routes RestoreOriginal without a compatibility shortcut") ? 0 : 1;

    const auto target = GetCropTarget();
    const bool isStrict = !feature->SendRequest({
            CropHostAction::None, std::monostate{} })
        && !feature->SendRequest({
            CropHostAction::Start,
            CropHostNodeRequest{ 0 } })
        && !feature->SendRequest({
            CropHostAction::Previous,
            std::monostate{} },
            [](CropExportResult) {})
        && !feature->SendRequest({
            CropHostAction::Export, target });
    const bool isInactiveHistoryRejected =
        !feature->SendRequest({
            CropHostAction::Previous, std::monostate{} })
        && !feature->SendRequest({
            CropHostAction::Next, std::monostate{} })
        && !feature->SendRequest({
            CropHostAction::Node,
            CropHostNodeRequest{ 0 } });
    failureCount += GetCaseResult(
        isStrict && isInactiveHistoryRejected,
        "Crop request matrix rejects None, payload mismatch and illegal callbacks") ? 0 : 1;

    const bool isStarted = feature->SendRequest({
        CropHostAction::Start, target });
    const auto startedState = feature->GetState();
    auto defaultOnlyTarget = target;
    defaultOnlyTarget.isTargetViewsUsed = false;
    defaultOnlyTarget.targetViews.viewIds = {
        "ignored-missing-view" };
    const bool isDefaultOnlyStarted =
        feature->SendRequest({
            CropHostAction::Start, defaultOnlyTarget });
    auto emptyExplicitTarget = target;
    emptyExplicitTarget.targetViews = {};
    const bool isEmptyExplicitRejected =
        !feature->SendRequest({
            CropHostAction::Start, emptyExplicitTarget });
    auto unknownExplicitTarget = target;
    unknownExplicitTarget.targetViews.viewIds = {
        "crop-primary", "missing-view" };
    const bool isUnknownExplicitRejected =
        !feature->SendRequest({
            CropHostAction::Start, unknownExplicitTarget });
    const auto preservedState = feature->GetState();
    const bool isModeSet = feature->SendRequest({
        CropHostAction::Mode,
        CropHostModeRequest{
            target, CropRemovalMode::RemoveInside } });
    const bool isBoxSet = feature->SendRequest({
        CropHostAction::Box, target });
    const double imageBounds[6] = {
        0.0, 3.0, 0.0, 3.0, 0.0, 3.0 };
    endpoint->renderer->ResetCamera(imageBounds);
    endpoint->renderWindow->Render();
    failureCount += GetCaseResult(
        isStarted
            && startedState.isActive
            && !startedState.isPublishing
            && isDefaultOnlyStarted
            && isEmptyExplicitRejected
            && isUnknownExplicitRejected
            && preservedState.isActive
            && preservedState.history.nodeCount
                == startedState.history.nodeCount
            && preservedState.history.operationCount
                == startedState.history.operationCount
            && isModeSet
            && isBoxSet,
        "Start, Mode and Box flow through CropHostFeature::SendRequest") ? 0 : 1;

    const bool isPreviousRejected = !feature->SendRequest({
        CropHostAction::Previous, std::monostate{} });
    const bool isNextRejected = !feature->SendRequest({
        CropHostAction::Next, std::monostate{} });
    const bool isNode = feature->SendRequest({
        CropHostAction::Node,
        CropHostNodeRequest{ 0 } });
    const bool isRearmed = feature->SendRequest({
        CropHostAction::Mode,
        CropHostModeRequest{
            target, CropRemovalMode::RemoveInside } });
    const bool isBoxRearmed = feature->SendRequest({
        CropHostAction::Box, target });
    failureCount += GetCaseResult(
        isPreviousRejected
            && isNextRejected
            && isNode
            && isRearmed
            && isBoxRearmed,
        "History actions preserve strict state while Node can be re-armed") ? 0 : 1;

    // 集成场景只驱动 Crop widget；临时移除相机 style，避免其先消费
    // 合成窗口里的鼠标拖拽事件。
    endpoint->interactor->SetInteractorStyle(nullptr);
    const bool isWidgetSent = SendCropInput(
        *feature, *endpoint, *timerEndpoint,
        imageBounds);
    const auto committedState = feature->GetState();
    failureCount += GetCaseResult(
        isWidgetSent
            && committedState.history.nodeCount == 1
            && committedState.history.operationCount == 1
            && committedState.history.hasEditableOp,
        "Box interaction creates one committed Crop operation") ? 0 : 1;

    const bool isPrevious = feature->SendRequest({
        CropHostAction::Previous, std::monostate{} });
    SendHostTick(*endpoint, *timerEndpoint);
    const auto previousState = feature->GetState();
    const bool isNext = feature->SendRequest({
        CropHostAction::Next, std::monostate{} });
    SendHostTick(*endpoint, *timerEndpoint);
    const auto nextState = feature->GetState();
    failureCount += GetCaseResult(
        isPrevious
            && previousState.history.nodeCount == 0
            && isNext
            && nextState.history.nodeCount == 1,
        "Previous and Next move the committed Crop history in both directions") ? 0 : 1;

    const bool isHistoryExited = feature->SendRequest({
        CropHostAction::Exit, std::monostate{} });
    const bool isPreviousAfterExit = feature->SendRequest({
        CropHostAction::Previous, std::monostate{} });
    SendHostTick(*endpoint, *timerEndpoint);
    const auto previousAfterExit = feature->GetState();
    const bool isNextAfterExit = feature->SendRequest({
        CropHostAction::Next, std::monostate{} });
    SendHostTick(*endpoint, *timerEndpoint);
    const auto nextAfterExit = feature->GetState();
    failureCount += GetCaseResult(
        isHistoryExited
            && !previousAfterExit.isActive
            && isPreviousAfterExit
            && previousAfterExit.history.nodeCount == 0
            && isNextAfterExit
            && nextAfterExit.history.nodeCount == 1,
        "Exit hides Crop widgets without locking committed history navigation") ? 0 : 1;

    const auto cropExpected =
        contextProbe->m_getImageSnapshot();
    int staleCompleteCount = 0;
    CropExportResult staleResult;
    const bool isStaleExported =
        feature->SendRequest({
                CropHostAction::Export, target },
            [&staleCompleteCount, &staleResult](
                CropExportResult result) {
                ++staleCompleteCount;
                staleResult = std::move(result);
            });
    const bool isPublishPreviousRejected =
        !feature->SendRequest({
            CropHostAction::Previous, std::monostate{} });
    const bool isPublishNextRejected =
        !feature->SendRequest({
            CropHostAction::Next, std::monostate{} });
    const bool isPublishNodeRejected =
        !feature->SendRequest({
            CropHostAction::Node,
            CropHostNodeRequest{ 0 } });
    bool isConflictReloadComplete = false;
    bool isConflictReloadSucceeded = false;
    const bool isConflictReloadSent = SendReload(
        session,
        isConflictReloadComplete,
        isConflictReloadSucceeded);
    for (int poll = 0;
        isConflictReloadSent
            && !isConflictReloadComplete
            && poll < 500;
        ++poll) {
        // 主视图没有 Session timer handler，因此这里只允许 AppService
        // 先提交 Reload，不消费 Crop worker。
        SendTicks(*endpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto reloadSnapshot =
        contextProbe->m_getImageSnapshot();
    for (int poll = 0;
        staleCompleteCount == 0
            && poll < 500;
        ++poll) {
        SendHostTick(*endpoint, *timerEndpoint);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto staleState = feature->GetState();
    const auto currentAfterReject =
        contextProbe->m_getImageSnapshot();
    failureCount += GetCaseResult(
        cropExpected
            && isStaleExported
            && isPublishPreviousRejected
            && isPublishNextRejected
            && isPublishNodeRejected
            && isConflictReloadSent
            && isConflictReloadComplete
            && isConflictReloadSucceeded
            && reloadSnapshot
            && reloadSnapshot->version
                == cropExpected->version + 1
            && staleCompleteCount == 1
            && !staleResult.isSucceeded
            && staleResult.failureReason
                == CropFailure::VersionMismatch
            && currentAfterReject == reloadSnapshot
            && staleState.history.nodeCount == 0
            && staleState.history.operationCount == 0
            && staleState.history.baseNodeCount == 0
            && !staleState.history.hasEditableOp,
        "Reload-first order rejects delayed Crop CAS without partial commit") ? 0 : 1;

    SendTicks(*endpoint, 2);
    const bool isRestarted = feature->SendRequest({
        CropHostAction::Start, target });
    const bool isRestartModeSet = feature->SendRequest({
        CropHostAction::Mode,
        CropHostModeRequest{
            target, CropRemovalMode::RemoveInside } });
    const bool isRestartBoxSet = feature->SendRequest({
        CropHostAction::Box, target });
    endpoint->renderer->ResetCamera(imageBounds);
    endpoint->renderWindow->Render();
    const bool isNextWidgetSent = SendCropInput(
        *feature, *endpoint, *timerEndpoint,
        imageBounds);

    auto gapFeature =
        std::make_shared<GapHostFeature>(
            GetGapConfig());
    const bool isGapAttached =
        session.AttachFeature(gapFeature);
    int staleGapCount = 0;
    bool isStaleGapSucceeded = false;
    const bool isStaleGapAccepted =
        gapFeature->SendRequest(
            { GapHostAction::Start,
                GetGapConfig().defaultStart },
            [&staleGapCount, &isStaleGapSucceeded](
                const bool isSuccess) {
                ++staleGapCount;
                isStaleGapSucceeded = isSuccess;
            });
    const auto publishExpected =
        contextProbe->m_getImageSnapshot();
    int publishCompleteCount = 0;
    CropExportResult publishResult;
    const bool isPublishExported =
        feature->SendRequest({
                CropHostAction::Export, target },
            [&publishCompleteCount, &publishResult](
                CropExportResult result) {
                ++publishCompleteCount;
                publishResult = std::move(result);
            });
    failureCount += GetCaseResult(
        isGapAttached && isStaleGapAccepted
            && isPublishExported,
        "Crop/Gap conflict requests are admitted") ? 0 : 1;
    for (int poll = 0;
        contextProbe->m_getImageSnapshot()
                == publishExpected
            && poll < 500;
        ++poll) {
        endpoint->renderWindow->Render();
        (void)feature->OnHostTick();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto cropSnapshot =
        contextProbe->m_getImageSnapshot();
    for (int poll = 0;
        gapFeature->GetState().analysisState
                != GapAnalysisState::Idle
            && poll < 10;
        ++poll) {
        (void)gapFeature->OnHostTick();
    }
    const auto staleGapState =
        gapFeature->GetState();
    const bool isStaleGapOverlayRejected =
        !gapFeature->SendRequest({
            GapHostAction::Overlay,
            std::monostate{} });
    failureCount += GetCaseResult(
        isGapAttached
            && isStaleGapAccepted
            && isPublishExported
            && publishExpected
            && cropSnapshot
            && cropSnapshot != publishExpected
            && cropSnapshot->version
                == publishExpected->version + 1
            && staleGapCount == 0
            && !isStaleGapSucceeded
            && staleGapState.analysisState
                == GapAnalysisState::Idle
            && staleGapState.statistics.objectVoxelCount == 0
            && staleGapState.statistics.voidVoxelCount == 0
            && !staleGapState.isViewActive
            && !staleGapState.isExitPending
            && isStaleGapOverlayRejected,
        "Crop publication retires pending Gap result before display or callback") ? 0 : 1;

    for (int poll = 0;
        publishCompleteCount == 0
            && poll < 500;
        ++poll) {
        // Gap 已按新 DataVersion 退休；此后才让主视图消费 Crop
        // 批次并由 Session 投递 Crop 成功 callback。
        SendTicks(*endpoint, 1);
        SendHostTick(*endpoint, *timerEndpoint);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const bool isGapDetached =
        session.DetachFeature(*gapFeature);
    bool isFinalReloadComplete = false;
    bool isFinalReloadSucceeded = false;
    const bool isFinalReloadSent = SendReload(
        session,
        isFinalReloadComplete,
        isFinalReloadSucceeded);
    for (int poll = 0;
        isFinalReloadSent
            && !isFinalReloadComplete
            && poll < 500;
        ++poll) {
        SendTicks(*endpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto finalSnapshot =
        contextProbe->m_getImageSnapshot();
    failureCount += GetCaseResult(
        isRestarted
            && isRestartModeSet
            && isRestartBoxSet
            && isNextWidgetSent
            && isGapAttached
            && isStaleGapAccepted
            && publishExpected
            && isPublishExported
            && publishCompleteCount == 1
            && publishResult.isSucceeded
            && cropSnapshot
            && cropSnapshot->version
                == publishExpected->version + 1
            && isFinalReloadSent
            && isFinalReloadComplete
            && isFinalReloadSucceeded
            && finalSnapshot
            && finalSnapshot->version
                == cropSnapshot->version + 1
            && finalSnapshot != cropSnapshot
            && isGapDetached
            && staleGapCount == 0,
        "Crop-first order publishes before a later legal Reload becomes current") ? 0 : 1;

    const bool isBox = feature->SendRequest({
        CropHostAction::Box, target });
    const bool isPlaneRestored = feature->SendRequest({
        CropHostAction::Plane, target });
    failureCount += GetCaseResult(
        isBox && isPlaneRestored,
        "Box and Plane remain available after version conflict coverage") ? 0 : 1;

    const bool isExited = feature->SendRequest({
        CropHostAction::Exit, std::monostate{} });
    const auto exitedState = feature->GetState();
    auto firstPolyData =
        vtkSmartPointer<vtkPolyData>::New();
    auto nextPolyData =
        vtkSmartPointer<vtkPolyData>::New();
    const bool hasPolyDataContract = feature->SendRequest({
            CropHostAction::SetPolyData,
            CropHostPolyDataRequest{
                firstPolyData, 1 } })
        && !feature->SendRequest({
            CropHostAction::SetPolyData,
            CropHostPolyDataRequest{
                firstPolyData, 2 } })
        && !feature->SendRequest({
            CropHostAction::SetPolyData,
            CropHostPolyDataRequest{
                nextPolyData, 1 } })
        && feature->SendRequest({
            CropHostAction::SetPolyData,
            CropHostPolyDataRequest{
                nextPolyData, 2 } })
        && feature->SendRequest({
            CropHostAction::ClearPolyData,
            std::monostate{} });
    failureCount += GetCaseResult(
        isExited
            && !exitedState.isActive
            && !exitedState.isPublishing
            && hasPolyDataContract,
        "Exit, SetPolyData and ClearPolyData remain atomic requests") ? 0 : 1;

    bool hasDetachedCallback = false;
    const bool isPendingAccepted = feature->SendRequest({
            CropHostAction::Export, target },
        [&hasDetachedCallback](CropExportResult) {
            hasDetachedCallback = true;
        });
    const auto useCount = feature.use_count();
    const bool isDetached =
        session.DetachFeature(*feature);
    const auto detachedState = feature->GetState();
    const bool isProbeDetached =
        session.DetachFeature(*contextProbe);
    SendHostTick(*endpoint, *timerEndpoint);
    failureCount += GetCaseResult(
        !isPendingAccepted
            && isDetached
            && isProbeDetached
            && feature.use_count() == useCount
            && !detachedState.isActive
            && !detachedState.isPublishing
            && detachedState.history.operationCount == 0
            && !hasDetachedCallback,
        "Crop detach leaves the upper owner alive and suppresses queued callbacks") ? 0 : 1;
    return failureCount;
}
