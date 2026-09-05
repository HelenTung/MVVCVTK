#include "QtHostMethodCases.h"

#include "App/AppState.h"
#include "Data/DataManager.h"
#include "Host/HostFeature.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/VtkAppHostSession.h"

#include <vtkCommand.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkWeakPointer.h>

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
class FakeHostFeature final : public HostFeature {
public:
    explicit FakeHostFeature(std::string id)
        : m_id(std::move(id))
    {
    }

    std::string_view GetFeatureId() const noexcept override
    {
        return m_id;
    }

    bool AttachHost(const HostFeatureContext& context) override
    {
        if (isAttachThrowing) {
            throw 1;
        }
        if (isAttached) {
            return true;
        }
        if (!context.views || !context.host) {
            return false;
        }
        HostInputBinding binding;
        binding.featureId = m_id;
        binding.targetViews.viewIds = {
            "lifecycle" };
        binding.onInput =
            [](const InteractionEvent&) {
                return InteractionResult{};
            };
        if (!context.host->AttachInput(
                std::move(binding))) {
            return false;
        }
        m_views = context.views;
        m_host = context.host;
        isAttached = true;
        ++attachCount;
        return !isAttachFailing;
    }

    bool DetachHost() override
    {
        if (isDetachThrowing) {
            throw 1;
        }
        if (!isAttached) {
            return true;
        }
        ++detachTryCount;
        if (isDetachFailing) {
            return false;
        }
        if (m_host
            && !m_host->DetachInput(m_id)) {
            return false;
        }
        m_views.reset();
        m_host.reset();
        isAttached = false;
        ++detachCount;
        return true;
    }

    bool OnHostTick() override
    {
        if (isTickThrowing) {
            throw 1;
        }
        ++tickCount;
        return true;
    }

    bool SendOwnerComplete(std::function<void()> complete)
    {
        return m_host
            && m_host->SendOwnerComplete(std::move(complete));
    }

    bool SetActiveViews(const HostViewTargets& targets)
    {
        if (!m_views || !m_host) {
            return false;
        }
        const auto views = m_views->GetViews(targets);
        if ((!targets.viewIds.empty()
                || !targets.viewRoles.empty())
            && views.empty()) {
            return false;
        }
        std::vector<std::string> viewIds;
        viewIds.reserve(views.size());
        for (const auto& view : views) {
            if (!view.id.empty()) {
                viewIds.push_back(view.id);
            }
        }
        return m_host->SetActiveViews(viewIds);
    }

    std::string m_id;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<FeatureHostControl> m_host;
    int attachCount = 0;
    int detachCount = 0;
    int detachTryCount = 0;
    int tickCount = 0;
    bool isAttached = false;
    bool isAttachFailing = false;
    bool isAttachThrowing = false;
    bool isDetachThrowing = false;
    bool isDetachFailing = false;
    bool isTickThrowing = false;
};

HostSessionConfig GetSessionConfig()
{
    HostRenderViewConfig view;
    view.id = "lifecycle";
    view.role = HostRenderViewRole::Primary3D;
    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    return config;
}

HostCoreServices GetCoreServices()
{
    HostCoreServices core;
    core.sharedDataMgr =
        std::make_shared<RawVolumeDataManager>();
    core.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    core.sharedState =
        std::make_shared<SharedInteractionState>(
            core.sharedStateBroadcaster);
    return core;
}

bool SendTimer(
    vtkRenderWindowInteractor* interactor,
    const int idOffset = 0)
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
    timerId += idOffset;
    interactor->InvokeEvent(vtkCommand::TimerEvent, &timerId);
    return true;
}

bool GetSafeExit(
    const std::string_view caseName)
{
    if (GetMethodExecutable().empty()
        || caseName.empty()) {
        return false;
    }

#ifdef _WIN32
    std::string command =
        "\"" + GetMethodExecutable()
        + "\" --death "
        + std::string(caseName);
    std::vector<char> commandLine(
        command.begin(), command.end());
    commandLine.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        return false;
    }

    constexpr DWORD timeoutMs = 10'000;
    const DWORD waitResult =
        WaitForSingleObject(
            process.hProcess,
            timeoutMs);
    DWORD exitCode = 0;
    if (waitResult == WAIT_OBJECT_0) {
        (void)GetExitCodeProcess(
            process.hProcess,
            &exitCode);
    }
    else {
        (void)TerminateProcess(
            process.hProcess,
            87);
        (void)WaitForSingleObject(
            process.hProcess,
            timeoutMs);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return waitResult == WAIT_OBJECT_0
        && exitCode == 0;
#else
    return false;
#endif
}
}

