#include "Host/HostCommandRouter.h"

#include "AppService.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <vtkCommand.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
struct HostKeyInput {
    char keyCode = 0;
    std::string keySym;
    bool isControlPressed = false;
};

struct HostHotkeyTarget {
    std::weak_ptr<StdRenderContext> context;
    bool useFeature = false;
    bool useContext = false;
    // standalone 切片导出没有显式来源配置时，按触发窗口补足来源 role。
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
};

static char ToUpperAscii(char value)
{
    return (value >= 'a' && value <= 'z')
        ? static_cast<char>(value - 'a' + 'A')
        : value;
}

static HostKeyInput BuildHostKeyInput(const InteractionEvent& event)
{
    HostKeyInput input;
    input.keyCode = event.keyCode;
    input.keySym = event.keySym;
    input.isControlPressed = event.ctrl;
    return input;
}

static bool MatchesCharacterKey(const HostKeyInput& input, char key)
{
    if (key == 0) {
        return false;
    }

    const char upperKey = ToUpperAscii(key);
    const std::string keySymbol(1, key);
    const std::string upperKeySymbol(1, upperKey);
    return input.keyCode == key
        || input.keyCode == upperKey
        || input.keySym == keySymbol
        || input.keySym == upperKeySymbol;
}

static bool MatchesKeySymbol(const HostKeyInput& input, const std::string& keySym)
{
    return !keySym.empty() && input.keySym == keySym;
}

static bool IsToolOn(
    const std::shared_ptr<StdRenderContext>& context,
    ToolMode mode)
{
    return context && context->GetToolMode() == mode;
}

static bool ExitTool(
    const std::shared_ptr<StdRenderContext>& context,
    ToolMode mode)
{
    if (!IsToolOn(context, mode)) {
        return false;
    }

    context->SetToolMode(ToolMode::Navigation);
    return true;
}

static void AppendUniqueRenderView(
    std::vector<const HostRenderViewRuntime*>& views,
    const HostRenderViewRuntime* view)
{
    if (!view) {
        return;
    }
    const auto it = std::find(views.begin(), views.end(), view);
    if (it == views.end()) {
        views.push_back(view);
    }
}

static void AppendUniqueRenderViews(
    std::vector<const HostRenderViewRuntime*>& target,
    const std::vector<const HostRenderViewRuntime*>& source)
{
    for (const auto* view : source) {
        AppendUniqueRenderView(target, view);
    }
}

static bool HasView(
    const std::vector<const HostRenderViewRuntime*>& views,
    const HostRenderViewRuntime* view)
{
    return std::find(views.begin(), views.end(), view) != views.end();
}

class HostHotkeyHandler final {
public:
    std::weak_ptr<const HostCommandRouter> commandRouter;
    HostOrthogonalCropActivationRequest orthogonalCropRequest;
    HostGapAnalysisActivationRequest gapAnalysisRequest;
    HostHotkeyBindings hotkeys;
    HostDataExportConfig dataExportConfig;

