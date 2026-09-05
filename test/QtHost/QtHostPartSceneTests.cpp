#include "QtHostMethodCases.h"

#include "Host/PartSegmentationHostFeature.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/VtkAppHostSession.h"

#include <vtkCommand.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr auto featureId = "PartSegmentation";
constexpr auto primaryViewId = "part-scene-primary";
constexpr auto timerViewId = "part-scene-timer";

struct TestPartNode final {
    PartObjectId objectId;
    std::string name;
    bool isVisible = true;
    PartMetrics metrics;
};

struct TestPartSetNode final {
    PartSetId partSetId;
    std::uint64_t resultRevision = 0;
    std::uint64_t catalogRevision = 0;
    std::vector<TestPartNode> parts;
};

struct TestVolumeNode final {
    DataRevisionRef sourceRevision;
    std::optional<TestPartSetNode> partSet;
};

static_assert(!std::is_pointer_v<decltype(TestPartNode::objectId)>);
static_assert(!std::is_pointer_v<decltype(TestPartNode::metrics)>);
static_assert(!std::is_pointer_v<decltype(TestPartSetNode::partSetId)>);

HostSessionConfig GetSessionConfig()
{
    HostSessionConfig config;
    HostRenderViewConfig primary;
    primary.id = primaryViewId;
    primary.role = HostRenderViewRole::Primary3D;
    primary.window.viewInit.viewMode = HostRenderMode::CompositeIsoSurface;
    primary.window.viewInit.hasIso = true;
    primary.window.viewInit.isoThreshold = 0.5;
    config.renderViews.push_back(std::move(primary));

    HostRenderViewConfig timer;
    timer.id = timerViewId;
    timer.role = HostRenderViewRole::Auxiliary;
    timer.window.viewInit.viewMode = HostRenderMode::Volume;
    config.renderViews.push_back(std::move(timer));
    return config;
}

PartSegmentationConfig GetPartConfig()
{
    PartSegmentationConfig config;
    config.defaultStart.targetViews.viewIds = { primaryViewId };
    config.defaultStart.threshold = 0.5;
    config.defaultStart.minPartVoxels = 1;
    return config;
}