int StartLifecycleDeathCase(
    const std::string_view caseName)
{
    if (caseName == "detach-failure") {
        auto session =
            std::make_unique<VtkAppHostSession>(
                GetSessionConfig());
        if (!session->BuildSession()) {
            return 10;
        }
        auto feature =
            std::make_shared<FakeHostFeature>(
                "death-detach");
        feature->isDetachFailing = true;
        if (!session->AttachFeature(feature)) {
            return 11;
        }
        session.reset();
        return 0;
    }
    if (caseName == "wrong-thread") {
        auto session =
            std::make_unique<VtkAppHostSession>(
                GetSessionConfig());
        if (!session->BuildSession()) {
            return 12;
        }
        std::thread worker(
            [session = std::move(session)]() mutable {
                session.reset();
            });
        worker.join();
        return 0;
    }
    if (caseName == "view-set-thread") {
        auto views = std::make_unique<HostViewRuntimeRegistry>();
        const auto config = GetSessionConfig();
        if (!views->Build(
                GetCoreServices(), config.renderViews)) {
            return 13;
        }
        std::thread worker(
            [views = std::move(views)]() mutable {
                views.reset();
            });
        worker.join();
        return 0;
    }
    if (caseName == "expired-input-detach-failure") {
        auto session =
            std::make_unique<VtkAppHostSession>(
                GetSessionConfig());
        if (!session->BuildSession()) {
            return 14;
        }
        auto feature =
            std::make_shared<FakeHostFeature>(
                "death-expired");
        if (!session->AttachFeature(feature)
            || !feature->m_host
            || !feature->m_host->DetachInput(
                feature->m_id)) {
            return 15;
        }
        feature.reset();
        session.reset();
        return 0;
    }
    return 16;
}