    InteractionResult Handle(
        const InteractionEvent& event,
        const HostHotkeyTarget& target)
    {
        const auto router = commandRouter.lock();
        if (!router) {
            return {};
        }

        const HostKeyInput keyInput = BuildHostKeyInput(event);
        const bool isExitKey = MatchesKeySymbol(keyInput, hotkeys.exitKeySym);
        const bool isModelTransformToggleKey =
            target.useContext && MatchesCharacterKey(keyInput, hotkeys.modelTransformToggleKey);
        const bool isSaveTransformedDataKey =
            target.useContext && MatchesCharacterKey(keyInput, hotkeys.saveTransformedDataKey);
        const bool isSaveSliceImagesKey =
            target.useContext && MatchesCharacterKey(keyInput, hotkeys.saveSliceImagesKey);
        const bool isGapOverlayToggleKey =
            target.useFeature && MatchesCharacterKey(keyInput, hotkeys.gapOverlayToggleKey);
        const bool isCropToggleKey =
            target.useFeature && MatchesCharacterKey(keyInput, hotkeys.cropToggleKey);
        const bool isPlanarCropToggleKey =
            target.useFeature && MatchesCharacterKey(keyInput, hotkeys.planarCropToggleKey);
        const bool isInsidePreviewKey =
            target.useFeature && MatchesCharacterKey(keyInput, hotkeys.keepInsidePreviewKey);
        const bool isOutsidePreviewKey =
            target.useFeature && MatchesCharacterKey(keyInput, hotkeys.removeInsidePreviewKey);
        const bool isSubmitKeyCode =
            target.useFeature && MatchesCharacterKey(keyInput, hotkeys.submitKey);
        const bool isSubmitKey = isSubmitKeyCode && keyInput.isControlPressed;
        const bool shouldBlockVtkStereoKey = isSubmitKeyCode && !keyInput.isControlPressed;
        const bool canHandleExitCommand = isExitKey && CanExit(target);
        const bool isManagedHotkey =
            isModelTransformToggleKey
            || isSaveTransformedDataKey
            || isSaveSliceImagesKey
            || isCropToggleKey
            || isPlanarCropToggleKey
            || isInsidePreviewKey
            || isOutsidePreviewKey
            || isSubmitKey
            || shouldBlockVtkStereoKey
            || isGapOverlayToggleKey
            || canHandleExitCommand;

        if (event.vtkEventId == vtkCommand::CharEvent && isManagedHotkey) {
            return { true, true };
        }

        if (event.vtkEventId == vtkCommand::KeyPressEvent) {
            if (isModelTransformToggleKey && ToggleModel(target)) {
                return { true, true };
            }

            if (isSaveTransformedDataKey && SaveVolume(*router)) {
                return { true, true };
            }

            if (isSaveSliceImagesKey && SaveSlices(target, *router)) {
                return { true, true };
            }

            if (isCropToggleKey && !m_cropToggleKeyDown) {
                m_cropToggleKeyDown = true;
                HostCommandRouterRequest request;
                request.command = HostCommandKind::CropBox;
                request.orthogonalCropRequest = orthogonalCropRequest;
                router->DispatchHostCommand(std::move(request));
                return { true, true };
            }

            if (isPlanarCropToggleKey && !m_planarCropToggleKeyDown) {
                m_planarCropToggleKeyDown = true;
                HostCommandRouterRequest request;
                request.command = HostCommandKind::CropPlane;
                request.orthogonalCropRequest = orthogonalCropRequest;
                router->DispatchHostCommand(std::move(request));
                return { true, true };
            }

            if (isGapOverlayToggleKey && !m_gapOverlayToggleKeyDown) {
                m_gapOverlayToggleKeyDown = true;
                HostCommandRouterRequest request;
                request.command = HostCommandKind::GapToggle;
                request.gapAnalysisRequest = gapAnalysisRequest;
                router->DispatchHostCommand(std::move(request));
                return { true, true };
            }

            if (isExitKey && ExitMode(target, *router)) {
                return { true, true };
            }

            if (isInsidePreviewKey) {
                HostCommandRouterRequest request;
                request.command = HostCommandKind::CropPreview;
                request.orthogonalCropRequest = orthogonalCropRequest;
                request.cropPreviewMode = HostCropPreviewMode::KeepInside;
                router->DispatchHostCommand(std::move(request));
                return { true, true };
            }

            if (isOutsidePreviewKey) {
                HostCommandRouterRequest request;
                request.command = HostCommandKind::CropPreview;
                request.orthogonalCropRequest = orthogonalCropRequest;
                request.cropPreviewMode = HostCropPreviewMode::RemoveInside;
                router->DispatchHostCommand(std::move(request));
                return { true, true };
            }

            if (isSubmitKey && !m_submitKeyDown) {
                m_submitKeyDown = true;
                HostCommandRouterRequest request;
                request.command = HostCommandKind::CropApply;
                request.orthogonalCropRequest = orthogonalCropRequest;
                router->DispatchHostCommand(std::move(request));
                return { true, true };
            }

            if (isManagedHotkey) {
                return { true, true };
            }
        }

        if (event.vtkEventId == vtkCommand::KeyReleaseEvent) {
            if (isCropToggleKey) {
                m_cropToggleKeyDown = false;
                return { true, true };
            }
            if (isPlanarCropToggleKey) {
                m_planarCropToggleKeyDown = false;
                return { true, true };
            }
            if (isGapOverlayToggleKey) {
                m_gapOverlayToggleKeyDown = false;
                return { true, true };
            }
            if (isInsidePreviewKey || isOutsidePreviewKey) {
                return { true, true };
            }
            if (isSubmitKeyCode) {
                m_submitKeyDown = false;
                return { true, true };
            }
        }

        return {};
    }

private:
    bool m_cropToggleKeyDown = false;
    bool m_planarCropToggleKeyDown = false;
    bool m_gapOverlayToggleKeyDown = false;
    bool m_submitKeyDown = false;

