#include "Host/HostCommandRouter.h"
#include "HostCommandRouterTests.h"

#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <vtkCommand.h>

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

    bool AttachHotkeys(
        const HostContextInput& contextInput,
        const HostCommandInputConfig& commandInput,
        const HostHotkeyBindings& hotkeys)
    {
        if (!m_router) {
            return false;
        }

        HostCommandRouterRequest request;
        request.command = HostCommandKind::Hotkeys;
        request.renderContextInput = contextInput;
        request.commandInput = commandInput;
        request.hotkeys = hotkeys;
        return m_router->DispatchCommand(std::move(request));
    }

    bool SetViewConfig(const HostViewConfig& viewConfig)
    {
        if (!m_router) {
            return false;
        }

        HostCommandRouterRequest request;
        request.command = HostCommandKind::ViewConfig;
        request.viewConfig = viewConfig;
        return m_router->DispatchCommand(std::move(request));
    }

    bool SendRequest(HostCommandRouterRequest request)
    {
        return m_router && m_router->DispatchCommand(std::move(request));
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

InteractionEvent GetKeyEvent(
    unsigned long eventId,
    char keyCode,
    const char* keySym,
    bool isCtrlDown = false)
{
    InteractionEvent event;
    event.vtkEventId = eventId;
    event.keyCode = keyCode;
    event.keySym = keySym ? keySym : "";
    event.isCtrlDown = isCtrlDown;
    return event;
}

bool GetExpected(bool isExpected, const char* message)
{
    if (!isExpected) {
        std::cerr << message << '\n';
    }
    return isExpected;
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
        fixture.SetViewConfig(viewConfig),
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

bool StartModelRepeatCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    auto context = std::make_shared<StdRenderContext>();
    isPassed = GetExpected(
        fixture.CreateView("model", HostRenderViewRole::Primary3D, context),
        "Model case should create the target view.") && isPassed;
    isPassed = GetExpected(fixture.BuildRouter(), "Model case should build the router.") && isPassed;

    HostContextInput contextInput;
    contextInput.isHotkeyEnabled = true;
    contextInput.targetViewIds = { "model" };
    HostCommandInputConfig commandInput;
    HostHotkeyBindings hotkeys;
    hotkeys.modelSwitchKey = 'm';
    isPassed = GetExpected(
        fixture.AttachHotkeys(contextInput, commandInput, hotkeys),
        "Model case should attach hotkeys.") && isPassed;

    const auto press = GetKeyEvent(vtkCommand::KeyPressEvent, 'm', "m");
    const auto firstResult = context->OnInput(press);
    isPassed = GetExpected(
        firstResult.isHandled && firstResult.hasVtkAbort
            && context->GetToolMode() == ToolMode::ModelTransform,
        "First model press should enter model transform mode.") && isPassed;
    isPassed = GetExpected(
        context->GetToolModeSetCount() == 1,
        "First model press should change the tool mode once.") && isPassed;

    const auto repeatResult = context->OnInput(press);
    isPassed = GetExpected(
        repeatResult.isHandled && repeatResult.hasVtkAbort,
        "Repeated model press should remain handled and aborted.") && isPassed;
    isPassed = GetExpected(
        context->GetToolMode() == ToolMode::ModelTransform
            && context->GetToolModeSetCount() == 1,
        "Repeated model press should not toggle the tool mode again.") && isPassed;

    const auto charResult = context->OnInput(GetKeyEvent(vtkCommand::CharEvent, 'm', "m"));
    isPassed = GetExpected(
        charResult.isHandled && charResult.hasVtkAbort
            && context->GetToolModeSetCount() == 1,
        "Model CharEvent should be consumed without another toggle.") && isPassed;

    const auto releaseResult = context->OnInput(GetKeyEvent(vtkCommand::KeyReleaseEvent, 'm', "m"));
    isPassed = GetExpected(
        releaseResult.isHandled && releaseResult.hasVtkAbort,
        "Model release should clear and consume the managed key state.") && isPassed;

    context->OnInput(press);
    isPassed = GetExpected(
        context->GetToolMode() == ToolMode::Navigation
            && context->GetToolModeSetCount() == 2,
        "Next physical model press should toggle exactly once after release.") && isPassed;
    return isPassed;
}

bool StartEscapeRepeatCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    auto context = std::make_shared<StdRenderContext>();
    isPassed = GetExpected(
        fixture.CreateView("feature", HostRenderViewRole::Primary3D, context),
        "Escape case should create the target view.") && isPassed;
    isPassed = GetExpected(fixture.BuildRouter(), "Escape case should build the router.") && isPassed;

    HostContextInput contextInput;
    HostCommandInputConfig commandInput;
    commandInput.isHotkeyEnabled = true;
    commandInput.targetViewIds = { "feature" };
    HostHotkeyBindings hotkeys;
    hotkeys.exitKeySym = "Escape";
    isPassed = GetExpected(
        fixture.AttachHotkeys(contextInput, commandInput, hotkeys),
        "Escape case should attach hotkeys.") && isPassed;

    fixture.GetBindings()->SetCropActive(true);
    const auto escapePress = GetKeyEvent(vtkCommand::KeyPressEvent, 27, "Escape");
    const auto firstResult = context->OnInput(escapePress);
    isPassed = GetExpected(
        firstResult.isHandled && firstResult.hasVtkAbort,
        "Managed Escape press should be handled and aborted.") && isPassed;
    isPassed = GetExpected(
        fixture.GetBindings()->GetFeatureExitCount() == 1
            && !fixture.GetBindings()->GetCropActive(),
        "First Escape press should exit the active feature exactly once.") && isPassed;

    const auto repeatResult = context->OnInput(escapePress);
    isPassed = GetExpected(
        repeatResult.isHandled && repeatResult.hasVtkAbort
            && fixture.GetBindings()->GetFeatureExitCount() == 1,
        "Escape repeat should stay consumed after the last feature exits.") && isPassed;

    const auto charResult = context->OnInput(GetKeyEvent(vtkCommand::CharEvent, 27, "Escape"));
    isPassed = GetExpected(
        charResult.isHandled && charResult.hasVtkAbort
            && fixture.GetBindings()->GetFeatureExitCount() == 1,
        "Managed Escape CharEvent should stay consumed without another exit.") && isPassed;

    const auto releaseResult = context->OnInput(GetKeyEvent(vtkCommand::KeyReleaseEvent, 27, "Escape"));
    isPassed = GetExpected(
        releaseResult.isHandled && releaseResult.hasVtkAbort,
        "Managed Escape release should clear the down state.") && isPassed;

    const auto idlePress = context->OnInput(escapePress);
    const auto idleChar = context->OnInput(GetKeyEvent(vtkCommand::CharEvent, 27, "Escape"));
    isPassed = GetExpected(
        !idlePress.isHandled && !idlePress.hasVtkAbort
            && !idleChar.isHandled && !idleChar.hasVtkAbort,
        "Fresh idle Escape press and CharEvent should remain unhandled.") && isPassed;
    return isPassed;
}

