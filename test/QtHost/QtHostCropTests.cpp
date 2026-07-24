#include "QtHostMethodCases.h"

#include "Host/CropHostFeature.h"
#include "Host/VtkAppHostSession.h"

#include <vtkCommand.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {
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
    const bool isBuilt = session.BuildSession();
    const bool isAttached =
        session.AttachFeature(feature);
    const bool isInputAttached =
        session.AttachHotkeys({}, {});
    const auto* endpoint = session.GetPrimaryEndpoint();
    if (!isBuilt || !isAttached || !isInputAttached
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
    SendTicks(*endpoint, 1);
    failureCount += GetCaseResult(
        !isPendingAccepted
            && isDetached
            && feature.use_count() == useCount
            && !hasDetachedCallback,
        "Crop detach leaves the upper owner alive and suppresses queued callbacks") ? 0 : 1;
    return failureCount;
}
