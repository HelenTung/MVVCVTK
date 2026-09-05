#include "QtHostMethodCases.h"
#include "../TestDataPort.h"

#include "Data/DataPayloads.h"
#include "Host/GapHostFeature.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/VtkAppHostSession.h"

#include <vtkActor.h>
#include <vtkCommand.h>
#include <vtkImageSlice.h>
#include <vtkPropCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class HostControlStub final : public FeatureHostControl {
public:
    bool AttachInput(HostInputBinding binding) override
    {
        if (m_isAttached
            || binding.featureId.empty()
            || !binding.onInput) {
            return false;
        }
        m_featureId = std::move(binding.featureId);
        m_isAttached = true;
        ++m_attachCount;
        return true;
    }

    bool DetachInput(
        const std::string_view featureId) override
    {
        if (!m_isAttached
            || featureId != m_featureId) {
            return false;
        }
        m_featureId.clear();
        m_isAttached = false;
        ++m_detachCount;
        return true;
    }

    int GetAttachCount() const noexcept
    {
        return m_attachCount;
    }

    int GetDetachCount() const noexcept
    {
        return m_detachCount;
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
        std::function<void()> complete) override
    {
        if (!complete) return false;
        complete();
        return true;
    }

private:
    std::string m_featureId;
    std::vector<FeatureSceneDelta> m_sceneDeltas;
    int m_attachCount = 0;
    int m_detachCount = 0;
    bool m_isAttached = false;
};

class ViewDirectoryStub final : public FeatureViewDirectory {
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

class ContextProbeFeature final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override
    {
        return "gap.context.probe";
    }

    bool AttachHost(const HostFeatureContext& context) override
    {
        if (!context.data) return false;
        m_data = context.data;
        return true;
    }

    bool DetachHost() override
    {
        m_data.reset();
        return true;
    }

    bool OnHostTick() override
    {
        return true;
    }

    std::shared_ptr<TrustedDataPort> m_data;
};

using DataPortStub = TestDataPort;

HostSessionConfig GetGapSessionConfig()
{
    HostRenderViewConfig primary;
    primary.id = "gap-primary";
    primary.role = HostRenderViewRole::Primary3D;
    primary.window.viewInit.viewMode =
        HostRenderMode::CompositeIsoSurface;
    primary.window.viewInit.hasIso = true;
    primary.window.viewInit.isoThreshold = 0.5;

    HostRenderViewConfig slice;
    slice.id = "gap-slice";
    slice.role = HostRenderViewRole::TopDownSlice;
    slice.window.viewInit.viewMode =
        HostRenderMode::SliceTopDown;

    HostSessionConfig config;
    config.renderViews.push_back(
        std::move(primary));
    config.renderViews.push_back(
        std::move(slice));
    return config;
}

GapHostConfig GetGapConfig()
{
    GapHostConfig config;
    config.defaultStart.targetViews.viewIds = {
        "gap-primary", "gap-slice" };
    config.defaultStart.surface.isoMode =
        GapIsoMode::AbsoluteValue;
    config.defaultStart.surface.absoluteIsoValue = 0.172;
    config.defaultStart.surface.backgroundMean = -1.617f;
    config.defaultStart.surface.materialMean = 0.453f;
    config.defaultStart.voidParams.isFilterEnabled = false;
    config.defaultStart.voidParams.minVolumeMM3 = 0.0;
    config.inputViews.viewIds = { "gap-primary" };
    config.keys.switchOverlay.keyCode = 'j';
    config.keys.exit.keySym = "Escape";
    return config;
}

GapHostRequest GetGapRequest(const GapHostAction action)
{
    GapHostRequest request;
    request.action = action;
    return request;
}

GapHostRequest GetStartRequest(
    const GapHostStartParams& params)
{
    auto request = GetGapRequest(GapHostAction::Start);
    request.start = params;
    return request;
}

