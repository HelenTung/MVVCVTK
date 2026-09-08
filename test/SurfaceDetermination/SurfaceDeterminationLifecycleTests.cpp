// 测试用途：验证表面请求的线程归属、取消替换、输入过期、绑定投影和结果提交。
#include "SurfaceDeterminationTestCases.h"

#include "Host/SurfaceDeterminationHostFeature.h"
#include "SurfaceDeterminationService.h"
#include "SurfaceGenerationStore.h"
#include "SurfaceContracts.h"
#include "SurfaceDeterminationTestSupport.h"

#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace SurfaceTest;

class OverlayStub final : public OverlayService {
public:
    bool AttachOverlay(std::shared_ptr<FeatureOverlay> overlay) override
    {
        if (isAttachRejected || !overlay) return false;
        overlays.push_back(std::move(overlay));
        return true;
    }

    void RemoveOverlay(
        std::shared_ptr<FeatureOverlay> overlay) noexcept override
    {
        overlays.erase(
            std::remove(overlays.begin(), overlays.end(), overlay),
            overlays.end());
    }

    void ClearOverlays() noexcept override
    {
        overlays.clear();
    }

    std::vector<std::shared_ptr<FeatureOverlay>> overlays;
    bool isAttachRejected = false;
};

class ViewDirectoryStub final : public FeatureViewDirectory {
public:
    ViewDirectoryStub()
    {
        views.push_back({ "primary", HostRenderViewRole::Primary3D });
    }

    std::vector<HostFeatureView> GetViews(
        const HostViewTargets& targets) const override
    {
        std::vector<HostFeatureView> selected;
        for (const HostFeatureView& view : views) {
            const bool hasId = std::find(
                targets.viewIds.begin(), targets.viewIds.end(), view.id)
                != targets.viewIds.end();
            const bool hasRole = std::find(
                targets.viewRoles.begin(), targets.viewRoles.end(), view.role)
                != targets.viewRoles.end();
            if (hasId || hasRole) selected.push_back(view);
        }
        return selected;
    }

    std::shared_ptr<FeatureViewService> GetFeaturePort(
        const std::string&) const override
    {
        return nullptr;
    }

    std::shared_ptr<OverlayService> GetOverlayPort(
        const std::string& viewId) const override
    {
        return viewId == "primary" ? overlay : nullptr;
    }

    std::optional<HostInputView> GetInputView(
        const HostViewTarget&) const override
    {
        return std::nullopt;
    }

    std::vector<HostFeatureView> views;
    std::shared_ptr<OverlayStub> overlay = std::make_shared<OverlayStub>();
};

class DataPortStub final : public TestDataPort {
public:
    explicit DataPortStub(VtkImageGridSnapshot initial) { SetCurrent(std::move(initial)); }
    std::function<void()> beforeSurfaceCommit;
    DataCommitResult SetDataCommit(DataTransaction transaction) override
    {
        if (beforeSurfaceCommit &&
            std::any_of(transaction.outputs.begin(), transaction.outputs.end(),
                        [](const auto &output) { return output.type == DataTypes::surfaceMesh; }))
        {
            auto before = std::move(beforeSurfaceCommit);
            beforeSurfaceCommit = {};
            before();
        }
        return TestDataPort::SetDataCommit(std::move(transaction));
    }
    void SetCurrent(VtkImageGridSnapshot next)
    {
        if (next) (void)SetPrimaryImage(next->image, next->validityMask);
    }
};

class HostControlStub final : public FeatureHostControl {
public:
    bool AttachInput(HostInputBinding) override { return true; }
    bool DetachInput(std::string_view) override { return true; }

    bool SetActiveViews(
        const std::vector<std::string>& viewIds) override
    {
        if (isActiveViewsRejected) return false;
        activeViews = viewIds;
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
        ++sceneCount;
        if (isSceneRejected || delta.requestId == 0 || delta.viewIds.empty()) return false;
        lastDelta = std::move(delta);
        return true;
    }

    bool SendOwnerComplete(std::function<void()> complete) override
    {
        if (!complete) return false;
        complete();
        return true;
    }

    std::vector<std::string> activeViews;
    FeatureSceneDelta lastDelta;
    bool isActiveViewsRejected = false;
    bool isSceneRejected = false;
    int sceneCount = 0;
};

struct TestHost final {
    explicit TestHost(VtkImageGridSnapshot source)
        : data(std::make_shared<DataPortStub>(std::move(source)))
    {
        context.views = views;
        context.data = data;
        context.host = host;
    }

    std::shared_ptr<ViewDirectoryStub> views =
        std::make_shared<ViewDirectoryStub>();
    std::shared_ptr<DataPortStub> data;
    std::shared_ptr<HostControlStub> host =
        std::make_shared<HostControlStub>();
    HostFeatureContext context;
};

SurfaceDeterminationRequest GetStartRequest(
    const SurfaceDeterminationMethod method =
        SurfaceDeterminationMethod::LocalAdaptiveIso50)
{
    SurfaceDeterminationRequest request;
    request.action = SurfaceDeterminationAction::Start;
    request.start = GetParams(method);
    return request;
}

bool WaitUntil(
    SurfaceDeterminationHostFeature& feature,
    const std::function<bool()>& getDone,
    const std::chrono::milliseconds timeout = std::chrono::seconds(8))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!feature.OnHostTick()) return false;
        if (getDone()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return getDone();
}

SurfaceDeterminationConfig GetConfig()
{
    SurfaceDeterminationConfig config;
    config.maxWorkingBytes = 128U * 1024U * 1024U;
    return config;
}

void TestAttachAndOwnerThread(Checks& checks)
{
    TestHost testHost(BuildSphere());
    auto feature = std::make_shared<SurfaceDeterminationHostFeature>(
        GetConfig());
    HostFeatureContext invalid;
    checks.Get(!feature->AttachHost(invalid), "attach rejects missing ports");
    checks.Get(feature->AttachHost(testHost.context), "attach succeeds");
    checks.Get(!feature->AttachHost(testHost.context), "repeat attach is rejected");

    std::atomic<bool> sendRejected{ false };
    std::atomic<bool> detachRejected{ false };
    std::thread other([&] {
        sendRejected = feature->SendRequest(GetStartRequest()).status
            == SurfaceAdmissionStatus::Unavailable;
        detachRejected = !feature->DetachHost();
    });
    other.join();
    checks.Get(sendRejected.load(), "non-owner request is rejected");
    checks.Get(detachRejected.load(), "non-owner detach is rejected");
    checks.Get(feature->DetachHost(), "owner detach succeeds");
    checks.Get(feature->DetachHost(), "repeat detach is idempotent");
}

