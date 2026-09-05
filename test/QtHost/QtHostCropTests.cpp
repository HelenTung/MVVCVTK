#include "QtHostMethodCases.h"
#include "../TestDataPort.h"

#include "AppState.h"
#include "AppStateEvents.h"
#include "App/Services/FeatureViewService.h"
#include "DataManager.h"
#include "Host/CropHostFeature.h"
#ifdef MVVCVTK_HAS_GAP_ANALYSIS
#include "Host/GapHostFeature.h"
#endif
#include "Host/HostCoreServices.h"
#include "Host/HostFeature.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/VtkAppHostSession.h"
#include "Render/Contracts/OverlayService.h"
#include "VolumeTypes.h"

#include <vtkCommand.h>
#include <vtkCubeSource.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <vtkVolume.h>
#include <vtkVolumeCollection.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view cropInputBinding =
    "feature.crop.input";

// 显式控制 Feature 完成阶段，保证异步 Reload 的 CAS 先于 Crop 提交。
class CropTickGate final : public HostFeature {
public:
    explicit CropTickGate(std::shared_ptr<CropHostFeature> feature)
        : m_feature(std::move(feature)) {}
    std::string_view GetFeatureId() const noexcept override
    { return m_feature->GetFeatureId(); }
    FeatureDataContract GetDataContract() const override
    { return m_feature->GetDataContract(); }
    bool AttachHost(const HostFeatureContext& context) override
    { return m_feature->AttachHost(context); }
    bool DetachHost() override { return m_feature->DetachHost(); }
    bool OnHostTick() override { return isPaused || m_feature->OnHostTick(); }
    bool isPaused = false;
private:
    std::shared_ptr<CropHostFeature> m_feature;
};

class ContextProbeFeature final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override
    {
        return "crop.context.probe";
    }

    bool AttachHost(
        const HostFeatureContext& context) override
    {
        if (!context.views || !context.data) {
            return false;
        }
        m_views = context.views;
        m_data = context.data;
        return true;
    }

    bool DetachHost() override
    {
        m_views.reset();
        m_data.reset();
        return true;
    }

    bool OnHostTick() override
    {
        return true;
    }

    std::shared_ptr<FeatureViewService> GetViewService(
        const std::string& viewId) const
    {
        return m_views && !viewId.empty()
            ? m_views->GetFeaturePort(viewId)
            : nullptr;
    }

    std::shared_ptr<OverlayService> GetViewOverlay(
        const std::string& viewId) const
    {
        return m_views && !viewId.empty()
            ? m_views->GetOverlayPort(viewId)
            : nullptr;
    }

    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
};

class HostControlProbe final : public FeatureHostControl {
public:
    bool AttachInput(HostInputBinding) override
    {
        return true;
    }

    bool DetachInput(std::string_view) override
    {
        return true;
    }

    bool SetActiveViews(
        const std::vector<std::string>&) override
    {
        return true;
    }

    bool SetViewStatus(
        const std::vector<std::string>&,
        const std::string&) override
    {
        return true;
    }

    bool SendSceneDelta(FeatureSceneDelta delta) override
    {
        if (delta.requestId == 0 || delta.viewIds.empty()) return false;
        m_sceneDeltas.push_back(std::move(delta));
        return true;
    }

    bool SendOwnerComplete(
        std::function<void()>) override
    {
        return true;
    }

private:
    std::vector<FeatureSceneDelta> m_sceneDeltas;
};

class ViewPortProbe final : public FeatureViewDirectory {
public:
    std::vector<HostFeatureView> GetViews(
        const HostViewTargets&) const override
    {
        return {};
    }

    std::shared_ptr<FeatureViewService> GetFeaturePort(
        const std::string&) const override
    {
        return {};
    }

    std::shared_ptr<OverlayService> GetOverlayPort(
        const std::string&) const override
    {
        return {};
    }

    std::optional<HostInputView> GetInputView(
        const HostViewTarget&) const override
    {
        return std::nullopt;
    }
};

using DataPortProbe = TestDataPort;

class UnknownCropPayload final : public IDataPayload {
public:
    DataTypeId GetDataType() const override
    {
        return Type;
    }

    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        return std::make_shared<const UnknownCropPayload>();
    }

    inline static const DataTypeId Type{
        "test.crop.unsupported", 1 };
};

class ThrowingStateSink final : public IStateEventSink {
public:
    void SendFlags(UpdateFlags) override
    {
        throw 1;
    }
};

bool GetCoreWriterContract()
{
    TestDataPort data;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 2, 2);
    image->AllocateScalars(VTK_FLOAT, 1);
    const auto first = data.SetPrimaryImage(image);
    if (!first || !first->binding) return false;
    const auto staleBinding = *first->binding;
    const auto second = data.SetPrimaryImage(image);
    if (!second || !second->binding) return false;
    DataTransaction stale;
    stale.bindings.push_back(DataBindingUpdate{
        std::string(primaryVolumeBinding),
        staleBinding.revision,
        true,
        staleBinding.target,
        first->data->self });
    const auto rejected = data.SetDataCommit(std::move(stale));
    const auto current = data.GetPrimaryImage();
    return rejected.status == DataCommitStatus::Rejected
        && current && current->binding
        && current->data->self == second->data->self
        && current->binding->revision > staleBinding.revision;
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
    timerView.window.viewInit.viewMode =
        HostRenderMode::Volume;
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
    config.keys.box.keyCode = 'a';
    config.keys.plane.keyCode = 'b';
    config.keys.noMode.keyCode = 'c';
    config.keys.keepMode.keyCode = 'd';
    config.keys.removeMode.keyCode = 'e';
    config.keys.previous.keyCode = 'f';
    config.keys.next.keyCode = 'g';
    config.keys.buildResult.keyCode = 'h';
    config.keys.restoreOriginal.keyCode = 'i';
    config.keys.exit.keyCode = 'j';
    for (std::size_t index = 0;
        index < config.keys.nodes.size(); ++index) {
        config.keys.nodes[index].keyCode =
            static_cast<char>('0' + index);
    }
    return config;
}

CropHostTarget GetCropTarget()
{
    return GetCropConfig().defaultTarget;
}

CropHostRequest GetCropRequest(const CropHostAction action)
{
    CropHostRequest request;
    request.action = action;
    return request;
}

CropHostRequest GetTargetRequest(
    const CropHostAction action,
    const CropHostTarget& target)
{
    auto request = GetCropRequest(action);
    request.target = target;
    return request;
}

CropHostRequest GetModeRequest(
    const CropHostTarget& target,
    const CropRemovalMode removalMode)
{
    auto request = GetTargetRequest(
        CropHostAction::Mode, target);
    request.removalMode = removalMode;
    return request;
}

