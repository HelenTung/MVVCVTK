#include "QtHostMethodCases.h"

#include "AppState.h"
#include "AppStateEvents.h"
#include "DataManager.h"
#include "Host/CropHostFeature.h"
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

bool SendReload(
    VtkAppHostSession& session,
    bool& isComplete,
    bool& isSucceeded)
{
    HostReloadRequest reload;
    reload.voxels.resize(3 * 4 * 2);
    for (std::size_t index = 0;
        index < reload.voxels.size(); ++index) {
        reload.voxels[index] =
            static_cast<float>(index % 3) * 0.5f;
    }
    reload.geometry.dimensions = { 3, 4, 2 };
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

}

int GetCropFailCount()
{
    int failureCount = 0;
    VtkAppHostSession session(GetCropSessionConfig());
    auto feature = std::make_shared<CropHostFeature>(
        GetCropConfig());
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
    if (!isBuilt || !isAttached || !isProbeAttached
        || !isInputAttached
        || !endpoint
        || !endpoint->interactor
        || !endpoint->renderWindow
        || !endpoint->renderer) {
        GetCaseResult(false,
            "Crop fixture builds the public Session/Feature chain");
        return 1;
    }
    endpoint->renderWindow->SetOffScreenRendering(1);
    endpoint->renderWindow->SetSize(200, 200);

    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = {
        "crop-primary", false,
        HostRenderViewRole::Primary3D };
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
        isTimerAttached
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
    failureCount += GetCaseResult(
        isStrict,
        "Crop request matrix rejects None, payload mismatch and illegal callbacks") ? 0 : 1;

    const bool isStarted = feature->SendRequest({
        CropHostAction::Start, target });
    const bool isModeSet = feature->SendRequest({
        CropHostAction::Mode,
        CropHostModeRequest{
            target, CropRemovalMode::RemoveInside } });
    const bool isBoxSet = feature->SendRequest({
        CropHostAction::Box, target });
    const double imageBounds[6] = {
        0.0, 2.0, 0.0, 3.0, 0.0, 1.0 };
    endpoint->renderer->ResetCamera(imageBounds);
    endpoint->renderWindow->Render();
    failureCount += GetCaseResult(
        isStarted && isModeSet && isBoxSet,
        "Start, Mode and Box flow through CropHostFeature::SendRequest") ? 0 : 1;

    const bool isPreviousRejected = !feature->SendRequest({
        CropHostAction::Previous, std::monostate{} });
    const bool isNextRejected = !feature->SendRequest({
        CropHostAction::Next, std::monostate{} });
    const bool isNode = feature->SendRequest({
        CropHostAction::Node,
        CropHostNodeRequest{ 0 } });
    const bool isPlane = feature->SendRequest({
        CropHostAction::Plane, target });
    failureCount += GetCaseResult(
        isPreviousRejected
            && isNextRejected
            && isNode
            && isPlane,
        "History actions preserve strict state while Node and Plane are accepted") ? 0 : 1;

    bool hasExportFailure = false;
    const bool isExported = feature->SendRequest({
            CropHostAction::Export, target },
        [&hasExportFailure](CropExportResult result) {
            hasExportFailure =
                result.failureReason == CropFailure::BadInput;
        });
    const bool wasExportDeferred = !hasExportFailure;
    SendTicks(*endpoint, 1);
    failureCount += GetCaseResult(
        !isExported
            && wasExportDeferred
            && hasExportFailure,
        "Export reports an empty committed prefix through the owner-thread queue") ? 0 : 1;

    const bool isExited = feature->SendRequest({
        CropHostAction::Exit, std::monostate{} });
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
        isExited && hasPolyDataContract,
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
    const bool isProbeDetached =
        session.DetachFeature(*contextProbe);
    SendTicks(*endpoint, 1);
    failureCount += GetCaseResult(
        !isPendingAccepted
            && isDetached
            && isProbeDetached
            && feature.use_count() == useCount
            && !hasDetachedCallback,
        "Crop detach leaves the upper owner alive and suppresses queued callbacks") ? 0 : 1;
    return failureCount;
}
