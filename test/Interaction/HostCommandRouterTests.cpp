#include "Host/HostCommandRouter.h"
#include "HostCommandRouterTests.h"

#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class HostRouterCases final {
public:

class HostRouterFixture final {
public:
    HostRouterFixture()
        : m_bindings(std::make_shared<HostFeatureBindings>())
    {
    }

    bool CreateView(
        const std::string& id,
        HostRenderViewRole role,
        std::shared_ptr<StdRenderContext> context)
    {
        return m_views.CreateView(id, role, std::move(context));
    }

    bool BuildRouter()
    {
        m_router = std::make_unique<HostCommandRouter>(m_core, m_views, m_bindings);
        return m_router != nullptr;
    }

    bool SetViewConfig(const HostViewConfig& viewConfig)
    {
        return SendCommand(HostCommand{ HostViewCommand{ viewConfig } });
    }

    bool SendCommand(HostCommand command)
    {
        return m_router && m_router->DispatchCommand(std::move(command));
    }

    std::shared_ptr<VizService> GetViewService(const std::string& id) const
    {
        const auto* view = m_views.GetViewBySelector(
            id,
            false,
            HostRenderViewRole::Auxiliary);
        return view ? view->service : nullptr;
    }

    const std::shared_ptr<HostFeatureBindings>& GetBindings() const
    {
        return m_bindings;
    }

private:
    HostCoreServices m_core;
    HostRenderViewSet m_views;
    std::shared_ptr<HostFeatureBindings> m_bindings;
    std::unique_ptr<HostCommandRouter> m_router;
};

bool GetExpected(bool isExpected, const char* message)
{
    if (!isExpected) {
        std::cerr << message << '\n';
    }
    return isExpected;
}

HostCommandInputConfig GetGapInput(const std::string& viewId)
{
    HostCommandInputConfig commandInput;
    commandInput.isHotkeyEnabled = true;
    commandInput.targetViewIds = { viewId };
    commandInput.gapViewRequest.targetViewIds = { "gapOverlay" };
    commandInput.gapViewRequest.targetViewRoles = { HostRenderViewRole::Auxiliary };
    commandInput.gapViewRequest.isDefaultOverlayUsed = true;
    commandInput.gapViewRequest.algorithm.emplace();
    commandInput.gapViewRequest.algorithm->surface.isoMode =
        HostGapAnalysisIsoMode::AbsoluteValue;
    commandInput.gapViewRequest.algorithm->surface.dataRangeRatio = 0.25;
    commandInput.gapViewRequest.algorithm->surface.absoluteIsoValue = 84.0;
    commandInput.gapViewRequest.algorithm->voidDetection.grayMin = 12.0f;
    commandInput.gapViewRequest.algorithm->voidDetection.grayMax = 220.0f;
    commandInput.gapViewRequest.algorithm->voidDetection.minVolumeMM3 = 1.5f;
    commandInput.gapViewRequest.algorithm->voidDetection.angleThresholdDeg = 28.0f;
    commandInput.gapViewRequest.algorithm->voidDetection.tensorWindowSize = 5;
    commandInput.gapViewRequest.algorithm->voidDetection.erosionIterations = 2;
    return commandInput;
}

bool GetGapRequestEqual(
    const HostGapViewRequest& actual,
    const HostGapViewRequest& expected)
{
    if (actual.targetViewIds != expected.targetViewIds
        || actual.targetViewRoles != expected.targetViewRoles
        || actual.isDefaultOverlayUsed != expected.isDefaultOverlayUsed
        || actual.algorithm.has_value() != expected.algorithm.has_value()) {
        return false;
    }
    if (!actual.algorithm) {
        return true;
    }

    const auto& actualSurface = actual.algorithm->surface;
    const auto& expectedSurface = expected.algorithm->surface;
    const auto& actualVoid = actual.algorithm->voidDetection;
    const auto& expectedVoid = expected.algorithm->voidDetection;
    return actualSurface.isoMode == expectedSurface.isoMode
        && actualSurface.dataRangeRatio == expectedSurface.dataRangeRatio
        && actualSurface.absoluteIsoValue == expectedSurface.absoluteIsoValue
        && actualVoid.grayMin == expectedVoid.grayMin
        && actualVoid.grayMax == expectedVoid.grayMax
        && actualVoid.minVolumeMM3 == expectedVoid.minVolumeMM3
        && actualVoid.angleThresholdDeg == expectedVoid.angleThresholdDeg
        && actualVoid.tensorWindowSize == expectedVoid.tensorWindowSize
        && actualVoid.erosionIterations == expectedVoid.erosionIterations;
}