void TestSuccessVisibilityAndClear(Checks& checks)
{
    TestHost testHost(BuildSphere());
    auto feature = std::make_shared<SurfaceDeterminationHostFeature>(
        GetConfig());
    checks.Get(feature->AttachHost(testHost.context), "success test attaches");
    std::atomic<int> callbackCount{ 0 };
    SurfaceDeterminationResult completed;
    const auto admission = feature->SendRequest(
        GetStartRequest(),
        [&](SurfaceDeterminationResult result) {
            completed = std::move(result);
            ++callbackCount;
        });
    checks.Get(
        admission.status == SurfaceAdmissionStatus::Accepted,
        "start is accepted");
    checks.Get(
        WaitUntil(*feature, [&] { return callbackCount.load() == 1; }),
        "start completes on owner tick");
    const auto state = feature->GetState();
    const auto snapshot = feature->GetSurfaceSnapshot();
    const auto operations = feature->GetOperationStates();
    const auto repeated = feature->GetOperationStates();
    checks.Get(operations.size() == 1 && repeated.size() == 1
        && operations.front().operation.requestId == admission.requestId
        && operations.front().status == FeatureRunStatus::Succeeded
        && operations.front().stateRevision == repeated.front().stateRevision
        && operations.front().inputs.size() == 1 && operations.front().outputs.size() == 2
        && testHost.host->lastDelta.hasDisplayUpdate && testHost.host->lastDelta.displays.size() == 1
        && testHost.host->lastDelta.displays.front().operation == operations.front().operation,
        "Surface operation and adopted mesh share a stable execution identity");
    checks.Get(
        completed.status == SurfaceResultStatus::Succeeded
            && state.stage == SurfaceDeterminationStage::Ready,
        "successful request reaches Ready");
    checks.Get(
        snapshot && snapshot->resultRevision == 1
            && snapshot->sourceRevision == testHost.data->GetPrimaryImage()->data->self,
        "successful request publishes immutable generation");
    if (snapshot) {
        const auto graph = testHost.data->GetDataGraph();
        const auto data = graph.view->GetData(snapshot->meshRevision);
        const auto mesh = data
            ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(data->payload)
            : nullptr;
        checks.Get(mesh && mesh->GetPointAttributes().size() == 8,
                   "generic mesh publishes measurement quality attributes");
        if (mesh && mesh->GetPointAttributes().size() == 8)
        {
            const auto& attributes = mesh->GetPointAttributes();
            checks.Get(attributes[0].name == "measurement.valid"
                && attributes[0].values.size() == snapshot->points->size()
                && attributes[4].componentCount == 3
                && attributes[4].values.size() == snapshot->points->size() * 3,
                "quality schema aligns with exact measurement vertices");
            for (std::size_t index = 0; index < snapshot->points->size(); ++index) {
                if (attributes[0].values[index] != 1.0) continue;
                checks.Get(attributes[1].values[index] == (*snapshot->points)[index].fitResidual
                    && attributes[2].values[index] == (*snapshot->points)[index].validSupportRatio,
                    "valid quality values retain producer measurements");
            }
        }
    }
    checks.Get(
        testHost.views->overlay->overlays.size() == 1
            && testHost.host->activeViews.size() == 1,
        "successful request attaches owned display");

    SurfaceDeterminationRequest hide;
    hide.action = SurfaceDeterminationAction::SetVisibility;
    hide.isVisible = false;
    checks.Get(
        feature->SendRequest(hide).status
            == SurfaceAdmissionStatus::Accepted,
        "hide request is accepted");
    checks.Get(
        testHost.views->overlay->overlays.empty()
            && feature->GetSurfaceSnapshot().get() == snapshot.get(),
        "hide removes display without replacing measurement generation");
    SurfaceDeterminationRequest show = hide;
    show.isVisible = true;
    checks.Get(
        feature->SendRequest(show).status
            == SurfaceAdmissionStatus::Accepted,
        "show request is accepted");
    const auto shown = feature->GetSurfaceSnapshot();
    checks.Get(
        shown.get() == snapshot.get()
            && shown->parameterFingerprint == snapshot->parameterFingerprint
            && shown->resultRevision == snapshot->resultRevision,
        "display changes preserve revision and parameter fingerprint");

    SurfaceDeterminationRequest clear;
    clear.action = SurfaceDeterminationAction::Clear;
    checks.Get(
        feature->SendRequest(clear).status
            == SurfaceAdmissionStatus::Accepted,
        "clear request is accepted");
    checks.Get(
        !feature->GetSurfaceSnapshot()
            && feature->GetState().stage == SurfaceDeterminationStage::Idle,
        "clear retires the generation");
    checks.Get(feature->DetachHost(), "success test detaches");
}

void TestThresholdPublication(Checks& checks)
{
    TestHost testHost(BuildSphere());
    auto config = GetConfig();
    config.maxWorkingBytes = 64U * 1024U;
    auto feature = std::make_shared<SurfaceDeterminationHostFeature>(config);
    checks.Get(feature->AttachHost(testHost.context), "threshold feature attaches");
    auto request = GetStartRequest(SurfaceDeterminationMethod::AutomaticIso50);
    request.start->initialIsoValue.reset();
    std::optional<SurfaceDeterminationResult> completed;
    checks.Get(feature->SendRequest(request, [&](SurfaceDeterminationResult result) {
        completed = std::move(result);
    }).status == SurfaceAdmissionStatus::Accepted, "threshold request admitted");
    checks.Get(WaitUntil(*feature, [&] { return completed.has_value(); }), "threshold callback arrives");
    const auto generation = feature->GetSurfaceSnapshot();
    const auto graph = testHost.data->GetDataGraph();
    const auto binding = testHost.data->GetDataBinding(graph, "analysis.surface-determination.active");
    checks.Get(completed && completed->status == SurfaceResultStatus::Succeeded
        && completed->isoEstimate && !generation && !completed->isPublished
        && completed->purpose == SurfaceTaskPurpose::Estimate
        && (!binding || !binding->target) && feature->GetState().isoEstimate
        && testHost.views->overlay->overlays.empty(),
        "threshold is transient and never replaces a formal graph binding");
    // 可靠估计之后的新估计失败，不得替换上一正式修订。
    testHost.data->SetCurrent(BuildSnapshot({16,16,16}, {1,1,1}, {0,0,0},
        {1,0,0,0,1,0,0,0,1}, VTK_FLOAT, [](const Point3&) { return 1.0; }));
    (void)feature->OnHostTick();
    completed.reset();
    checks.Get(feature->SendRequest(request, [&](SurfaceDeterminationResult result) {
        completed = std::move(result);
    }).status == SurfaceAdmissionStatus::Accepted, "flat replacement is admitted for validation");
    checks.Get(WaitUntil(*feature, [&] { return completed.has_value(); })
        && completed && completed->failureReason == SurfaceFailureReason::ThresholdUnreliable
        && !completed->isoEstimate && !feature->GetSurfaceSnapshot(),
        "source replacement clears stale threshold and failed estimation yields no value");
    checks.Get(feature->DetachHost(), "threshold feature detaches without mesh bindings");
}

