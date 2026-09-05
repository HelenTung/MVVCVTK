#include "QtHostMethodCases.h"

#include "Host/PartSegmentationHostFeature.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/VtkAppHostSession.h"
#include "App/Services/FeatureViewService.h"

#include <vtkCommand.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkWin32OpenGLRenderWindow.h>
#include <vtkObjectFactory.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr auto featureId = "PartSegmentation";
constexpr auto primaryViewId = "part-scene-primary";
constexpr auto timerViewId = "part-scene-timer";

class PartRenderWindow final : public vtkWin32OpenGLRenderWindow {
public:
    static PartRenderWindow* New();
    vtkTypeMacro(PartRenderWindow, vtkWin32OpenGLRenderWindow);
    void Render() override
    {
        if (isFailing) throw std::runtime_error("Part render retry probe");
        vtkWin32OpenGLRenderWindow::Render();
    }
    bool isFailing = false;
};
vtkStandardNewMacro(PartRenderWindow);

class PartViewProbe final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override { return "PartViewProbe"; }
    bool AttachHost(const HostFeatureContext& context) override { views = context.views; return true; }
    bool DetachHost() override { views.reset(); return true; }
    bool OnHostTick() override { return true; }
    std::shared_ptr<FeatureViewDirectory> views;
};

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
    primary.inputMode = HostInputMode::HostInjected;
    primary.renderWindow = vtkSmartPointer<PartRenderWindow>::New();
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
    config.isSelectionEnabled = true;
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
    auto viewProbe = std::make_shared<PartViewProbe>();
    const bool isBuilt = session.BuildSession();
    const bool isAttached = isBuilt && session.AttachFeature(feature) && session.AttachFeature(viewProbe);
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
    auto firstSnapshot = feature->GetPartSetSnapshot();
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

    const auto joined = session.GetStateSnapshot();
    bool hasWrongThreadSnapshot = true;
    std::thread reader([&] { hasWrongThreadSnapshot = session.GetStateSnapshot().has_value(); });
    reader.join();
    failureCount += GetCaseResult(joined && !hasWrongThreadSnapshot
        && joined->sessionGeneration != 0 && joined->operations.size() == 1
        && joined->operations.front().status == FeatureRunStatus::Succeeded
        && joined->operations.front().outputs.size() == 3
        && primaryScene->displays.size() == 1
        && primaryScene->displays.front().operation == joined->operations.front().operation,
        "Combined snapshot correlates published operations and displays on owner thread") ? 0 : 1;

    HostSemanticTarget oldTarget;
    if (!primaryScene->displays.empty()) oldTarget.display = primaryScene->displays.front();
    oldTarget.sceneEpoch = primaryScene->sceneEpoch;
    oldTarget.resultRevision = firstSnapshot->resultRevision;
    constexpr char digits[] = "0123456789abcdef";
    oldTarget.objectId.assign(32, '0');
    const auto objectId = firstSnapshot->parts.front().binding.object.objectId;
    for (std::size_t index = 0; index < 16; ++index) {
        oldTarget.objectId[15 - index] = digits[(objectId.high >> (index * 4)) & 15U];
        oldTarget.objectId[31 - index] = digits[(objectId.low >> (index * 4)) & 15U];
    }
    auto* renderer = primary->renderer;
    const auto point = firstSnapshot->parts.front().metrics.centroidInputPhysical;
    const auto port = viewProbe->views->GetFeaturePort(primaryViewId);
    const auto modelToWorld = port->GetModelToWorld();
    std::array<double, 3> world = point;
    if (modelToWorld) {
        for (std::size_t row = 0; row < 3; ++row) {
            world[row] = (*modelToWorld)[row * 4 + 3];
            for (std::size_t axis = 0; axis < 3; ++axis)
                world[row] += (*modelToWorld)[row * 4 + axis] * point[axis];
        }
    }
    renderer->SetWorldPoint(world[0], world[1], world[2], 1.0);
    renderer->WorldToDisplay();
    const auto* pixel = renderer->GetDisplayPoint();
    const int x = static_cast<int>(pixel[0]);
    const int y = static_cast<int>(pixel[1]);
    const auto sendPointer = [&](const HostInputKind kind) {
        return session.GetInputEndpoint()->SendInput({ primaryViewId, kind, x, y });
    };
    const auto cancelledPress = sendPointer(HostInputKind::PrimaryPress);
    const auto cancel = sendPointer(HostInputKind::Cancel);
    const auto cancelled = session.GetStateSnapshot();
    failureCount += GetCaseResult(cancelledPress.isHandled && cancel.isSucceeded && cancelled
        && joined && cancelled->graphCommitId == joined->graphCommitId
        && feature->GetPartSetSnapshot()->catalogRevision == firstSnapshot->catalogRevision,
        "Cancel discards the real Part preview without publishing a data revision") ? 0 : 1;
    (void)sendPointer(HostInputKind::PrimaryPress);
    for (int move = 0; move < 4; ++move) (void)sendPointer(HostInputKind::PointerMove);
    const auto preview = session.GetStateSnapshot();
    const auto previewCatalog = feature->GetPartSetSnapshot();
    (void)sendPointer(HostInputKind::PrimaryRelease);
    const auto selected = feature->GetPartSetSnapshot();
    if (!selected || selected->catalogRevision == firstSnapshot->catalogRevision) {
        std::cerr << "Semantic pick diagnostic: pixel=" << x << ',' << y
            << " world=" << point[0] << ',' << point[1] << ',' << point[2]
            << " display=" << primaryScene->displays.size()
            << " epoch=" << primaryScene->sceneEpoch << '/' << primaryScene->renderedEpoch
            << " previewGraph=" << (preview ? preview->graphCommitId : 0)
            << " beforeGraph=" << (joined ? joined->graphCommitId : 0) << '\n';
    }
    failureCount += GetCaseResult(joined && preview && previewCatalog && selected
        && preview->graphCommitId == joined->graphCommitId
        && previewCatalog->catalogRevision == firstSnapshot->catalogRevision
        && selected->catalogRevision == firstSnapshot->catalogRevision + 1
        && selected->parts.front().presentation.isSelected,
        "Real Part picking keeps drag preview transient and commits one selection on Release") ? 0 : 1;
    firstSnapshot = selected;
    (void)SendTimer(timer->interactor);
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

    auto* renderProbe = PartRenderWindow::SafeDownCast(primary->renderWindow);
    renderProbe->isFailing = true;
    HostViewSetRequest appearance;
    appearance.targetView = { primaryViewId, false, HostRenderViewRole::Primary3D };
    appearance.background = HostBackgroundColor{ 0.13, 0.17, 0.21 };
    (void)session.SendRequest(std::move(appearance));
    (void)SendTimer(timer->interactor);
    auto replacement = std::make_shared<std::optional<PartSegmentationResult>>();
    PartSegmentationRequest replacementRequest;
    replacementRequest.action = PartSegmentationAction::Start;
    const auto replacementAdmission = feature->SendRequest(replacementRequest,
        [replacement](PartSegmentationResult value) { *replacement = std::move(value); });
    const auto oldScene = session.GetSceneViewState({ primaryViewId, false, HostRenderViewRole::Primary3D });
    std::optional<HostStateSnapshot> ready;
    for (int poll = 0; poll < 5000; ++poll) {
        ready = session.GetStateSnapshot();
        if (ready && std::any_of(ready->operations.begin(), ready->operations.end(),
            [&](const auto& value) { return value.operation.requestId == replacementAdmission.requestId
                && value.status == FeatureRunStatus::Ready; })) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto sameReady = session.GetStateSnapshot();
    failureCount += GetCaseResult(ready && sameReady && ready->operations.size() == 2
        && ready->operations.front().status == FeatureRunStatus::Ready
        && ready->operations.front().outputs.empty()
        && ready->operations.front().stateRevision == sameReady->operations.front().stateRevision
        && oldScene && ready->scenes.front().sceneEpoch == oldScene->sceneEpoch
        && oldScene->renderedEpoch < oldScene->sceneEpoch
        && ready->scenes.front().displays == oldScene->displays && !replacement->has_value(),
        "Worker Ready advances during RenderPending while the committed old display remains") ? 0 : 1;
    renderProbe->isFailing = false;
    const bool replacementPumped = replacementAdmission.status == PartAdmissionStatus::Accepted
        && PumpUntil(*primary, *timer, [replacement] { return replacement->has_value(); });
    const auto replacementResult = replacementPumped ? *replacement : std::nullopt;
    const auto replacementSnapshot = feature->GetPartSetSnapshot();
    const auto* replacementPart = replacementSnapshot
        ? GetPartByObject(*replacementSnapshot, firstObject) : nullptr;
    failureCount += GetCaseResult(replacementSnapshot
        && feature->SetPartState(oldTarget, PartStatePatch{}, replacementSnapshot->catalogRevision).status
            == PartMutationStatus::StaleReference,
        "Semantic target from result A cannot mutate replacement B with the same stable object") ? 0 : 1;
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
