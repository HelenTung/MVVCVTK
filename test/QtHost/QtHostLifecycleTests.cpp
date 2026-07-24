#include "QtHostMethodCases.h"

#include "Host/HostFeature.h"
#include "Host/VtkAppHostSession.h"

#include <vtkCommand.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkWeakPointer.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

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
        if (!context.sendOwnerComplete) {
            return false;
        }
        m_sendOwnerComplete = context.sendOwnerComplete;
        isAttached = true;
        ++attachCount;
        return true;
    }

    bool DetachHost() override
    {
        if (isDetachThrowing) {
            throw 1;
        }
        if (!isAttached) {
            return true;
        }
        isAttached = false;
        m_sendOwnerComplete = {};
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
        return m_sendOwnerComplete
            && m_sendOwnerComplete(std::move(complete));
    }

    std::string m_id;
    std::function<bool(std::function<void()>)>
        m_sendOwnerComplete;
    int attachCount = 0;
    int detachCount = 0;
    int tickCount = 0;
    bool isAttached = false;
    bool isAttachThrowing = false;
    bool isDetachThrowing = false;
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
}

int GetLifecycleFailCount()
{
    int failureCount = 0;
    auto session = std::make_unique<VtkAppHostSession>(
        GetSessionConfig());
    auto feature = std::make_shared<FakeHostFeature>("feature-a");

    failureCount += GetCaseResult(
        !session->AttachFeature(feature)
            && feature->attachCount == 0,
        "Feature attach requires an already-built Session") ? 0 : 1;
    failureCount += GetCaseResult(
        session->BuildSession(),
        "Lifecycle fixture builds a Session") ? 0 : 1;

    const auto beforeUseCount = feature.use_count();
    const bool isAttached = session->AttachFeature(feature);
    failureCount += GetCaseResult(
        isAttached
            && feature->attachCount == 1
            && feature.use_count() == beforeUseCount,
        "Session registers only a weak Feature handle") ? 0 : 1;

    auto crossThreadFeature =
        std::make_shared<FakeHostFeature>("feature-worker");
    bool isCrossAttachAccepted = true;
    bool isCrossDetachAccepted = true;
    std::thread lifecycleWorker([&]() {
        isCrossAttachAccepted =
            session->AttachFeature(crossThreadFeature);
        isCrossDetachAccepted =
            session->DetachFeature(*feature);
    });
    lifecycleWorker.join();
    failureCount += GetCaseResult(
        !isCrossAttachAccepted
            && !isCrossDetachAccepted
            && crossThreadFeature->attachCount == 0
            && feature->detachCount == 0,
        "Feature attach and detach are restricted to the Session owner thread") ? 0 : 1;

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
        endpoint->interactor->InvokeEvent(
            vtkCommand::TimerEvent);
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