void TestCancelAndSupersede(Checks& checks)
{
    TestHost cancelHost(BuildSphere());
    auto cancelFeature = std::make_shared<SurfaceDeterminationHostFeature>(
        GetConfig());
    checks.Get(
        cancelFeature->AttachHost(cancelHost.context),
        "cancel test attaches");
    std::atomic<int> startCallbacks{ 0 };
    SurfaceDeterminationResult cancelled;
    const auto start = cancelFeature->SendRequest(
        GetStartRequest(SurfaceDeterminationMethod::GradientPeak),
        [&](SurfaceDeterminationResult result) {
            cancelled = std::move(result);
            ++startCallbacks;
        });
    SurfaceDeterminationRequest stop;
    stop.action = SurfaceDeterminationAction::Stop;
    stop.targetRequestId = start.requestId;
    std::atomic<int> stopCallbacks{ 0 };
    const auto stopAdmission = cancelFeature->SendRequest(
        stop,
        [&](SurfaceDeterminationResult) { ++stopCallbacks; });
    checks.Get(
        stopAdmission.status == SurfaceAdmissionStatus::Accepted
            && stopCallbacks.load() == 1,
        "stop request is acknowledged exactly once");
    checks.Get(
        WaitUntil(
            *cancelFeature,
            [&] { return startCallbacks.load() == 1; }),
        "cancelled start completes exactly once");
    checks.Get(
        cancelled.status == SurfaceResultStatus::Cancelled
            && !cancelFeature->GetSurfaceSnapshot(),
        "cancel does not publish a partial generation");
    checks.Get(cancelFeature->DetachHost(), "cancel test detaches");

    TestHost supersedeHost(BuildSphere());
    auto supersedeFeature = std::make_shared<SurfaceDeterminationHostFeature>(
        GetConfig());
    checks.Get(
        supersedeFeature->AttachHost(supersedeHost.context),
        "supersede test attaches");
    std::atomic<int> firstCount{ 0 };
    std::atomic<int> secondCount{ 0 };
    SurfaceDeterminationResult firstResult;
    SurfaceDeterminationResult secondResult;
    const auto first = supersedeFeature->SendRequest(
        GetStartRequest(SurfaceDeterminationMethod::GradientPeak),
        [&](SurfaceDeterminationResult result) {
            firstResult = std::move(result);
            ++firstCount;
        });
    const auto second = supersedeFeature->SendRequest(
        GetStartRequest(SurfaceDeterminationMethod::LocalAdaptiveIso50),
        [&](SurfaceDeterminationResult result) {
            secondResult = std::move(result);
            ++secondCount;
        });
    checks.Get(
        first.status == SurfaceAdmissionStatus::Accepted
            && second.status == SurfaceAdmissionStatus::Accepted,
        "consecutive starts are both admitted with latest-wins semantics");
    checks.Get(
        WaitUntil(
            *supersedeFeature,
            [&] {
                return firstCount.load() == 1 && secondCount.load() == 1;
            }),
        "superseded and latest callbacks both complete once");
    checks.Get(
        firstResult.status == SurfaceResultStatus::Cancelled,
        "superseded request is cancelled");
    checks.Get(
        firstResult.requestId == first.requestId
            && secondResult.requestId == second.requestId
            && secondResult.status == SurfaceResultStatus::Succeeded
            && supersedeFeature->GetSurfaceSnapshot()
            && supersedeFeature->GetSurfaceSnapshot()->resultRevision == 1
            && supersedeFeature->GetSurfaceSnapshot()->method
                == SurfaceDeterminationMethod::LocalAdaptiveIso50,
        "callbacks keep request identity and only latest publishes revision one");
    checks.Get(supersedeFeature->DetachHost(), "supersede test detaches");
}

void TestSourceStaleAndRollback(Checks& checks)
{
    TestHost staleHost(BuildSphere());
    auto staleFeature = std::make_shared<SurfaceDeterminationHostFeature>(
        GetConfig());
    checks.Get(staleFeature->AttachHost(staleHost.context), "stale test attaches");
    std::atomic<int> staleCount{ 0 };
    SurfaceDeterminationResult staleResult;
    staleFeature->SendRequest(
        GetStartRequest(SurfaceDeterminationMethod::GradientPeak),
        [&](SurfaceDeterminationResult result) {
            staleResult = std::move(result);
            ++staleCount;
        });
    auto replacementState = *BuildSphere();
    staleHost.data->SetCurrent(
        std::make_shared<const VtkImageGridView>(
            std::move(replacementState)));
    checks.Get(
        WaitUntil(*staleFeature, [&] { return staleCount.load() == 1; }),
        "stale request completes");
    checks.Get(
        staleResult.failureReason == SurfaceFailureReason::SourceChanged
            && staleFeature->GetState().stage
                == SurfaceDeterminationStage::Stale
            && staleFeature->GetState().requestId != 0
            && !staleFeature->GetSurfaceSnapshot(),
        "source identity/version change rejects old result and keeps request id");
    checks.Get(staleFeature->DetachHost(), "stale test detaches");

    TestHost activeStaleHost(BuildSphere());
    auto activeStaleFeature =
        std::make_shared<SurfaceDeterminationHostFeature>(GetConfig());
    checks.Get(
        activeStaleFeature->AttachHost(activeStaleHost.context),
        "active stale test attaches");
    std::atomic<int> activeReady{ 0 };
    activeStaleFeature->SendRequest(
        GetStartRequest(SurfaceDeterminationMethod::LocalAdaptiveIso50),
        [&](SurfaceDeterminationResult) { ++activeReady; });
    checks.Get(
        WaitUntil(
            *activeStaleFeature,
            [&] { return activeReady.load() == 1; }),
        "active stale baseline generation completes");
    const auto readyState = activeStaleFeature->GetState();
    auto nextState = *BuildSphere();
    activeStaleHost.data->SetCurrent(
        std::make_shared<const VtkImageGridView>(std::move(nextState)));
    checks.Get(activeStaleFeature->OnHostTick(), "active stale tick succeeds");
    const auto activeStaleState = activeStaleFeature->GetState();
    checks.Get(
        activeStaleState.stage == SurfaceDeterminationStage::Stale
            && activeStaleState.requestId == readyState.requestId
            && activeStaleState.sourceRevision == readyState.sourceRevision
            && !activeStaleFeature->GetSurfaceSnapshot()
            && activeStaleHost.views->overlay->overlays.empty(),
        "source change retires an already active generation and display");
    checks.Get(
        activeStaleFeature->DetachHost(),
        "active stale test detaches");

    TestHost rollbackHost(BuildSphere());
    auto rollbackFeature = std::make_shared<SurfaceDeterminationHostFeature>(
        GetConfig());
    checks.Get(
        rollbackFeature->AttachHost(rollbackHost.context),
        "rollback test attaches");
    std::atomic<int> firstCount{ 0 };
    rollbackFeature->SendRequest(
        GetStartRequest(),
        [&](SurfaceDeterminationResult) { ++firstCount; });
    checks.Get(
        WaitUntil(*rollbackFeature, [&] { return firstCount.load() == 1; }),
        "rollback baseline generation completes");
    const auto baseline = rollbackFeature->GetSurfaceSnapshot();
    rollbackHost.views->overlay->isAttachRejected = true;
    std::atomic<int> failureCount{ 0 };
    SurfaceDeterminationResult failed;
    rollbackFeature->SendRequest(
        GetStartRequest(SurfaceDeterminationMethod::LocalAdaptiveIso50),
        [&](SurfaceDeterminationResult result) {
            failed = std::move(result);
            ++failureCount;
        });
    checks.Get(
        WaitUntil(
            *rollbackFeature,
            [&] { return failureCount.load() == 1; }),
        "display failure completes");
    checks.Get(
        failed.status == SurfaceResultStatus::Succeeded
            && failed.failureReason == SurfaceFailureReason::DisplayFailed
            && rollbackFeature->GetSurfaceSnapshot()
            && rollbackFeature->GetSurfaceSnapshot()->resultRevision == baseline->resultRevision + 1
            && rollbackFeature->GetState().stage
                == SurfaceDeterminationStage::Ready
            && rollbackHost.views->overlay->overlays.empty(),
        "display failure keeps new formal data and retires old display");
    const auto committedId = rollbackHost.data->GetDataGraph().commitId;
    rollbackHost.views->overlay->isAttachRejected = false;
    checks.Get(rollbackFeature->OnHostTick()
            && rollbackHost.views->overlay->overlays.size() == 1
            && rollbackFeature->GetState().failureReason == SurfaceFailureReason::None
            && rollbackHost.data->GetDataGraph().commitId == committedId
            && failureCount == 1 && baseline->resultRevision == 1,
        "display retry uses committed data once and preserves historical snapshot");
    checks.Get(rollbackFeature->DetachHost(), "rollback test detaches");
}