int GetLifecycleFailCount()
{
    int failureCount = 0;
    std::weak_ptr<IHostViewDirectory> staleDirectory;
    std::optional<HostDataRoute> retainedDataRoute;
    std::optional<HostViewRoute> retainedViewRoute;
    std::vector<HostInputRoute> retainedInputRoutes;
    bool isRouteBuilt = false;
    bool isCrossThreadRejected = false;
    {
        HostViewRuntimeRegistry routeOwner;
        const auto config = GetSessionConfig();
        isRouteBuilt = routeOwner.Build(
            GetCoreServices(), config.renderViews);
        staleDirectory = routeOwner.GetViewDirectory();
        const auto directory = staleDirectory.lock();
        HostViewTarget routeTarget;
        routeTarget.viewId = "lifecycle";
        HostViewTargets routeTargets;
        routeTargets.viewIds = { "lifecycle" };
        retainedDataRoute = directory
            ? directory->GetDataRoute(routeTarget)
            : std::optional<HostDataRoute>{};
        retainedViewRoute = directory
            ? directory->GetViewRoute(routeTarget)
            : std::optional<HostViewRoute>{};
        retainedInputRoutes = directory
            ? directory->GetInputRoutes(routeTargets)
            : std::vector<HostInputRoute>{};
        std::thread crossThread([&]() {
            const auto current = staleDirectory.lock();
            isCrossThreadRejected =
                current
                && !current->GetDataRoute(routeTarget)
                && !current->GetViewRoute(routeTarget)
                && current->GetInputRoutes(routeTargets).empty();
        });
        crossThread.join();
    }
    failureCount += GetCaseResult(
        isRouteBuilt
            && isCrossThreadRejected
            && retainedDataRoute
            && retainedDataRoute->data.expired()
            && retainedViewRoute
            && retainedViewRoute->view.expired()
            && retainedViewRoute->update.expired()
            && retainedViewRoute->context.expired()
            && retainedInputRoutes.size() == 1
            && retainedInputRoutes.front().context.expired()
            && retainedViewRoute->stopView
            && !retainedViewRoute->stopView()
            && staleDirectory.expired(),
        "View routes expire safely after their owner is destroyed") ? 0 : 1;

    VtkAppHostSession emptySession(HostSessionConfig{});
    HostLoadRequest rejectedLoad;
    rejectedLoad.filePath = "missing.raw";
    int rejectedCallbackCount = 0;
    failureCount += GetCaseResult(
        !emptySession.SendRequest(
            std::move(rejectedLoad),
            [&rejectedCallbackCount](bool) {
                ++rejectedCallbackCount;
            })
            && rejectedCallbackCount == 0,
        "Session Build failure rejects request without callback") ? 0 : 1;

    auto emptyViewConfig = GetSessionConfig();
    emptyViewConfig.renderViews.front().id.clear();
    VtkAppHostSession emptyViewSession(
        std::move(emptyViewConfig));
    auto duplicateViewConfig = GetSessionConfig();
    duplicateViewConfig.renderViews.push_back(
        duplicateViewConfig.renderViews.front());
    VtkAppHostSession duplicateViewSession(
        std::move(duplicateViewConfig));
    failureCount += GetCaseResult(
        !emptyViewSession.BuildSession()
            && emptyViewSession.Stop()
            && emptyViewSession.GetIsStopped()
            && !duplicateViewSession.BuildSession()
            && duplicateViewSession.Stop()
            && duplicateViewSession.GetIsStopped(),
        "Session rejects empty and duplicate render view IDs before topology commit") ? 0 : 1;

    VtkAppHostSession stopSession(GetSessionConfig());
    const bool isStopBuilt = stopSession.BuildSession();
    bool isWorkerStopAccepted = true;
    std::thread stopWorker([&]() {
        isWorkerStopAccepted = stopSession.Stop();
    });
    stopWorker.join();
    const auto workerStopState = stopSession.GetStopState();
    HostViewTarget stoppedTarget;
    stoppedTarget.viewId = "lifecycle";
    const auto requestedStopScene =
        stopSession.GetSceneViewState(stoppedTarget);
    const auto requestedStopScenes = stopSession.GetSceneViewStates();
    const bool isOwnerStopAccepted = stopSession.Stop();
    const auto stoppedRead = stopSession.GetImageReadResult(1024);
    failureCount += GetCaseResult(
        isStopBuilt
            && !isWorkerStopAccepted
            && workerStopState == HostStopState::StopRequested
            && !requestedStopScene
            && requestedStopScenes.empty()
            && isOwnerStopAccepted
            && stopSession.GetIsStopped()
            && stopSession.GetStopState() == HostStopState::Stopped
            && stopSession.GetRenderViewEndpoints().empty()
            && !stopSession.GetRenderViewEndpoint("lifecycle")
            && !stopSession.GetPrimaryEndpoint()
            && !stopSession.GetRenderViewState(stoppedTarget)
            && stopSession.GetRenderViewStates().empty()
            && !stopSession.GetSceneViewState(stoppedTarget)
            && stopSession.GetSceneViewStates().empty()
            && !stopSession.GetImageReadState()
            && stoppedRead.error == ImageReadError::NoImage
            && stoppedRead.requiredBytes == 0
            && !stoppedRead.state
            && stopSession.GetStopState() == HostStopState::Stopped
            && stopSession.Stop(),
        "Session Stop is retryable, idempotent, and getters do not rebuild it") ? 0 : 1;

    std::vector<std::function<void()>> ownerTasks;
    auto deferredConfig = GetSessionConfig();
    deferredConfig.sendOwnerTask = [&ownerTasks](
        std::function<void()> task) {
        if (!task) return false;
        ownerTasks.push_back(std::move(task));
        return true;
    };
    auto deferredSession =
        std::make_unique<VtkAppHostSession>(
            std::move(deferredConfig));
    const bool isDeferredBuilt =
        deferredSession->BuildSession();
    const auto* deferredEndpoint =
        deferredSession->GetPrimaryEndpoint();
    vtkWeakPointer<vtkRenderWindowInteractor> deferredInteractor =
        deferredEndpoint ? deferredEndpoint->interactor : nullptr;
    std::thread deferredWorker(
        [session = std::move(deferredSession)]() mutable {
            session.reset();
        });
    deferredWorker.join();
    const bool isDeferredQueued = ownerTasks.size() == 1
        && deferredInteractor;
    if (!ownerTasks.empty()) {
        auto ownerTask = std::move(ownerTasks.front());
        ownerTasks.clear();
        ownerTask();
    }
    failureCount += GetCaseResult(
        isDeferredBuilt
            && isDeferredQueued
            && !deferredInteractor
            && VtkAppHostSession::GetPendingStopCount() == 0,
        "Wrong-thread destruction delegates final Stop to the Qt owner executor") ? 0 : 1;

    const auto staleInitialCount =
        VtkAppHostSession::GetPendingStopCount();
    std::vector<std::function<void()>> staleTasks;
    auto staleConfig = GetSessionConfig();
    staleConfig.sendOwnerTask = [&staleTasks](
        std::function<void()> task) {
        if (!task) return false;
        staleTasks.push_back(std::move(task));
        return true;
    };
    auto staleSession = std::make_unique<VtkAppHostSession>(
        std::move(staleConfig));
    const bool isStaleBuilt = staleSession->BuildSession();
    const auto* staleEndpoint = staleSession->GetPrimaryEndpoint();
    vtkWeakPointer<vtkRenderWindowInteractor> staleInteractor =
        staleEndpoint ? staleEndpoint->interactor : nullptr;
    std::thread staleWorker(
        [session = std::move(staleSession)]() mutable {
            session.reset();
        });
    staleWorker.join();
    const bool isStaleQueued = staleTasks.size() == 1
        && VtkAppHostSession::GetPendingStopCount()
            == staleInitialCount + 1;
    const bool isStalePumped =
        VtkAppHostSession::SendPendingStops();
    auto liveSession = std::make_unique<VtkAppHostSession>(
        GetSessionConfig());
    const bool isLiveBuilt = liveSession->BuildSession();
    const auto* liveEndpoint = liveSession->GetPrimaryEndpoint();
    vtkWeakPointer<vtkRenderWindowInteractor> liveInteractor =
        liveEndpoint ? liveEndpoint->interactor : nullptr;
    if (!staleTasks.empty()) {
        auto staleTask = std::move(staleTasks.front());
        staleTasks.clear();
        staleTask();
    }
    const bool isLiveRetained = liveInteractor
        && liveSession->GetPrimaryEndpoint() == liveEndpoint;
    const bool isLiveStopped = liveSession->Stop();
    failureCount += GetCaseResult(
        isStaleBuilt
            && isStaleQueued
            && isStalePumped
            && !staleInteractor
            && isLiveBuilt
            && isLiveRetained
            && isLiveStopped
            && !liveInteractor
            && VtkAppHostSession::GetPendingStopCount()
                == staleInitialCount,
        "A delayed Stop callback is token-idempotent and cannot stop a newer Session") ? 0 : 1;

    const auto getReaperValid = [](
        const std::optional<bool> isDispatcherThrowing) {
        const auto initialCount =
            VtkAppHostSession::GetPendingStopCount();
        int diagnosticCount = 0;
        auto config = GetSessionConfig();
        config.sendDiagnostic = [&diagnosticCount](const std::string&) {
            ++diagnosticCount;
        };
        if (isDispatcherThrowing) {
            config.sendOwnerTask = [isDispatcherThrowing](
                std::function<void()>) {
                if (*isDispatcherThrowing) {
                    throw std::runtime_error("dispatcher failure");
                }
                return false;
            };
        }
        auto session = std::make_unique<VtkAppHostSession>(
            std::move(config));
        if (!session->BuildSession()) return false;
        const auto* endpoint = session->GetPrimaryEndpoint();
        vtkWeakPointer<vtkRenderWindowInteractor> interactor =
            endpoint ? endpoint->interactor : nullptr;
        std::thread worker(
            [session = std::move(session)]() mutable {
                session.reset();
            });
        worker.join();
        const bool isRetained = interactor
            && VtkAppHostSession::GetPendingStopCount()
                == initialCount + 1;
        const bool isReleased =
            VtkAppHostSession::SendPendingStops();
        return isRetained
            && isReleased
            && !interactor
            && diagnosticCount > 0
            && VtkAppHostSession::GetPendingStopCount()
                == initialCount;
    };
    failureCount += GetCaseResult(
        getReaperValid(std::nullopt),
        "Missing owner dispatcher retains a pumpable StopPending session") ? 0 : 1;
    failureCount += GetCaseResult(
        getReaperValid(false),
        "Rejected owner dispatch remains recoverable by the owner reaper") ? 0 : 1;
    failureCount += GetCaseResult(
        getReaperValid(true),
        "Throwing owner dispatch remains recoverable by the owner reaper") ? 0 : 1;

    const auto pendingBaseCount =
        VtkAppHostSession::GetPendingStopCount();
    std::vector<std::function<void()>> retryTasks;
    auto retryConfig = GetSessionConfig();
    retryConfig.sendOwnerTask = [&retryTasks](
        std::function<void()> task) {
        if (!task) return false;
        retryTasks.push_back(std::move(task));
        return true;
    };
    auto retrySession = std::make_unique<VtkAppHostSession>(
        std::move(retryConfig));
    auto reaperFeature = std::make_shared<FakeHostFeature>(
        "reaper-retry");
    reaperFeature->isDetachFailing = true;
    const bool isRetryBuilt = retrySession->BuildSession()
        && retrySession->AttachFeature(reaperFeature);
    const auto* retryEndpoint = retrySession->GetPrimaryEndpoint();
    vtkWeakPointer<vtkRenderWindowInteractor> retryInteractor =
        retryEndpoint ? retryEndpoint->interactor : nullptr;
    std::thread retryWorker(
        [session = std::move(retrySession)]() mutable {
            session.reset();
        });
    retryWorker.join();
    const bool isRetryQueued = retryTasks.size() == 1
        && retryInteractor
        && VtkAppHostSession::GetPendingStopCount()
            == pendingBaseCount + 1;
    if (!retryTasks.empty()) {
        auto ownerTask = std::move(retryTasks.front());
        retryTasks.clear();
        ownerTask();
    }
    const bool isFirstStopRetained = retryInteractor
        && VtkAppHostSession::GetPendingStopCount()
            == pendingBaseCount + 1;
    reaperFeature->isDetachFailing = false;
    const bool isRetryReleased =
        VtkAppHostSession::SendPendingStops();
    failureCount += GetCaseResult(
        isRetryBuilt
            && isRetryQueued
            && isFirstStopRetained
            && isRetryReleased
            && !retryInteractor
            && reaperFeature->detachTryCount >= 2
            && reaperFeature->detachCount == 1
            && VtkAppHostSession::GetPendingStopCount()
                == pendingBaseCount,
        "Deferred Stop failure remains owned and succeeds through owner retry") ? 0 : 1;

    VtkAppHostSession moveSource(GetSessionConfig());
    VtkAppHostSession moveTarget(std::move(moveSource));
    HostLoadRequest movedLoad;
    movedLoad.filePath = "missing.raw";
    int movedCallbackCount = 0;
    failureCount += GetCaseResult(
        !moveSource.SendRequest(
            std::move(movedLoad),
            [&movedCallbackCount](bool) {
                ++movedCallbackCount;
            })
            && movedCallbackCount == 0
            && moveSource.GetRenderViewEndpoints().empty()
            && !moveSource.GetRenderViewEndpoint("lifecycle")
            && !moveSource.GetPrimaryEndpoint(),
        "Moved-from Session rejects requests and endpoint access") ? 0 : 1;

    auto session = std::make_unique<VtkAppHostSession>(
        GetSessionConfig());
    auto feature = std::make_shared<FakeHostFeature>("feature-a");
    const HostViewTarget unbuiltTarget{
        "lifecycle", false, HostRenderViewRole::Primary3D };

    failureCount += GetCaseResult(
        !session->AttachFeature(feature)
            && feature->attachCount == 0,
        "Feature attach requires an already-built Session") ? 0 : 1;
    failureCount += GetCaseResult(
        !session->GetSceneViewState(unbuiltTarget)
            && session->GetSceneViewStates().empty(),
        "Scene getters reject an unbuilt Session") ? 0 : 1;
    const bool isLifecycleBuilt = session->BuildSession();
    const bool isLifecycleStarted =
        isLifecycleBuilt && session->Start();
    failureCount += GetCaseResult(
        isLifecycleBuilt && isLifecycleStarted,
        "Lifecycle fixture builds a Session") ? 0 : 1;

    auto rejectedAttach =
        std::make_shared<FakeHostFeature>(
            "feature-rejected");
    rejectedAttach->isAttachFailing = true;
    const bool isRejectedAttach =
        !session->AttachFeature(rejectedAttach);
    auto rejectedReplacement =
        std::make_shared<FakeHostFeature>(
            "feature-rejected");
    const bool isRejectedReplacementAttached =
        session->AttachFeature(rejectedReplacement);
    const bool isRejectedReplacementDetached =
        session->DetachFeature(*rejectedReplacement);
    failureCount += GetCaseResult(
        isRejectedAttach
            && rejectedAttach->attachCount == 1
            && rejectedAttach->detachCount == 1
            && !rejectedAttach->isAttached
            && isRejectedReplacementAttached
            && isRejectedReplacementDetached,
        "Rejected Feature attach rolls back partial host binding") ? 0 : 1;

    const auto beforeUseCount = feature.use_count();
    const bool isAttached = session->AttachFeature(feature);
    HostViewTargets activeViews;
    activeViews.viewIds = { "lifecycle" };
    HostViewTargets missingViews;
    missingViews.viewIds = { "missing" };
    const bool isActiveSet =
        feature->SetActiveViews(activeViews);
    const bool isMissingRejected =
        !feature->SetActiveViews(missingViews);
    const bool isActiveCleared =
        feature->SetActiveViews({});
    failureCount += GetCaseResult(
        isAttached
            && feature->attachCount == 1
            && isActiveSet
            && isMissingRejected
            && isActiveCleared
            && feature.use_count() == beforeUseCount + 1,
        "Session owns an attached Feature and validates active views") ? 0 : 1;

    auto sceneFeatureA =
        std::make_shared<FakeHostFeature>("a-scene-feature");
    auto sceneFeatureZ =
        std::make_shared<FakeHostFeature>("z-scene-feature");
    const bool isSceneFeatureASet = session->AttachFeature(sceneFeatureA)
        && sceneFeatureA->SetActiveViews(activeViews);
    const bool isSceneFeatureZSet = session->AttachFeature(sceneFeatureZ)
        && sceneFeatureZ->SetActiveViews(activeViews);
    const HostViewTarget sceneTarget{
        "lifecycle", false, HostRenderViewRole::Primary3D };
    const auto* sceneEndpoint = session->GetPrimaryEndpoint();
    const bool isActiveSceneFlushed = sceneEndpoint
        && SendTimer(sceneEndpoint->interactor);
    const auto activeScene = session->GetSceneViewState(sceneTarget);
    const std::vector<std::string> expectedSceneFeatureIds{
        "a-scene-feature", "z-scene-feature" };
    sceneFeatureZ->isDetachFailing = true;
    const bool isSceneDetachRejected =
        !session->DetachFeature(*sceneFeatureZ);
    const bool isRejectedSceneFlushed = sceneEndpoint
        && SendTimer(sceneEndpoint->interactor);
    const auto rejectedScene = session->GetSceneViewState(sceneTarget);
    sceneFeatureZ->isDetachFailing = false;
    const bool isSceneFeatureZDetached =
        session->DetachFeature(*sceneFeatureZ);
    const bool isDetachedSceneFlushed = sceneEndpoint
        && SendTimer(sceneEndpoint->interactor);
    const auto detachedScene = session->GetSceneViewState(sceneTarget);
    const bool isSceneFeatureADetached =
        session->DetachFeature(*sceneFeatureA);
    failureCount += GetCaseResult(
        isSceneFeatureASet
            && isSceneFeatureZSet
            && isActiveSceneFlushed
            // feature-a 仍只保留 AttachInput；清空 active views 后不得进入场景投影。
            && feature->isAttached
            && activeScene
            && activeScene->activeFeatureIds == expectedSceneFeatureIds
            && isSceneDetachRejected
            && isRejectedSceneFlushed
            && rejectedScene
            && rejectedScene->activeFeatureIds == expectedSceneFeatureIds
            && isSceneFeatureZDetached
            && isDetachedSceneFlushed
            && detachedScene
            && detachedScene->activeFeatureIds
                == std::vector<std::string>{ "a-scene-feature" }
            && isSceneFeatureADetached,
        "Scene snapshot projects sorted active Features and detach rollback") ? 0 : 1;

    auto crossThreadFeature =
        std::make_shared<FakeHostFeature>("feature-worker");
    bool isCrossBuildAccepted = true;
    bool isCrossRequestAccepted = true;
    bool isCrossAttachAccepted = true;
    bool isCrossDetachAccepted = true;
    bool hasCrossScene = true;
    bool hasCrossScenes = true;
    std::thread lifecycleWorker([&]() {
        isCrossBuildAccepted = session->BuildSession();
        HostViewResetRequest reset;
        reset.targetView.viewId = "lifecycle";
        isCrossRequestAccepted =
            session->SendRequest(std::move(reset));
        isCrossAttachAccepted =
            session->AttachFeature(crossThreadFeature);
        isCrossDetachAccepted =
            session->DetachFeature(*feature);
        hasCrossScene = session->GetSceneViewState(sceneTarget).has_value();
        hasCrossScenes = !session->GetSceneViewStates().empty();
    });
    lifecycleWorker.join();
    const auto ownerScene = session->GetSceneViewState(sceneTarget);
    failureCount += GetCaseResult(
        !isCrossBuildAccepted
            && !isCrossRequestAccepted
            && !isCrossAttachAccepted
            && !isCrossDetachAccepted
            && !hasCrossScene
            && !hasCrossScenes
            && ownerScene
            && crossThreadFeature->attachCount == 0
            && feature->detachCount == 0,
        "Session requests and Feature lifecycle require the owner thread") ? 0 : 1;

    auto duplicateId =
        std::make_shared<FakeHostFeature>("feature-a");
    auto emptyId =
        std::make_shared<FakeHostFeature>("");
    failureCount += GetCaseResult(
        !session->AttachFeature(feature)
            && !session->AttachFeature(duplicateId)
            && !session->AttachFeature(emptyId),
        "Feature registry rejects duplicate object, duplicate ID and empty ID") ? 0 : 1;

    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = {
        "lifecycle", false,
        HostRenderViewRole::Primary3D };
    const auto* endpoint = session->GetPrimaryEndpoint();
    const auto ownerThread = std::this_thread::get_id();
    std::thread::id completeThread;
    bool isComplete = false;
    bool isQueued = false;
    feature->tickCount = 0;
    std::thread worker([&]() {
        isQueued = feature->SendOwnerComplete([&]() {
            completeThread = std::this_thread::get_id();
            isComplete = true;
        });
    });
    worker.join();
    failureCount += GetCaseResult(
        isQueued && !isComplete,
        "Feature completion is queued without running on the worker thread") ? 0 : 1;
    if (session->AttachTimer(timer)
        && endpoint
        && endpoint->interactor) {
        (void)SendTimer(endpoint->interactor, 1);
    }
    failureCount += GetCaseResult(
        feature->tickCount == 0 && !isComplete,
        "A foreign timer ID cannot drive the Session tick") ? 0 : 1;
    if (endpoint && endpoint->interactor) {
        (void)SendTimer(endpoint->interactor);
    }
    failureCount += GetCaseResult(
        feature->tickCount == 1
            && isComplete
            && completeThread == ownerThread,
        "Session tick calls the Feature and drains completion on the owner thread") ? 0 : 1;

    std::weak_ptr<FakeHostFeature> weakFeature = feature;
    const bool isDetached =
        session->DetachFeature(*feature);
    failureCount += GetCaseResult(
        isDetached
            && feature->detachCount == 1
            && feature->isAttached == false,
        "Detach disconnects Feature without destroying it") ? 0 : 1;
    feature.reset();
    failureCount += GetCaseResult(
        weakFeature.expired(),
        "Detached Feature expires after its upper owner releases it") ? 0 : 1;

    auto retryFeature =
        std::make_shared<FakeHostFeature>("feature-retry");
    const bool isRetryAttached =
        session->AttachFeature(retryFeature);
    HostViewTargets retryViews;
    retryViews.viewIds = { "lifecycle" };
    HostViewTarget retryView;
    retryView.viewId = "lifecycle";
    const bool isRetryViewSet =
        retryFeature->SetActiveViews(retryViews);
    const auto activeRetryState =
        session->GetRenderViewState(retryView);
    retryFeature->isDetachFailing = true;
    const bool isFirstDetachRejected =
        !session->DetachFeature(*retryFeature);
    const auto rejectedRetryState =
        session->GetRenderViewState(retryView);
    const bool isRetryPending = retryFeature->isAttached;
    retryFeature->isDetachFailing = false;
    const bool isRetryDetached =
        session->DetachFeature(*retryFeature);
    failureCount += GetCaseResult(
        isRetryAttached
            && isRetryViewSet
            && activeRetryState
            && activeRetryState->isFeatureActive
            && isFirstDetachRejected
            && isRetryPending
            && rejectedRetryState
            && rejectedRetryState->isFeatureActive
            && isRetryDetached
            && retryFeature->detachTryCount == 2
            && retryFeature->detachCount == 1,
        "Detach failure restores active views and preserves the registry for retry") ? 0 : 1;

    auto expiredFeature =
        std::make_shared<FakeHostFeature>(
            "feature-expired");
    const bool isExpiredAttached =
        session->AttachFeature(expiredFeature);
    std::weak_ptr<FakeHostFeature> expiredWeak =
        expiredFeature;
    expiredFeature.reset();
    auto replacement =
        std::make_shared<FakeHostFeature>(
            "feature-expired");
    const bool isBeforeTickRejected =
        !session->AttachFeature(replacement);
    const bool isRetained = !expiredWeak.expired();
    auto retainedFeature = expiredWeak.lock();
    const bool isRetainedDetached = retainedFeature
        && session->DetachFeature(*retainedFeature);
    retainedFeature.reset();
    const bool isAfterDetachAttached =
        session->AttachFeature(replacement);
    const bool isReplacementDetached =
        session->DetachFeature(*replacement);
    failureCount += GetCaseResult(
        isExpiredAttached
            && isRetained
            && isBeforeTickRejected
            && isRetainedDetached
            && expiredWeak.expired()
            && isAfterDetachAttached
            && isReplacementDetached,
        "Session retains an attached Feature until explicit owner-thread detach") ? 0 : 1;

    failureCount += GetCaseResult(
        GetSafeExit("detach-failure"),
        "Detach failure no longer terminates the host process") ? 0 : 1;
    failureCount += GetCaseResult(
        GetSafeExit("wrong-thread"),
        "Wrong-thread Session destruction no longer terminates the host process") ? 0 : 1;
    failureCount += GetCaseResult(
        GetSafeExit("view-set-thread"),
        "Wrong-thread ViewSet destruction no longer terminates the host process") ? 0 : 1;
    failureCount += GetCaseResult(
        GetSafeExit(
            "expired-input-detach-failure"),
        "Failed Feature cleanup remains owned without terminating the process") ? 0 : 1;

    auto fallback =
        std::make_shared<FakeHostFeature>("feature-b");
    session->AttachFeature(fallback);
    const auto* fallbackEndpoint =
        session->GetPrimaryEndpoint();
    vtkWeakPointer<vtkRenderWindowInteractor> interactor =
        fallbackEndpoint ? fallbackEndpoint->interactor : nullptr;
    session.reset();
    failureCount += GetCaseResult(
        fallback->detachCount == 1
            && fallback->isAttached == false
            && !interactor,
        "Session destruction defensively detaches live Features in owner order") ? 0 : 1;

    auto throwingSession =
        std::make_unique<VtkAppHostSession>(
            GetSessionConfig());
    throwingSession->BuildSession();
    auto throwing =
        std::make_shared<FakeHostFeature>("throwing");
    throwing->isAttachThrowing = true;
    failureCount += GetCaseResult(
        !throwingSession->AttachFeature(throwing),
        "Feature attach exceptions do not cross the Session boundary") ? 0 : 1;
    return failureCount;
}