bool StartViewModeCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    auto context = std::make_shared<StdRenderContext>();
    isPassed = GetExpected(
        fixture.CreateView("mode", HostRenderViewRole::Primary3D, context),
        "View mode case should create the target view.") && isPassed;
    isPassed = GetExpected(
        fixture.BuildRouter(),
        "View mode case should build the router.") && isPassed;

    const auto service = fixture.GetViewService("mode");
    isPassed = GetExpected(
        service != nullptr,
        "View mode case should expose the target service.") && isPassed;

    HostViewConfig viewConfig;
    viewConfig.viewId = "mode";
    viewConfig.mode = VizMode::SliceTop_down;
    isPassed = GetExpected(
        fixture.SendCommand(HostCommand{ HostViewCommand{ viewConfig } }),
        "View mode case should dispatch the config command.") && isPassed;
    isPassed = GetExpected(
        service && service->GetVizModeSetCount() == 1
            && service->GetVizMode() == VizMode::SliceTop_down,
        "View mode command should update the service exactly once.") && isPassed;
    isPassed = GetExpected(
        context->GetCameraStyleSetCount() == 1
            && context->GetVizMode() == VizMode::SliceTop_down,
        "View mode command should update the render context exactly once.") && isPassed;
    isPassed = GetExpected(
        service && service->GetVizMode() == context->GetVizMode(),
        "View mode command should keep service and context modes equal.") && isPassed;
    return isPassed;
}

bool StartNoContextModeCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    isPassed = GetExpected(
        fixture.CreateView("modeNoContext", HostRenderViewRole::Primary3D, nullptr),
        "Missing-context mode case should create the target view.") && isPassed;
    isPassed = GetExpected(
        fixture.BuildRouter(),
        "Missing-context mode case should build the router.") && isPassed;

    const auto service = fixture.GetViewService("modeNoContext");
    HostViewConfig viewConfig;
    viewConfig.viewId = "modeNoContext";
    viewConfig.mode = VizMode::SliceTop_down;
    isPassed = GetExpected(
        !fixture.SetViewConfig(viewConfig),
        "View mode command should reject a target without render context.") && isPassed;
    isPassed = GetExpected(
        service && service->GetVizModeSetCount() == 0,
        "Rejected view mode command should not update the service.") && isPassed;
    return isPassed;
}

bool StartLoadDispatchCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    isPassed = GetExpected(
        fixture.CreateView("load", HostRenderViewRole::Primary3D, nullptr),
        "Load case should create a primary view.") && isPassed;
    isPassed = GetExpected(fixture.BuildRouter(), "Load case should build the router.") && isPassed;

    int callbackCount = 0;
    bool isCallbackSuccess = false;
    HostLoadCommand command;
    command.request.isInitialLoadEnabled = true;
    command.request.filePath = "sample.raw";
    command.request.geometry.emplace(
        std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
    command.onComplete = [&callbackCount, &isCallbackSuccess](bool isSuccess) {
        ++callbackCount;
        isCallbackSuccess = isSuccess;
    };

    isPassed = GetExpected(
        fixture.SendCommand(HostCommand{ std::move(command) }),
        "Load case should accept the valid request.") && isPassed;
    const auto service = fixture.GetViewService("load");
    isPassed = GetExpected(
        service && service->GetLoadCount() == 1
            && service->GetLoadPath() == "sample.raw",
        "Load case should call the selected service exactly once.") && isPassed;
    isPassed = GetExpected(
        callbackCount == 1 && isCallbackSuccess
            && fixture.GetBindings()->GetCropInputCount() == 1,
        "Load case should forward completion and refresh crop input.") && isPassed;

    int rejectedCallbackCount = 0;
    HostLoadCommand rejectedCommand;
    rejectedCommand.request.isInitialLoadEnabled = true;
    rejectedCommand.onComplete = [&rejectedCallbackCount](bool) {
        ++rejectedCallbackCount;
    };
    isPassed = GetExpected(
        !fixture.SendCommand(HostCommand{ std::move(rejectedCommand) })
            && rejectedCallbackCount == 0,
        "Rejected Load command should not invoke its callback.") && isPassed;
    return isPassed;
}