bool SetBinding(TestDataPort& data, std::string_view name,
    std::optional<DataRevisionRef> target)
{
    const auto binding = data.GetDataBinding(data.GetDataGraph(), name)
        .value_or(DataBinding{ std::string(name), {}, 0 });
    DataTransaction transaction;
    transaction.bindings.push_back({ std::string(name), binding.revision, true, binding.target, target });
    return data.SetDataCommit(std::move(transaction)).status == DataCommitStatus::Succeeded;
}

void TestBindingProjection(Checks& checks)
{
    TestHost testHost(BuildSphere());
    SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(testHost.context), "binding test attaches");
    int completed = 0;
    const auto start = [&] {
        return feature.SendRequest(GetStartRequest(), [&](SurfaceDeterminationResult) { ++completed; }).status
            == SurfaceAdmissionStatus::Accepted;
    };
    checks.Get(start() && WaitUntil(feature, [&] { return completed == 1; }), "binding A publishes");
    const auto first = feature.GetSurfaceSnapshot();
    const auto firstState = feature.GetState();
    checks.Get(start() && WaitUntil(feature, [&] { return completed == 2; }), "binding B publishes");
    const auto second = feature.GetSurfaceSnapshot();
    const auto secondDisplays = testHost.host->lastDelta.displays;
    if (!first || !second) { checks.Get(false, "binding fixtures have results"); return; }
    checks.Get(SetBinding(*testHost.data, surfaceResultBinding, first->dataRevision), "activate historical A");
    checks.Get(feature.GetSurfaceSnapshot().get() == first.get(), "current query follows binding before tick without mutation");
    const auto commitId = testHost.data->GetDataGraph().commitId;
    checks.Get(feature.OnHostTick() && feature.GetState().resultRevision == first->resultRevision
            && feature.GetState().acceptedPointCount == firstState.acceptedPointCount
            && feature.GetState().truncatedPointCount == firstState.truncatedPointCount
            && testHost.views->overlay->overlays.size() == 1
            && testHost.data->GetDataGraph().commitId == commitId,
        "owner projection follows A without publishing a new version");
    const auto historicalDisplays = testHost.host->lastDelta.displays;
    const auto operations = feature.GetOperationStates();
    const auto activation = std::find_if(operations.begin(), operations.end(), [&](const auto& operation) {
        return !historicalDisplays.empty() && operation.operation == historicalDisplays.front().operation;
    });
    checks.Get(historicalDisplays.size() == 1 && secondDisplays.size() == 1
            && historicalDisplays.front().data == first->meshRevision
            && !(historicalDisplays.front().operation == secondDisplays.front().operation)
            && activation != operations.end()
            && std::find(activation->outputs.begin(), activation->outputs.end(), first->dataRevision)
                != activation->outputs.end()
            && std::find(activation->outputs.begin(), activation->outputs.end(), second->dataRevision)
                == activation->outputs.end(),
        "historical display activation identifies A without reusing B operation or outputs");
    checks.Get(SetBinding(*testHost.data, surfaceResultBinding, second->dataRevision)
            && SetBinding(*testHost.data, surfaceResultBinding, first->dataRevision)
            && feature.OnHostTick() && testHost.views->overlay->overlays.size() == 1,
        "result binding ABA rebuilds one projection");
    checks.Get(SetBinding(*testHost.data, surfaceResultBinding, {})
            && !feature.GetSurfaceSnapshot() && feature.OnHostTick()
            && testHost.views->overlay->overlays.empty()
            && feature.GetState().stage == SurfaceDeterminationStage::Idle
            && first->points && !first->points->empty(),
        "clear binding retires current projection but preserves fixed history");
    checks.Get(feature.DetachHost(), "binding test detaches");
}

void TestBindingAbaAndDisplayRetry(Checks& checks)
{
    TestHost testHost(BuildSphere());
    SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(testHost.context), "ABA test attaches");
    int completed = 0;
    SurfaceDeterminationResult result;
    const auto start = [&] {
        return feature.SendRequest(GetStartRequest(), [&](SurfaceDeterminationResult value) {
            result = std::move(value); ++completed;
        }).status == SurfaceAdmissionStatus::Accepted;
    };
    testHost.host->isSceneRejected = true;
    checks.Get(start() && WaitUntil(feature, [&] { return completed == 1; }), "rejected scene still completes data request");
    const auto first = feature.GetSurfaceSnapshot();
    checks.Get(first && result.status == SurfaceResultStatus::Succeeded
            && result.failureReason == SurfaceFailureReason::DisplayFailed,
        "scene rejection is separate from data success");
    const auto commitId = testHost.data->GetDataGraph().commitId;
    for (int index = 0; index < 5; ++index) checks.Get(feature.OnHostTick(), "rejected scene retry tick");
    checks.Get(testHost.data->GetDataGraph().commitId == commitId
            && testHost.views->overlay->overlays.size() == 1 && completed == 1,
        "rejected scene retries neither append history nor duplicate overlays/completion");
    testHost.host->isSceneRejected = false;
    checks.Get(feature.OnHostTick() && feature.GetState().failureReason == SurfaceFailureReason::None,
        "scene admission recovers");
    if (!first) return;
    checks.Get(start() && SetBinding(*testHost.data, surfaceResultBinding, {})
            && SetBinding(*testHost.data, surfaceResultBinding, first->dataRevision)
            && WaitUntil(feature, [&] { return completed == 2; })
            && result.status == SurfaceResultStatus::Cancelled
            && feature.GetSurfaceSnapshot().get() == first.get(),
        "result binding ABA cancels late request despite identical target");
    checks.Get(start() && SetBinding(*testHost.data, surfaceResultBinding, {})
            && WaitUntil(feature, [&] { return completed == 3; })
            && result.status == SurfaceResultStatus::Cancelled
            && !feature.GetSurfaceSnapshot() && feature.GetState().pointCount == 0,
        "cleared binding cannot be restored by an old task cancellation");
    const auto source = testHost.data->GetPrimaryImage();
    checks.Get(start() && SetBinding(*testHost.data, primaryVolumeBinding, {})
            && SetBinding(*testHost.data, primaryVolumeBinding, source->data->self)
            && WaitUntil(feature, [&] { return completed == 4; })
            && result.failureReason == SurfaceFailureReason::SourceChanged,
        "primary binding ABA rejects late result");
    checks.Get(feature.DetachHost(), "ABA test detaches");
}

