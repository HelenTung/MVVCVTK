#include "Host/HostCommandRouter.h"

#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <vtkCommand.h>

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

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

} // namespace

int GetHostRouterFailCount()
{
    int failureCount = 0;
    failureCount += StartViewModeCase() ? 0 : 1;
    failureCount += StartNoContextModeCase() ? 0 : 1;
    failureCount += StartModelRepeatCase() ? 0 : 1;
    failureCount += StartEscapeRepeatCase() ? 0 : 1;
    failureCount += StartCrossViewReleaseCase() ? 0 : 1;
    failureCount += StartSubmitStateCase() ? 0 : 1;
    return failureCount;
}