bool StartReloadDispatchCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    isPassed = GetExpected(
        fixture.CreateView("reload", HostRenderViewRole::Primary3D, nullptr),
        "Reload case should create a primary view.") && isPassed;
    isPassed = GetExpected(
        fixture.BuildRouter(),
        "Reload case should build the router.") && isPassed;
    const auto service = fixture.GetViewService("reload");
    isPassed = GetExpected(
        service != nullptr,
        "Reload case should expose the selected service.") && isPassed;

    HostReloadCommand invalidCommand;
    invalidCommand.request.voxels.assign(7, 1.0f);
    invalidCommand.request.dimensions = { 2, 2, 2 };
    invalidCommand.request.geometry.emplace(
        std::array<float, 3>{ 0.5f, 1.0f, 1.5f },
        std::array<float, 3>{ 4.0f, 5.0f, 6.0f });
    int invalidCallbackCount = 0;
    invalidCommand.onComplete = [&invalidCallbackCount](bool) {
        ++invalidCallbackCount;
    };
    isPassed = GetExpected(
        !fixture.SendCommand(HostCommand{ std::move(invalidCommand) })
            && service && service->GetReloadCount() == 0
            && invalidCallbackCount == 0,
        "Reload case should reject a voxel-count mismatch without a callback.") && isPassed;

    HostReloadCommand invalidDimensionsCommand;
    invalidDimensionsCommand.request.voxels.assign(1, 1.0f);
    invalidDimensionsCommand.request.dimensions = { 1, 0, 1 };
    invalidDimensionsCommand.request.geometry.emplace(
        std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
    int invalidDimensionsCallbackCount = 0;
    invalidDimensionsCommand.onComplete = [&invalidDimensionsCallbackCount](bool) {
        ++invalidDimensionsCallbackCount;
    };
    isPassed = GetExpected(
        !fixture.SendCommand(HostCommand{ std::move(invalidDimensionsCommand) })
            && service->GetReloadCount() == 0
            && invalidDimensionsCallbackCount == 0,
        "Reload case should reject a non-positive dimension.") && isPassed;

    HostReloadCommand missingGeometryCommand;
    missingGeometryCommand.request.voxels.assign(1, 1.0f);
    missingGeometryCommand.request.dimensions = { 1, 1, 1 };
    int missingGeometryCallbackCount = 0;
    missingGeometryCommand.onComplete = [&missingGeometryCallbackCount](bool) {
        ++missingGeometryCallbackCount;
    };
    isPassed = GetExpected(
        !fixture.SendCommand(HostCommand{ std::move(missingGeometryCommand) })
            && service->GetReloadCount() == 0
            && missingGeometryCallbackCount == 0,
        "Reload case should reject missing geometry.") && isPassed;

    HostReloadCommand command;
    command.request.voxels = {
        0.0f, 1.0f, 2.0f, 3.0f,
        4.0f, 5.0f, 6.0f, 7.0f
    };
    command.request.dimensions = { 2, 2, 2 };
    command.request.geometry.emplace(
        std::array<float, 3>{ 0.5f, 1.0f, 1.5f },
        std::array<float, 3>{ 4.0f, 5.0f, 6.0f });
    int callbackCount = 0;
    bool isCallbackSuccess = false;
    command.onComplete = [&callbackCount, &isCallbackSuccess](bool isSuccess) {
        ++callbackCount;
        isCallbackSuccess = isSuccess;
    };

    isPassed = GetExpected(
        fixture.SendCommand(HostCommand{ std::move(command) }),
        "Reload case should synchronously accept a valid request.") && isPassed;
    isPassed = GetExpected(
        service && service->GetReloadCount() == 1
            && service->GetReloadVoxels()
                == std::vector<float>{ 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f }
            && service->GetReloadDimensions() == std::array<int, 3>{ 2, 2, 2 }
            && service->GetReloadSpacing() == std::array<float, 3>{ 0.5f, 1.0f, 1.5f }
            && service->GetReloadOrigin() == std::array<float, 3>{ 4.0f, 5.0f, 6.0f },
        "Reload case should forward voxels, dimensions and geometry unchanged.") && isPassed;
    isPassed = GetExpected(
        callbackCount == 0 && fixture.GetBindings()->GetCropInputCount() == 0,
        "Reload admission should not be reported as asynchronous completion.") && isPassed;
    isPassed = GetExpected(
        service && service->SendReloadComplete(true)
            && callbackCount == 1 && isCallbackSuccess
            && !service->SendReloadComplete(true)
            && fixture.GetBindings()->GetCropInputCount() == 1,
        "Reload success should forward completion and refresh crop input.") && isPassed;

    HostReloadCommand failedCommand;
    failedCommand.request.voxels.assign(1, 9.0f);
    failedCommand.request.dimensions = { 1, 1, 1 };
    failedCommand.request.geometry.emplace(
        std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
    int failureCallbackCount = 0;
    bool isFailureSuccess = true;
    failedCommand.onComplete = [
        &failureCallbackCount,
        &isFailureSuccess
    ](bool isSuccess) {
        ++failureCallbackCount;
        isFailureSuccess = isSuccess;
    };
    isPassed = GetExpected(
        fixture.SendCommand(HostCommand{ std::move(failedCommand) })
            && service && service->SendReloadComplete(false)
            && failureCallbackCount == 1 && !isFailureSuccess
            && !service->SendReloadComplete(false)
            && fixture.GetBindings()->GetCropClearCount() == 0,
        "Reload failure should preserve current crop input and forward completion.") && isPassed;

    service->SetReloadAccepted(false);
    HostReloadCommand rejectedCommand;
    rejectedCommand.request.voxels.assign(1, 3.0f);
    rejectedCommand.request.dimensions = { 1, 1, 1 };
    rejectedCommand.request.geometry.emplace(
        std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
    int rejectedCallbackCount = 0;
    rejectedCommand.onComplete = [&rejectedCallbackCount](bool) {
        ++rejectedCallbackCount;
    };
    isPassed = GetExpected(
        !fixture.SendCommand(HostCommand{ std::move(rejectedCommand) })
            && service->GetReloadCount() == 2,
        "Reload service rejection should propagate its synchronous result.") && isPassed;
    isPassed = GetExpected(
        rejectedCallbackCount == 0 && service->SendReloadComplete(false)
            && rejectedCallbackCount == 1
            && !service->SendReloadComplete(false)
            && fixture.GetBindings()->GetCropClearCount() == 0,
        "Reload service rejection may deliver a deferred false callback without clearing crop input.") && isPassed;
    return isPassed;
}

bool StartVolumeExportCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    isPassed = GetExpected(
        fixture.CreateView("volume", HostRenderViewRole::Primary3D, nullptr),
        "Volume export case should create a primary view.") && isPassed;
    isPassed = GetExpected(
        fixture.BuildRouter(),
        "Volume export case should build the router.") && isPassed;

    int callbackCount = 0;
    bool isCallbackSuccess = false;
    HostExportCommand command;
    command.request.hasVolumeExport = true;
    command.request.transformedDataOutputPath = "volume.raw";
    command.onComplete = [&callbackCount, &isCallbackSuccess](bool isSuccess) {
        ++callbackCount;
        isCallbackSuccess = isSuccess;
    };
    isPassed = GetExpected(
        fixture.SendCommand(HostCommand{ std::move(command) }),
        "Volume export case should accept the request.") && isPassed;
    const auto service = fixture.GetViewService("volume");
    isPassed = GetExpected(
        service && service->GetExportCount() == 1
            && service->GetExportPath() == "volume.raw"
            && callbackCount == 1 && isCallbackSuccess,
        "Volume export case should call only the volume service path.") && isPassed;

    int rejectedCallbackCount = 0;
    HostExportCommand rejectedCommand;
    rejectedCommand.onComplete = [&rejectedCallbackCount](bool) {
        ++rejectedCallbackCount;
    };
    isPassed = GetExpected(
        !fixture.SendCommand(HostCommand{ std::move(rejectedCommand) })
            && rejectedCallbackCount == 0,
        "Rejected Export command should not invoke its callback.") && isPassed;
    return isPassed;
}

bool StartSliceExportCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    isPassed = GetExpected(
        fixture.CreateView("slice", HostRenderViewRole::TopDownSlice, nullptr),
        "Slice export case should create a slice view.") && isPassed;
    isPassed = GetExpected(
        fixture.BuildRouter(),
        "Slice export case should build the router.") && isPassed;

    int callbackCount = 0;
    bool isCallbackSuccess = false;
    HostExportCommand command;
    command.request.hasSliceExport = true;
    command.request.sliceOutputDir = "slices";
    command.request.sliceSourceViewId = "slice";
    command.onComplete = [&callbackCount, &isCallbackSuccess](bool isSuccess) {
        ++callbackCount;
        isCallbackSuccess = isSuccess;
    };
    isPassed = GetExpected(
        fixture.SendCommand(HostCommand{ std::move(command) }),
        "Slice export case should accept the request.") && isPassed;
    const auto service = fixture.GetViewService("slice");
    isPassed = GetExpected(
        service && service->GetSliceCount() == 1
            && service->GetSlicePath() == "slices"
            && service->GetExportCount() == 0
            && callbackCount == 1 && isCallbackSuccess,
        "Slice export case should call only the slice service path.") && isPassed;
    return isPassed;
}