void TestCommitObserverReentry(Checks& checks)
{
    TestHost host(BuildSphere());
    SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(host.context), "observer test attaches");
    int count = 0;
    checks.Get(feature.SendRequest(GetStartRequest(), [&](SurfaceDeterminationResult) { ++count; }).status
            == SurfaceAdmissionStatus::Accepted && WaitUntil(feature, [&] { return count == 1; }),
        "observer baseline publishes");
    const auto first = feature.GetSurfaceSnapshot();
    if (!first) return;
    bool hasReentered = false;
    bool hasPreservedNext = false;
    SurfaceDeterminationAdmission next;
    SurfaceDeterminationResult secondResult;
    SurfaceDeterminationResult nextResult;
    const auto observer = host.data->AttachDataChange([&](const DataChangeSet& change) {
        if (hasReentered || change.published.empty()) return;
        hasReentered = true;
        checks.Get(SetBinding(*host.data, surfaceResultBinding, first->dataRevision),
            "observer activates historical result");
        next = feature.SendRequest(GetStartRequest(), [&](SurfaceDeterminationResult value) {
            nextResult = std::move(value); ++count;
        });
    });
    const auto second = feature.SendRequest(GetStartRequest(), [&](SurfaceDeterminationResult value) {
        secondResult = std::move(value); ++count;
        const auto state = feature.GetState();
        hasPreservedNext = state.requestId == next.requestId
            && state.stage == SurfaceDeterminationStage::Preparing;
    });
    checks.Get(second.status == SurfaceAdmissionStatus::Accepted
            && WaitUntil(feature, [&] { return count == 3; }) && hasReentered && hasPreservedNext
            && next.status == SurfaceAdmissionStatus::Accepted
            && secondResult.requestId == second.requestId && secondResult.resultRevision == 2
            && nextResult.requestId == next.requestId && nextResult.resultRevision == 3,
        "observer reentry preserves each result identity and the next request state");
    checks.Get(host.data->DetachDataChange(observer) && feature.DetachHost(), "observer test detaches");

    TestHost detachHost(BuildSphere());
    SurfaceDeterminationHostFeature detached(GetConfig());
    checks.Get(detached.AttachHost(detachHost.context), "observer detach fixture attaches");
    bool didDetach = false;
    int detachedCount = 0;
    SurfaceDeterminationResult detachedResult;
    const auto detachObserver = detachHost.data->AttachDataChange([&](const DataChangeSet& change) {
        if (didDetach || change.published.empty()) return;
        didDetach = detached.DetachHost();
    });
    checks.Get(detached.SendRequest(GetStartRequest(), [&](SurfaceDeterminationResult value) {
        detachedResult = std::move(value); ++detachedCount;
    }).status == SurfaceAdmissionStatus::Accepted, "observer detach request admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!didDetach && std::chrono::steady_clock::now() < deadline) {
        (void)detached.OnHostTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    checks.Get(didDetach && detachedCount == 1 && detachedResult.resultRevision == 1
            && detachedResult.status == SurfaceResultStatus::Cancelled,
        "observer detach cancels callback once without losing committed result identity");
    checks.Get(detachHost.data->DetachDataChange(detachObserver), "observer detach removes observer");
}

