#include "Host/HostCommandRouter.h"

#include "AppService.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "Interaction/InteractionEvent.h"
#include "Interaction/InteractionResult.h"
#include "StdRenderContext.h"

#include <vtkCommand.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

class HostCommandRouter::Impl final {
public:
    Impl(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews,
        std::shared_ptr<HostFeatureBindings> featureBindings);
    ~Impl();

    bool DispatchCommand(HostCommandRouterRequest request) const;

private:
    InteractionResult SendHotkey(
        const InteractionEvent& event,
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext,
        HostRenderViewRole role,
        const HostCommandRouterRequest& request,
        bool& isCropDown,
        bool& isPlaneDown,
        bool& isGapDown,
        bool& isSubmitDown) const;

    bool LoadVolume(
        const InitialVolumeLoadConfig& initialVolume,
        std::function<void(bool isSuccess)> loadComplete) const;
    bool SendFeature(const HostCommandRouterRequest& request) const;
    bool ExportData(
        const HostDataExportConfig& dataExportConfig,
        std::function<void(bool isSuccess)> onComplete) const;
    bool SetViewConfig(const HostViewConfig& viewConfig) const;
    bool AttachHotkeys(const HostCommandRouterRequest& request) const;
    void SetViewAdded(
        std::vector<const HostRenderViewRuntime*>& views,
        const HostRenderViewRuntime* view) const;
    void SetViewsAdded(
        std::vector<const HostRenderViewRuntime*>& target,
        const std::vector<const HostRenderViewRuntime*>& source) const;
    bool GetViewFound(
        const std::vector<const HostRenderViewRuntime*>& views,
        const HostRenderViewRuntime* view) const;
    bool SendHotkeyPress(
        HotkeyAction action,
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext,
        HostRenderViewRole role,
        const HostCommandRouterRequest& request,
        bool& isCropDown,
        bool& isPlaneDown,
        bool& isGapDown,
        bool& isSubmitDown) const;
    bool SetHotkeyRelease(
        HotkeyAction action,
        bool& isCropDown,
        bool& isPlaneDown,
        bool& isGapDown,
        bool& isSubmitDown) const;
    bool SendHotkeyFeature(
        HotkeyAction action,
        const HostCommandRouterRequest& request) const;
    bool ExportHotkeyVolume(const HostCommandRouterRequest& request) const;
    bool ExportHotkeySlices(
        const HostCommandRouterRequest& request,
        bool hasContext,
        HostRenderViewRole role) const;
    bool ExitHotkeyMode(
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext) const;
    HotkeyAction GetHotkeyAction(
        const InteractionEvent& event,
        bool hasFeature,
        bool hasContext,
        const HostHotkeyBindings& hotkeys) const;
    bool GetHotkeyUse(
        HotkeyAction action,
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext) const;
    bool GetExitReady(
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext) const;
    bool GetModelOn(const std::weak_ptr<StdRenderContext>& context) const;
    bool SwitchModel(const std::weak_ptr<StdRenderContext>& context) const;

    char GetUpperAscii(char value) const;
    bool GetCharKeyMatched(const InteractionEvent& event, char key) const;
    bool GetKeySymMatched(const InteractionEvent& event, const std::string& keySym) const;
    bool GetToolOn(
        const std::shared_ptr<StdRenderContext>& context,
        ToolMode mode) const;
    bool ExitTool(
        const std::shared_ptr<StdRenderContext>& context,
        ToolMode mode) const;

    const HostCoreServices* m_core = nullptr;
    const HostRenderViewSet* m_renderViews = nullptr;
    std::weak_ptr<HostFeatureBindings> m_featureBindings;
    mutable std::vector<std::weak_ptr<StdRenderContext>> m_keyContexts;
};

HostCommandRouter::Impl::Impl(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews,
    std::shared_ptr<HostFeatureBindings> featureBindings)
    : m_core(&core)
    , m_renderViews(&renderViews)
    , m_featureBindings(std::move(featureBindings))
{
}

