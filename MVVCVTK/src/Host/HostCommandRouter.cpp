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
    bool hasFeature = false;
    bool hasContext = false;
    // standalone 切片导出没有显式来源配置时，按触发窗口补足来源 role。
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
};

enum class HotkeyAction {
    None,
    Model,
    ExportVolume,
    ExportSlices,
    CropBox,
    CropPlane,
    GapToggle,
    Exit,
    KeepPreview,
    RemovePreview,
    Submit,
    SubmitBlock
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
    input.isControlPressed = event.isCtrlDown;
    return input;
}

static bool GetCharKeyMatched(const HostKeyInput& input, char key)
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

static bool GetKeySymMatched(const HostKeyInput& input, const std::string& keySym)
{
    return !keySym.empty() && input.keySym == keySym;
}

static bool GetToolOn(
    const std::shared_ptr<StdRenderContext>& context,
    ToolMode mode)
{
    return context && context->GetToolMode() == mode;
}

static bool ExitTool(
    const std::shared_ptr<StdRenderContext>& context,
    ToolMode mode)
{
    if (!GetToolOn(context, mode)) {
        return false;
    }

    context->SetToolMode(ToolMode::Navigation);
    return true;
}

static void SetViewAdded(
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

static void SetViewsAdded(
    std::vector<const HostRenderViewRuntime*>& target,
    const std::vector<const HostRenderViewRuntime*>& source)
{
    for (const auto* view : source) {
        SetViewAdded(target, view);
    }
}

static bool GetViewFound(
    const std::vector<const HostRenderViewRuntime*>& views,
    const HostRenderViewRuntime* view)
{
    return std::find(views.begin(), views.end(), view) != views.end();
}

static bool GetViewEdit(const HostViewConfig& config)
{
    return config.mode.has_value()
        || config.material.has_value()
        || config.opacity.has_value()
        || config.tfNodes.has_value()
        || config.iso.has_value()
        || config.background.has_value()
        || config.spacing.has_value()
        || config.windowLevel.has_value();
}

class HostHotkeyHandler final {
public:
    std::weak_ptr<const HostCommandRouter> commandRouter;
    std::weak_ptr<HostFeatureBindings> featureBindings;
    HostCropRequest orthogonalCropRequest;
    HostGapRequest gapAnalysisRequest;
    HostHotkeyBindings hotkeys;
    HostDataExportConfig dataExportConfig;

    InteractionResult Send(
        const InteractionEvent& event,
        const HostHotkeyTarget& target)
    {
        const auto router = commandRouter.lock();
        if (!router) {
            return {};
        }

        const HostKeyInput keyInput = BuildHostKeyInput(event);
        const HotkeyAction action = GetAction(keyInput, target);
        const bool isManaged = GetHotkeyUse(action, target);

        if (event.vtkEventId == vtkCommand::CharEvent && isManaged) {
            return { true, true };
        }

        if (event.vtkEventId == vtkCommand::KeyPressEvent) {
            if (OnPress(action, target, *router)) {
                return { true, true };
            }

            if (isManaged) {
                return { true, true };
            }
        }

        if (event.vtkEventId == vtkCommand::KeyReleaseEvent) {
            if (OnRelease(action)) {
                return { true, true };
            }
        }

        return {};
    }

private:
    bool m_isCropDown = false;
    bool m_isPlaneDown = false;
    bool m_isGapDown = false;
    bool m_isSubmitDown = false;

    HotkeyAction GetAction(
        const HostKeyInput& input,
        const HostHotkeyTarget& target) const
    {
        if (target.hasContext && GetCharKeyMatched(input, hotkeys.modelTransformToggleKey)) {
            return HotkeyAction::Model;
        }
        if (target.hasContext && GetCharKeyMatched(input, hotkeys.saveTransformedDataKey)) {
            return HotkeyAction::ExportVolume;
        }
        if (target.hasContext && GetCharKeyMatched(input, hotkeys.saveSliceImagesKey)) {
            return HotkeyAction::ExportSlices;
        }
        if (target.hasFeature && GetCharKeyMatched(input, hotkeys.cropToggleKey)) {
            return HotkeyAction::CropBox;
        }
        if (target.hasFeature && GetCharKeyMatched(input, hotkeys.planarCropToggleKey)) {
            return HotkeyAction::CropPlane;
        }
        if (target.hasFeature && GetCharKeyMatched(input, hotkeys.gapOverlayToggleKey)) {
            return HotkeyAction::GapToggle;
        }
        if (GetKeySymMatched(input, hotkeys.exitKeySym)) {
            return HotkeyAction::Exit;
        }
        if (target.hasFeature && GetCharKeyMatched(input, hotkeys.keepInsidePreviewKey)) {
            return HotkeyAction::KeepPreview;
        }
        if (target.hasFeature && GetCharKeyMatched(input, hotkeys.removeInsidePreviewKey)) {
            return HotkeyAction::RemovePreview;
        }
        if (target.hasFeature && GetCharKeyMatched(input, hotkeys.submitKey)) {
            return input.isControlPressed ? HotkeyAction::Submit : HotkeyAction::SubmitBlock;
        }
        return HotkeyAction::None;
    }

    bool GetHotkeyUse(
        HotkeyAction action,
        const HostHotkeyTarget& target) const
    {
        if (action == HotkeyAction::None) {
            return false;
        }
        if (action == HotkeyAction::Exit) {
            return GetExitReady(target);
        }
        return true;
    }

    bool OnPress(
        HotkeyAction action,
        const HostHotkeyTarget& target,
        const HostCommandRouter& router)
    {
        switch (action) {
        case HotkeyAction::Model:
            return SwitchModel(target);
        case HotkeyAction::ExportVolume:
            return ExportVolume(router);
        case HotkeyAction::ExportSlices:
            return ExportSlices(target, router);
        case HotkeyAction::CropBox:
            if (m_isCropDown) {
                return false;
            }
            m_isCropDown = true;
            return SendFeature(router, action);
        case HotkeyAction::CropPlane:
            if (m_isPlaneDown) {
                return false;
            }
            m_isPlaneDown = true;
            return SendFeature(router, action);
        case HotkeyAction::GapToggle:
            if (m_isGapDown) {
                return false;
            }
            m_isGapDown = true;
            return SendFeature(router, action);
        case HotkeyAction::Exit:
            if (!GetExitReady(target)) {
                return false;
            }
            return ExitMode(target, router);
        case HotkeyAction::KeepPreview:
        case HotkeyAction::RemovePreview:
            return SendFeature(router, action);
        case HotkeyAction::Submit:
            if (m_isSubmitDown) {
                return false;
            }
            m_isSubmitDown = true;
            return SendFeature(router, action);
        case HotkeyAction::SubmitBlock:
            return true;
        case HotkeyAction::None:
        default:
            return false;
        }
    }

    bool OnRelease(HotkeyAction action)
    {
        switch (action) {
        case HotkeyAction::CropBox:
            m_isCropDown = false;
            return true;
        case HotkeyAction::CropPlane:
            m_isPlaneDown = false;
            return true;
        case HotkeyAction::GapToggle:
            m_isGapDown = false;
            return true;
        case HotkeyAction::KeepPreview:
        case HotkeyAction::RemovePreview:
            return true;
        case HotkeyAction::Submit:
        case HotkeyAction::SubmitBlock:
            m_isSubmitDown = false;
            return true;
        case HotkeyAction::None:
        case HotkeyAction::Model:
        case HotkeyAction::ExportVolume:
        case HotkeyAction::ExportSlices:
        case HotkeyAction::Exit:
        default:
            return false;
        }
    }

    bool GetModelOn(const HostHotkeyTarget& target) const
    {
        const auto context = target.context.lock();
        return GetToolOn(context, ToolMode::ModelTransform);
    }

    bool SwitchModel(const HostHotkeyTarget& target) const
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

    bool ExportVolume(const HostCommandRouter& router) const
    {
        HostCommandRouterRequest request;
        request.command = HostCommandKind::Export;
        request.dataExportConfig = dataExportConfig;
        request.dataExportConfig.hasVolumeExport = true;
        request.dataExportConfig.hasSliceExport = false;
        return router.DispatchCommand(std::move(request));
    }

    bool ExportSlices(
        const HostHotkeyTarget& target,
        const HostCommandRouter& router) const
    {
        if (!target.hasContext) {
            return false;
        }

        HostCommandRouterRequest request;
        request.command = HostCommandKind::Export;
        request.dataExportConfig = dataExportConfig;
        request.dataExportConfig.hasVolumeExport = false;
        request.dataExportConfig.hasSliceExport = true;
        if (request.dataExportConfig.sliceImagesSourceViewId.empty()
            && !request.dataExportConfig.isSliceRoleUsed) {
            request.dataExportConfig.isSliceRoleUsed = true;
            request.dataExportConfig.sliceImagesSourceViewRole = target.role;
        }
        return router.DispatchCommand(std::move(request));
    }

    bool SendFeature(
        const HostCommandRouter& router,
        HotkeyAction action) const
    {
        HostCommandRouterRequest request;
        switch (action) {
        case HotkeyAction::CropBox:
            request.command = HostCommandKind::CropBox;
            request.orthogonalCropRequest = orthogonalCropRequest;
            break;
        case HotkeyAction::CropPlane:
            request.command = HostCommandKind::CropPlane;
            request.orthogonalCropRequest = orthogonalCropRequest;
            break;
        case HotkeyAction::GapToggle:
            request.command = HostCommandKind::GapToggle;
            request.gapAnalysisRequest = gapAnalysisRequest;
            break;
        case HotkeyAction::KeepPreview:
            request.command = HostCommandKind::CropPreview;
            request.orthogonalCropRequest = orthogonalCropRequest;
            request.cropPreviewMode = HostCropPreviewMode::KeepInside;
            break;
        case HotkeyAction::RemovePreview:
            request.command = HostCommandKind::CropPreview;
            request.orthogonalCropRequest = orthogonalCropRequest;
            request.cropPreviewMode = HostCropPreviewMode::RemoveInside;
            break;
        case HotkeyAction::Submit:
            request.command = HostCommandKind::CropApply;
            request.orthogonalCropRequest = orthogonalCropRequest;
            break;
        default:
            return false;
        }
        return router.DispatchCommand(std::move(request));
    }

    bool GetExitReady(const HostHotkeyTarget& target) const
    {
        bool hasFeature = false;
        if (target.hasFeature) {
            if (const auto bindings = featureBindings.lock()) {
                hasFeature = bindings->GetCropActive()
                    || bindings->GetGapView();
            }
        }
        return hasFeature || (target.hasContext && GetModelOn(target));
    }

    bool ExitMode(
        const HostHotkeyTarget& target,
        const HostCommandRouter& router) const
    {
        if (target.hasFeature) {
            HostCommandRouterRequest request;
            request.command = HostCommandKind::FeatureExit;
            if (router.DispatchCommand(std::move(request))) {
                return true;
            }
        }
        if (target.hasContext && GetModelOn(target)) {
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

bool HostCommandRouter::DispatchCommand(HostCommandRouterRequest request) const
{
    switch (request.command) {
    case HostCommandKind::Load:
        return LoadVolume(
            request.initialVolume,
            std::move(request.loadComplete));
    case HostCommandKind::Export:
        return ExportData(
            request.dataExportConfig,
            std::move(request.dataExportComplete));
    case HostCommandKind::Hotkeys:
        return AttachHotkeys(request);
    case HostCommandKind::ViewConfig:
        return SetViewConfig(request.viewConfig);
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
        return SendFeature(request);
    case HostCommandKind::None:
    default:
        std::cerr << "[Host] Command dispatch skipped: command was not specified." << std::endl;
        return false;
    }
}

bool HostCommandRouter::LoadVolume(
    const InitialVolumeLoadConfig& initialVolume,
    std::function<void(bool isSuccess)> loadComplete) const
{
    if (!initialVolume.isInitialLoadEnabled) {
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

    std::function<bool()> refreshCropInput;
    const auto featureBindings = m_featureBindings;
    if (const auto lockedBindings = featureBindings.lock()) {
        refreshCropInput = lockedBindings->BuildCropInput();
    }

    primaryView->service->LoadFileAsync(
        initialVolume.filePath,
        initialVolume.geometry->spacing,
        initialVolume.geometry->origin,
        [
            refreshCropInput = std::move(refreshCropInput),
            featureBindings,
            loadComplete = std::move(loadComplete)
        ](bool isSuccess) mutable {
            if (isSuccess && refreshCropInput) {
                refreshCropInput();
            }
            else if (!isSuccess) {
                if (const auto lockedBindings = featureBindings.lock()) {
                    lockedBindings->ClearCropInput();
                }
            }
            if (loadComplete) {
                loadComplete(isSuccess);
            }
        });
    return true;
}

bool HostCommandRouter::SendFeature(const HostCommandRouterRequest& request) const
{
    const auto bindings = m_featureBindings.lock();
    if (!bindings) {
        std::cerr << "[Host] Feature command dispatch skipped: feature bindings are not ready." << std::endl;
        return false;
    }

    switch (request.command) {
    case HostCommandKind::CropStart:
        return bindings->StartCrop(request.orthogonalCropRequest);
    case HostCommandKind::CropBox:
        return bindings->SwitchCropBox(request.orthogonalCropRequest);
    case HostCommandKind::CropPlane:
        return bindings->SwitchCropPlane(request.orthogonalCropRequest);
    case HostCommandKind::CropPreview:
        return bindings->SwitchCropView(
            request.orthogonalCropRequest,
            request.cropPreviewMode);
    case HostCommandKind::CropApply:
        return bindings->SendCrop(request.orthogonalCropRequest);
    case HostCommandKind::CropExit:
        return bindings->ExitCrop();
    case HostCommandKind::FeatureExit:
        return bindings->ExitFeature();
    case HostCommandKind::GapStart:
        return bindings->StartGapView(request.gapAnalysisRequest);
    case HostCommandKind::GapToggle:
        return bindings->SwitchGapView(request.gapAnalysisRequest);
    case HostCommandKind::GapOverlay:
        return bindings->SwitchGapLayer();
    case HostCommandKind::GapExit:
        return bindings->ExitGapView();
    case HostCommandKind::None:
    case HostCommandKind::Load:
    case HostCommandKind::Export:
    case HostCommandKind::Hotkeys:
    case HostCommandKind::ViewConfig:
    default:
        return false;
    }
}

bool HostCommandRouter::ExportData(
    const HostDataExportConfig& dataExportConfig,
    std::function<void(bool isSuccess)> onComplete) const
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

        exportView->service->ExportDataAsync(
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
    else if (dataExportConfig.isSliceRoleUsed) {
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

    exportView->service->ExportSlicesAsync(
        dataExportConfig.sliceImagesOutputDirectory,
        dataExportConfig.sliceImagesRotationAngleDeg,
        std::move(onComplete));
    return true;
}

bool HostCommandRouter::SetViewConfig(const HostViewConfig& viewConfig) const
{
    if (!m_renderViews) {
        std::cerr << "[Host] View config skipped: host render views are not ready." << std::endl;
        return false;
    }
    if (!GetViewEdit(viewConfig)) {
        std::cerr << "[Host] View config skipped: no config value was provided." << std::endl;
        return false;
    }

    const HostRenderViewRuntime* targetView = nullptr;
    if (!viewConfig.viewId.empty()) {
        targetView = m_renderViews->GetViewById(viewConfig.viewId);
    }
    else if (viewConfig.isViewRoleUsed) {
        targetView = m_renderViews->GetFirstViewByRole(viewConfig.viewRole);
    }

    if (!targetView || !targetView->service) {
        std::cerr << "[Host] View config skipped: target render view is missing." << std::endl;
        return false;
    }

    const auto& service = targetView->service;
    if (viewConfig.mode) {
        service->SetVizMode(*viewConfig.mode);
    }
    if (viewConfig.material) {
        service->SetMaterial(*viewConfig.material);
    }
    if (viewConfig.opacity) {
        service->SetOpacity(*viewConfig.opacity);
    }
    if (viewConfig.tfNodes) {
        service->SetTransferFunction(*viewConfig.tfNodes);
    }
    if (viewConfig.iso) {
        service->SetIsoThreshold(*viewConfig.iso);
    }
    if (viewConfig.background) {
        service->SetBackground(*viewConfig.background);
    }
    if (viewConfig.spacing) {
        service->SetSpacing(
            (*viewConfig.spacing)[0],
            (*viewConfig.spacing)[1],
            (*viewConfig.spacing)[2]);
    }
    if (viewConfig.windowLevel) {
        service->SetWindowLevel(
            viewConfig.windowLevel->windowWidth,
            viewConfig.windowLevel->windowCenter);
    }
    return true;
}

bool HostCommandRouter::AttachHotkeys(
    const HostCommandRouterRequest& request) const
{
    if (!m_renderViews) {
        return false;
    }
    if (!request.commandInput.isHotkeyEnabled
        && !request.renderContextInput.isHotkeyEnabled) {
        return true;
    }

    std::vector<const HostRenderViewRuntime*> featureTargetViews;
    if (request.commandInput.isHotkeyEnabled) {
        featureTargetViews = m_renderViews->GetViewsByIdsAndRoles(
            request.commandInput.targetViewIds,
            request.commandInput.targetViewRoles);
        if (featureTargetViews.empty()) {
            std::cerr << "[Host] Standalone feature hotkeys skipped: no target render view was requested." << std::endl;
        }
    }

    std::vector<const HostRenderViewRuntime*> renderContextTargetViews;
    if (request.renderContextInput.isHotkeyEnabled) {
        renderContextTargetViews = m_renderViews->GetViewsByIdsAndRoles(
            request.renderContextInput.targetViewIds,
            request.renderContextInput.targetViewRoles);
        if (renderContextTargetViews.empty()) {
            std::cerr << "[Host] Standalone render context hotkeys skipped: no target render view was requested." << std::endl;
        }
    }

    std::vector<const HostRenderViewRuntime*> keyTargetViews;
    SetViewsAdded(keyTargetViews, featureTargetViews);
    SetViewsAdded(keyTargetViews, renderContextTargetViews);
    if (keyTargetViews.empty()) {
        return false;
    }

    for (const auto* view : keyTargetViews) {
        if (!view || !view->context) {
            continue;
        }

        auto handler = std::make_shared<HostHotkeyHandler>();
        handler->commandRouter = weak_from_this();
        handler->featureBindings = m_featureBindings;
        handler->orthogonalCropRequest = request.commandInput.orthogonalCropRequest;
        handler->gapAnalysisRequest = request.commandInput.gapAnalysisRequest;
        handler->hotkeys = request.hotkeys;
        handler->dataExportConfig = request.dataExportConfig;

        HostHotkeyTarget target;
        target.context = view->context;
        target.hasFeature = GetViewFound(featureTargetViews, view);
        target.hasContext = GetViewFound(renderContextTargetViews, view);
        target.role = view->config.role;
        view->context->SetKeyHandler(
            [handler, target](const InteractionEvent& event) {
                return handler->Send(event, target);
            });
    }
    return true;
}