void TestCompletionCapacity(Checks& checks)
{
    SurfaceDeterminationService service;
    const auto source = BuildSphere();
    auto params = GetParams(SurfaceDeterminationMethod::GlobalIsoPreview);
    std::size_t acceptedCount = 0;
    bool wasBounded = false;
    for (std::uint64_t requestId = 1; requestId <= 80; ++requestId) {
        const auto admission = service.Start(
            source,
            params,
            128U * 1024U * 1024U,
            requestId);
        if (admission == SurfaceAdmissionStatus::Accepted) {
            ++acceptedCount;
        }
        else {
            wasBounded = admission == SurfaceAdmissionStatus::Unavailable;
            break;
        }
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (service.GetIsBusy()
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::size_t completionCount = 0;
    std::size_t successCount = 0;
    bool completionFieldsValid = true;
    std::unordered_set<std::uint64_t> completionIds;
    while (auto complete = service.GetComplete()) {
        ++completionCount;
        completionIds.insert(complete->requestId);
        completionFieldsValid = completionFieldsValid
            && complete->result.sourceRevision == source->data->self
            && ((complete->result.status == SurfaceResultStatus::Succeeded
                    && complete->result.failureReason
                        == SurfaceFailureReason::None)
                || (complete->result.status == SurfaceResultStatus::Cancelled
                    && complete->result.failureReason
                        == SurfaceFailureReason::Cancelled));
        if (complete->result.status == SurfaceResultStatus::Succeeded) {
            ++successCount;
        }
    }
    checks.Get(wasBounded, "service bounds outstanding completion capacity");
    checks.Get(
        acceptedCount != 0 && completionCount == acceptedCount,
        "every capacity-admitted request produces one completion");
    checks.Get(
        completionIds.size() == acceptedCount && successCount <= 1,
        "capacity completions have unique request IDs and only latest may succeed");
    bool hasEveryAcceptedId = true;
    for (std::uint64_t requestId = 1; requestId <= acceptedCount; ++requestId) {
        hasEveryAcceptedId = hasEveryAcceptedId
            && completionIds.count(requestId) == 1;
    }
    checks.Get(
        completionFieldsValid && hasEveryAcceptedId,
        "capacity completions preserve every admitted ID, source, and status reason");
    checks.Get(
        service.Stop(std::chrono::steady_clock::now()
            + std::chrono::seconds(2)),
        "capacity test worker stops cleanly");
}


void TestPurposeIsolation(Checks& checks)
{
    TestHost host(BuildSphere());
    SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(host.context), "purpose fixture attaches");
    std::optional<SurfaceDeterminationResult> completed;
    const auto run = [&](SurfaceDeterminationRequest request) {
        completed.reset();
        return feature.SendRequest(std::move(request), [&](auto value) { completed = std::move(value); }).status
            == SurfaceAdmissionStatus::Accepted && WaitUntil(feature, [&] { return completed.has_value(); });
    };
    checks.Get(run(GetStartRequest()), "purpose formal baseline completes");
    const auto formal = feature.GetSurfaceSnapshot();
    const auto commit = host.data->GetDataGraph().commitId;
    auto estimate = GetStartRequest(SurfaceDeterminationMethod::AutomaticIso50);
    estimate.start->initialIsoValue.reset();
    checks.Get(run(estimate) && completed->isoEstimate && !completed->isPublished
        && feature.GetSurfaceSnapshot() == formal && host.data->GetDataGraph().commitId == commit,
        "estimate preserves formal identity and graph commit");
    auto preview = GetStartRequest(SurfaceDeterminationMethod::GlobalIsoPreview);
    checks.Get(run(preview) && !completed->isPublished && feature.GetPreviewSnapshot()
        && !GetDataRevisionRefValid(feature.GetPreviewSnapshot()->dataRevision)
        && feature.GetSurfaceSnapshot() == formal && host.data->GetDataGraph().commitId == commit
        && host.host->lastDelta.hasDisplayUpdate && host.host->lastDelta.displays.empty(),
        "preview has transient identity and removes formal picking description");
    SurfaceDeterminationRequest clear; clear.action = SurfaceDeterminationAction::ClearPreview;
    checks.Get(feature.SendRequest(clear).status == SurfaceAdmissionStatus::Accepted
        && !feature.GetPreviewSnapshot() && feature.GetSurfaceSnapshot() == formal
        && host.host->lastDelta.displays.size() == 1 && host.data->GetDataGraph().commitId == commit,
        "clearing preview restores formal display without graph writes");
    checks.Get(run(preview) && run(preview), "repeated previews complete");
    checks.Get(feature.SendRequest(clear).status == SurfaceAdmissionStatus::Accepted
        && feature.GetState().pointCount == formal->points->size()
        && feature.GetState().purpose == SurfaceTaskPurpose::Determine,
        "repeated previews do not overwrite formal restore state");
    auto headless = GetStartRequest(); headless.start->targetViews = {};
    checks.Get(run(headless) && completed->isPublished && completed->isActivated
        && completed->failureReason == SurfaceFailureReason::None && host.views->overlay->overlays.empty(),
        "headless formal completes without render objects");
    const auto batch = feature.GetSurfaceSnapshot();
    bool same = batch && formal && batch->points->size() == formal->points->size();
    if (same) for (std::size_t i=0;i<batch->points->size();++i)
        same = same && (*batch->points)[i].positionModel == (*formal->points)[i].positionModel;
    checks.Get(same, "headless and displayed computation produce identical positions");
    checks.Get(feature.DetachHost(), "purpose fixture detaches");
}

void TestExplicitScopesAndHistory(Checks& checks)
{
    TestHost host(BuildSphere());
    SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(host.context), "scope fixture attaches");
    const auto source = host.data->GetPrimaryImage();
    host.data->SetCurrent(BuildPlane());
    auto a = GetStartRequest(); a.start->sourceVolume = source->data->self;
    a.start->resultScope = "part/A"; a.start->targetViews = {}; a.start->modelUnit = "mm";
    auto b = a; b.start->resultScope = "part/B";
    std::vector<SurfaceDeterminationResult> results;
    const auto onComplete = [&](auto value) { results.push_back(std::move(value)); };
    checks.Get(feature.SendRequest(a,onComplete).status == SurfaceAdmissionStatus::Accepted
        && feature.SendRequest(b,onComplete).status == SurfaceAdmissionStatus::Accepted
        && WaitUntil(feature,[&]{return results.size()==2;}), "two scopes queue without cancelling each other");
    const auto first = feature.GetSurfaceSnapshot("part/A");
    const auto second = feature.GetSurfaceSnapshot("part/B");
    checks.Get(first && second && first->dataRevision != second->dataRevision
        && results[0].status == SurfaceResultStatus::Succeeded && results[1].status == SurfaceResultStatus::Succeeded
        && !feature.GetSurfaceSnapshot() && first->sourceRevision == source->data->self
        && first->modelUnit == "mm" && first->resolvedParams.initialIsoValue,
        "non-primary current input publishes independent explicit scopes with provenance");
    if (!first || !second) { (void)feature.DetachHost(); return; }
    const auto meshData = host.data->GetData(host.data->GetDataGraph(), first->meshRevision);
    checks.Get(meshData->provenance &&
                   meshData->provenance->canonicalParameters == first->canonicalParameters &&
                   first->canonicalParameters.find("surface-parameters 2") == 0,
               "full canonical recipe is the mesh provenance");
    DataTransaction revise;
    revise.outputs.push_back({source->data->self.entityId,source->data->self.generation,
        DataTypes::imageGrid3D,{},source->data->payload,{}});
    checks.Get(host.data->SetDataCommit(std::move(revise)).status == DataCommitStatus::Succeeded,
        "source entity advances without changing primary");
    checks.Get(!feature.GetSurfaceSnapshot("part/A") && feature.GetSurfaceSnapshot(first->dataRevision) == first,
        "current scope rejects stale head while explicit history remains readable");
    const auto graphBefore = host.data->GetDataGraph();
    auto historical=a; historical.start->sourcePolicy=DataPublishPolicy::AllowHistoricalResult;
    results.clear();
    checks.Get(feature.SendRequest(historical,onComplete).status == SurfaceAdmissionStatus::Accepted
        && WaitUntil(feature,[&]{return results.size()==1;}),"historical computation admitted");
    checks.Get(results.size()==1 && results[0].isPublished && !results[0].isActivated
        && results[0].status == SurfaceResultStatus::Succeeded
        && feature.GetSurfaceSnapshot(results[0].dataRevision)
        && feature.GetState().stage==SurfaceDeterminationStage::Ready,
        "historical calculation records terminal state and immutable output without activation");
    const auto bindingName=SurfaceContract::GetBindingName("part/A");
    const auto previous=host.data->GetDataBinding(graphBefore,bindingName);
    const auto current=host.data->GetDataBinding(host.data->GetDataGraph(),bindingName);
    checks.Get(previous && current && previous->revision==current->revision && previous->target==current->target,
        "historical publication leaves the exact prior scope binding unchanged");
    checks.Get(feature.DetachHost(),"scope fixture detaches");
}

void TestPurposeChannelsAndAdmission(Checks& checks)
{
    TestHost host(BuildSphere()); SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(host.context),"channels attach");
    std::vector<SurfaceDeterminationResult> results;
    auto formal=GetStartRequest(); formal.start->targetViews={};
    auto preview=GetStartRequest(SurfaceDeterminationMethod::GlobalIsoPreview); preview.start->targetViews={};
    const auto receive=[&](auto result){results.push_back(std::move(result));};
    const auto f=feature.SendRequest(formal,receive);
    const auto p=feature.SendRequest(preview,receive);
    const auto q=feature.SendRequest(preview,receive);
    checks.Get(f.status==SurfaceAdmissionStatus::Accepted && p.status==SurfaceAdmissionStatus::Accepted
        && q.status==SurfaceAdmissionStatus::Accepted && WaitUntil(feature,[&]{return results.size()==3;}),
        "formal and preview channels complete once");
    const auto find=[&](std::uint64_t id){return std::find_if(results.begin(),results.end(),[&](const auto& r){return r.requestId==id;});};
    checks.Get(find(f.requestId)!=results.end() && find(f.requestId)->isPublished
        && find(p.requestId)!=results.end() && find(p.requestId)->status==SurfaceResultStatus::Cancelled
        && find(q.requestId)!=results.end() && find(q.requestId)->status==SurfaceResultStatus::Succeeded
        && feature.GetSurfaceSnapshot() && feature.GetPreviewSnapshot(),
        "new preview supersedes only its own channel and retains concurrent formal publication");
    int refused=0;
    auto invalid=preview; invalid.start->purpose=SurfaceTaskPurpose::Determine;
    checks.Get(feature.SendRequest(invalid,[&](auto){++refused;}).status==SurfaceAdmissionStatus::InvalidRequest,
        "global preview cannot be mislabeled as formal");
    invalid=formal; invalid.start->resultScope=std::string("\xC0\xAF",2);
    checks.Get(feature.SendRequest(invalid,[&](auto){++refused;}).status==SurfaceAdmissionStatus::InvalidRequest && refused==0,
        "invalid UTF8 scope is rejected without callback");
    invalid=formal; invalid.start->modelUnit="gray";
    checks.Get(feature.SendRequest(invalid).status==SurfaceAdmissionStatus::InvalidRequest,"gray unit cannot label geometry");
    checks.Get(feature.DetachHost(),"channels detach");
}