bool StartCropDispatchCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Crop start case should build the router.");
    }
    HostCropViewRequest cropRequest;
    cropRequest.referenceViewId = "startRef";
    cropRequest.previewViewIds = { "startPreview" };
    cropRequest.isPreviewViewsUsed = true;
    const bool isAccepted = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostCropCommand{ HostCropAction::Start, cropRequest } }
    });
    const auto& actualRequest = fixture.GetBindings()->GetLastCropRequest();
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetCropStartCount() == 1
            && fixture.GetBindings()->GetCropBoxCount() == 0
            && fixture.GetBindings()->GetCropPlaneCount() == 0
            && fixture.GetBindings()->GetCropViewCount() == 0
            && fixture.GetBindings()->GetCropSendCount() == 0
            && fixture.GetBindings()->GetCropActive()
            && actualRequest.referenceViewId == cropRequest.referenceViewId
            && actualRequest.previewViewIds == cropRequest.previewViewIds
            && actualRequest.isPreviewViewsUsed == cropRequest.isPreviewViewsUsed,
        "Crop start case should dispatch one complete start command.");
}

bool StartCropBoxDispatchCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Crop box case should build the router.");
    }
    HostCropViewRequest cropRequest;
    cropRequest.referenceViewId = "cropRef";
    cropRequest.previewViewIds = { "cropPreview" };
    cropRequest.isPreviewViewsUsed = true;
    const bool isAccepted = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostCropCommand{ HostCropAction::Box, cropRequest } }
    });
    const auto& actualRequest = fixture.GetBindings()->GetLastCropRequest();
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetCropBoxCount() == 1
            && fixture.GetBindings()->GetCropStartCount() == 0
            && actualRequest.referenceViewId == cropRequest.referenceViewId
            && actualRequest.previewViewIds == cropRequest.previewViewIds
            && actualRequest.isPreviewViewsUsed == cropRequest.isPreviewViewsUsed,
        "Crop box case should dispatch one complete box command.");
}

