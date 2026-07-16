#include "Host/HostHotkeyRouter.h"
#include "HostHotkeyRouterTests.h"

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

class HostHotkeyCases final {
public:
    class Fixture final {
    public:
        Fixture()
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

        bool BuildRouters()
        {
            m_commandRouter = std::make_shared<HostCommandRouter>(
                m_core,
                m_views,
                m_bindings);
            m_hotkeyRouter = std::make_unique<HostHotkeyRouter>(
                m_views,
                m_bindings,
                m_commandRouter);
            return m_commandRouter != nullptr && m_hotkeyRouter != nullptr;
        }

        bool AttachHotkeys(
            const HostContextInput& contextInput,
            const HostDataExportConfig& exportConfig,
            const HostCommandInputConfig& commandInput,
            const HostHotkeyBindings& hotkeys)
        {
            return m_hotkeyRouter
                && m_hotkeyRouter->AttachHotkeys(
                    contextInput,
                    exportConfig,
                    commandInput,
                    hotkeys);
        }

        bool ResetHotkeyRouter()
        {
            m_hotkeyRouter.reset();
            return !m_hotkeyRouter;
        }

        bool ResetCommandRouter()
        {
            m_commandRouter.reset();
            return !m_commandRouter;
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
        std::shared_ptr<HostCommandRouter> m_commandRouter;
        std::unique_ptr<HostHotkeyRouter> m_hotkeyRouter;
    };

    static bool GetExpected(bool isExpected, const char* message)
    {
        if (!isExpected) {
            std::cerr << message << '\n';
        }
        return isExpected;
    }