void TestCoordinateFramePropagation(Checks& checks)
{
    TestHost host(BuildSphere()); SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(host.context),"frame attaches");
    const auto source=host.data->GetPrimaryImage();
    const auto* image=dynamic_cast<const ImageGrid3DPayload*>(source->data->payload.get());
    auto geometry=image->GetGeometry(); geometry.coordinateFrame="LPS";
    const auto entity=host.data->CreateDataEntityId(); const DataRevisionRef ref{entity,1};
    DataTransaction transaction;
    transaction.outputs.push_back({entity,0,DataTypes::imageGrid3D,{},image->CreateGeometrySnapshot(geometry),{}});
    checks.Get(host.data->SetDataCommit(std::move(transaction)).status==DataCommitStatus::Succeeded,"explicit LPS source is registered");
    auto request=GetStartRequest();request.start->sourceVolume=ref;request.start->targetViews={};
    std::optional<SurfaceDeterminationResult> result;
    checks.Get(feature.SendRequest(request,[&](auto r){result=std::move(r);}).status==SurfaceAdmissionStatus::Accepted
        && WaitUntil(feature,[&]{return bool(result);}),"LPS source computes");
    const auto generation=feature.GetSurfaceSnapshot();
    const auto meshData=generation?host.data->GetData(host.data->GetDataGraph(),generation->meshRevision):DataSnapshot{};
    const auto* mesh=meshData?dynamic_cast<const SurfaceMeshPayload*>(meshData->payload.get()):nullptr;
    checks.Get(generation && mesh && generation->coordinateFrame=="LPS" && mesh->GetCoordinateFrame()=="LPS"
        && generation->modelUnit.empty() && generation->points->front().positionModel[0]>0,
        "LPS coordinates propagate without sign reversal or invented length unit");
    checks.Get(feature.DetachHost(),"frame detaches");
}

void TestCancelBeforeOwnerCommit(Checks& checks)
{
    SurfaceDeterminationService service;
    checks.Get(service.Start(BuildSphere(),GetParams(),128U*1024U*1024U,1)==SurfaceAdmissionStatus::Accepted,
        "ready cancellation is admitted");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while(service.GetIsBusy() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    checks.Get(!service.GetIsBusy() && service.StopRequest(1),"cancel covers completed but unpublished candidates");
    const auto result=service.GetComplete();
    checks.Get(result && result->result.status==SurfaceResultStatus::Cancelled && result->result.points.empty()
        && !service.GetComplete(),"ready cancellation produces exactly one empty cancelled completion");
    checks.Get(service.Stop(std::chrono::steady_clock::now()+std::chrono::seconds(2)),"ready cancellation stops worker");
}

void TestDetachCallbackReentry(Checks& checks)
{
    TestHost host(BuildSphere()); SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(host.context), "detach reentry attaches");
    bool ready = false;
    checks.Get(feature.SendRequest(GetStartRequest(), [&](auto) { ready = true; }).status == SurfaceAdmissionStatus::Accepted
        && WaitUntil(feature, [&] { return ready; }), "detach observer has an active formal binding");
    bool sawTeardown = false;
    const auto observer = host.data->AttachDataChange([&](const DataChangeSet& change) {
        if (!change.published.empty() || change.bindings.empty()) return;
        sawTeardown = true;
        checks.Get(feature.SendRequest(GetStartRequest()).status == SurfaceAdmissionStatus::Stopping
            && !feature.OnHostTick(), "teardown observer cannot admit work or advance display state");
    });
    int count = 0;
    auto request = GetStartRequest(); request.start->targetViews = {};
    const auto callback = [&](SurfaceDeterminationResult result) {
        ++count;
        checks.Get(result.status == SurfaceResultStatus::Cancelled && feature.DetachHost(),
            "detach callback observes completed teardown and may detach again");
    };
    checks.Get(feature.SendRequest(request, callback).status == SurfaceAdmissionStatus::Accepted,
        "first detach candidate admitted");
    request.start->resultScope = "second";
    checks.Get(feature.SendRequest(request, callback).status == SurfaceAdmissionStatus::Accepted
        && feature.DetachHost() && count == 2 && sawTeardown, "teardown completes both callbacks once without map reentry");
    checks.Get(host.data->DetachDataChange(observer), "teardown observer removed");
    checks.Get(feature.AttachHost(host.context) && feature.DetachHost(), "feature can reattach after callback teardown");
}

void TestClearedResultFailure(Checks& checks)
{
    TestHost host(BuildSphere()); SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(host.context), "clear failure fixture attaches");
    int count = 0;
    checks.Get(feature.SendRequest(GetStartRequest(), [&](auto) { ++count; }).status == SurfaceAdmissionStatus::Accepted
        && WaitUntil(feature, [&] { return count == 1; }), "clear failure baseline completes");
    SurfaceDeterminationRequest clear; clear.action = SurfaceDeterminationAction::Clear;
    checks.Get(feature.SendRequest(clear).status == SurfaceAdmissionStatus::Accepted, "formal result is cleared");
    auto empty = GetStartRequest(); empty.start->initialIsoValue = 1e9;
    checks.Get(feature.SendRequest(empty, [&](auto) { ++count; }).status == SurfaceAdmissionStatus::Accepted
        && WaitUntil(feature, [&] { return count == 2; }) && !feature.GetSurfaceSnapshot()
        && feature.GetState().stage == SurfaceDeterminationStage::Failed && feature.GetState().pointCount == 0,
        "later failure cannot restore a cleared formal summary");
    checks.Get(feature.DetachHost(), "clear failure fixture detaches");
}