bool StartCropSendDispatchCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Crop submit case should build the router.");
    }
    HostCropViewRequest cropRequest;
    cropRequest.referenceViewId = "submitRef";
    const bool isAccepted = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostCropCommand{
            HostCropAction::Submit,
            cropRequest
        } }
    });
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetCropSendCount() == 1
            && fixture.GetBindings()->GetLastCropRequest().referenceViewId
                == cropRequest.referenceViewId,
        "Crop submit case should dispatch exactly one submit command.");
}

bool StartGapDispatchCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Gap start case should build the router.");
    }
    const auto gapRequest = GetGapInput("typedGap").gapViewRequest;
    const bool isAccepted = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostGapCommand{ HostGapAction::Start, gapRequest } }
    });
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetGapStartCount() == 1
            && GetGapRequestEqual(
                fixture.GetBindings()->GetLastGapRequest(),
                gapRequest),
        "Gap start case should dispatch exactly one start command.");
}

bool StartGapOverlayCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Gap overlay case should build the router.");
    }
    fixture.GetBindings()->SetGapView(true);
    const bool isAccepted = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostGapCommand{ HostGapAction::Overlay } }
    });
    fixture.GetBindings()->SetCommandResult(false);
    const bool isRejected = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostGapCommand{ HostGapAction::Overlay } }
    });
    return GetExpected(
        isAccepted && !isRejected
            && fixture.GetBindings()->GetGapLayerCount() == 2
            && fixture.GetBindings()->GetGapView(),
        "Gap overlay case should preserve active state and report business rejection.");
}