bool StartCrossViewReleaseCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    auto contextView = std::make_shared<StdRenderContext>();
    auto featureView = std::make_shared<StdRenderContext>();
    isPassed = GetExpected(
        fixture.CreateView("context", HostRenderViewRole::Primary3D, contextView)
            && fixture.CreateView("feature", HostRenderViewRole::Auxiliary, featureView),
        "Cross-view case should create both target views.") && isPassed;
    isPassed = GetExpected(fixture.BuildRouter(), "Cross-view case should build the router.") && isPassed;

    HostContextInput contextInput;
    contextInput.isHotkeyEnabled = true;
    contextInput.targetViewIds = { "context" };
    HostCommandInputConfig commandInput;
    commandInput.isHotkeyEnabled = true;
    commandInput.targetViewIds = { "feature" };
    HostHotkeyBindings hotkeys;
    hotkeys.modelSwitchKey = 'm';
    isPassed = GetExpected(
        fixture.AttachHotkeys(contextInput, commandInput, hotkeys),
        "Cross-view case should attach hotkeys.") && isPassed;

    const auto modelPress = GetKeyEvent(vtkCommand::KeyPressEvent, 'm', "m");
    contextView->OnInput(modelPress);
    isPassed = GetExpected(
        contextView->GetToolMode() == ToolMode::ModelTransform
            && contextView->GetToolModeSetCount() == 1,
        "Context-only view should handle the first model press.") && isPassed;

    const auto releaseResult = featureView->OnInput(
        GetKeyEvent(vtkCommand::KeyReleaseEvent, 'm', "m"));
    isPassed = GetExpected(
        releaseResult.isHandled && releaseResult.hasVtkAbort,
        "Feature-only view should clear a model key released after focus moves.") && isPassed;

    contextView->OnInput(modelPress);
    isPassed = GetExpected(
        contextView->GetToolMode() == ToolMode::Navigation
            && contextView->GetToolModeSetCount() == 2,
        "Model press should work again after cross-view release.") && isPassed;
    return isPassed;
}