bool SendReload(
    VtkAppHostSession& session,
    bool& isComplete,
    bool& isSucceeded)
{
    constexpr int side = 5;
    HostReloadRequest reload;
    reload.voxels.resize(side * side * side);
    for (int z = 0; z < side; ++z) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const bool isBoundary =
                    x == 0 || x == side - 1
                    || y == 0 || y == side - 1
                    || z == 0 || z == side - 1;
                const auto index = static_cast<std::size_t>(
                    x + side * (y + side * z));
                reload.voxels[index] =
                    isBoundary ? 1.0f : 0.0f;
            }
        }
    }
    reload.geometry.dimensions = { side, side, side };
    reload.geometry.spacing = { 1.0f, 1.0f, 1.0f };
    reload.geometry.origin = { 0.0f, 0.0f, 0.0f };

    return session.SendRequest(
        std::move(reload),
        [&isComplete, &isSucceeded](const bool value) {
            isSucceeded = value;
            isComplete = true;
        });
}

void SendTicks(
    const HostRenderViewEndpoint& endpoint,
    const int tickCount)
{
    int timerId = endpoint.interactor->GetTimerEventId();
    if (timerId == 0) {
        for (int candidate = 1; candidate <= 64; ++candidate) {
            if (endpoint.interactor->GetTimerDuration(candidate) != 0) {
                timerId = candidate;
                break;
            }
        }
    }
    if (timerId == 0) return;
    for (int tick = 0; tick < tickCount; ++tick) {
        endpoint.interactor->InvokeEvent(
            vtkCommand::TimerEvent, &timerId);
    }
}

bool GetKeyHandled(
    const HostRenderViewEndpoint& endpoint,
    const char keyCode,
    const char* keySym)
{
    endpoint.interactor->SetKeyEventInformation(
        0, 0, keyCode, 0, keySym);
    const bool isPressed =
        endpoint.interactor->InvokeEvent(
            vtkCommand::KeyPressEvent) != 0;
    const bool isReleased =
        endpoint.interactor->InvokeEvent(
            vtkCommand::KeyReleaseEvent) != 0;
    return isPressed && isReleased;
}