bool StartFeatureExitOrderCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    isPassed = GetExpected(
        fixture.BuildRouter(),
        "Feature exit order case should build the router.") && isPassed;
    fixture.GetBindings()->SetCropActive(true);
    fixture.GetBindings()->SetGapView(true);

    const bool isFirstAccepted = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostExitCommand{} }
    });
    isPassed = GetExpected(
        isFirstAccepted
            && !fixture.GetBindings()->GetCropActive()
            && fixture.GetBindings()->GetGapView(),
        "First feature exit should stop Crop and preserve Gap.") && isPassed;

    const bool isSecondAccepted = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostExitCommand{} }
    });
    isPassed = GetExpected(
        isSecondAccepted
            && !fixture.GetBindings()->GetGapView()
            && fixture.GetBindings()->GetFeatureExitCount() == 2
            && fixture.GetBindings()->GetCropExitCount() == 2
            && fixture.GetBindings()->GetGapExitCount() == 1,
        "Second feature exit should retry Crop before stopping Gap.") && isPassed;
    return isPassed;
}

bool StartCropExitCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Crop exit case should build the router.");
    }
    fixture.GetBindings()->SetCropActive(true);
    const bool isAccepted = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostCropCommand{ HostCropAction::Exit } }
    });
    const bool isRejected = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostCropCommand{ HostCropAction::Exit } }
    });
    return GetExpected(
        isAccepted && !isRejected
            && fixture.GetBindings()->GetCropExitCount() == 2
            && !fixture.GetBindings()->GetCropActive(),
        "Crop exit case should clear active state and reject a second exit.");
}

bool StartGapExitCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Gap exit case should build the router.");
    }
    fixture.GetBindings()->SetGapView(true);
    const bool isAccepted = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostGapCommand{ HostGapAction::Exit } }
    });
    const bool isRejected = fixture.SendCommand(HostCommand{
        HostFeatureCommand{ HostGapCommand{ HostGapAction::Exit } }
    });
    return GetExpected(
        isAccepted && !isRejected
            && fixture.GetBindings()->GetGapExitCount() == 2
            && !fixture.GetBindings()->GetGapView(),
        "Gap exit case should clear active state and reject a second exit.");
}

    int GetFailCount()
    {
        int failureCount = 0;
        failureCount += StartViewModeCase() ? 0 : 1;
        failureCount += StartNoContextModeCase() ? 0 : 1;
        failureCount += StartLoadDispatchCase() ? 0 : 1;
        failureCount += StartReloadDispatchCase() ? 0 : 1;
        failureCount += StartVolumeExportCase() ? 0 : 1;
        failureCount += StartSliceExportCase() ? 0 : 1;
        failureCount += StartCropDispatchCase() ? 0 : 1;
        failureCount += StartCropBoxDispatchCase() ? 0 : 1;
        failureCount += StartCropSendDispatchCase() ? 0 : 1;
        failureCount += StartGapDispatchCase() ? 0 : 1;
        failureCount += StartGapOverlayCase() ? 0 : 1;
        failureCount += StartFeatureExitOrderCase() ? 0 : 1;
        failureCount += StartCropExitCase() ? 0 : 1;
        failureCount += StartGapExitCase() ? 0 : 1;
        return failureCount;
    }
};

int HostRouterSuite::GetFailCount() const
{
    return HostRouterCases().GetFailCount();
}