bool StartSubmitStateCase()
{
    bool isPassed = true;
    HostRouterFixture fixture;
    auto context = std::make_shared<StdRenderContext>();
    isPassed = GetExpected(
        fixture.CreateView("submit", HostRenderViewRole::Primary3D, context),
        "Submit case should create the target view.") && isPassed;
    isPassed = GetExpected(fixture.BuildRouter(), "Submit case should build the router.") && isPassed;

    HostContextInput contextInput;
    HostCommandInputConfig commandInput;
    commandInput.isHotkeyEnabled = true;
    commandInput.targetViewIds = { "submit" };
    HostHotkeyBindings hotkeys;
    hotkeys.submitKey = '3';
    isPassed = GetExpected(
        fixture.AttachHotkeys(contextInput, commandInput, hotkeys),
        "Submit case should attach hotkeys.") && isPassed;

    const auto ctrlPress = GetKeyEvent(vtkCommand::KeyPressEvent, '3', "3", true);
    const auto plainPress = GetKeyEvent(vtkCommand::KeyPressEvent, '3', "3", false);
    const auto firstResult = context->OnInput(ctrlPress);
    isPassed = GetExpected(
        firstResult.isHandled && firstResult.hasVtkAbort
            && fixture.GetBindings()->GetCropSendCount() == 1,
        "First Ctrl+Submit press should dispatch one crop submit.") && isPassed;

    const auto changedModifierRepeat = context->OnInput(plainPress);
    const auto charResult = context->OnInput(GetKeyEvent(vtkCommand::CharEvent, '3', "3"));
    isPassed = GetExpected(
        changedModifierRepeat.isHandled && changedModifierRepeat.hasVtkAbort
            && charResult.isHandled && charResult.hasVtkAbort
            && fixture.GetBindings()->GetCropSendCount() == 1,
        "SubmitBlock repeat and CharEvent should share the Ctrl+Submit down state.") && isPassed;

    const auto plainRelease = context->OnInput(GetKeyEvent(vtkCommand::KeyReleaseEvent, '3', "3"));
    isPassed = GetExpected(
        plainRelease.isHandled && plainRelease.hasVtkAbort,
        "Submit release without Ctrl should clear the Ctrl+Submit state.") && isPassed;

    context->OnInput(plainPress);
    context->OnInput(ctrlPress);
    isPassed = GetExpected(
        fixture.GetBindings()->GetCropSendCount() == 1,
        "Ctrl repeat during a plain SubmitBlock press should not dispatch submit.") && isPassed;

    context->OnInput(GetKeyEvent(vtkCommand::KeyReleaseEvent, '3', "3", true));
    context->OnInput(ctrlPress);
    isPassed = GetExpected(
        fixture.GetBindings()->GetCropSendCount() == 2,
        "Next physical Ctrl+Submit press should dispatch after shared state release.") && isPassed;
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

    bool hasCallback = false;
    HostCommandRouterRequest request;
    request.command = HostCommandKind::Load;
    request.initialVolume.isInitialLoadEnabled = true;
    request.initialVolume.filePath = "sample.raw";
    request.initialVolume.geometry.emplace(
        std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
    request.loadComplete = [&hasCallback](bool isSuccess) {
        hasCallback = isSuccess;
    };

    isPassed = GetExpected(
        fixture.SendRequest(std::move(request)),
        "Load case should accept the valid request.") && isPassed;
    const auto service = fixture.GetViewService("load");
    isPassed = GetExpected(
        service && service->GetLoadCount() == 1
            && service->GetLoadPath() == "sample.raw",
        "Load case should call the selected service exactly once.") && isPassed;
    isPassed = GetExpected(
        hasCallback && fixture.GetBindings()->GetCropInputCount() == 1,
        "Load case should forward completion and refresh crop input.") && isPassed;
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

    HostCommandRouterRequest invalidRequest;
    invalidRequest.command = HostCommandKind::Reload;
    invalidRequest.volumeBuffer.voxels.assign(7, 1.0f);
    invalidRequest.volumeBuffer.dimensions = { 2, 2, 2 };
    invalidRequest.volumeBuffer.geometry.emplace(
        std::array<float, 3>{ 0.5f, 1.0f, 1.5f },
        std::array<float, 3>{ 4.0f, 5.0f, 6.0f });
    bool hasInvalidCallback = false;
    invalidRequest.reloadComplete = [&hasInvalidCallback](bool) {
        hasInvalidCallback = true;
    };
    isPassed = GetExpected(
        !fixture.SendRequest(std::move(invalidRequest))
            && service && service->GetReloadCount() == 0
            && !hasInvalidCallback,
        "Reload case should reject a voxel-count mismatch without a callback.") && isPassed;

    HostCommandRouterRequest invalidDimensionsRequest;
    invalidDimensionsRequest.command = HostCommandKind::Reload;
    invalidDimensionsRequest.volumeBuffer.voxels.assign(1, 1.0f);
    invalidDimensionsRequest.volumeBuffer.dimensions = { 1, 0, 1 };
    invalidDimensionsRequest.volumeBuffer.geometry.emplace(
        std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
    isPassed = GetExpected(
        !fixture.SendRequest(std::move(invalidDimensionsRequest))
            && service->GetReloadCount() == 0,
        "Reload case should reject a non-positive dimension.") && isPassed;

    HostCommandRouterRequest missingGeometryRequest;
    missingGeometryRequest.command = HostCommandKind::Reload;
    missingGeometryRequest.volumeBuffer.voxels.assign(1, 1.0f);
    missingGeometryRequest.volumeBuffer.dimensions = { 1, 1, 1 };
    isPassed = GetExpected(
        !fixture.SendRequest(std::move(missingGeometryRequest))
            && service->GetReloadCount() == 0,
        "Reload case should reject missing geometry.") && isPassed;

    HostCommandRouterRequest request;
    request.command = HostCommandKind::Reload;
    request.volumeBuffer.voxels = {
        0.0f, 1.0f, 2.0f, 3.0f,
        4.0f, 5.0f, 6.0f, 7.0f
    };
    request.volumeBuffer.dimensions = { 2, 2, 2 };
    request.volumeBuffer.geometry.emplace(
        std::array<float, 3>{ 0.5f, 1.0f, 1.5f },
        std::array<float, 3>{ 4.0f, 5.0f, 6.0f });
    bool hasCallback = false;
    bool isCallbackSuccess = false;
    request.reloadComplete = [&hasCallback, &isCallbackSuccess](bool isSuccess) {
        hasCallback = true;
        isCallbackSuccess = isSuccess;
    };

    isPassed = GetExpected(
        fixture.SendRequest(std::move(request)),
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
        !hasCallback && fixture.GetBindings()->GetCropInputCount() == 0,
        "Reload admission should not be reported as asynchronous completion.") && isPassed;
    isPassed = GetExpected(
        service && service->SendReloadComplete(true)
            && hasCallback && isCallbackSuccess
            && fixture.GetBindings()->GetCropInputCount() == 1,
        "Reload success should forward completion and refresh crop input.") && isPassed;

    HostCommandRouterRequest failedRequest;
    failedRequest.command = HostCommandKind::Reload;
    failedRequest.volumeBuffer.voxels.assign(1, 9.0f);
    failedRequest.volumeBuffer.dimensions = { 1, 1, 1 };
    failedRequest.volumeBuffer.geometry.emplace(
        std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
    bool hasFailureCallback = false;
    bool isFailureSuccess = true;
    failedRequest.reloadComplete = [
        &hasFailureCallback,
        &isFailureSuccess
    ](bool isSuccess) {
        hasFailureCallback = true;
        isFailureSuccess = isSuccess;
    };
    isPassed = GetExpected(
        fixture.SendRequest(std::move(failedRequest))
            && service && service->SendReloadComplete(false)
            && hasFailureCallback && !isFailureSuccess
            && fixture.GetBindings()->GetCropClearCount() == 0,
        "Reload failure should preserve current crop input and forward completion.") && isPassed;

    service->SetReloadAccepted(false);
    HostCommandRouterRequest rejectedRequest;
    rejectedRequest.command = HostCommandKind::Reload;
    rejectedRequest.volumeBuffer.voxels.assign(1, 3.0f);
    rejectedRequest.volumeBuffer.dimensions = { 1, 1, 1 };
    rejectedRequest.volumeBuffer.geometry.emplace(
        std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
    bool hasRejectedCallback = false;
    rejectedRequest.reloadComplete = [&hasRejectedCallback](bool) {
        hasRejectedCallback = true;
    };
    isPassed = GetExpected(
        !fixture.SendRequest(std::move(rejectedRequest))
            && service->GetReloadCount() == 2,
        "Reload service rejection should propagate its synchronous result.") && isPassed;
    isPassed = GetExpected(
        !hasRejectedCallback && service->SendReloadComplete(false)
            && hasRejectedCallback
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

    bool hasCallback = false;
    HostCommandRouterRequest request;
    request.command = HostCommandKind::Export;
    request.dataExportConfig.hasVolumeExport = true;
    request.dataExportConfig.transformedDataOutputPath = "volume.raw";
    request.dataExportComplete = [&hasCallback](bool isSuccess) {
        hasCallback = isSuccess;
    };
    isPassed = GetExpected(
        fixture.SendRequest(std::move(request)),
        "Volume export case should accept the request.") && isPassed;
    const auto service = fixture.GetViewService("volume");
    isPassed = GetExpected(
        service && service->GetExportCount() == 1
            && service->GetExportPath() == "volume.raw"
            && hasCallback,
        "Volume export case should call only the volume service path.") && isPassed;
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

    HostCommandRouterRequest request;
    request.command = HostCommandKind::Export;
    request.dataExportConfig.hasSliceExport = true;
    request.dataExportConfig.sliceOutputDir = "slices";
    request.dataExportConfig.sliceSourceViewId = "slice";
    isPassed = GetExpected(
        fixture.SendRequest(std::move(request)),
        "Slice export case should accept the request.") && isPassed;
    const auto service = fixture.GetViewService("slice");
    isPassed = GetExpected(
        service && service->GetSliceCount() == 1
            && service->GetSlicePath() == "slices"
            && service->GetExportCount() == 0,
        "Slice export case should call only the slice service path.") && isPassed;
    return isPassed;
}

bool StartCropBoxDispatchCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Crop box case should build the router.");
    }
    HostCommandRouterRequest request;
    request.command = HostCommandKind::CropBox;
    const bool isAccepted = fixture.SendRequest(std::move(request));
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetCropBoxCount() == 1
            && fixture.GetBindings()->GetCropStartCount() == 0,
        "Crop box case should dispatch exactly one box command.");
}

bool StartCropSendDispatchCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Crop submit case should build the router.");
    }
    HostCommandRouterRequest request;
    request.command = HostCommandKind::CropApply;
    const bool isAccepted = fixture.SendRequest(std::move(request));
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetCropSendCount() == 1,
        "Crop submit case should dispatch exactly one submit command.");
}