bool GetReloadReady(
    VtkAppHostSession& session,
    const HostRenderViewEndpoint& endpoint)
{
    bool isComplete = false;
    bool isSucceeded = false;
    const bool isSent = SendReload(
        session, isComplete, isSucceeded);
    for (int poll = 0;
        isSent && !isComplete && poll < 500;
        ++poll) {
        SendTicks(endpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    return isSent && isComplete && isSucceeded;
}

template <typename PropType>
int GetPropCount(vtkRenderer* renderer)
{
    if (!renderer) {
        return 0;
    }
    auto* props = renderer->GetViewProps();
    if (!props) {
        return 0;
    }
    int count = 0;
    props->InitTraversal();
    while (auto* prop = props->GetNextProp()) {
        if (PropType::SafeDownCast(prop)) {
            ++count;
        }
    }
    return count;
}

bool GetFormalGapResultValid(
    const TrustedDataPort& data,
    const GapHostState& state)
{
    const auto graph = data.GetDataGraph();
    const auto labels = data.GetData(graph, state.labelMap);
    const auto voids = data.GetData(graph, state.voidTable);
    const auto mesh = data.GetData(graph, state.voidMesh);
    const auto statistics = data.GetData(graph, state.statisticsData);
    const auto result = data.GetData(graph, state.resultSet);
    const auto* voidTable = voids
        ? dynamic_cast<const RecordTablePayload*>(voids->payload.get())
        : nullptr;
    const auto* statisticsTable = statistics
        ? dynamic_cast<const RecordTablePayload*>(statistics->payload.get())
        : nullptr;
    const auto* resultSet = result
        ? dynamic_cast<const DataCollectionPayload*>(result->payload.get())
        : nullptr;
    if (!labels || !voidTable || !mesh || !statisticsTable || !resultSet
        || !voidTable->GetValid() || !statisticsTable->GetValid()
        || !resultSet->GetValid()) {
        return false;
    }

    constexpr std::array<std::string_view, 27> voidColumns = {
        "void-id", "voxel-count", "volume-mm3",
        "equivalent-diameter-mm", "radius-mm", "diameter-mm",
        "center-mm", "centroid-mm", "voxel-bbox", "seed-voxel",
        "gray-min", "gray-max", "gray-mean",
        "gray-standard-deviation", "gray-deviation", "gap-mm",
        "compactness", "surface-area-mm2", "sphericity",
        "pca-deviation", "pca-maximum-deviation-ratio",
        "pca-minimum-deviation-ratio", "projected-area-x-mm2",
        "projected-area-y-mm2", "projected-area-z-mm2",
        "projected-size-voxel", "defect-probability"
    };
    const auto& columns = voidTable->GetColumns();
    const bool hasEveryVoidColumn = columns.size() == voidColumns.size()
        && std::all_of(
            voidColumns.begin(), voidColumns.end(),
            [&columns](const std::string_view name) {
                return std::any_of(
                    columns.begin(), columns.end(),
                    [name](const RecordColumn& column) {
                        return column.name == name;
                    });
            });
    const auto hasResultItem = [resultSet](
        const std::string_view role,
        const DataRevisionRef& ref) {
        return std::any_of(
            resultSet->GetItems().begin(), resultSet->GetItems().end(),
            [role, &ref](const DataCollectionEntry& item) {
                return item.role == role && item.data == ref;
            });
    };
    return hasEveryVoidColumn
        && statisticsTable->GetColumns().size() == 5
        && resultSet->GetItems().size() == 4
        && hasResultItem("labels", state.labelMap)
        && hasResultItem("void-regions", state.voidTable)
        && hasResultItem("void-surface", state.voidMesh)
        && hasResultItem("statistics", state.statisticsData);
}

} // namespace

int GetGapFailCount()
{
    int failureCount = 0;
    const auto directHost =
        std::make_shared<HostControlStub>();
    HostFeatureContext directContext;
    directContext.views =
        std::make_shared<ViewDirectoryStub>();
    directContext.data = std::make_shared<DataPortStub>();
    directContext.host = directHost;
    GapHostFeature standaloneFeature(GetGapConfig());
    const bool isStandaloneRejected =
        !standaloneFeature.AttachHost(directContext);
    auto directFeature =
        std::make_shared<GapHostFeature>(
            GetGapConfig());
    auto duplicateFeature =
        std::make_shared<GapHostFeature>(
            GetGapConfig());
    const auto directInitialState =
        directFeature->GetState();
    const bool isDirectAttached =
        directFeature->AttachHost(directContext);
    const bool isDuplicateRejected =
        !directFeature->AttachHost(directContext);
    const bool isDuplicateIdRejected =
        !duplicateFeature->AttachHost(directContext);
    const bool isDirectDetached =
        directFeature->DetachHost();
    const auto directDetachedState =
        directFeature->GetState();
    failureCount += GetCaseResult(
        directInitialState.analysisState
                == GapAnalysisState::Idle
            && !directInitialState.isViewActive
            && isStandaloneRejected
            && isDirectAttached
            && isDuplicateRejected
            && isDuplicateIdRejected
            && isDirectDetached
            && directDetachedState.analysisState
                == GapAnalysisState::Idle
            && directDetachedState.statistics.objectVoxelCount == 0
            && directHost->GetAttachCount() == 1
            && directHost->GetDetachCount() == 1,
        "Gap attachment requires shared ownership and preserves one input binding") ? 0 : 1;

    VtkAppHostSession session(GetGapSessionConfig());
    auto feature = std::make_shared<GapHostFeature>(
        GetGapConfig());
    const auto start = GetGapConfig().defaultStart;
    int unattachedCallbackCount = 0;
    const bool isUnattachedRejected =
        !feature->SendRequest(
            GetStartRequest(start),
            [&unattachedCallbackCount](GapHostResult) {
                ++unattachedCallbackCount;
            });
    const auto beforeUseCount = feature.use_count();
    const bool isBuilt = session.BuildSession();
    auto contextProbe = std::make_shared<ContextProbeFeature>();
    const bool isProbeAttached =
        isBuilt && session.AttachFeature(contextProbe);
    const bool isAttached =
        isProbeAttached && session.AttachFeature(feature);
    const bool isInputAttached =
        session.AttachHotkeys({});
    const auto* endpoint = session.GetPrimaryEndpoint();
    const auto* sliceEndpoint =
        session.GetRenderViewEndpoint("gap-slice");
    if (!isBuilt || !isAttached || !isInputAttached
        || !contextProbe->m_data
        || !endpoint
        || !endpoint->renderer
        || !endpoint->interactor
        || !endpoint->renderWindow
        || !sliceEndpoint
        || !sliceEndpoint->renderer) {
        GetCaseResult(
            false,
            "Gap fixture builds the public Session/Feature chain");
        return 1;
    }
    endpoint->renderWindow->SetOffScreenRendering(1);
    endpoint->renderWindow->SetSize(160, 160);

    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = {
        "gap-primary", false,
        HostRenderViewRole::Primary3D };
    const bool isTimerAttached =
        session.AttachTimer(timer);
    const bool isStarted = isTimerAttached && session.Start();
    const bool isReloadReady =
        GetReloadReady(session, *endpoint);
    const auto primaryBasePropCount =
        endpoint->renderer->GetViewProps()->GetNumberOfItems();
    const auto sliceBasePropCount =
        sliceEndpoint->renderer->GetViewProps()->GetNumberOfItems();
    const int primaryBaseActorCount =
        GetPropCount<vtkActor>(endpoint->renderer);
    const int sliceBaseImageCount =
        GetPropCount<vtkImageSlice>(sliceEndpoint->renderer);
    failureCount += GetCaseResult(
        isTimerAttached
            && isStarted
            && isReloadReady
            && isUnattachedRejected
            && unattachedCallbackCount == 0
            && feature.use_count() == beforeUseCount + 1,
        "Session owns the attached Gap Feature and publishes input") ? 0 : 1;

    GapHostStartParams missingTarget =
        GetGapConfig().defaultStart;
    missingTarget.targetViews = {};
    int invalidStartCallbackCount = 0;
    const bool isStrict =
        !feature->SendRequest(GetGapRequest(
            GapHostAction::None))
        && !feature->SendRequest(GetGapRequest(
            GapHostAction::Start))
        && !feature->SendRequest(
            GetStartRequest(missingTarget),
            [&invalidStartCallbackCount](GapHostResult) {
                ++invalidStartCallbackCount;
            })
        && !feature->SendRequest(GetGapRequest(
            GapHostAction::Overlay))
        && !feature->SendRequest(GetGapRequest(
            GapHostAction::Exit))
        && !feature->SendRequest(
            GetGapRequest(GapHostAction::Overlay),
            [](GapHostResult) {});
    bool isWrongThreadAccepted = true;
    int wrongThreadCallbackCount = 0;
    std::thread wrongThread([&] {
        isWrongThreadAccepted = feature->SendRequest(
            GetStartRequest(start),
            [&wrongThreadCallbackCount](GapHostResult) {
                ++wrongThreadCallbackCount;
            });
    });
    wrongThread.join();
    failureCount += GetCaseResult(
        isStrict
            && invalidStartCallbackCount == 0
            && !isWrongThreadAccepted
            && wrongThreadCallbackCount == 0
            && feature->GetState().analysisState
                == GapAnalysisState::Idle,
        "Gap request fields, callback actions and owner thread stay strict") ? 0 : 1;

    const bool isHotkeyStartHandled =
        GetKeyHandled(*endpoint, 'j', "j");
    for (int poll = 0;
        feature->GetState().analysisState
                != GapAnalysisState::Succeeded
            && poll < 500;
        ++poll) {
        SendTicks(*endpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    SendTicks(*endpoint, 1);
    const auto hotkeyStartState = feature->GetState();
    const bool hasFormalGapResult = GetFormalGapResultValid(
        *contextProbe->m_data, hotkeyStartState);
    const auto hotkeyPrimaryPropCount =
        endpoint->renderer->GetViewProps()->GetNumberOfItems();
    const auto hotkeySlicePropCount =
        sliceEndpoint->renderer->GetViewProps()->GetNumberOfItems();
    const int hotkeyPrimaryActorCount =
        GetPropCount<vtkActor>(endpoint->renderer);
    const int hotkeySliceImageCount =
        GetPropCount<vtkImageSlice>(sliceEndpoint->renderer);
    const bool isExitKeyHandled =
        GetKeyHandled(*endpoint, 0, "Escape");
    for (int poll = 0;
        feature->GetState().isExitPending
            && poll < 500;
        ++poll) {
        SendTicks(*endpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    SendTicks(*endpoint, 1);
    const auto hotkeyExitState = feature->GetState();
    const auto hotkeyExitPrimaryPropCount =
        endpoint->renderer->GetViewProps()->GetNumberOfItems();
    const auto hotkeyExitSlicePropCount =
        sliceEndpoint->renderer->GetViewProps()->GetNumberOfItems();
    const int hotkeyExitPrimaryActorCount =
        GetPropCount<vtkActor>(endpoint->renderer);
    const int hotkeyExitSliceImageCount =
        GetPropCount<vtkImageSlice>(sliceEndpoint->renderer);
    failureCount += GetCaseResult(
        isHotkeyStartHandled
            && hotkeyStartState.analysisState
                == GapAnalysisState::Succeeded
            && hotkeyStartState.isViewActive
            && hasFormalGapResult
            && hotkeyPrimaryPropCount
                == primaryBasePropCount + 1
            && hotkeySlicePropCount
                == sliceBasePropCount + 1
            && hotkeyPrimaryActorCount
                == primaryBaseActorCount + 1
            && hotkeySliceImageCount
                == sliceBaseImageCount + 1
            && isExitKeyHandled
            && hotkeyExitState.analysisState
                == GapAnalysisState::Succeeded
            && hotkeyExitState.commitId
                == hotkeyStartState.commitId
            && hotkeyExitState.sourceRevision
                == hotkeyStartState.sourceRevision
            && hotkeyExitState.labelMap
                == hotkeyStartState.labelMap
            && hotkeyExitState.voidTable
                == hotkeyStartState.voidTable
            && hotkeyExitState.voidMesh
                == hotkeyStartState.voidMesh
            && hotkeyExitState.statisticsData
                == hotkeyStartState.statisticsData
            && hotkeyExitState.resultSet
                == hotkeyStartState.resultSet
            && !hotkeyExitState.isViewActive
            && !hotkeyExitState.isExitPending
            && hotkeyExitPrimaryPropCount
                == primaryBasePropCount
            && hotkeyExitSlicePropCount
                == sliceBasePropCount
            && hotkeyExitPrimaryActorCount
                == primaryBaseActorCount
            && hotkeyExitSliceImageCount
                == sliceBaseImageCount,
        "Gap Start reuses one result in mesh and slice views, then Exit removes both") ? 0 : 1;

    int firstCompleteCount = 0;
    bool isFirstSucceeded = false;
    const auto ownerThread = std::this_thread::get_id();
    std::thread::id callbackThread;
    const bool isFirstAccepted =
        feature->SendRequest(
            GetStartRequest(start),
            [&firstCompleteCount, &isFirstSucceeded,
                &callbackThread](
                GapHostResult result) {
                ++firstCompleteCount;
                isFirstSucceeded = result.status
                        == GapResultStatus::Succeeded
                    || result.status
                        == GapResultStatus::SucceededWithDisplayFailure;
                callbackThread = std::this_thread::get_id();
            });
    const auto acceptedState = feature->GetState();
    bool hasRejectedCallback = false;
    const bool isSecondRejected =
        !feature->SendRequest(
            GetStartRequest(start),
            [&hasRejectedCallback](GapHostResult) {
                hasRejectedCallback = true;
            });
    for (int poll = 0;
        isFirstAccepted
            && firstCompleteCount == 0
            && poll < 500;
        ++poll) {
        SendTicks(*endpoint, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    failureCount += GetCaseResult(
        isFirstAccepted
            && acceptedState.analysisState
                != GapAnalysisState::Idle
            && acceptedState.isViewActive
            && !acceptedState.isExitPending
            && isSecondRejected,
        "Accepted Gap Start keeps its view when a second Start is rejected") ? 0 : 1;
    failureCount += GetCaseResult(
        firstCompleteCount == 1
            && isFirstSucceeded
            && callbackThread != std::thread::id{}
            && callbackThread == ownerThread
            && !hasRejectedCallback,
        "Gap Start callback runs once on the owner thread") ? 0 : 1;
    failureCount += GetCaseResult(
        feature->GetState().statistics.voidVoxelCount > 0
            && feature->GetState().statistics.objectVoxelCount
                + feature->GetState().statistics.voidVoxelCount == 125
            && feature->GetState().analysisState
                == GapAnalysisState::Succeeded,
        "Rejected Gap Start preserves the accepted analysis result") ? 0 : 1;

    endpoint->interactor->SetKeyEventInformation(
        0, 0, 'j', 0, "j");
    const bool isOverlayKeyHandled =
        endpoint->interactor->InvokeEvent(
            vtkCommand::KeyPressEvent) != 0;
    const bool isOverlayReleaseHandled =
        endpoint->interactor->InvokeEvent(
            vtkCommand::KeyReleaseEvent) != 0;
    const bool isOverlaySwitched =
        feature->SendRequest(GetGapRequest(
            GapHostAction::Overlay));
    failureCount += GetCaseResult(
        isOverlayKeyHandled
            && isOverlayReleaseHandled
            && isOverlaySwitched,
        "Gap 热键与显式请求必须进入同一 Feature 动作链") ? 0 : 1;

    const bool isNextReloadReady =
        GetReloadReady(session, *endpoint);
    SendTicks(*endpoint, 2);
    const auto staleState = feature->GetState();
    const bool isStaleOverlayRejected =
        !feature->SendRequest(GetGapRequest(
            GapHostAction::Overlay));
    failureCount += GetCaseResult(
        isNextReloadReady
            && isStaleOverlayRejected
            && staleState.analysisState == GapAnalysisState::Stale
            && staleState.statistics.objectVoxelCount > 0
            && !staleState.isViewActive
            && !staleState.isExitPending,
        "Primary Binding change exits the stale Gap view without deleting history") ? 0 : 1;

    const bool isFirstDetached =
        session.DetachFeature(*feature);
    auto pendingFeature =
        std::make_shared<GapHostFeature>(
            GetGapConfig());
    const auto pendingUseCount =
        pendingFeature.use_count();
    const bool isPendingAttached =
        session.AttachFeature(pendingFeature);
    int detachedCallbackCount = 0;
    bool isDetachedCallbackSucceeded = true;
    const bool isPendingAccepted =
        pendingFeature->SendRequest(
            GetStartRequest(start),
            [&detachedCallbackCount,
                &isDetachedCallbackSucceeded](const GapHostResult& result) {
                ++detachedCallbackCount;
                isDetachedCallbackSucceeded = result.status == GapResultStatus::Succeeded;
            });
    const bool isDetached =
        session.DetachFeature(*pendingFeature);
    const auto detachedState = pendingFeature->GetState();
    SendTicks(*endpoint, 2);
    int detachedSendCount = 0;
    const bool isDetachedRequestRejected =
        !pendingFeature->SendRequest(
            GetStartRequest(start),
            [&detachedSendCount](GapHostResult) {
                ++detachedSendCount;
            });
    failureCount += GetCaseResult(
        isFirstDetached
            && isPendingAttached
            && isPendingAccepted,
        "Gap accepts a pending request before detach") ? 0 : 1;
    failureCount += GetCaseResult(
        isDetached
            && isDetachedRequestRejected
            && detachedSendCount == 0
            && detachedState.analysisState
                == GapAnalysisState::Idle
            && detachedState.statistics.objectVoxelCount == 0
            && !detachedState.isViewActive
            && !detachedState.isExitPending,
        "Gap detach succeeds on the Session owner thread") ? 0 : 1;
    failureCount += GetCaseResult(
        pendingFeature.use_count() == pendingUseCount,
        "Gap detach keeps the upper owner alive") ? 0 : 1;
    failureCount += GetCaseResult(
        detachedCallbackCount == 1
            && !isDetachedCallbackSucceeded,
        "Gap detach cancels an accepted callback exactly once") ? 0 : 1;
    return failureCount;
}