HostCommandRouter::Impl::~Impl()
{
    for (const auto& context : m_keyContexts) {
        if (const auto lockedContext = context.lock()) {
            lockedContext->ClearKeyHandler();
        }
    }
    m_keyContexts.clear();
}

char HostCommandRouter::Impl::GetUpperAscii(char value) const
{
    return (value >= 'a' && value <= 'z')
        ? static_cast<char>(value - 'a' + 'A')
        : value;
}

bool HostCommandRouter::Impl::GetCharKeyMatched(
    const InteractionEvent& event,
    char key) const
{
    if (key == 0) {
        return false;
    }

    const char upperKey = GetUpperAscii(key);
    const std::string keySymbol(1, key);
    const std::string upperKeySymbol(1, upperKey);
    return event.keyCode == key
        || event.keyCode == upperKey
        || event.keySym == keySymbol
        || event.keySym == upperKeySymbol;
}

bool HostCommandRouter::Impl::GetKeySymMatched(
    const InteractionEvent& event,
    const std::string& keySym) const
{
    return !keySym.empty() && event.keySym == keySym;
}

bool HostCommandRouter::Impl::GetToolOn(
    const std::shared_ptr<StdRenderContext>& context,
    ToolMode mode) const
{
    return context && context->GetToolMode() == mode;
}

bool HostCommandRouter::Impl::ExitTool(
    const std::shared_ptr<StdRenderContext>& context,
    ToolMode mode) const
{
    if (!GetToolOn(context, mode)) {
        return false;
    }

    context->SetToolMode(ToolMode::Navigation);
    return true;
}

bool HostCommandRouter::Impl::DispatchCommand(HostCommandRouterRequest request) const
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