    bool IsModelOn(const HostHotkeyTarget& target) const
    {
        const auto context = target.context.lock();
        return IsToolOn(context, ToolMode::ModelTransform);
    }

    bool ToggleModel(const HostHotkeyTarget& target) const
    {
        auto context = target.context.lock();
        if (!context) {
            return false;
        }

        context->SetToolMode(context->GetToolMode() == ToolMode::ModelTransform
            ? ToolMode::Navigation
            : ToolMode::ModelTransform);
        return true;
    }

    bool SaveVolume(const HostCommandRouter& router) const
    {
        HostCommandRouterRequest request;
        request.command = HostCommandKind::Export;
        request.dataExportConfig = dataExportConfig;
        request.dataExportConfig.hasVolumeExport = true;
        request.dataExportConfig.hasSliceExport = false;
        return router.DispatchHostCommand(std::move(request));
    }

    bool SaveSlices(
        const HostHotkeyTarget& target,
        const HostCommandRouter& router) const
    {
        if (!target.useContext) {
            return false;
        }

        HostCommandRouterRequest request;
        request.command = HostCommandKind::Export;
        request.dataExportConfig = dataExportConfig;
        request.dataExportConfig.hasVolumeExport = false;
        request.dataExportConfig.hasSliceExport = true;
        if (request.dataExportConfig.sliceImagesSourceViewId.empty()
            && !request.dataExportConfig.useSliceImagesSourceViewRole) {
            request.dataExportConfig.useSliceImagesSourceViewRole = true;
            request.dataExportConfig.sliceImagesSourceViewRole = target.role;
        }
        return router.DispatchHostCommand(std::move(request));
    }

    bool CanExit(const HostHotkeyTarget& target) const
    {
        return target.useContext && IsModelOn(target);
    }

    bool ExitMode(
        const HostHotkeyTarget& target,
        const HostCommandRouter& router) const
    {
        if (target.useFeature) {
            HostCommandRouterRequest request;
            request.command = HostCommandKind::FeatureExit;
            if (router.DispatchHostCommand(std::move(request))) {
                return true;
            }
        }
        if (target.useContext && IsModelOn(target)) {
            ExitTool(
                target.context.lock(),
                ToolMode::ModelTransform);
            return true;
        }
        return false;
    }
};
} // namespace

HostCommandRouter::HostCommandRouter(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews,
    std::shared_ptr<HostFeatureBindings> featureBindings)
    : m_core(&core)
    , m_renderViews(&renderViews)
    , m_featureBindings(std::move(featureBindings))
{
}

bool HostCommandRouter::DispatchHostCommand(HostCommandRouterRequest request) const
{
    switch (request.command) {
    case HostCommandKind::Load:
        return DispatchInitialVolumeLoad(
            request.initialVolume,
            std::move(request.loadComplete));
    case HostCommandKind::Export:
        return DispatchDataExport(
            request.dataExportConfig,
            std::move(request.dataExportComplete));
    case HostCommandKind::Hotkeys:
        return AttachStandaloneHotkeys(request);
    case HostCommandKind::CropStart:
    case HostCommandKind::CropBox:
    case HostCommandKind::CropPlane:
    case HostCommandKind::CropPreview:
    case HostCommandKind::CropApply:
    case HostCommandKind::CropExit:
    case HostCommandKind::FeatureExit:
    case HostCommandKind::GapStart:
    case HostCommandKind::GapToggle:
    case HostCommandKind::GapOverlay:
    case HostCommandKind::GapExit:
        return DispatchFeatureCommand(request);
    case HostCommandKind::None:
    default:
        std::cerr << "[Host] Command dispatch skipped: command was not specified." << std::endl;
        return false;
    }
}