    static InteractionEvent BuildKeyEvent(
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

    static HostCommandInputConfig BuildGapInput(const std::string& viewId)
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

    static bool GetGapRequestEqual(
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

    static bool GetCropRequestEqual(
        const HostCropViewRequest& actual,
        const HostCropViewRequest& expected)
    {
        return actual.referenceViewId == expected.referenceViewId
            && actual.isReferenceRoleUsed == expected.isReferenceRoleUsed
            && actual.referenceRole == expected.referenceRole
            && actual.previewViewIds == expected.previewViewIds
            && actual.previewViewRoles == expected.previewViewRoles
            && actual.isPreviewViewsUsed == expected.isPreviewViewsUsed;
    }

    static bool GetCropAction(
        const std::shared_ptr<HostFeatureBindings>& bindings,
        HostCropAction action)
    {
        const auto* command = std::get_if<HostCropCommand>(
            &bindings->GetLastCommand());
        return command && command->action == action;
    }

    static bool GetGapAction(
        const std::shared_ptr<HostFeatureBindings>& bindings,
        HostGapAction action)
    {
        const auto* command = std::get_if<HostGapCommand>(
            &bindings->GetLastCommand());
        return command && command->action == action;
    }

    static bool SendKey(
        const std::shared_ptr<StdRenderContext>& context,
        char key,
        const char* keySym,
        bool isCtrlDown = false)
    {
        const auto press = context->OnInput(BuildKeyEvent(
            vtkCommand::KeyPressEvent,
            key,
            keySym,
            isCtrlDown));
        const auto release = context->OnInput(BuildKeyEvent(
            vtkCommand::KeyReleaseEvent,
            key,
            keySym,
            isCtrlDown));
        return press.isHandled && press.hasVtkAbort
            && release.isHandled && release.hasVtkAbort;
    }

    static bool StartModelRepeatCase()
    {
        bool isPassed = true;
        Fixture fixture;
        auto context = std::make_shared<StdRenderContext>();
        isPassed = GetExpected(
            fixture.CreateView("model", HostRenderViewRole::Primary3D, context)
                && fixture.BuildRouters(),
            "Model case should build its view and routers.") && isPassed;

        HostContextInput contextInput;
        contextInput.isHotkeyEnabled = true;
        contextInput.targetViewIds = { "model" };
        HostHotkeyBindings hotkeys;
        hotkeys.modelSwitchKey = 'm';
        isPassed = GetExpected(
            fixture.AttachHotkeys(contextInput, {}, {}, hotkeys),
            "Model case should attach hotkeys.") && isPassed;

        const auto press = BuildKeyEvent(vtkCommand::KeyPressEvent, 'm', "m");
        const auto first = context->OnInput(press);
        const auto repeat = context->OnInput(press);
        const auto character = context->OnInput(
            BuildKeyEvent(vtkCommand::CharEvent, 'm', "m"));
        isPassed = GetExpected(
            first.isHandled && first.hasVtkAbort
                && repeat.isHandled && repeat.hasVtkAbort
                && character.isHandled && character.hasVtkAbort
                && context->GetToolMode() == ToolMode::ModelTransform
                && context->GetToolModeSetCount() == 1,
            "Model repeat and CharEvent should be consumed without a second toggle.") && isPassed;

        const auto release = context->OnInput(
            BuildKeyEvent(vtkCommand::KeyReleaseEvent, 'm', "m"));
        context->OnInput(press);
        isPassed = GetExpected(
            release.isHandled && release.hasVtkAbort
                && context->GetToolMode() == ToolMode::Navigation
                && context->GetToolModeSetCount() == 2,
            "Model release should allow exactly one next toggle.") && isPassed;
        return isPassed;
    }

    static bool StartEscapeRepeatCase()
    {
        bool isPassed = true;
        Fixture fixture;
        auto context = std::make_shared<StdRenderContext>();
        isPassed = GetExpected(
            fixture.CreateView("feature", HostRenderViewRole::Primary3D, context)
                && fixture.BuildRouters(),
            "Escape case should build its view and routers.") && isPassed;
        HostCommandInputConfig commandInput;
        commandInput.isHotkeyEnabled = true;
        commandInput.targetViewIds = { "feature" };
        HostHotkeyBindings hotkeys;
        hotkeys.exitKeySym = "Escape";
        HostContextInput contextInput;
        contextInput.isHotkeyEnabled = true;
        contextInput.targetViewIds = { "feature" };
        isPassed = GetExpected(
            fixture.AttachHotkeys(contextInput, {}, commandInput, hotkeys),
            "Escape case should attach hotkeys.") && isPassed;

        fixture.GetBindings()->SetCropActive(true);
        const auto press = BuildKeyEvent(vtkCommand::KeyPressEvent, 27, "Escape");
        const auto first = context->OnInput(press);
        const auto repeat = context->OnInput(press);
        const auto character = context->OnInput(
            BuildKeyEvent(vtkCommand::CharEvent, 27, "Escape"));
        isPassed = GetExpected(
            first.isHandled && first.hasVtkAbort
                && repeat.isHandled && repeat.hasVtkAbort
                && character.isHandled && character.hasVtkAbort
                && fixture.GetBindings()->GetFeatureExitCount() == 1
                && !fixture.GetBindings()->GetCropActive(),
            "Managed Escape should exit once and consume repeat/Char events.") && isPassed;

        const auto release = context->OnInput(
            BuildKeyEvent(vtkCommand::KeyReleaseEvent, 27, "Escape"));
        const auto idlePress = context->OnInput(press);
        const auto idleChar = context->OnInput(
            BuildKeyEvent(vtkCommand::CharEvent, 27, "Escape"));
        isPassed = GetExpected(
            release.isHandled && release.hasVtkAbort
                && !idlePress.isHandled && !idlePress.hasVtkAbort
                && !idleChar.isHandled && !idleChar.hasVtkAbort,
            "Fresh idle Escape should remain available to later handlers.") && isPassed;
        context->SetToolMode(ToolMode::ModelTransform);
        const auto modelExit = context->OnInput(press);
        isPassed = GetExpected(
            modelExit.isHandled && modelExit.hasVtkAbort
                && context->GetToolMode() == ToolMode::Navigation
                && fixture.GetBindings()->GetFeatureExitCount() == 2,
            "Escape should fall back to exiting model transform when no feature is active.") && isPassed;
        return isPassed;
    }

    static bool StartCrossViewCase()
    {
        bool isPassed = true;
        Fixture fixture;
        auto contextView = std::make_shared<StdRenderContext>();
        auto featureView = std::make_shared<StdRenderContext>();
        isPassed = GetExpected(
            fixture.CreateView("context", HostRenderViewRole::Primary3D, contextView)
                && fixture.CreateView("feature", HostRenderViewRole::Auxiliary, featureView)
                && fixture.BuildRouters(),
            "Cross-view case should build both views and routers.") && isPassed;
        HostContextInput contextInput;
        contextInput.isHotkeyEnabled = true;
        contextInput.targetViewIds = { "context" };
        HostCommandInputConfig commandInput;
        commandInput.isHotkeyEnabled = true;
        commandInput.targetViewIds = { "feature" };
        HostHotkeyBindings hotkeys;
        hotkeys.modelSwitchKey = 'm';
        isPassed = GetExpected(
            fixture.AttachHotkeys(contextInput, {}, commandInput, hotkeys),
            "Cross-view case should attach hotkeys.") && isPassed;

        const auto press = BuildKeyEvent(vtkCommand::KeyPressEvent, 'm', "m");
        const auto first = contextView->OnInput(press);
        const auto release = featureView->OnInput(
            BuildKeyEvent(vtkCommand::KeyReleaseEvent, 'm', "m"));
        contextView->OnInput(press);
        isPassed = GetExpected(
            first.isHandled && first.hasVtkAbort
                && release.isHandled && release.hasVtkAbort
                && contextView->GetToolMode() == ToolMode::Navigation
                && contextView->GetToolModeSetCount() == 2,
            "Cross-view release should clear the shared physical-key state.") && isPassed;
        return isPassed;
    }

    static bool StartSubmitStateCase()
    {
        bool isPassed = true;
        Fixture fixture;
        auto context = std::make_shared<StdRenderContext>();
        isPassed = GetExpected(
            fixture.CreateView("submit", HostRenderViewRole::Primary3D, context)
                && fixture.BuildRouters(),
            "Submit case should build its view and routers.") && isPassed;
        HostCommandInputConfig commandInput;
        commandInput.isHotkeyEnabled = true;
        commandInput.targetViewIds = { "submit" };
        HostHotkeyBindings hotkeys;
        hotkeys.submitKey = '3';
        isPassed = GetExpected(
            fixture.AttachHotkeys({}, {}, commandInput, hotkeys),
            "Submit case should attach hotkeys.") && isPassed;

        const auto ctrlPress = BuildKeyEvent(
            vtkCommand::KeyPressEvent, '3', "3", true);
        const auto plainPress = BuildKeyEvent(
            vtkCommand::KeyPressEvent, '3', "3");
        const auto first = context->OnInput(ctrlPress);
        const auto changedRepeat = context->OnInput(plainPress);
        const auto character = context->OnInput(
            BuildKeyEvent(vtkCommand::CharEvent, '3', "3"));
        isPassed = GetExpected(
            first.isHandled && first.hasVtkAbort
                && changedRepeat.isHandled && changedRepeat.hasVtkAbort
                && character.isHandled && character.hasVtkAbort
                && fixture.GetBindings()->GetCropSendCount() == 1,
            "Submit and SubmitBlock should share one down-state slot.") && isPassed;

        context->OnInput(BuildKeyEvent(vtkCommand::KeyReleaseEvent, '3', "3"));
        context->OnInput(plainPress);
        context->OnInput(ctrlPress);
        isPassed = GetExpected(
            fixture.GetBindings()->GetCropSendCount() == 1,
            "Ctrl repeat during plain SubmitBlock should not dispatch.") && isPassed;
        context->OnInput(BuildKeyEvent(vtkCommand::KeyReleaseEvent, '3', "3", true));
        context->OnInput(ctrlPress);
        isPassed = GetExpected(
            fixture.GetBindings()->GetCropSendCount() == 2,
            "A new physical Ctrl+Submit press should dispatch after release.") && isPassed;
        return isPassed;
    }

    static bool StartGapRepeatCase()
    {
        bool isPassed = true;
        Fixture fixture;
        auto context = std::make_shared<StdRenderContext>();
        isPassed = GetExpected(
            fixture.CreateView("gap", HostRenderViewRole::Primary3D, context)
                && fixture.BuildRouters(),
            "Gap repeat case should build its view and routers.") && isPassed;
        const auto commandInput = BuildGapInput("gap");
        HostHotkeyBindings hotkeys;
        hotkeys.gapSwitchKey = 'j';
        isPassed = GetExpected(
            fixture.AttachHotkeys({}, {}, commandInput, hotkeys),
            "Gap repeat case should attach hotkeys.") && isPassed;

        const auto press = BuildKeyEvent(vtkCommand::KeyPressEvent, 'j', "j");
        const auto first = context->OnInput(press);
        const auto repeat = context->OnInput(press);
        const auto character = context->OnInput(
            BuildKeyEvent(vtkCommand::CharEvent, 'j', "j"));
        isPassed = GetExpected(
            first.isHandled && first.hasVtkAbort
                && repeat.isHandled && repeat.hasVtkAbort
                && character.isHandled && character.hasVtkAbort
                && fixture.GetBindings()->GetGapViewCount() == 1
                && GetGapRequestEqual(
                    fixture.GetBindings()->GetLastGapRequest(),
                    commandInput.gapViewRequest),
            "Gap repeat should dispatch one complete typed request.") && isPassed;
        context->OnInput(BuildKeyEvent(vtkCommand::KeyReleaseEvent, 'j', "j"));
        context->OnInput(press);
        isPassed = GetExpected(
            fixture.GetBindings()->GetGapViewCount() == 2,
            "Gap switch should dispatch again after release.") && isPassed;
        return isPassed;
    }

    static bool StartFailureCase()
    {
        bool isPassed = true;
        Fixture fixture;
        auto context = std::make_shared<StdRenderContext>();
        isPassed = GetExpected(
            fixture.CreateView("failure", HostRenderViewRole::Primary3D, context)
                && fixture.BuildRouters(),
            "Failure case should build its view and routers.") && isPassed;
        const auto commandInput = BuildGapInput("failure");
        HostHotkeyBindings hotkeys;
        hotkeys.gapSwitchKey = 'j';
        isPassed = GetExpected(
            fixture.AttachHotkeys({}, {}, commandInput, hotkeys)
                && fixture.GetBindings()->SetCommandResult(false),
            "Failure case should attach and inject rejection.") && isPassed;

        const auto press = BuildKeyEvent(vtkCommand::KeyPressEvent, 'j', "j");
        const auto first = context->OnInput(press);
        const auto repeat = context->OnInput(press);
        const auto character = context->OnInput(
            BuildKeyEvent(vtkCommand::CharEvent, 'j', "j"));
        isPassed = GetExpected(
            first.isHandled && first.hasVtkAbort
                && repeat.isHandled && repeat.hasVtkAbort
                && character.isHandled && character.hasVtkAbort
                && fixture.GetBindings()->GetGapViewCount() == 1
                && !fixture.GetBindings()->GetGapView(),
            "Rejected business command should still consume one physical press.") && isPassed;
        context->OnInput(BuildKeyEvent(vtkCommand::KeyReleaseEvent, 'j', "j"));
        context->OnInput(press);
        isPassed = GetExpected(
            fixture.GetBindings()->GetGapViewCount() == 2,
            "Rejected command should be retryable after release.") && isPassed;
        return isPassed;
    }

    static bool StartLifecycleCase()
    {
        bool isPassed = true;
        {
            Fixture fixture;
            auto context = std::make_shared<StdRenderContext>();
            isPassed = GetExpected(
                fixture.CreateView("teardown", HostRenderViewRole::Primary3D, context)
                    && fixture.BuildRouters(),
                "Teardown case should build its view and routers.") && isPassed;
            auto commandInput = BuildGapInput("teardown");
            HostHotkeyBindings hotkeys;
            hotkeys.gapSwitchKey = 'j';
            fixture.AttachHotkeys({}, {}, commandInput, hotkeys);
            fixture.ResetHotkeyRouter();
            const auto result = context->OnInput(
                BuildKeyEvent(vtkCommand::KeyPressEvent, 'j', "j"));
            isPassed = GetExpected(
                !result.isHandled && !result.hasVtkAbort,
                "Destroyed hotkey router should leave no context handler.") && isPassed;
        }
        {
            Fixture fixture;
            auto oldContext = std::make_shared<StdRenderContext>();
            auto newContext = std::make_shared<StdRenderContext>();
            isPassed = GetExpected(
                fixture.CreateView("old", HostRenderViewRole::Primary3D, oldContext)
                    && fixture.CreateView("new", HostRenderViewRole::Auxiliary, newContext)
                    && fixture.BuildRouters(),
                "Rebind case should build both views and routers.") && isPassed;
            HostHotkeyBindings hotkeys;
            hotkeys.gapSwitchKey = 'j';
            fixture.AttachHotkeys({}, {}, BuildGapInput("old"), hotkeys);
            fixture.AttachHotkeys({}, {}, BuildGapInput("new"), hotkeys);
            const auto press = BuildKeyEvent(vtkCommand::KeyPressEvent, 'j', "j");
            const auto oldResult = oldContext->OnInput(press);
            const auto newResult = newContext->OnInput(press);
            isPassed = GetExpected(
                !oldResult.isHandled && !oldResult.hasVtkAbort
                    && newResult.isHandled && newResult.hasVtkAbort
                    && fixture.GetBindings()->GetGapViewCount() == 1,
                "Rebind should clear the old view and attach the new view.") && isPassed;
        }
        return isPassed;
    }

    static bool StartExpiredRouterCase()
    {
        bool isPassed = true;
        Fixture fixture;
        auto context = std::make_shared<StdRenderContext>();
        isPassed = GetExpected(
            fixture.CreateView("expired", HostRenderViewRole::Primary3D, context)
                && fixture.BuildRouters(),
            "Expired-router case should build its view and routers.") && isPassed;
        HostCommandInputConfig commandInput;
        commandInput.isHotkeyEnabled = true;
        commandInput.targetViewIds = { "expired" };
        HostHotkeyBindings hotkeys;
        hotkeys.cropSwitchKey = 'b';
        isPassed = GetExpected(
            fixture.AttachHotkeys({}, {}, commandInput, hotkeys)
                && fixture.ResetCommandRouter(),
            "Expired-router case should retain only the input router.") && isPassed;

        const auto press = BuildKeyEvent(vtkCommand::KeyPressEvent, 'b', "b");
        const auto first = context->OnInput(press);
        const auto release = context->OnInput(
            BuildKeyEvent(vtkCommand::KeyReleaseEvent, 'b', "b"));
        const auto next = context->OnInput(press);
        isPassed = GetExpected(
            first.isHandled && first.hasVtkAbort
                && release.isHandled && release.hasVtkAbort
                && next.isHandled && next.hasVtkAbort
                && fixture.GetBindings()->GetCropBoxCount() == 0,
            "Expired business router should be safe while matched input remains consumed and retryable.") && isPassed;
        return isPassed;
    }

    static bool StartTypedMappingCase()
    {
        bool isPassed = true;
        Fixture fixture;
        auto context = std::make_shared<StdRenderContext>();
        isPassed = GetExpected(
            fixture.CreateView("mapping", HostRenderViewRole::TopDownSlice, context)
                && fixture.CreateView(
                    "otherSlice",
                    HostRenderViewRole::FrontBackSlice,
                    std::make_shared<StdRenderContext>())
                && fixture.BuildRouters(),
            "Typed mapping case should build its slice view and routers.") && isPassed;

        HostContextInput contextInput;
        contextInput.isHotkeyEnabled = true;
        contextInput.targetViewIds = { "mapping" };
        HostCommandInputConfig commandInput = BuildGapInput("mapping");
        commandInput.cropViewRequest.referenceViewId = "cropTarget";
        commandInput.cropViewRequest.isReferenceRoleUsed = true;
        commandInput.cropViewRequest.referenceRole = HostRenderViewRole::Composite3D;
        commandInput.cropViewRequest.previewViewIds = { "previewA", "previewB" };
        commandInput.cropViewRequest.previewViewRoles = { HostRenderViewRole::Auxiliary };
        commandInput.cropViewRequest.isPreviewViewsUsed = true;
        HostDataExportConfig exportConfig;
        exportConfig.transformedDataOutputPath = "volume.nii";
        exportConfig.sliceOutputDir = "slices";
        exportConfig.sliceAngleDeg = 17.5;
        HostHotkeyBindings hotkeys;
        hotkeys.saveTransformedDataKey = 'v';
        hotkeys.saveSliceImagesKey = 's';
        hotkeys.cropSwitchKey = 'b';
        hotkeys.planarSwitchKey = 'p';
        hotkeys.keepInsidePreviewKey = 'k';
        hotkeys.removeInsidePreviewKey = 'r';
        hotkeys.submitKey = '3';
        hotkeys.gapSwitchKey = 'j';
        hotkeys.exitKeySym = "Escape";
        isPassed = GetExpected(
            fixture.AttachHotkeys(contextInput, exportConfig, commandInput, hotkeys),
            "Typed mapping case should attach all mappings.") && isPassed;

        const auto service = fixture.GetViewService("mapping");
        const auto otherService = fixture.GetViewService("otherSlice");
        isPassed = GetExpected(
            SendKey(context, 'v', "v")
                && service && service->GetExportCount() == 1
                && service->GetSliceCount() == 0
                && service->GetExportPath() == "volume.nii",
            "Volume key should build a volume-only typed export command.") && isPassed;
        isPassed = GetExpected(
            SendKey(context, 's', "s")
                && service && service->GetExportCount() == 1
                && service->GetSliceCount() == 1
                && service->GetSlicePath() == "slices"
                && service->GetSliceAngleDeg() == exportConfig.sliceAngleDeg
                && otherService && otherService->GetSliceCount() == 0,
            "Slice key should use the source view role fallback.") && isPassed;
        isPassed = GetExpected(
            SendKey(context, 'b', "b")
                && fixture.GetBindings()->GetCropBoxCount() == 1
                && GetCropAction(fixture.GetBindings(), HostCropAction::Box)
                && GetCropRequestEqual(
                    fixture.GetBindings()->GetLastCropRequest(),
                    commandInput.cropViewRequest),
            "Box key should send the configured typed crop request.") && isPassed;
        isPassed = GetExpected(
            SendKey(context, 'p', "p")
                && fixture.GetBindings()->GetCropPlaneCount() == 1
                && GetCropAction(fixture.GetBindings(), HostCropAction::Plane)
                && GetCropRequestEqual(
                    fixture.GetBindings()->GetLastCropRequest(),
                    commandInput.cropViewRequest),
            "Plane key should send one typed crop plane command.") && isPassed;
        isPassed = GetExpected(
            SendKey(context, 'k', "k")
                && fixture.GetBindings()->GetCropViewCount() == 1
                && GetCropAction(fixture.GetBindings(), HostCropAction::Preview)
                && fixture.GetBindings()->GetLastPreviewMode()
                    == HostCropPreviewMode::KeepInside
                && GetCropRequestEqual(
                    fixture.GetBindings()->GetLastCropRequest(),
                    commandInput.cropViewRequest),
            "Keep key should preserve the preview mode payload.") && isPassed;
        isPassed = GetExpected(
            SendKey(context, 'r', "r")
                && fixture.GetBindings()->GetCropViewCount() == 2
                && GetCropAction(fixture.GetBindings(), HostCropAction::Preview)
                && fixture.GetBindings()->GetLastPreviewMode()
                    == HostCropPreviewMode::RemoveInside
                && GetCropRequestEqual(
                    fixture.GetBindings()->GetLastCropRequest(),
                    commandInput.cropViewRequest),
            "Remove key should preserve the preview mode payload.") && isPassed;
        isPassed = GetExpected(
            SendKey(context, '3', "3", true)
                && fixture.GetBindings()->GetCropSendCount() == 1
                && GetCropAction(fixture.GetBindings(), HostCropAction::Submit)
                && GetCropRequestEqual(
                    fixture.GetBindings()->GetLastCropRequest(),
                    commandInput.cropViewRequest),
            "Ctrl+Submit should send one typed crop submit command.") && isPassed;
        isPassed = GetExpected(
            SendKey(context, 'j', "j")
                && fixture.GetBindings()->GetGapViewCount() == 1
                && GetGapAction(fixture.GetBindings(), HostGapAction::Switch)
                && GetGapRequestEqual(
                    fixture.GetBindings()->GetLastGapRequest(),
                    commandInput.gapViewRequest),
            "Gap key should send the complete typed gap request.") && isPassed;
        fixture.GetBindings()->SetCropActive(true);
        isPassed = GetExpected(
            SendKey(context, 27, "Escape")
                && fixture.GetBindings()->GetFeatureExitCount() == 1
                && std::holds_alternative<HostExitCommand>(
                    fixture.GetBindings()->GetLastCommand()),
            "Escape should send a typed feature exit command.") && isPassed;
        return isPassed;
    }

    int GetFailCount() const
    {
        int failureCount = 0;
        failureCount += StartModelRepeatCase() ? 0 : 1;
        failureCount += StartEscapeRepeatCase() ? 0 : 1;
        failureCount += StartCrossViewCase() ? 0 : 1;
        failureCount += StartSubmitStateCase() ? 0 : 1;
        failureCount += StartGapRepeatCase() ? 0 : 1;
        failureCount += StartFailureCase() ? 0 : 1;
        failureCount += StartLifecycleCase() ? 0 : 1;
        failureCount += StartExpiredRouterCase() ? 0 : 1;
        failureCount += StartTypedMappingCase() ? 0 : 1;
        return failureCount;
    }
};

int HostHotkeySuite::GetFailCount() const
{
    return HostHotkeyCases().GetFailCount();
}