bool StartGapDispatchCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Gap start case should build the router.");
    }
    HostCommandRouterRequest request;
    request.command = HostCommandKind::GapStart;
    const bool isAccepted = fixture.SendRequest(std::move(request));
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetGapStartCount() == 1,
        "Gap start case should dispatch exactly one start command.");
}

bool StartGapOverlayCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Gap overlay case should build the router.");
    }
    fixture.GetBindings()->SetGapView(true);
    HostCommandRouterRequest request;
    request.command = HostCommandKind::GapOverlay;
    const bool isAccepted = fixture.SendRequest(std::move(request));
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetGapLayerCount() == 1,
        "Gap overlay case should dispatch exactly one overlay command.");
}

bool StartCropExitCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Crop exit case should build the router.");
    }
    fixture.GetBindings()->SetCropActive(true);
    HostCommandRouterRequest request;
    request.command = HostCommandKind::CropExit;
    const bool isAccepted = fixture.SendRequest(std::move(request));
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetCropExitCount() == 1,
        "Crop exit case should dispatch exactly one exit command.");
}

bool StartGapExitCase()
{
    HostRouterFixture fixture;
    if (!fixture.BuildRouter()) {
        return GetExpected(false, "Gap exit case should build the router.");
    }
    fixture.GetBindings()->SetGapView(true);
    HostCommandRouterRequest request;
    request.command = HostCommandKind::GapExit;
    const bool isAccepted = fixture.SendRequest(std::move(request));
    return GetExpected(
        isAccepted && fixture.GetBindings()->GetGapExitCount() == 1,
        "Gap exit case should dispatch exactly one exit command.");
}

    int GetFailCount()
    {
        int failureCount = 0;
        failureCount += StartViewModeCase() ? 0 : 1;
        failureCount += StartNoContextModeCase() ? 0 : 1;
        failureCount += StartModelRepeatCase() ? 0 : 1;
        failureCount += StartEscapeRepeatCase() ? 0 : 1;
        failureCount += StartCrossViewReleaseCase() ? 0 : 1;
        failureCount += StartSubmitStateCase() ? 0 : 1;
        failureCount += StartLoadDispatchCase() ? 0 : 1;
        failureCount += StartReloadDispatchCase() ? 0 : 1;
        failureCount += StartVolumeExportCase() ? 0 : 1;
        failureCount += StartSliceExportCase() ? 0 : 1;
        failureCount += StartCropBoxDispatchCase() ? 0 : 1;
        failureCount += StartCropSendDispatchCase() ? 0 : 1;
        failureCount += StartGapDispatchCase() ? 0 : 1;
        failureCount += StartGapOverlayCase() ? 0 : 1;
        failureCount += StartCropExitCase() ? 0 : 1;
        failureCount += StartGapExitCase() ? 0 : 1;
        return failureCount;
    }
};

int HostRouterSuite::GetFailCount() const
{
    return HostRouterCases().GetFailCount();
}