bool HostCommandRouter::DispatchInitialVolumeLoad(
    const InitialVolumeLoadConfig& initialVolume,
    std::function<void(bool success)> loadComplete) const
{
    if (!initialVolume.enableInitialLoad) {
        return true;
    }
    if (!m_core || !m_renderViews) {
        std::cerr << "[Host] Initial volume load skipped: host services are not ready." << std::endl;
        return false;
    }
    if (initialVolume.filePath.empty()) {
        std::cerr << "[Host] Initial volume load skipped: file path was not specified." << std::endl;
        return false;
    }
    if (!initialVolume.geometry) {
        std::cerr << "[Host] Initial volume load skipped: volume geometry was not specified." << std::endl;
        return false;
    }

    const auto* primaryView = m_renderViews->GetPrimaryView();
    if (!primaryView || !primaryView->service) {
        std::cerr << "[Host] Initial volume load skipped: primary render view is missing." << std::endl;
        return false;
    }

    primaryView->service->LoadFileAsync(
        initialVolume.filePath,
        initialVolume.geometry->spacing,
        initialVolume.geometry->origin,
        std::move(loadComplete));
    return true;
}

bool HostCommandRouter::DispatchFeatureCommand(const HostCommandRouterRequest& request) const
{
    const auto bindings = m_featureBindings.lock();
    if (!bindings) {
        std::cerr << "[Host] Feature command dispatch skipped: feature bindings are not ready." << std::endl;
        return false;
    }

    switch (request.command) {
    case HostCommandKind::CropStart:
        return bindings->ActivateOrthogonalCrop(request.orthogonalCropRequest);
    case HostCommandKind::CropBox:
        return bindings->ToggleInteractiveCrop(request.orthogonalCropRequest);
    case HostCommandKind::CropPlane:
        return bindings->ToggleInteractivePlanarCrop(request.orthogonalCropRequest);
    case HostCommandKind::CropPreview:
        return bindings->ToggleCropPreview(
            request.orthogonalCropRequest,
            request.cropPreviewMode);
    case HostCommandKind::CropApply:
        return bindings->ApplyCropSubmit(request.orthogonalCropRequest);
    case HostCommandKind::CropExit:
        return bindings->ExitInteractiveCrop();
    case HostCommandKind::FeatureExit:
        return bindings->ExitActiveFeatureMode();
    case HostCommandKind::GapStart:
        return bindings->ActivateGapAnalysisDisplay(request.gapAnalysisRequest);
    case HostCommandKind::GapToggle:
        return bindings->ToggleGapAnalysisDisplay(request.gapAnalysisRequest);
    case HostCommandKind::GapOverlay:
        return bindings->ToggleGapAnalysisOverlayVisibility();
    case HostCommandKind::GapExit:
        return bindings->ExitGapAnalysisDisplay();
    case HostCommandKind::None:
    case HostCommandKind::Load:
    case HostCommandKind::Export:
    case HostCommandKind::Hotkeys:
    default:
        return false;
    }
}

