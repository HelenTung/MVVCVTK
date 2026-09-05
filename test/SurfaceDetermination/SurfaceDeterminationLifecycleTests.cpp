#include "SurfaceDeterminationTestCases.h"

#include "Host/SurfaceDeterminationHostFeature.h"
#include "SurfaceDeterminationService.h"
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

class DataPortStub final : public TrustedFeatureDataPort {
public:
    explicit DataPortStub(TrustedImageSnapshot initial)
        : current(std::move(initial))
    {
    }

    TrustedImageSnapshot GetImageSnapshot() const override
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return current;
    }

    bool SetImageState(
        TrustedImageState imageState,
        const TrustedImageSnapshot& expected,
        TrustedImageSnapshot& published) override
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!expected || current.get() != expected.get()) return false;
        imageState.version = expected->version + 1;
        current = std::make_shared<const TrustedImageState>(
            std::move(imageState));
        published = current;
        return true;
    }

    void SetCurrent(TrustedImageSnapshot next)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        current = std::move(next);
    }

private:
    mutable std::mutex mutex;
    TrustedImageSnapshot current;
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
        return delta.requestId != 0 && !delta.viewIds.empty();
    }

    bool SendOwnerComplete(std::function<void()> complete) override
    {
        if (!complete) return false;
        complete();
        return true;
    }

    std::vector<std::string> activeViews;
    bool isActiveViewsRejected = false;
};

struct TestHost final {
    explicit TestHost(TrustedImageSnapshot source)
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
    checks.Get(
        completed.status == SurfaceResultStatus::Succeeded
            && state.stage == SurfaceDeterminationStage::Ready,
        "successful request reaches Ready");
    checks.Get(
        snapshot && snapshot->resultRevision == 1
            && snapshot->sourceVersion == 1,
        "successful request publishes immutable generation");
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
        GetStartRequest(SurfaceDeterminationMethod::GlobalIsoPreview),
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
                == SurfaceDeterminationMethod::GlobalIsoPreview,
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
    replacementState.version = 2;
    staleHost.data->SetCurrent(
        std::make_shared<const TrustedImageState>(
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
        GetStartRequest(SurfaceDeterminationMethod::GlobalIsoPreview),
        [&](SurfaceDeterminationResult) { ++activeReady; });
    checks.Get(
        WaitUntil(
            *activeStaleFeature,
            [&] { return activeReady.load() == 1; }),
        "active stale baseline generation completes");
    const auto readyState = activeStaleFeature->GetState();
    auto nextState = *BuildSphere();
    nextState.version = 2;
    activeStaleHost.data->SetCurrent(
        std::make_shared<const TrustedImageState>(std::move(nextState)));
    checks.Get(activeStaleFeature->OnHostTick(), "active stale tick succeeds");
    const auto activeStaleState = activeStaleFeature->GetState();
    checks.Get(
        activeStaleState.stage == SurfaceDeterminationStage::Stale
            && activeStaleState.requestId == readyState.requestId
            && activeStaleState.sourceVersion == readyState.sourceVersion
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
        GetStartRequest(SurfaceDeterminationMethod::GlobalIsoPreview),
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
        failed.failureReason == SurfaceFailureReason::DisplayFailed
            && rollbackFeature->GetSurfaceSnapshot().get() == baseline.get()
            && rollbackFeature->GetState().stage
                == SurfaceDeterminationStage::Ready
            && rollbackHost.views->overlay->overlays.size() == 1
            && rollbackHost.host->activeViews.size() == 1,
        "display failure preserves prior active generation reason="
            + std::to_string(static_cast<int>(failed.failureReason))
            + " same=" + std::to_string(
                rollbackFeature->GetSurfaceSnapshot().get() == baseline.get())
            + " stage=" + std::to_string(static_cast<int>(
                rollbackFeature->GetState().stage)));
    rollbackHost.views->overlay->isAttachRejected = false;
    checks.Get(rollbackFeature->DetachHost(), "rollback test detaches");
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
            && complete->result.sourceVersion == source->version
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

} // namespace

int GetSurfaceLifecycleFailCount()
{
    Checks checks;
    TestAttachAndOwnerThread(checks);
    TestSuccessVisibilityAndClear(checks);
    TestCancelAndSupersede(checks);
    TestSourceStaleAndRollback(checks);
    TestCompletionCapacity(checks);
    return checks.failureCount;
}