CropHostRequest GetNodeRequest(
    const std::size_t nodeCount)
{
    auto request = GetCropRequest(CropHostAction::Node);
    request.nodeCount = nodeCount;
    return request;
}

CropHostRequest GetPolyRequest(
    vtkSmartPointer<vtkPolyData> polyData)
{
    auto request = GetCropRequest(
        CropHostAction::SetPolyData);
    request.polyData = std::move(polyData);
    return request;
}

#ifdef MVVCVTK_HAS_GAP_ANALYSIS
GapHostConfig GetGapConfig()
{
    GapHostConfig config;
    config.defaultStart.targetViews.viewIds = {
        "crop-primary" };
    config.defaultStart.surface.isoMode =
        GapIsoMode::DataRangeRatio;
    config.defaultStart.surface.dataRangeRatio = 0.5;
    config.defaultStart.surface.backgroundMean = 0.0f;
    config.defaultStart.surface.materialMean = 1.0f;
    config.defaultStart.voidParams.isFilterEnabled = false;
    config.defaultStart.voidParams.minVolumeMM3 = 0.0;
    config.inputViews.viewIds = { "crop-primary" };
    config.keys.switchOverlay.keyCode = 'j';
    config.keys.exit.keySym = "Escape";
    return config;
}
#endif

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
    return session.SendRequest(
        std::move(reload),
        [&isComplete, &isSucceeded](const bool value) {
            isSucceeded = value;
            isComplete = true;
        });
}

bool SendTimer(vtkRenderWindowInteractor* interactor)
{
    if (!interactor) return false;
    int timerId = interactor->GetTimerEventId();
    if (timerId == 0) {
        for (int candidate = 1; candidate <= 64; ++candidate) {
            if (interactor->GetTimerDuration(candidate) != 0) {
                timerId = candidate;
                break;
            }
        }
    }
    if (timerId == 0) return false;
    interactor->InvokeEvent(vtkCommand::TimerEvent, &timerId);
    return true;
}

void SendTicks(
    const HostRenderViewEndpoint& endpoint,
    const int tickCount)
{
    for (int tick = 0; tick < tickCount; ++tick) {
        (void)SendTimer(endpoint.interactor);
    }
}

bool GetKeyHandled(
    const HostRenderViewEndpoint& endpoint,
    const char keyCode)
{
    const std::string key(1, keyCode);
    endpoint.interactor->SetKeyEventInformation(
        0, 0, keyCode, 0, key.c_str());
    const bool isPressed =
        endpoint.interactor->InvokeEvent(
            vtkCommand::KeyPressEvent) != 0;
    const bool isReleased =
        endpoint.interactor->InvokeEvent(
            vtkCommand::KeyReleaseEvent) != 0;
    return isPressed && isReleased;
}

void SendHostTick(
    const HostRenderViewEndpoint& primary,
    const HostRenderViewEndpoint& timer)
{
    (void)primary;
    (void)SendTimer(timer.interactor);
    (void)SendTimer(timer.interactor);
}

