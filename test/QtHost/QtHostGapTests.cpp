#include "QtHostMethodCases.h"

#include "Host/GapHostFeature.h"
#include "Host/HostRenderViewSet.h"
#include "Host/VtkAppHostSession.h"

#include <vtkCommand.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

class InputPortStub final : public HostInputPort {
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

private:
    std::string m_featureId;
    int m_attachCount = 0;
    int m_detachCount = 0;
    bool m_isAttached = false;
};

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
        GapIsoMode::DataRangeRatio;
    config.defaultStart.surface.dataRangeRatio = 0.5;
    config.defaultStart.voidParams.grayMin = 0.0f;
    config.defaultStart.voidParams.grayMax = 0.25f;
    config.defaultStart.voidParams.minVolumeMM3 = 0.0;
    config.defaultStart.voidParams.angleThresholdDeg = 40.0f;
    config.defaultStart.voidParams.tensorWindowSize = 1;
    config.defaultStart.voidParams.erosionIterations = 0;
    config.inputViews.viewIds = { "gap-primary" };
    config.keys.switchOverlay.keyCode = 'j';
    config.keys.exit.keySym = "Escape";
    return config;
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
        endpoint.interactor->InvokeEvent(
            vtkCommand::TimerEvent);
        endpoint.renderWindow->Render();
    }
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

} // namespace

int GetGapFailCount()
{
    int failureCount = 0;
    HostRenderViewSet directViews;
    InputPortStub directInput;
    HostFeatureContext directContext;
    directContext.renderViews = &directViews;
    directContext.inputPort = &directInput;
    directContext.getImageSnapshot = []() {
        return ImageSnapshot{};
    };
    auto directFeature =
        std::make_shared<GapHostFeature>(
            GetGapConfig());
    const bool isDirectAttached =
        directFeature->AttachHost(directContext);
    const bool isDuplicateRejected =
        !directFeature->AttachHost(directContext);
    const bool isDirectDetached =
        directFeature->DetachHost();
    failureCount += GetCaseResult(
        isDirectAttached
            && isDuplicateRejected
            && isDirectDetached
            && directInput.GetAttachCount() == 1
            && directInput.GetDetachCount() == 1,
        "Gap Feature accepts only the first direct AttachHost call") ? 0 : 1;

    VtkAppHostSession session(GetGapSessionConfig());
    auto feature = std::make_shared<GapHostFeature>(
        GetGapConfig());
    const auto beforeUseCount = feature.use_count();
    const bool isBuilt = session.BuildSession();
    const bool isAttached = session.AttachFeature(feature);
    const bool isInputAttached =
        session.AttachHotkeys({}, {});
    const auto* endpoint = session.GetPrimaryEndpoint();
    if (!isBuilt || !isAttached || !isInputAttached
        || !endpoint
        || !endpoint->interactor
        || !endpoint->renderWindow) {
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
    const bool isReloadReady =
        GetReloadReady(session, *endpoint);
    failureCount += GetCaseResult(
        isTimerAttached
            && isReloadReady
            && feature.use_count() == beforeUseCount,
        "Session owns no strong Gap Feature handle and publishes input") ? 0 : 1;

    GapHostStartRequest missingTarget =
        GetGapConfig().defaultStart;
    missingTarget.targetViews = {};
    const bool isStrict =
        !feature->SendRequest({
            GapHostAction::None, std::monostate{} })
        && !feature->SendRequest({
            GapHostAction::Start, std::monostate{} })
        && !feature->SendRequest({
            GapHostAction::Start, missingTarget })
        && !feature->SendRequest({
            GapHostAction::Overlay, std::monostate{} })
        && !feature->SendRequest({
            GapHostAction::Exit, std::monostate{} })
        && !feature->SendRequest({
            GapHostAction::Overlay, std::monostate{} },
            [](bool) {});
    failureCount += GetCaseResult(
        isStrict,
        "Gap request matrix rejects invalid payload, target, callback and inactive actions") ? 0 : 1;

    int firstCompleteCount = 0;
    bool isFirstSucceeded = false;
    const auto start = GetGapConfig().defaultStart;
    const bool isFirstAccepted =
        feature->SendRequest(
            { GapHostAction::Start, start },
            [&firstCompleteCount, &isFirstSucceeded](
                const bool isSuccess) {
                ++firstCompleteCount;
                isFirstSucceeded = isSuccess;
            });
    bool hasRejectedCallback = false;
    const bool isSecondRejected =
        !feature->SendRequest(
            { GapHostAction::Start, start },
            [&hasRejectedCallback](bool) {
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
            && isSecondRejected
            && firstCompleteCount == 1
            && isFirstSucceeded
            && !hasRejectedCallback,
        "Rejected Start preserves the accepted callback generation") ? 0 : 1;

    const bool isOverlaySwitched =
        feature->SendRequest({
            GapHostAction::Overlay,
            std::monostate{} });
    failureCount += GetCaseResult(
        isOverlaySwitched,
        "Gap overlay is controlled through the real Feature API") ? 0 : 1;

    const bool isNextReloadReady =
        GetReloadReady(session, *endpoint);
    SendTicks(*endpoint, 2);
    const bool isStaleOverlayRejected =
        !feature->SendRequest({
            GapHostAction::Overlay,
            std::monostate{} });
    failureCount += GetCaseResult(
        isNextReloadReady && isStaleOverlayRejected,
        "DataVersion change exits the stale Gap view") ? 0 : 1;

    const bool isFirstDetached =
        session.DetachFeature(*feature);
    auto pendingFeature =
        std::make_shared<GapHostFeature>(
            GetGapConfig());
    const auto pendingUseCount =
        pendingFeature.use_count();
    const bool isPendingAttached =
        session.AttachFeature(pendingFeature);
    bool hasDetachedCallback = false;
    const bool isPendingAccepted =
        pendingFeature->SendRequest(
            { GapHostAction::Start, start },
            [&hasDetachedCallback](bool) {
                hasDetachedCallback = true;
            });
    const bool isDetached =
        session.DetachFeature(*pendingFeature);
    SendTicks(*endpoint, 2);
    failureCount += GetCaseResult(
        isFirstDetached
            && isPendingAttached
            && isPendingAccepted,
        "Gap accepts a pending request before detach") ? 0 : 1;
    failureCount += GetCaseResult(
        isDetached,
        "Gap detach succeeds on the Session owner thread") ? 0 : 1;
    failureCount += GetCaseResult(
        pendingFeature.use_count() == pendingUseCount,
        "Gap detach keeps the upper owner alive") ? 0 : 1;
    failureCount += GetCaseResult(
        !hasDetachedCallback,
        "Gap detach blocks stale callbacks") ? 0 : 1;
    return failureCount;
}