bool HostCommandRouter::Impl::LoadVolume(
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

bool HostCommandRouter::Impl::SendFeature(const HostCommandRouterRequest& request) const
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

bool HostCommandRouter::Impl::ExportData(
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
    if (!m_renderViews->GetRoleIsSliceView(exportView->config.role)) {
        std::cerr << "[Host] Slice image export skipped: source render view is not a slice view." << std::endl;
        return false;
    }

    exportView->service->ExportSlicesAsync(
        dataExportConfig.sliceImagesOutputDirectory,
        dataExportConfig.sliceImagesRotationAngleDeg,
        std::move(onComplete));
    return true;
}

bool HostCommandRouter::Impl::SetViewConfig(const HostViewConfig& viewConfig) const
{
    const bool hasViewEdit =
        viewConfig.mode.has_value()
        || viewConfig.material.has_value()
        || viewConfig.opacity.has_value()
        || viewConfig.tfNodes.has_value()
        || viewConfig.iso.has_value()
        || viewConfig.background.has_value()
        || viewConfig.spacing.has_value()
        || viewConfig.windowLevel.has_value();

    if (!m_renderViews) {
        std::cerr << "[Host] View config skipped: host render views are not ready." << std::endl;
        return false;
    }
    if (!hasViewEdit) {
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

InteractionResult HostCommandRouter::Impl::SendHotkey(
    const InteractionEvent& event,
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext,
    HostRenderViewRole role,
    const HostCommandRouterRequest& request,
    bool& isCropDown,
    bool& isPlaneDown,
    bool& isGapDown,
    bool& isSubmitDown) const
{
    const HotkeyAction action = GetHotkeyAction(
        event,
        hasFeature,
        hasContext,
        request.hotkeys);
    const bool isManaged = GetHotkeyUse(action, context, hasFeature, hasContext);

    if (event.vtkEventId == vtkCommand::CharEvent && isManaged) {
        return { true, true };
    }

    if (event.vtkEventId == vtkCommand::KeyPressEvent) {
        if (SendHotkeyPress(
            action,
            context,
            hasFeature,
            hasContext,
            role,
            request,
            isCropDown,
            isPlaneDown,
            isGapDown,
            isSubmitDown)) {
            return { true, true };
        }

        if (isManaged) {
            return { true, true };
        }
    }

    if (event.vtkEventId == vtkCommand::KeyReleaseEvent) {
        if (SetHotkeyRelease(
            action,
            isCropDown,
            isPlaneDown,
            isGapDown,
            isSubmitDown)) {
            return { true, true };
        }
    }

    return {};
}

HotkeyAction HostCommandRouter::Impl::GetHotkeyAction(
    const InteractionEvent& event,
    bool hasFeature,
    bool hasContext,
    const HostHotkeyBindings& hotkeys) const
{
    if (hasContext && GetCharKeyMatched(event, hotkeys.modelTransformToggleKey)) {
        return HotkeyAction::Model;
    }
    if (hasContext && GetCharKeyMatched(event, hotkeys.saveTransformedDataKey)) {
        return HotkeyAction::ExportVolume;
    }
    if (hasContext && GetCharKeyMatched(event, hotkeys.saveSliceImagesKey)) {
        return HotkeyAction::ExportSlices;
    }
    if (hasFeature && GetCharKeyMatched(event, hotkeys.cropToggleKey)) {
        return HotkeyAction::CropBox;
    }
    if (hasFeature && GetCharKeyMatched(event, hotkeys.planarCropToggleKey)) {
        return HotkeyAction::CropPlane;
    }
    if (hasFeature && GetCharKeyMatched(event, hotkeys.gapOverlayToggleKey)) {
        return HotkeyAction::GapToggle;
    }
    if (GetKeySymMatched(event, hotkeys.exitKeySym)) {
        return HotkeyAction::Exit;
    }
    if (hasFeature && GetCharKeyMatched(event, hotkeys.keepInsidePreviewKey)) {
        return HotkeyAction::KeepPreview;
    }
    if (hasFeature && GetCharKeyMatched(event, hotkeys.removeInsidePreviewKey)) {
        return HotkeyAction::RemovePreview;
    }
    if (hasFeature && GetCharKeyMatched(event, hotkeys.submitKey)) {
        return event.isCtrlDown ? HotkeyAction::Submit : HotkeyAction::SubmitBlock;
    }
    return HotkeyAction::None;
}

bool HostCommandRouter::Impl::GetHotkeyUse(
    HotkeyAction action,
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext) const
{
    if (action == HotkeyAction::None) {
        return false;
    }
    if (action == HotkeyAction::Exit) {
        return GetExitReady(context, hasFeature, hasContext);
    }
    return true;
}

bool HostCommandRouter::Impl::SendHotkeyPress(
    HotkeyAction action,
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext,
    HostRenderViewRole role,
    const HostCommandRouterRequest& request,
    bool& isCropDown,
    bool& isPlaneDown,
    bool& isGapDown,
    bool& isSubmitDown) const
{
    switch (action) {
    case HotkeyAction::Model:
        return SwitchModel(context);
    case HotkeyAction::ExportVolume:
        return ExportHotkeyVolume(request);
    case HotkeyAction::ExportSlices:
        return ExportHotkeySlices(request, hasContext, role);
    case HotkeyAction::CropBox:
        if (isCropDown) {
            return false;
        }
        isCropDown = true;
        return SendHotkeyFeature(action, request);
    case HotkeyAction::CropPlane:
        if (isPlaneDown) {
            return false;
        }
        isPlaneDown = true;
        return SendHotkeyFeature(action, request);
    case HotkeyAction::GapToggle:
        if (isGapDown) {
            return false;
        }
        isGapDown = true;
        return SendHotkeyFeature(action, request);
    case HotkeyAction::Exit:
        if (!GetExitReady(context, hasFeature, hasContext)) {
            return false;
        }
        return ExitHotkeyMode(context, hasFeature, hasContext);
    case HotkeyAction::KeepPreview:
    case HotkeyAction::RemovePreview:
        return SendHotkeyFeature(action, request);
    case HotkeyAction::Submit:
        if (isSubmitDown) {
            return false;
        }
        isSubmitDown = true;
        return SendHotkeyFeature(action, request);
    case HotkeyAction::SubmitBlock:
        return true;
    case HotkeyAction::None:
    default:
        return false;
    }
}

bool HostCommandRouter::Impl::SetHotkeyRelease(
    HotkeyAction action,
    bool& isCropDown,
    bool& isPlaneDown,
    bool& isGapDown,
    bool& isSubmitDown) const
{
    switch (action) {
    case HotkeyAction::CropBox:
        isCropDown = false;
        return true;
    case HotkeyAction::CropPlane:
        isPlaneDown = false;
        return true;
    case HotkeyAction::GapToggle:
        isGapDown = false;
        return true;
    case HotkeyAction::KeepPreview:
    case HotkeyAction::RemovePreview:
        return true;
    case HotkeyAction::Submit:
    case HotkeyAction::SubmitBlock:
        isSubmitDown = false;
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

bool HostCommandRouter::Impl::GetModelOn(const std::weak_ptr<StdRenderContext>& context) const
{
    return GetToolOn(context.lock(), ToolMode::ModelTransform);
}

bool HostCommandRouter::Impl::SwitchModel(const std::weak_ptr<StdRenderContext>& context) const
{
    auto renderContext = context.lock();
    if (!renderContext) {
        return false;
    }

    renderContext->SetToolMode(renderContext->GetToolMode() == ToolMode::ModelTransform
        ? ToolMode::Navigation
        : ToolMode::ModelTransform);
    return true;
}

bool HostCommandRouter::Impl::ExportHotkeyVolume(const HostCommandRouterRequest& request) const
{
    HostCommandRouterRequest exportRequest;
    exportRequest.command = HostCommandKind::Export;
    exportRequest.dataExportConfig = request.dataExportConfig;
    exportRequest.dataExportConfig.hasVolumeExport = true;
    exportRequest.dataExportConfig.hasSliceExport = false;
    return DispatchCommand(std::move(exportRequest));
}

bool HostCommandRouter::Impl::ExportHotkeySlices(
    const HostCommandRouterRequest& request,
    bool hasContext,
    HostRenderViewRole role) const
{
    if (!hasContext) {
        return false;
    }

    HostCommandRouterRequest exportRequest;
    exportRequest.command = HostCommandKind::Export;
    exportRequest.dataExportConfig = request.dataExportConfig;
    exportRequest.dataExportConfig.hasVolumeExport = false;
    exportRequest.dataExportConfig.hasSliceExport = true;
    if (exportRequest.dataExportConfig.sliceImagesSourceViewId.empty()
        && !exportRequest.dataExportConfig.isSliceRoleUsed) {
        exportRequest.dataExportConfig.isSliceRoleUsed = true;
        exportRequest.dataExportConfig.sliceImagesSourceViewRole = role;
    }
    return DispatchCommand(std::move(exportRequest));
}

bool HostCommandRouter::Impl::SendHotkeyFeature(
    HotkeyAction action,
    const HostCommandRouterRequest& request) const
{
    HostCommandRouterRequest featureRequest;
    switch (action) {
    case HotkeyAction::CropBox:
        featureRequest.command = HostCommandKind::CropBox;
        featureRequest.orthogonalCropRequest = request.commandInput.orthogonalCropRequest;
        break;
    case HotkeyAction::CropPlane:
        featureRequest.command = HostCommandKind::CropPlane;
        featureRequest.orthogonalCropRequest = request.commandInput.orthogonalCropRequest;
        break;
    case HotkeyAction::GapToggle:
        featureRequest.command = HostCommandKind::GapToggle;
        featureRequest.gapAnalysisRequest = request.commandInput.gapAnalysisRequest;
        break;
    case HotkeyAction::KeepPreview:
        featureRequest.command = HostCommandKind::CropPreview;
        featureRequest.orthogonalCropRequest = request.commandInput.orthogonalCropRequest;
        featureRequest.cropPreviewMode = HostCropPreviewMode::KeepInside;
        break;
    case HotkeyAction::RemovePreview:
        featureRequest.command = HostCommandKind::CropPreview;
        featureRequest.orthogonalCropRequest = request.commandInput.orthogonalCropRequest;
        featureRequest.cropPreviewMode = HostCropPreviewMode::RemoveInside;
        break;
    case HotkeyAction::Submit:
        featureRequest.command = HostCommandKind::CropApply;
        featureRequest.orthogonalCropRequest = request.commandInput.orthogonalCropRequest;
        break;
    default:
        return false;
    }
    return DispatchCommand(std::move(featureRequest));
}

bool HostCommandRouter::Impl::GetExitReady(
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext) const
{
    bool hasFeatureActive = false;
    if (hasFeature) {
        if (const auto bindings = m_featureBindings.lock()) {
            hasFeatureActive = bindings->GetCropActive()
                || bindings->GetGapView();
        }
    }
    return hasFeatureActive || (hasContext && GetModelOn(context));
}

bool HostCommandRouter::Impl::ExitHotkeyMode(
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext) const
{
    if (hasFeature) {
        HostCommandRouterRequest request;
        request.command = HostCommandKind::FeatureExit;
        if (DispatchCommand(std::move(request))) {
            return true;
        }
    }
    if (hasContext && GetModelOn(context)) {
        ExitTool(
            context.lock(),
            ToolMode::ModelTransform);
        return true;
    }
    return false;
}

bool HostCommandRouter::Impl::AttachHotkeys(
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

        const std::weak_ptr<StdRenderContext> context = view->context;
        m_keyContexts.push_back(context);
        const bool hasFeature = GetViewFound(featureTargetViews, view);
        const bool hasContext = GetViewFound(renderContextTargetViews, view);
        const HostRenderViewRole role = view->config.role;
        auto isCropDown = std::make_shared<bool>(false);
        auto isPlaneDown = std::make_shared<bool>(false);
        auto isGapDown = std::make_shared<bool>(false);
        auto isSubmitDown = std::make_shared<bool>(false);
        view->context->SetKeyHandler(
            [
                impl = this,
                context,
                hasFeature,
                hasContext,
                role,
                request,
                isCropDown,
                isPlaneDown,
                isGapDown,
                isSubmitDown
            ](const InteractionEvent& event) {
                return impl->SendHotkey(
                    event,
                    context,
                    hasFeature,
                    hasContext,
                    role,
                    request,
                    *isCropDown,
                    *isPlaneDown,
                    *isGapDown,
                    *isSubmitDown);
            });
    }
    return true;
}

void HostCommandRouter::Impl::SetViewAdded(
    std::vector<const HostRenderViewRuntime*>& views,
    const HostRenderViewRuntime* view) const
{
    if (!view) {
        return;
    }
    const auto it = std::find(views.begin(), views.end(), view);
    if (it == views.end()) {
        views.push_back(view);
    }
}

void HostCommandRouter::Impl::SetViewsAdded(
    std::vector<const HostRenderViewRuntime*>& target,
    const std::vector<const HostRenderViewRuntime*>& source) const
{
    for (const auto* view : source) {
        SetViewAdded(target, view);
    }
}

bool HostCommandRouter::Impl::GetViewFound(
    const std::vector<const HostRenderViewRuntime*>& views,
    const HostRenderViewRuntime* view) const
{
    return std::find(views.begin(), views.end(), view) != views.end();
}

HostCommandRouter::HostCommandRouter(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews,
    std::shared_ptr<HostFeatureBindings> featureBindings)
    : m_impl(std::make_unique<HostCommandRouter::Impl>(
        core,
        renderViews,
        std::move(featureBindings)))
{
}

HostCommandRouter::~HostCommandRouter() = default;

bool HostCommandRouter::DispatchCommand(HostCommandRouterRequest request) const
{
    return m_impl && m_impl->DispatchCommand(std::move(request));
}