void TestBusinessInputLifecycle(Checks &checks)
{
    TestHost host(BuildPlane());
    SurfaceDeterminationHostFeature feature(GetConfig());
    checks.Get(feature.AttachHost(host.context), "business input fixture attaches");
    const auto source = host.data->GetPrimaryImage();
    const auto geometry =
        dynamic_cast<const ImageGrid3DPayload *>(source->data->payload.get())->GetGeometry();
    auto values = std::make_shared<std::vector<std::uint16_t>>();
    for (int z = 0; z < geometry.dimensions[2]; ++z)
        for (int y = 0; y < geometry.dimensions[1]; ++y)
            for (int x = 0; x < geometry.dimensions[0]; ++x)
                values->push_back(x <= 15 ? 2 : 7);
    const DataRevisionRef labelRef{host.data->CreateDataEntityId(), 1};
    DataTransaction labels;
    labels.outputs.push_back({labelRef.entityId,
                              0,
                              DataTypes::labelMap3D,
                              {{"source-volume", source->data->self}},
                              std::make_shared<const LabelMap3DPayload>(geometry, LabelMapValues{values}),
                              {}});
    checks.Get(host.data->SetDataCommit(std::move(labels)).status == DataCommitStatus::Succeeded,
               "business labels enter the real DataGraph");
    auto request = GetStartRequest();
    request.start->targetViews = {};
    request.start->sourceVolume = source->data->self;
    request.start->materialLabels = labelRef;
    request.start->materialPairs = {{2, 7}};
    request.start->componentSelection = SurfaceComponentSelection::All;
    request.start->resultScope = "material/part";
    request.start->modelUnit = "mm";
    request.start->initialIsoValue.reset();
    std::optional<SurfaceDeterminationResult> result;
    const auto run = [&](SurfaceDeterminationRequest next) {
        result.reset();
        return feature.SendRequest(std::move(next), [&](auto value) { result = std::move(value); }).status ==
                   SurfaceAdmissionStatus::Accepted &&
               WaitUntil(feature, [&] { return result.has_value(); });
    };
    checks.Get(run(request) && result->isPublished && result->status == SurfaceResultStatus::Succeeded,
               "label-to-surface completes without windows");
    const auto generation = feature.GetSurfaceSnapshot("material/part");
    if (!generation)
    {
        (void)feature.DetachHost();
        return;
    }
    const auto mesh = host.data->GetData(host.data->GetDataGraph(), generation->meshRevision);
    const auto *payload = dynamic_cast<const SurfaceMeshPayload *>(mesh->payload.get());
    checks.Get(mesh->inputs.size() == 2 && generation->inputs.size() == 2 && generation->interfaces &&
                   generation->interfaces->at(0).canonicalId == "2:7",
               "generic consumer can trace scalar and label revisions plus stable interface identity");
    checks.Get(payload && payload->GetPointAttributes().size() == 8 &&
                   payload->GetPointAttributes()[5].name == "measurement.flags",
               "generic mesh exposes quality reasons and interface/override indexes");
    const auto valid = feature.GetResultValidity(generation->dataRevision);
    checks.Get(valid.status == SurfaceRestoreStatus::Current && valid.canDisplay && valid.canRecompute &&
                   valid.canMeasure,
               "current frozen business result supports replay and quality-gated measurement");
    const auto accepted =
        std::find_if(generation->points->begin(), generation->points->end(),
                     [](const auto &point) { return point.flags == SurfacePointFlags::None; });
    if (accepted != generation->points->end())
    {
        const auto diagnostic = feature.GetProfileDiagnostic(
            generation->dataRevision, static_cast<std::uint64_t>(accepted - generation->points->begin()));
        checks.Get(diagnostic.isAvailable && diagnostic.point.positionModel == accepted->positionModel &&
                       !diagnostic.candidates.empty(),
                   "public diagnostic uses the generation's saved source and recipe");
    }
    const auto labelData = host.data->GetData(host.data->GetDataGraph(), labelRef);
    DataTransaction revise;
    revise.outputs.push_back(
        {labelRef.entityId, 1, DataTypes::labelMap3D, labelData->inputs, labelData->payload, {}});
    checks.Get(host.data->SetDataCommit(std::move(revise)).status == DataCommitStatus::Succeeded,
               "label revision advances independently of source");
    const auto historical = feature.GetResultValidity(generation->dataRevision);
    checks.Get(!feature.GetSurfaceSnapshot("material/part") &&
                   feature.GetSurfaceSnapshot(generation->dataRevision) &&
                   historical.status == SurfaceRestoreStatus::Historical && historical.canDisplay &&
                   historical.canRecompute && !historical.canMeasure,
               "label changes invalidate current measurement while exact history remains reviewable");
    auto historicalRequest = request;
    historicalRequest.start->sourcePolicy = DataPublishPolicy::AllowHistoricalResult;
    checks.Get(run(historicalRequest) && result->isPublished && !result->isActivated,
               "explicit historical label recipe computes without activation");
    auto currentRequest = request;
    currentRequest.start->materialLabels = DataRevisionRef{labelRef.entityId, 2};
    const auto label2 = host.data->GetData(host.data->GetDataGraph(), *currentRequest.start->materialLabels);
    host.data->beforeSurfaceCommit = [&] {
        DataTransaction race;
        race.outputs.push_back(
            {labelRef.entityId, 2, DataTypes::labelMap3D, label2->inputs, label2->payload, {}});
        checks.Get(host.data->TestDataPort::SetDataCommit(std::move(race)).status ==
                       DataCommitStatus::Succeeded,
                   "publication race advances the label head");
    };
    checks.Get(run(currentRequest) && result->status == SurfaceResultStatus::Failed && !result->isPublished,
               "label CAS refuses a race between final validation and DataGraph commit");
    auto initial = GetStartRequest();
    initial.start->targetViews = {};
    initial.start->sourceVolume = source->data->self;
    initial.start->initialSurface = generation->meshRevision;
    initial.start->resultScope = "initial/part";
    checks.Get(run(initial) && result->isPublished, "generic initial surface is a frozen business input");
    const auto imported = feature.GetSurfaceSnapshot("initial/part");
    DataTransaction meshRevise;
    meshRevise.outputs.push_back({mesh->self.entityId,
                                  mesh->self.generation,
                                  DataTypes::surfaceMesh,
                                  mesh->inputs,
                                  mesh->payload,
                                  {}});
    checks.Get(host.data->SetDataCommit(std::move(meshRevise)).status == DataCommitStatus::Succeeded,
               "initial mesh revision advances");
    checks.Get(imported && !feature.GetSurfaceSnapshot("initial/part") &&
                   feature.GetResultValidity(imported->dataRevision).status ==
                       SurfaceRestoreStatus::Historical,
               "initial mesh changes invalidate downstream current surface");
    auto wrong = request;
    wrong.start->materialLabels = source->data->self;
    checks.Get(feature.SendRequest(wrong).status == SurfaceAdmissionStatus::InvalidRequest,
               "wrong dependency payload is rejected at admission");
    checks.Get(feature.DetachHost(), "business input fixture detaches");
}

} // namespace

int GetSurfaceLifecycleFailCount()
{
    Checks checks;
    TestBusinessInputLifecycle(checks);
    TestAttachAndOwnerThread(checks);
    TestSuccessVisibilityAndClear(checks);
    TestThresholdPublication(checks);
    TestCancelAndSupersede(checks);
    TestSourceStaleAndRollback(checks);
    TestBindingProjection(checks);
    TestBindingAbaAndDisplayRetry(checks);
    TestCommitObserverReentry(checks);
    TestCompletionCapacity(checks);
    TestPurposeIsolation(checks);
    TestExplicitScopesAndHistory(checks);
    TestPurposeChannelsAndAdmission(checks);
    TestCoordinateFramePropagation(checks);
    TestCancelBeforeOwnerCommit(checks);
    TestDetachCallbackReentry(checks);
    TestClearedResultFailure(checks);
    return checks.failureCount;
}