bool HostCommandRouter::DispatchDataExport(
    const HostDataExportConfig& dataExportConfig,
    std::function<void(bool success)> onComplete) const
{
    if (!m_renderViews) {
        std::cerr << "[Host] Data export skipped: host render views are not ready." << std::endl;
        return false;
    }

    if (dataExportConfig.hasVolumeExport == dataExportConfig.hasSliceExport) {
        std::cerr << "[Host] Data export skipped: exactly one export request flag must be true." << std::endl;
        return false;
    }

    if (dataExportConfig.hasVolumeExport) {
        if (dataExportConfig.transformedDataOutputPath.empty()) {
            std::cerr << "[Host] Transformed data export skipped: output path was not configured." << std::endl;
            return false;
        }

        const auto* exportView = m_renderViews->GetPrimaryView();
        if (!exportView || !exportView->service) {
            std::cerr << "[Host] Transformed data export skipped: primary render view is missing." << std::endl;
            return false;
        }

        exportView->service->SaveTransformedDataAsync(
            dataExportConfig.transformedDataOutputPath,
            std::move(onComplete));
        return true;
    }

    if (dataExportConfig.sliceImagesOutputDirectory.empty()) {
        std::cerr << "[Host] Slice image export skipped: output directory was not configured." << std::endl;
        return false;
    }

    const HostRenderViewRuntime* exportView = nullptr;
    if (!dataExportConfig.sliceImagesSourceViewId.empty()) {
        exportView = m_renderViews->GetViewById(dataExportConfig.sliceImagesSourceViewId);
    }
    else if (dataExportConfig.useSliceImagesSourceViewRole) {
        exportView = m_renderViews->GetFirstViewByRole(dataExportConfig.sliceImagesSourceViewRole);
    }

    if (!exportView || !exportView->service) {
        std::cerr << "[Host] Slice image export skipped: source render view is missing." << std::endl;
        return false;
    }
    if (!HostRenderViewSet::GetRoleIsSliceView(exportView->config.role)) {
        std::cerr << "[Host] Slice image export skipped: source render view is not a slice view." << std::endl;
        return false;
    }

    exportView->service->SaveSliceImagesAsync(
        dataExportConfig.sliceImagesOutputDirectory,
        dataExportConfig.sliceImagesRotationAngleDeg,
        std::move(onComplete));
    return true;
}

bool HostCommandRouter::AttachStandaloneHotkeys(
    const HostCommandRouterRequest& request) const
{
    if (!m_renderViews) {
        return false;
    }
    if (!request.commandInput.enableStandaloneHotkeys
        && !request.renderContextInput.enableStandaloneHotkeys) {
        return true;
    }

    std::vector<const HostRenderViewRuntime*> featureTargetViews;
    if (request.commandInput.enableStandaloneHotkeys) {
        featureTargetViews = m_renderViews->GetViewsByIdsAndRoles(
            request.commandInput.targetViewIds,
            request.commandInput.targetViewRoles);
        if (featureTargetViews.empty()) {
            std::cerr << "[Host] Standalone feature hotkeys skipped: no target render view was requested." << std::endl;
        }
    }

    std::vector<const HostRenderViewRuntime*> renderContextTargetViews;
    if (request.renderContextInput.enableStandaloneHotkeys) {
        renderContextTargetViews = m_renderViews->GetViewsByIdsAndRoles(
            request.renderContextInput.targetViewIds,
            request.renderContextInput.targetViewRoles);
        if (renderContextTargetViews.empty()) {
            std::cerr << "[Host] Standalone render context hotkeys skipped: no target render view was requested." << std::endl;
        }
    }

    std::vector<const HostRenderViewRuntime*> keyTargetViews;
    AppendUniqueRenderViews(keyTargetViews, featureTargetViews);
    AppendUniqueRenderViews(keyTargetViews, renderContextTargetViews);
    if (keyTargetViews.empty()) {
        return false;
    }

    for (const auto* view : keyTargetViews) {
        if (!view || !view->context) {
            continue;
        }

        auto handler = std::make_shared<HostHotkeyHandler>();
        handler->commandRouter = weak_from_this();
        handler->orthogonalCropRequest = request.commandInput.orthogonalCropRequest;
        handler->gapAnalysisRequest = request.commandInput.gapAnalysisRequest;
        handler->hotkeys = request.hotkeys;
        handler->dataExportConfig = request.dataExportConfig;

        HostHotkeyTarget target;
        target.context = view->context;
        target.useFeature = HasView(featureTargetViews, view);
        target.useContext = HasView(renderContextTargetViews, view);
        target.role = view->config.role;
        view->context->SetKeyHandler(
            [handler, target](const InteractionEvent& event) {
                return handler->Handle(event, target);
            });
    }
    return true;
}