bool WaitForRenderInput(
    const HostRenderViewEndpoint& endpoint,
    const std::shared_ptr<FeatureViewService>& service,
    const VtkImageGridSnapshot& snapshot)
{
    if (!service || !snapshot || !snapshot->image) {
        return false;
    }
    const RenderInputStamp expected{
        snapshot->data->self
    };
    for (int poll = 0; poll < 500; ++poll) {
        const auto current = service->GetRenderInputStamp();
        if (current && *current == expected) {
            return true;
        }
        SendTicks(endpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    return false;
}

bool WaitForCropNode(
    CropHostFeature& feature,
    const HostRenderViewEndpoint& primary,
    const HostRenderViewEndpoint& timer,
    const std::size_t nodeCount)
{
    for (int poll = 0; poll < 500; ++poll) {
        if (feature.GetState().history.nodeCount == nodeCount) {
            return true;
        }
        SendHostTick(primary, timer);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    return feature.GetState().history.nodeCount == nodeCount;
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
    const auto target = GetCropTarget();
    int unattachedCallbackCount = 0;
    auto unattachedFeature =
        std::make_shared<CropHostFeature>(GetCropConfig());
    const bool isUnattachedRejected =
        !unattachedFeature->SendRequest(
            GetTargetRequest(
                CropHostAction::BuildResult, target),
            [&unattachedCallbackCount](CropBuildResult) {
                ++unattachedCallbackCount;
            });

    HostFeatureContext standaloneContext;
    standaloneContext.views =
        std::make_shared<ViewPortProbe>();
    standaloneContext.data =
        std::make_shared<DataPortProbe>();
    standaloneContext.host =
        std::make_shared<HostControlProbe>();
    CropHostFeature standaloneFeature(GetCropConfig());
    const bool isStandaloneRejected =
        !standaloneFeature.AttachHost(standaloneContext);
    failureCount += GetCaseResult(
        isUnattachedRejected
            && unattachedCallbackCount == 0
            && isStandaloneRejected,
        "Crop rejects unattached requests and non-shared attachment") ? 0 : 1;

    std::shared_ptr<FeatureViewService> retiredService;
    bool isLeaseFixtureReady = false;
    bool isPortsNarrow = false;
    bool isWrongThreadRejected = false;
    {
        VtkAppHostSession leaseSession(GetCropSessionConfig());
        auto leaseProbe = std::make_shared<ContextProbeFeature>();
        const bool isLeaseBuilt = leaseSession.BuildSession();
        const bool isLeaseAttached =
            leaseSession.AttachFeature(leaseProbe);
        retiredService = leaseProbe->GetViewService(
            "crop-primary");
        const auto retiredOverlay =
            leaseProbe->GetViewOverlay("crop-primary");
        isPortsNarrow = retiredService
            && retiredOverlay
            && static_cast<const void*>(retiredService.get())
                != static_cast<const void*>(retiredOverlay.get());
        const bool isPortReady = isLeaseBuilt
            && isLeaseAttached
            && retiredService
            && retiredService->SetRenderNeeded();
        std::thread wrongPortThread([&] {
            isWrongThreadRejected = retiredService
                && !retiredService->SetRenderNeeded();
        });
        wrongPortThread.join();
        const bool isLeaseDetached = isLeaseAttached
            && leaseSession.DetachFeature(*leaseProbe);
        isLeaseFixtureReady = isPortReady
            && isLeaseDetached;
    }
    const bool isRetiredPortRejected = retiredService
        && !retiredService->SetRenderNeeded();
    failureCount += GetCaseResult(
        isLeaseFixtureReady
            && isPortsNarrow
            && isWrongThreadRejected
            && isRetiredPortRejected,
        "Feature ports hide App identity and reject wrong-thread or retired leases") ? 0 : 1;

    VtkAppHostSession session(GetCropSessionConfig());
    auto feature = std::make_shared<CropHostFeature>(
        GetCropConfig());
    auto tickGate = std::make_shared<CropTickGate>(feature);
    const auto initialState = feature->GetState();
    auto contextProbe =
        std::make_shared<ContextProbeFeature>();
    const bool isBuilt = session.BuildSession();
    const bool isAttached =
        session.AttachFeature(tickGate);
    const bool isProbeAttached =
        session.AttachFeature(contextProbe);
    const bool isInputAttached =
        session.AttachHotkeys({});
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
    const bool isSessionStarted = isTimerAttached && session.Start();
    bool isReloadComplete = false;
    bool isReloadSucceeded = false;
    const bool isReloadSent = SendReload(
        session, isReloadComplete, isReloadSucceeded);
    for (int poll = 0;
        isReloadSent && !isReloadComplete && poll < 500;
        ++poll) {
        SendTicks(*timerEndpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    failureCount += GetCaseResult(
        !initialState.isActive
            && !initialState.isPublishing
            && initialState.history.operationCount == 0
            && isTimerAttached
            && isSessionStarted
            && isReloadSent
            && isReloadComplete
            && isReloadSucceeded,
        "Crop fixture publishes an image through Session data API") ? 0 : 1;

    const auto expectedSnapshot =
        contextProbe->m_data->GetPrimaryImage();
    const DataRevisionRef nextRef{
        expectedSnapshot->data->self.entityId,
        expectedSnapshot->data->self.generation + 1 };
    const auto buildReplacement = [&]() {
        DataTransaction transaction;
        transaction.outputs.push_back(DataRevisionDraft{
            expectedSnapshot->data->self.entityId,
            expectedSnapshot->data->self.generation,
            expectedSnapshot->data->type,
            expectedSnapshot->data->inputs,
            expectedSnapshot->data->payload,
            expectedSnapshot->data->provenance });
        transaction.bindings.push_back(DataBindingUpdate{
            std::string(primaryVolumeBinding),
            expectedSnapshot->binding->revision,
            true,
            expectedSnapshot->binding->target,
            nextRef });
        return transaction;
    };
    DataCommitResult wrongThreadWrite;
    std::thread wrongDataThread([&] {
        wrongThreadWrite = contextProbe->m_data->SetDataCommit(
            buildReplacement());
    });
    wrongDataThread.join();
    const auto throwingObserver = contextProbe->m_data->AttachDataChange(
        [](const DataChangeSet&) { throw 1; });
    DataCommitId observedCommit = 0;
    const auto readingObserver = contextProbe->m_data->AttachDataChange(
        [&contextProbe, &observedCommit](const DataChangeSet& change) {
            const auto graph = contextProbe->m_data->GetDataGraph();
            if (graph.commitId == change.commitId) {
                observedCommit = change.commitId;
            }
        });
    const auto published = contextProbe->m_data->SetDataCommit(
        buildReplacement());
    const bool isPublishedInputApplied = WaitForRenderInput(
        *timerEndpoint, contextProbe->GetViewService("crop-primary"),
        contextProbe->m_data->GetPrimaryImage());
    const bool areObserversDetached =
        contextProbe->m_data->DetachDataChange(throwingObserver)
        && contextProbe->m_data->DetachDataChange(readingObserver);
    const auto stale = contextProbe->m_data->SetDataCommit(
        buildReplacement());
    const auto publishedSnapshot =
        contextProbe->m_data->GetPrimaryImage();
    const auto publishedGraph = contextProbe->m_data->GetDataGraph();
    const auto hostDataState = session.GetRenderViewState({
        "crop-primary", false, HostRenderViewRole::Primary3D });
    failureCount += GetCaseResult(
        wrongThreadWrite.status == DataCommitStatus::Rejected
            && isPublishedInputApplied
            && published.status == DataCommitStatus::Succeeded
            && stale.status == DataCommitStatus::Rejected
            && observedCommit == published.commitId
            && areObserversDetached
            && publishedSnapshot
            && publishedSnapshot->data->self == nextRef
            && publishedSnapshot->binding->revision
                == expectedSnapshot->binding->revision + 1
            && !contextProbe->m_data->GetData(
                expectedSnapshot->graph, nextRef)
            && contextProbe->m_data->GetData(publishedGraph, nextRef)
            && hostDataState
            && hostDataState->dataRevision == nextRef
            && hostDataState->bindingRevision
                == publishedSnapshot->binding->revision,
        "Feature Data Port enforces owner thread, snapshot, observer and Binding CAS") ? 0 : 1;
    failureCount += GetCaseResult(
        GetCoreWriterContract(),
        "Core writer publishes before notification and weak closures expire safely") ? 0 : 1;

    const bool isUnknownTypeRegistered =
        contextProbe->m_data->SetDataType(DataTypeDescriptor{
            UnknownCropPayload::Type,
            { DataFacetId{ "unsupported-crop-input" } },
            [](const IDataPayload& payload, std::string&) {
                return dynamic_cast<const UnknownCropPayload*>(&payload)
                    != nullptr;
            } });
    const auto unknownEntity =
        contextProbe->m_data->CreateDataEntityId();
    const DataRevisionRef unknownRef{ unknownEntity, 1 };
    DataTransaction unknownInput;
    unknownInput.outputs.push_back(DataRevisionDraft{
        unknownEntity, 0, UnknownCropPayload::Type, {},
        std::make_shared<const UnknownCropPayload>(), std::nullopt });
    unknownInput.bindings.push_back(DataBindingUpdate{
        std::string(cropInputBinding), 0, true, {}, unknownRef });
    const auto unknownCommit = contextProbe->m_data->SetDataCommit(
        std::move(unknownInput));
    const auto beforeUnknownStart =
        contextProbe->m_data->GetDataGraph();
    const bool isUnknownInputRejected = !feature->SendRequest(
        GetTargetRequest(CropHostAction::Start, GetCropTarget()));
    const auto afterUnknownStart =
        contextProbe->m_data->GetDataGraph();
    DataTransaction clearUnknown;
    clearUnknown.bindings.push_back(DataBindingUpdate{
        std::string(cropInputBinding), 1, true, unknownRef, {} });
    const auto clearUnknownResult = contextProbe->m_data->SetDataCommit(
        std::move(clearUnknown));
    failureCount += GetCaseResult(
        isUnknownTypeRegistered
            && unknownCommit.status == DataCommitStatus::Succeeded
            && isUnknownInputRejected
            && beforeUnknownStart.commitId == afterUnknownStart.commitId
            && !feature->GetState().isActive
            && clearUnknownResult.status == DataCommitStatus::Succeeded
            && contextProbe->m_data->GetPrimaryImage()->data->self
                == nextRef,
        "Crop rejects unsupported facets without publishing partial state") ? 0 : 1;
    // 让主视图消费 probe 发布的新 snapshot；主视图不承载 Host timer，
    // 因此不会提前推进后续 Feature worker。
#ifdef MVVCVTK_HAS_GAP_ANALYSIS
    const bool isPrimaryRenderCurrent = WaitForRenderInput(
        *timerEndpoint,
        contextProbe->GetViewService("crop-primary"),
        publishedSnapshot);
    failureCount += GetCaseResult(
        isPrimaryRenderCurrent,
        "Feature publication converges the primary render input") ? 0 : 1;

    const bool isInitialNodeRejected =
        !feature->SendRequest(GetNodeRequest(0));
    bool areCommandKeysHandled = true;
    for (char keyCode = 'a'; keyCode <= 'j'; ++keyCode) {
        areCommandKeysHandled =
            GetKeyHandled(*endpoint, keyCode)
            && areCommandKeysHandled;
    }
    bool areNodeKeysHandled = true;
    for (char keyCode = '0'; keyCode <= '9'; ++keyCode) {
        areNodeKeysHandled =
            GetKeyHandled(*endpoint, keyCode)
            && areNodeKeysHandled;
    }
    failureCount += GetCaseResult(
        areCommandKeysHandled && areNodeKeysHandled,
        "Crop maps every command and node key through one input binding") ? 0 : 1;

    auto modeWithoutValue = GetTargetRequest(
        CropHostAction::Mode, target);
    auto buildWithoutTarget = GetCropRequest(
        CropHostAction::BuildResult);
    auto polyWithoutVersion = GetCropRequest(
        CropHostAction::SetPolyData);
    polyWithoutVersion.polyData =
        vtkSmartPointer<vtkPolyData>::New();
    int rejectedBuildCount = 0;
    const bool isStrict =
        !feature->SendRequest(GetCropRequest(
            CropHostAction::None))
        && !feature->SendRequest(GetCropRequest(
            CropHostAction::Start))
        && !feature->SendRequest(std::move(modeWithoutValue))
        && !feature->SendRequest(GetCropRequest(
            CropHostAction::Node))
        && !feature->SendRequest(
            std::move(buildWithoutTarget),
            [&rejectedBuildCount](CropBuildResult) {
                ++rejectedBuildCount;
            })
        && !feature->SendRequest(std::move(polyWithoutVersion))
        && !feature->SendRequest(
            GetCropRequest(CropHostAction::Previous),
            [](CropBuildResult) {})
        && !feature->SendRequest(GetTargetRequest(
            CropHostAction::BuildResult, target));
    const bool isSyncBuildRejected =
        !feature->SendRequest(
            GetTargetRequest(
                CropHostAction::BuildResult, target),
            [&rejectedBuildCount](CropBuildResult) {
                ++rejectedBuildCount;
            });
    SendHostTick(*endpoint, *timerEndpoint);
    const bool isInactiveHistoryRejected =
        !feature->SendRequest(GetCropRequest(
            CropHostAction::Previous))
        && !feature->SendRequest(GetCropRequest(
            CropHostAction::Next));
    bool isWrongThreadAccepted = true;
    int wrongThreadCallbackCount = 0;
    std::thread wrongThread([&] {
        isWrongThreadAccepted = feature->SendRequest(
            GetTargetRequest(
                CropHostAction::BuildResult, target),
            [&wrongThreadCallbackCount](CropBuildResult) {
                ++wrongThreadCallbackCount;
            });
    });
    wrongThread.join();
    failureCount += GetCaseResult(
        isStrict
            && isSyncBuildRejected
            && rejectedBuildCount == 0
            && isInitialNodeRejected
            && isInactiveHistoryRejected
            && !isWrongThreadAccepted
            && wrongThreadCallbackCount == 0,
        "Crop request fields and rejected BuildResult callbacks stay strict") ? 0 : 1;

    auto splitTarget = target;
    splitTarget.referenceView = {
        "crop-timer", false,
        HostRenderViewRole::Auxiliary };
    const bool isSplitStarted = feature->SendRequest(
        GetTargetRequest(
            CropHostAction::Start, splitTarget));
    SendHostTick(*endpoint, *timerEndpoint);
    const auto splitService =
        contextProbe->GetViewService("crop-timer");
    auto* splitVolumes = timerEndpoint
        && timerEndpoint->renderer
        ? timerEndpoint->renderer->GetVolumes()
        : nullptr;
    if (splitVolumes) {
        splitVolumes->InitTraversal();
    }
    auto* splitVolume = splitVolumes
        ? splitVolumes->GetNextVolume()
        : nullptr;
    auto* splitMapper = splitVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            splitVolume->GetMapper())
        : nullptr;
    auto* splitQualityInput = splitMapper
        ? splitMapper->GetInputConnection(0, 0)
        : nullptr;
    auto* splitQualityMask = splitMapper
        ? splitMapper->GetMaskInput()
        : nullptr;
    const double splitSampleDistance =
        splitMapper ? splitMapper->GetSampleDistance() : 0.0;
    const double splitImageDistance =
        splitMapper ? splitMapper->GetImageSampleDistance() : 0.0;
    // Box/Plane 最终都通过 referenceService 写入同一通用交互轴；
    // 直接切换该轴可稳定验证 scope 与 producer 锁定，不依赖 VTK picking 偶然性。
    const InteractionSource splitDragSource{
        "crop.context.probe", "drag"
    };
    const bool isSplitDragSet = splitService
        && splitService->SetInteracting(
            splitDragSource, true);
    SendHostTick(*endpoint, *timerEndpoint);
    const bool isSplitLocked = splitMapper
        && splitQualityInput
        && splitMapper->GetInputConnection(0, 0)
            == splitQualityInput
        && splitMapper->GetMaskInput() == splitQualityMask
        && splitMapper->GetAutoAdjustSampleDistances() == 0
        && std::abs(splitImageDistance - 1.0) < 1e-12
        && std::abs(
            splitMapper->GetMinimumImageSampleDistance() - 4.0)
            < 1e-12
        && std::abs(
            splitMapper->GetMaximumImageSampleDistance() - 4.0)
            < 1e-12
        && std::abs(
            splitMapper->GetImageSampleDistance() - 4.0)
            < 1e-12
        && std::abs(
            splitMapper->GetSampleDistance()
                - splitSampleDistance * 4.0) < 1e-12
        && splitMapper->GetUseJittering() == 0;
    const bool isSplitDragClear = splitService
        && splitService->SetInteracting(
            splitDragSource, false);
    SendHostTick(*endpoint, *timerEndpoint);
    const bool isSplitQualityRestored = splitMapper
        && splitMapper->GetInputConnection(0, 0)
            == splitQualityInput
        && splitMapper->GetMaskInput() == splitQualityMask
        && splitMapper->GetAutoAdjustSampleDistances() != 0
        && std::abs(
            splitMapper->GetImageSampleDistance()
                - splitImageDistance) < 1e-12
        && std::abs(
            splitMapper->GetSampleDistance()
                - splitSampleDistance) < 1e-12
        && splitMapper->GetUseJittering() != 0;
    failureCount += GetCaseResult(
        isSplitStarted
            && session.GetRenderViewState({
                "crop-primary", false,
                HostRenderViewRole::Auxiliary }).value_or(
                    HostRenderViewState{}).isFeatureActive
            && session.GetRenderViewState({
                "crop-timer", false,
                HostRenderViewRole::Auxiliary }).value_or(
                    HostRenderViewState{}).isFeatureActive
            && isSplitDragSet
            && isSplitLocked
            && isSplitDragClear
            && isSplitQualityRestored,
        "Crop keeps one quality input and mapper through split reference drag") ? 0 : 1;

    const bool isStarted = feature->SendRequest(
        GetTargetRequest(CropHostAction::Start, target));
    const auto startedState = feature->GetState();
    auto defaultOnlyTarget = target;
    defaultOnlyTarget.isTargetViewsUsed = false;
    defaultOnlyTarget.targetViews.viewIds = {
        "ignored-missing-view" };
    const bool isDefaultOnlyStarted =
        feature->SendRequest(GetTargetRequest(
            CropHostAction::Start, defaultOnlyTarget));
    auto emptyExplicitTarget = target;
    emptyExplicitTarget.targetViews = {};
    const bool isEmptyExplicitRejected =
        !feature->SendRequest(GetTargetRequest(
            CropHostAction::Start, emptyExplicitTarget));
    auto unknownExplicitTarget = target;
    unknownExplicitTarget.targetViews.viewIds = {
        "crop-primary", "missing-view" };
    const bool isUnknownExplicitRejected =
        !feature->SendRequest(GetTargetRequest(
            CropHostAction::Start, unknownExplicitTarget));
    const auto preservedState = feature->GetState();
    const bool isModeSet = feature->SendRequest(
        GetModeRequest(
            target, CropRemovalMode::RemoveInside));
    const bool isBoxSet = feature->SendRequest(
        GetTargetRequest(CropHostAction::Box, target));
    const double imageBounds[6] = {
        0.0, 3.0, 0.0, 3.0, 0.0, 3.0 };
    endpoint->renderer->ResetCamera(imageBounds);
    endpoint->renderWindow->Render();
    failureCount += GetCaseResult(
        isStarted
            && session.GetRenderViewState({
                "crop-primary", false,
                HostRenderViewRole::Auxiliary }).value_or(
                    HostRenderViewState{}).isFeatureActive
            && !session.GetRenderViewState({
                "crop-timer", false,
                HostRenderViewRole::Auxiliary }).value_or(
                    HostRenderViewState{}).isFeatureActive
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

    const bool isPreviousRejected =
        !feature->SendRequest(GetCropRequest(
            CropHostAction::Previous));
    const bool isNextRejected =
        !feature->SendRequest(GetCropRequest(
            CropHostAction::Next));
    bool isNode = false;
    for (int poll = 0; !isNode && poll < 500; ++poll) {
        isNode = feature->SendRequest(GetNodeRequest(0));
        if (!isNode) {
            SendHostTick(*endpoint, *timerEndpoint);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
    }
    const bool isRearmed = feature->SendRequest(
        GetModeRequest(
            target, CropRemovalMode::RemoveInside));
    const bool isBoxRearmed = feature->SendRequest(
        GetTargetRequest(CropHostAction::Box, target));
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

    const bool isPrevious = feature->SendRequest(
        GetCropRequest(CropHostAction::Previous));
    const bool isPreviousCommitted = isPrevious
        && WaitForCropNode(
            *feature, *endpoint, *timerEndpoint, 0);
    const auto previousState = feature->GetState();
    const bool isNext = feature->SendRequest(
        GetCropRequest(CropHostAction::Next));
    const bool isNextCommitted = isNext
        && WaitForCropNode(
            *feature, *endpoint, *timerEndpoint, 1);
    const auto nextState = feature->GetState();
    failureCount += GetCaseResult(
        isPreviousCommitted
            && previousState.history.nodeCount == 0
            && isNextCommitted
            && nextState.history.nodeCount == 1,
        "Previous and Next move the committed Crop history in both directions") ? 0 : 1;

    const bool isHistoryExited = feature->SendRequest(
        GetCropRequest(CropHostAction::Exit));
    const bool isPreviousAfterExit = feature->SendRequest(
        GetCropRequest(CropHostAction::Previous));
    const bool isPreviousAfterExitCommitted = isPreviousAfterExit
        && WaitForCropNode(
            *feature, *endpoint, *timerEndpoint, 0);
    const auto previousAfterExit = feature->GetState();
    const bool isNextAfterExit = feature->SendRequest(
        GetCropRequest(CropHostAction::Next));
    const bool isNextAfterExitCommitted = isNextAfterExit
        && WaitForCropNode(
            *feature, *endpoint, *timerEndpoint, 1);
    const auto nextAfterExit = feature->GetState();
    failureCount += GetCaseResult(
        isHistoryExited
            && !previousAfterExit.isActive
            && isPreviousAfterExitCommitted
            && previousAfterExit.history.nodeCount == 0
            && isNextAfterExitCommitted
            && nextAfterExit.history.nodeCount == 1,
        "Exit hides Crop widgets without locking committed history navigation") ? 0 : 1;

    const auto cropExpected =
        contextProbe->m_data->GetPrimaryImage();
    tickGate->isPaused = true;
    int staleCompleteCount = 0;
    CropBuildResult staleResult;
    const bool isStaleBuilt =
        feature->SendRequest(
            GetTargetRequest(
                CropHostAction::BuildResult, target),
            [&staleCompleteCount, &staleResult](
                CropBuildResult result) {
                ++staleCompleteCount;
                staleResult = std::move(result);
            });
    const bool isPublishPreviousRejected =
        !feature->SendRequest(GetCropRequest(
            CropHostAction::Previous));
    const bool isPublishNextRejected =
        !feature->SendRequest(GetCropRequest(
            CropHostAction::Next));
    const bool isPublishNodeRejected =
        !feature->SendRequest(GetNodeRequest(0));
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
        // 正常推进统一 Host 帧；仅延迟 Crop 的完成阶段。
        SendTicks(*timerEndpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto reloadSnapshot =
        contextProbe->m_data->GetPrimaryImage();
    tickGate->isPaused = false;
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
        contextProbe->m_data->GetPrimaryImage();
    failureCount += GetCaseResult(
        cropExpected
            && isStaleBuilt
            && isPublishPreviousRejected
            && isPublishNextRejected
            && isPublishNodeRejected
            && isConflictReloadSent
            && isConflictReloadComplete
            && isConflictReloadSucceeded
            && reloadSnapshot
            && reloadSnapshot->binding->revision
                == cropExpected->binding->revision + 1
            && reloadSnapshot->data->self
                != cropExpected->data->self
            && staleCompleteCount == 1
            && !staleResult.isSucceeded
            && staleResult.failureReason
                == CropFailure::VersionMismatch
            && currentAfterReject
            && currentAfterReject->data->self
                == reloadSnapshot->data->self
            && staleState.history.nodeCount == 0
            && staleState.history.operationCount == 0
            && staleState.history.baseNodeCount == 0
            && !staleState.history.hasEditableOp,
        "Reload-first order rejects delayed Crop CAS without partial commit") ? 0 : 1;

    SendTicks(*timerEndpoint, 2);
    const bool isRestarted = feature->SendRequest(
        GetTargetRequest(CropHostAction::Start, target));
    const bool isRestartModeSet = feature->SendRequest(
        GetModeRequest(
            target, CropRemovalMode::RemoveInside));
    const bool isRestartBoxSet = feature->SendRequest(
        GetTargetRequest(CropHostAction::Box, target));
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
    GapHostResult staleGapResult;
    const bool isStaleGapAccepted =
        gapFeature->SendRequest(
            { GapHostAction::Start,
                GetGapConfig().defaultStart },
            [&staleGapCount, &staleGapResult](
                GapHostResult result) {
                ++staleGapCount;
                staleGapResult = std::move(result);
            });
    const auto publishExpected =
        contextProbe->m_data->GetPrimaryImage();
    int publishCompleteCount = 0;
    CropBuildResult publishResult;
    const bool isPublishBuilt =
        feature->SendRequest(
            GetTargetRequest(
                CropHostAction::BuildResult, target),
            [&publishCompleteCount, &publishResult](
                CropBuildResult result) {
                ++publishCompleteCount;
                publishResult = std::move(result);
            });
    failureCount += GetCaseResult(
        isGapAttached && isStaleGapAccepted
            && isPublishBuilt,
        "Crop/Gap conflict requests are admitted") ? 0 : 1;
    for (int poll = 0;
        publishCompleteCount == 0 && poll < 500;
        ++poll) {
        endpoint->renderWindow->Render();
        SendTicks(*endpoint, 1);
        SendHostTick(*endpoint, *timerEndpoint);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto primaryAfterBuild =
        contextProbe->m_data->GetPrimaryImage();
    const auto resultGraph = contextProbe->m_data->GetDataGraph();
    const auto derivedCrop = publishResult.isSucceeded
        ? contextProbe->m_data->GetImageGrid(
            resultGraph, publishResult.outputRevision)
        : VtkImageGridSnapshot{};
    const bool isPrimarySet = feature->SendRequest(
        GetCropRequest(CropHostAction::SetPrimaryResult));
    const auto cropSnapshot =
        contextProbe->m_data->GetPrimaryImage();
    for (int poll = 0;
        (staleGapCount == 0
            || gapFeature->GetState().analysisState
                != GapAnalysisState::Stale
            || gapFeature->GetState().isExitPending)
            && poll < 500;
        ++poll) {
        SendHostTick(*endpoint, *timerEndpoint);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto staleGapState =
        gapFeature->GetState();
    const bool hasGapCommittedFirst =
        staleGapResult.status == GapResultStatus::Succeeded
        || staleGapResult.status
            == GapResultStatus::SucceededWithDisplayFailure;
    const bool hasCropCommittedFirst =
        staleGapResult.status == GapResultStatus::SourceChanged;
    const bool hasSerializedGapOutcome =
        (hasGapCommittedFirst
            && staleGapResult.commitId != 0
            && GetDataRevisionRefValid(staleGapResult.resultSet)
            && staleGapState.resultSet == staleGapResult.resultSet)
        || (hasCropCommittedFirst
            && staleGapResult.commitId == 0
            && !GetDataRevisionRefValid(staleGapResult.resultSet)
            && !GetDataRevisionRefValid(staleGapState.resultSet)
            && staleGapState.statistics.objectVoxelCount == 0
            && staleGapState.statistics.voidVoxelCount == 0);
    const bool isStaleGapOverlayRejected =
        !gapFeature->SendRequest({
            GapHostAction::Overlay });
    failureCount += GetCaseResult(
        isGapAttached
            && isStaleGapAccepted
            && isPublishBuilt
            && publishExpected
            && primaryAfterBuild
            && primaryAfterBuild->data->self
                == publishExpected->data->self
            && derivedCrop && derivedCrop->validityMask
            && isPrimarySet
            && cropSnapshot
            && cropSnapshot->data->self
                == publishResult.outputRevision
            && cropSnapshot->binding->revision
                == publishExpected->binding->revision + 1
            && staleGapCount == 1
            && staleGapState.analysisState
                == GapAnalysisState::Stale
            && !staleGapState.isViewActive
            && !staleGapState.isExitPending
            && hasSerializedGapOutcome
            && GetDataRevisionRefValid(publishResult.recipeRevision)
            && GetDataRevisionRefValid(publishResult.outputRevision)
            && isStaleGapOverlayRejected,
        "Crop build is non-destructive and explicit promotion invalidates stale Gap input") ? 0 : 1;

    const bool isSourceRestored = feature->SendRequest(
        GetCropRequest(CropHostAction::RestoreOriginal));
    const auto restoredSourceSnapshot =
        contextProbe->m_data->GetPrimaryImage();
    const bool isCropRepromoted = feature->SendRequest(
        GetCropRequest(CropHostAction::SetPrimaryResult));
    const auto activeCropSnapshot =
        contextProbe->m_data->GetPrimaryImage();
    failureCount += GetCaseResult(
        publishExpected
            && cropSnapshot
            && derivedCrop
            && isSourceRestored
            && restoredSourceSnapshot
            && restoredSourceSnapshot->data == publishExpected->data
            && restoredSourceSnapshot->data->self
                == publishResult.sourceRevision
            && restoredSourceSnapshot->binding->revision
                == cropSnapshot->binding->revision + 1
            && isCropRepromoted
            && activeCropSnapshot
            && activeCropSnapshot->data == derivedCrop->data
            && activeCropSnapshot->data->self
                == publishResult.outputRevision
            && activeCropSnapshot->binding->revision
                == restoredSourceSnapshot->binding->revision + 1,
        "RestoreOriginal and SetPrimaryResult use Binding-only ABA-safe transactions") ? 0 : 1;

    for (int poll = 0;
        publishCompleteCount == 0
            && poll < 500;
        ++poll) {
        // Gap 已按新的 primary Binding 关系退休；此后才让主视图消费 Crop
        // 批次并由 Session 投递 Crop 成功 callback。
        SendTicks(*timerEndpoint, 1);
        SendHostTick(*endpoint, *timerEndpoint);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }

    bool hasCroppedVoxel = false;
    if (activeCropSnapshot && activeCropSnapshot->validityMask) {
        const auto* maskValues = static_cast<const unsigned char*>(
            activeCropSnapshot->validityMask->GetScalarPointer());
        const auto maskCount =
            activeCropSnapshot->validityMask->GetNumberOfPoints();
        for (vtkIdType index = 0;
            maskValues && index < maskCount;
            ++index) {
            if (maskValues[index] == 0) {
                hasCroppedVoxel = true;
                break;
            }
        }
    }
    int postCropGapCompleteCount = 0;
    const bool isPostCropGapAccepted =
        publishCompleteCount == 1
        && publishResult.isSucceeded
        && activeCropSnapshot
        && activeCropSnapshot->validityMask
        && gapFeature->SendRequest(
            { GapHostAction::Start,
                GetGapConfig().defaultStart },
            [&postCropGapCompleteCount](GapHostResult) {
                ++postCropGapCompleteCount;
            });
    const auto postCropGapAcceptedState =
        gapFeature->GetState();
    for (int poll = 0;
        isPostCropGapAccepted
            && postCropGapCompleteCount == 0
            && poll < 500;
        ++poll) {
        SendTicks(*timerEndpoint, 1);
        SendHostTick(*endpoint, *timerEndpoint);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto postCropGapState =
        gapFeature->GetState();
    const bool isPostCropGapExited =
        gapFeature->SendRequest({ GapHostAction::Exit });
    failureCount += GetCaseResult(
        hasCroppedVoxel
            && isPostCropGapAccepted
            && postCropGapAcceptedState.analysisState
                != GapAnalysisState::Idle
            && postCropGapAcceptedState.isViewActive
            && postCropGapCompleteCount == 1
            && postCropGapState.analysisState
                == GapAnalysisState::Succeeded
            && contextProbe->m_data->GetPrimaryImage()->data->self
                == activeCropSnapshot->data->self
            && isPostCropGapExited,
        "Gap runs DefX from the materialized Crop baseline with a validity mask") ? 0 : 1;

    const auto exportId =
        std::chrono::steady_clock::now()
            .time_since_epoch().count();
    const auto exportDir =
        std::filesystem::temp_directory_path()
        / "MVVCVTK_crop_build"
        / std::to_string(exportId);
    HostDataExportRequest exportRequest;
    exportRequest.outputPath = exportDir.u8string();
    exportRequest.format = HostDataExportFormat::Raw;
    int exportCompleteCount = 0;
    bool isExportSucceeded = false;
    const bool isExportSent = session.SendRequest(
        std::move(exportRequest),
        [&exportCompleteCount, &isExportSucceeded](
            const bool isSuccess) {
            ++exportCompleteCount;
            isExportSucceeded = isSuccess;
        });
    for (int poll = 0;
        isExportSent
            && exportCompleteCount == 0
            && poll < 500;
        ++poll) {
        SendTicks(*timerEndpoint, 1);
        SendHostTick(*endpoint, *timerEndpoint);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    int exportDimensions[3] = {};
    if (activeCropSnapshot && activeCropSnapshot->image) {
        activeCropSnapshot->image->GetDimensions(
            exportDimensions);
    }
    const auto exportPath =
        exportDir
        / (std::to_string(exportDimensions[0])
            + "x"
            + std::to_string(exportDimensions[1])
            + "x"
            + std::to_string(exportDimensions[2])
            + "_transform.raw");
    const bool hasExportFile =
        std::filesystem::exists(exportPath);
    const std::size_t exportVoxelCount =
        exportDimensions[0] > 0
        && exportDimensions[1] > 0
        && exportDimensions[2] > 0
        ? static_cast<std::size_t>(exportDimensions[0])
            * static_cast<std::size_t>(exportDimensions[1])
            * static_cast<std::size_t>(exportDimensions[2])
        : 0;
    std::vector<float> exportedValues(exportVoxelCount);
    std::ifstream exportFile(exportPath, std::ios::binary);
    exportFile.read(
        reinterpret_cast<char*>(exportedValues.data()),
        static_cast<std::streamsize>(
            exportedValues.size() * sizeof(float)));
    const std::streamsize exportedBytes = exportFile.gcount();
    exportFile.close();
    std::error_code exportSizeError;
    const auto exportedSize = std::filesystem::file_size(
        exportPath, exportSizeError);

    bool hasCroppedRawContent = false;
    if (activeCropSnapshot
        && activeCropSnapshot->image
        && activeCropSnapshot->validityMask
        && !exportSizeError
        && exportedSize
            == exportedValues.size() * sizeof(float)
        && exportedBytes
            == static_cast<std::streamsize>(
                exportedValues.size() * sizeof(float))) {
        int extent[6] = {};
        double scalarRange[2] = {};
        activeCropSnapshot->image->GetExtent(extent);
        activeCropSnapshot->image->GetScalarRange(scalarRange);
        std::size_t invalidIndex = exportVoxelCount;
        std::size_t validIndex = exportVoxelCount;
        float validValue = 0.0f;
        std::size_t linearIndex = 0;
        // 此用例从未改变 Host model matrix，因此 RAW 网格与
        // crop snapshot 网格一致；另选原值非背景的无效体素，避免假阳性。
        for (int z = extent[4]; z <= extent[5]; ++z) {
            for (int y = extent[2]; y <= extent[3]; ++y) {
                for (int x = extent[0]; x <= extent[1]; ++x) {
                    const double maskValue = activeCropSnapshot
                        ->validityMask->GetScalarComponentAsDouble(
                            x, y, z, 0);
                    const double imageValue = activeCropSnapshot
                        ->image->GetScalarComponentAsDouble(
                            x, y, z, 0);
                    if (invalidIndex == exportVoxelCount
                        && maskValue == 0.0
                        && imageValue != scalarRange[0]) {
                        invalidIndex = linearIndex;
                    }
                    if (validIndex == exportVoxelCount
                        && maskValue != 0.0) {
                        validIndex = linearIndex;
                        validValue = static_cast<float>(imageValue);
                    }
                    ++linearIndex;
                }
            }
        }
        constexpr float tolerance = 1e-5f;
        hasCroppedRawContent =
            invalidIndex < exportedValues.size()
            && validIndex < exportedValues.size()
            && std::abs(
                exportedValues[invalidIndex]
                    - static_cast<float>(scalarRange[0]))
                <= tolerance
            && std::abs(
                exportedValues[validIndex] - validValue)
                <= tolerance
            && linearIndex == exportedValues.size();
    }
    failureCount += GetCaseResult(
        publishCompleteCount == 1
            && publishResult.isSucceeded
            && isExportSent
            && exportCompleteCount == 1
            && isExportSucceeded
            && hasExportFile,
        "Crop BuildResult publishes image before independent Host data export") ? 0 : 1;
    failureCount += GetCaseResult(
        hasCroppedRawContent,
        "Host RAW export materializes the published Crop validity mask") ? 0 : 1;
    std::error_code removeError;
    std::filesystem::remove_all(
        exportDir, removeError);
    const bool isGapDetached =
        session.DetachFeature(*gapFeature);
    const bool isSecondModeSet =
        feature->SendRequest(GetModeRequest(
            target, CropRemovalMode::RemoveInside));
    const bool isSecondBoxSet =
        feature->SendRequest(GetTargetRequest(
            CropHostAction::Box, target));
    endpoint->renderer->ResetCamera(imageBounds);
    endpoint->renderWindow->Render();
    const bool isSecondWidgetSent =
        SendCropInput(
            *feature,
            *endpoint,
            *timerEndpoint,
            imageBounds);
    const auto secondExpected =
        contextProbe->m_data->GetPrimaryImage();
    int secondCompleteCount = 0;
    CropBuildResult secondResult;
    const bool isSecondBuilt =
        feature->SendRequest(
            GetTargetRequest(
                CropHostAction::BuildResult, target),
            [&secondCompleteCount, &secondResult](
                CropBuildResult result) {
                ++secondCompleteCount;
                secondResult = std::move(result);
            });
    for (int poll = 0;
        secondCompleteCount == 0
            && poll < 500;
        ++poll) {
        SendTicks(*timerEndpoint, 1);
        SendHostTick(*endpoint, *timerEndpoint);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto secondSnapshot =
        contextProbe->m_data->GetPrimaryImage();
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
        SendTicks(*timerEndpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto finalSnapshot =
        contextProbe->m_data->GetPrimaryImage();
    failureCount += GetCaseResult(
        isRestarted
            && isRestartModeSet
            && isRestartBoxSet
            && isNextWidgetSent
            && isGapAttached
            && isStaleGapAccepted
            && publishExpected
            && isPublishBuilt
            && publishCompleteCount == 1
            && publishResult.isSucceeded
            && activeCropSnapshot
            && activeCropSnapshot->data->self
                == publishResult.outputRevision
            && isSecondModeSet
            && isSecondBoxSet
            && isSecondWidgetSent
            && secondExpected
            && secondExpected->data->self
                == activeCropSnapshot->data->self
            && isSecondBuilt
            && secondCompleteCount == 1
            && secondResult.isSucceeded
            && secondResult.sourceRevision
                == publishResult.outputRevision
            && secondResult.nodeCount == 1
            && GetDataRevisionRefValid(secondResult.recipeRevision)
            && GetDataRevisionRefValid(secondResult.outputRevision)
            && secondSnapshot
            && secondSnapshot->data->self
                == activeCropSnapshot->data->self
            && secondSnapshot->binding->revision
                == activeCropSnapshot->binding->revision
            && isFinalReloadSent
            && isFinalReloadComplete
            && isFinalReloadSucceeded
            && finalSnapshot
            && finalSnapshot->binding->revision
                == secondSnapshot->binding->revision + 1
            && finalSnapshot->data->self
                != secondSnapshot->data->self
            && isGapDetached
            && staleGapCount == 1
            && hasSerializedGapOutcome,
        "Repeated Crop build forms an explicit data DAG before a later legal Reload") ? 0 : 1;
#endif

    const bool isBox = feature->SendRequest(
        GetTargetRequest(CropHostAction::Box, target));
    const bool isPlaneRestored = feature->SendRequest(
        GetTargetRequest(CropHostAction::Plane, target));
    failureCount += GetCaseResult(
        isBox && isPlaneRestored,
        "Box and Plane remain available after version conflict coverage") ? 0 : 1;

    const bool isExited = feature->SendRequest(
        GetCropRequest(CropHostAction::Exit));
    const auto exitedState = feature->GetState();
    auto cube = vtkSmartPointer<vtkCubeSource>::New();
    cube->Update();
    auto firstPolyData = vtkSmartPointer<vtkPolyData>::New();
    firstPolyData->DeepCopy(cube->GetOutput());
    auto nextPolyData = vtkSmartPointer<vtkPolyData>::New();
    nextPolyData->DeepCopy(cube->GetOutput());
    const bool hasPolyDataContract =
        feature->SendRequest(GetPolyRequest(firstPolyData))
        && feature->SendRequest(GetPolyRequest(nextPolyData))
        && feature->SendRequest(GetCropRequest(
            CropHostAction::ClearPolyData));
    failureCount += GetCaseResult(
        isExited
            && !exitedState.isActive
            && !exitedState.isPublishing
            && hasPolyDataContract,
        "SetPolyData publishes isolated mesh revisions and Clear removes only its Binding") ? 0 : 1;

    bool hasDetachedCallback = false;
    const bool isPendingAccepted = feature->SendRequest(
        GetTargetRequest(
            CropHostAction::BuildResult, target),
        [&hasDetachedCallback](CropBuildResult) {
            hasDetachedCallback = true;
        });
    const auto useCount = feature.use_count();
    const bool isDetached =
        session.DetachFeature(*tickGate);
    tickGate.reset();
    const auto detachedState = feature->GetState();
    const bool isProbeDetached =
        session.DetachFeature(*contextProbe);
    SendHostTick(*endpoint, *timerEndpoint);
    int detachedSendCount = 0;
    const bool isDetachedRequestRejected =
        !feature->SendRequest(
            GetTargetRequest(
                CropHostAction::BuildResult, target),
            [&detachedSendCount](CropBuildResult) {
                ++detachedSendCount;
            });
    failureCount += GetCaseResult(
        !isPendingAccepted
            && isDetached
            && isProbeDetached
            && isDetachedRequestRejected
            && detachedSendCount == 0
            && feature.use_count() + 1 == useCount
            && !detachedState.isActive
            && !detachedState.isPublishing
            && detachedState.history.operationCount == 0
            && !hasDetachedCallback,
        "Crop detach leaves the upper owner alive and suppresses queued callbacks") ? 0 : 1;
    return failureCount;
}