HostReloadRequest GetReload(const bool isReplacementSource = false)
{
    constexpr int side = 8;
    HostReloadRequest reload;
    reload.metadata.identity.datasetId = isReplacementSource ? "part-scene-replacement" : "part-scene-source";
    reload.metadata.source.kind = ImageSourceKind::Memory;
    reload.metadata.source.uri = "memory://" + reload.metadata.identity.datasetId;
    reload.voxels.resize(
        static_cast<std::size_t>(side) * side * side,
        isReplacementSource ? 0.1F : 0.0F);
    for (int z = 1; z <= 2; ++z) {
        for (int y = 1; y <= 2; ++y) {
            for (int x = 1; x <= 2; ++x) {
                const auto index = static_cast<std::size_t>(
                    x + side * (y + side * z));
                reload.voxels[index] = 1.0F;
            }
        }
    }
    for (int z = 5; z <= 6; ++z) {
        for (int y = 5; y <= 6; ++y) {
            for (int x = 5; x <= 6; ++x) {
                const auto index = static_cast<std::size_t>(
                    x + side * (y + side * z));
                reload.voxels[index] = 1.0F;
            }
        }
    }
    reload.geometry.dimensions = { side, side, side };
    reload.geometry.spacing = { 0.5F, 0.75F, 1.25F };
    reload.geometry.origin = { 10.0F, 20.0F, 30.0F };
    return reload;
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

template <typename Predicate>
bool PumpUntil(
    const HostRenderViewEndpoint& primary,
    const HostRenderViewEndpoint& timer,
    Predicate predicate)
{
    // 首次建立两个窗口的渲染上下文；之后只驱动已显式绑定的 Session timer。
    if (primary.renderWindow->GetNeverRendered()) primary.renderWindow->Render();
    if (timer.renderWindow->GetNeverRendered()) timer.renderWindow->Render();
    for (int poll = 0; poll < 1000; ++poll) {
        if (predicate()) return true;
        if (!SendTimer(timer.interactor)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

bool Reload(
    VtkAppHostSession& session,
    const HostRenderViewEndpoint& primary,
    const HostRenderViewEndpoint& timer,
    HostReloadRequest reload)
{
    struct Completion final {
        bool isComplete = false;
        bool isSucceeded = false;
    };
    const auto completion = std::make_shared<Completion>();
    const bool isAccepted = session.SendRequest(
        std::move(reload),
        [completion](const bool value) {
            completion->isSucceeded = value;
            completion->isComplete = true;
        });
    const bool isPumped = isAccepted
        && PumpUntil(primary, timer, [completion] { return completion->isComplete; });
    if (!isAccepted || !isPumped || !completion->isSucceeded) {
        std::cerr
            << "Part reload diagnostic: accepted=" << isAccepted
            << " pumped=" << isPumped
            << " complete=" << completion->isComplete
            << " succeeded=" << completion->isSucceeded
            << '\n';
    }
    return isAccepted && isPumped && completion->isSucceeded;
}

std::optional<PartSegmentationResult> StartPart(
    PartSegmentationHostFeature& feature,
    const HostRenderViewEndpoint& primary,
    const HostRenderViewEndpoint& timer)
{
    const auto result = std::make_shared<std::optional<PartSegmentationResult>>();
    PartSegmentationRequest request;
    request.action = PartSegmentationAction::Start;
    const auto admission = feature.SendRequest(
        std::move(request),
        [result](PartSegmentationResult value) {
            *result = std::move(value);
        });
    if (admission.status != PartAdmissionStatus::Accepted
        || !PumpUntil(primary, timer, [result] { return result->has_value(); })) {
        return std::nullopt;
    }
    return *result;
}

bool HasFeature(
    const HostSceneViewState& scene,
    const std::string& expectedFeatureId)
{
    return std::find(
        scene.activeFeatureIds.begin(),
        scene.activeFeatureIds.end(),
        expectedFeatureId) != scene.activeFeatureIds.end();
}

TestVolumeNode BuildTree(
    const std::shared_ptr<const PartSetSnapshot>& snapshot)
{
    TestVolumeNode volume;
    if (!snapshot) return volume;
    volume.sourceRevision = snapshot->sourceRevision;
    TestPartSetNode partSet;
    partSet.partSetId = snapshot->partSetId;
    partSet.resultRevision = snapshot->resultRevision;
    partSet.catalogRevision = snapshot->catalogRevision;
    partSet.parts.reserve(snapshot->parts.size());
    for (const auto& part : snapshot->parts) {
        partSet.parts.push_back({
            part.binding.object.objectId,
            part.userState.name,
            part.presentation.isVisible,
            part.metrics
        });
    }
    volume.partSet = std::move(partSet);
    return volume;
}

bool GetTreeMatchesSnapshot(
    const TestVolumeNode& tree,
    const PartSetSnapshot& snapshot)
{
    if (!tree.partSet
        || tree.sourceRevision != snapshot.sourceRevision
        || tree.partSet->partSetId != snapshot.partSetId
        || tree.partSet->resultRevision != snapshot.resultRevision
        || tree.partSet->catalogRevision != snapshot.catalogRevision
        || tree.partSet->parts.size() != snapshot.parts.size()) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.parts.size(); ++index) {
        const auto& node = tree.partSet->parts[index];
        const auto& part = snapshot.parts[index];
        if (node.objectId != part.binding.object.objectId
            || node.name != part.userState.name
            || node.isVisible != part.presentation.isVisible
            || node.metrics != part.metrics) {
            return false;
        }
    }
    return true;
}

bool GetSameNode(const TestPartNode& left, const TestPartNode& right)
{
    return left.objectId == right.objectId
        && left.name == right.name
        && left.isVisible == right.isVisible
        && left.metrics == right.metrics;
}

const PartSnapshot* GetPartByObject(
    const PartSetSnapshot& snapshot,
    const PartObjectId objectId)
{
    const auto found = std::find_if(
        snapshot.parts.begin(), snapshot.parts.end(),
        [objectId](const PartSnapshot& part) {
            return part.binding.object.objectId == objectId;
        });
    return found == snapshot.parts.end() ? nullptr : &*found;
}

} // namespace

int GetPartSceneFailCount()
{
    int failureCount = 0;
    VtkAppHostSession session(GetSessionConfig());
    auto feature = std::make_shared<PartSegmentationHostFeature>(
        GetPartConfig());
    const bool isBuilt = session.BuildSession();
    const bool isAttached = isBuilt && session.AttachFeature(feature);
    const auto* primary = session.GetRenderViewEndpoint(primaryViewId);
    const auto* timer = session.GetRenderViewEndpoint(timerViewId);
    if (!isAttached || !primary || !timer
        || !primary->renderWindow || !timer->renderWindow
        || !primary->interactor || !timer->interactor) {
        GetCaseResult(false,
            "Part scene fixture builds the public Session/Feature chain");
        return 1;
    }
    primary->renderWindow->SetOffScreenRendering(1);
    timer->renderWindow->SetOffScreenRendering(1);
    primary->interactor->Initialize();
    timer->interactor->Initialize();
    HostTimerConfig timerConfig;
    timerConfig.isTimerEnabled = true;
    timerConfig.targetView = {
        timerViewId, false, HostRenderViewRole::Auxiliary };
    const bool isTimerAttached = session.AttachTimer(timerConfig);
    if (!isTimerAttached) {
        std::cerr << "Part timer diagnostic: attach failed\n";
    }
    const bool isStarted = isTimerAttached && session.Start();
    const bool isLoaded = isStarted
        && Reload(session, *primary, *timer, GetReload());
    const auto firstResult = isLoaded
        ? StartPart(*feature, *primary, *timer)
        : std::nullopt;
    const auto firstSnapshot = feature->GetPartSetSnapshot();
    const auto scenes = session.GetSceneViewStates();
    const auto primaryScene = session.GetSceneViewState({
        primaryViewId, false, HostRenderViewRole::Primary3D });
    const auto timerScene = session.GetSceneViewState({
        timerViewId, false, HostRenderViewRole::Auxiliary });
    const bool isSceneJoined = firstResult
        && firstResult->status == PartResultStatus::Succeeded
        && firstSnapshot
        && firstSnapshot->parts.size() == 2
        && scenes.size() == 2
        && primaryScene && HasFeature(*primaryScene, featureId)
        && timerScene && !HasFeature(*timerScene, featureId);
    if (!isSceneJoined) {
        std::cerr
            << "Part scene diagnostic: loaded=" << isLoaded
            << " result=" << firstResult.has_value()
            << " resultStatus=" << (firstResult
                ? static_cast<int>(firstResult->status) : -1)
            << " snapshot=" << static_cast<bool>(firstSnapshot)
            << " parts=" << (firstSnapshot
                ? firstSnapshot->parts.size() : 0)
            << " scenes=" << scenes.size()
            << " primary=" << primaryScene.has_value()
            << " primaryActive=" << (primaryScene
                ? HasFeature(*primaryScene, featureId) : false)
            << " timer=" << timerScene.has_value()
            << " timerActive=" << (timerScene
                ? HasFeature(*timerScene, featureId) : false)
            << '\n';
    }
    failureCount += GetCaseResult(
        isSceneJoined,
        "Part result joins existing Host scene snapshots by active Feature") ? 0 : 1;
    if (!isSceneJoined) {
        (void)session.DetachFeature(*feature);
        (void)session.Stop();
        return failureCount;
    }

    const auto firstTree = BuildTree(firstSnapshot);
    failureCount += GetCaseResult(
        firstTree.partSet
            && firstTree.partSet->parts.size() == 2
            && GetTreeMatchesSnapshot(firstTree, *firstSnapshot),
        "Host-side value tree mirrors Volume, PartSet, and Part snapshots") ? 0 : 1;

    const auto presentationRevision = primaryScene->presentationRevision;
    const auto dataRevision = primaryScene->presentation
        ? primaryScene->presentation->dataRevision : DataRevisionRef{};
    const auto firstObject = firstSnapshot->parts.front().binding.object.objectId;
    PartStatePatch patch;
    patch.name = "reviewed-primary-part";
    patch.isVisible = false;
    patch.isReviewed = true;
    const auto mutation = feature->SetPartState(
        firstSnapshot->parts.front().binding,
        patch,
        firstSnapshot->catalogRevision);
    const auto mutatedSnapshot = feature->GetPartSetSnapshot();
    const auto mutatedScene = session.GetSceneViewState({
        primaryViewId, false, HostRenderViewRole::Primary3D });
    const auto mutatedTree = BuildTree(mutatedSnapshot);
    const bool isOnlyTargetChanged = mutatedTree.partSet
        && firstTree.partSet
        && mutatedTree.partSet->parts.size()
            == firstTree.partSet->parts.size()
        && mutatedTree.partSet->parts.front().objectId == firstObject
        && mutatedTree.partSet->parts.front().name == *patch.name
        && !mutatedTree.partSet->parts.front().isVisible
        && GetSameNode(
            mutatedTree.partSet->parts.back(),
            firstTree.partSet->parts.back());
    failureCount += GetCaseResult(
        mutation.status == PartMutationStatus::Succeeded
            && mutation.catalogRevision == firstSnapshot->catalogRevision + 1
            && mutatedSnapshot
            && mutatedSnapshot->resultRevision == firstSnapshot->resultRevision
            && mutatedScene
            && mutatedScene->presentationRevision == presentationRevision
            && mutatedScene->presentation
            && mutatedScene->presentation->dataRevision == dataRevision
            && isOnlyTargetChanged,
        "Part mutation changes only the joined Part node and catalog revision") ? 0 : 1;

    const bool isFrameSent = SendTimer(timer->interactor);
    const auto renderedMutation = session.GetSceneViewState({
        primaryViewId, false, HostRenderViewRole::Primary3D });
    failureCount += GetCaseResult(
        isFrameSent && renderedMutation
            && renderedMutation->sceneEpoch > primaryScene->sceneEpoch
            && renderedMutation->renderedEpoch == renderedMutation->sceneEpoch,
        "Part presentation mutation renders through the owner frame") ? 0 : 1;

    const auto replacementResult = StartPart(*feature, *primary, *timer);
    const auto replacementSnapshot = feature->GetPartSetSnapshot();
    const auto* replacementPart = replacementSnapshot
        ? GetPartByObject(*replacementSnapshot, firstObject) : nullptr;
    failureCount += GetCaseResult(
        replacementResult
            && replacementResult->status == PartResultStatus::Succeeded
            && mutatedSnapshot
            && replacementSnapshot
            && replacementSnapshot->partSetId == firstSnapshot->partSetId
            && replacementSnapshot->resultRevision
                == firstSnapshot->resultRevision + 1
            && replacementSnapshot->catalogRevision
                == mutatedSnapshot->catalogRevision + 1
            && replacementPart
            && replacementPart->userState.name == *patch.name
            && !replacementPart->presentation.isVisible,
        "Exact replacement preserves PartSet and Part object identity") ? 0 : 1;

    const bool isReloaded = Reload(
        session, *primary, *timer, GetReload(true));
    const bool isStaleObserved = isReloaded
        && PumpUntil(*primary, *timer, [&feature] {
            const auto snapshot = feature->GetPartSetSnapshot();
            return snapshot && snapshot->isStale;
        });
    const auto staleSnapshot = feature->GetPartSetSnapshot();
    const auto staleScene = session.GetSceneViewState({
        primaryViewId, false, HostRenderViewRole::Primary3D });
    const auto staleMutation = staleSnapshot
        ? feature->SetPartState(
            staleSnapshot->parts.front().binding,
            PartStatePatch{},
            staleSnapshot->catalogRevision)
        : PartMutationResult{};
    failureCount += GetCaseResult(
        isStaleObserved
            && staleSnapshot
            && staleSnapshot->partSetId == firstSnapshot->partSetId
            && staleSnapshot->isStale
            && staleMutation.status == PartMutationStatus::StaleReference
            && staleScene
            && !HasFeature(*staleScene, featureId),
        "Source replacement retains a read-only stale subtree and removes display") ? 0 : 1;

    const auto clearResult =
        std::make_shared<std::optional<PartSegmentationResult>>();
    PartSegmentationRequest clear;
    clear.action = PartSegmentationAction::Clear;
    const auto clearAdmission = feature->SendRequest(
        std::move(clear),
        [clearResult](PartSegmentationResult result) {
            *clearResult = std::move(result);
        });
    const bool isClearPumped = clearAdmission.status == PartAdmissionStatus::Accepted
        && PumpUntil(*primary, *timer, [clearResult] { return clearResult->has_value(); });
    const auto clearedTree = BuildTree(feature->GetPartSetSnapshot());
    failureCount += GetCaseResult(
        clearAdmission.status == PartAdmissionStatus::Accepted
            && isClearPumped && clearResult->has_value()
            && clearResult->value().status == PartResultStatus::Succeeded
            && !feature->GetPartSetSnapshot()
            && !clearedTree.partSet,
        "Clear removes the joined PartSet subtree") ? 0 : 1;

    const bool isDetached = session.DetachFeature(*feature);
    const bool isStopped = session.Stop();
    failureCount += GetCaseResult(
        isDetached && isStopped,
        "Part scene fixture detaches and stops cleanly") ? 0 : 1;
    return failureCount;
}
