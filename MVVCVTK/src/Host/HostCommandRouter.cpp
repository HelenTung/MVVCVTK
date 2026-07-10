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
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class HostCommandRouter::Impl final {
public:
    Impl(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews,
        std::shared_ptr<HostFeatureBindings> featureBindings);
    ~Impl();

    bool DispatchCommand(HostCommandRouterRequest request) const;

private:
    enum class HotkeyAction {
        None,
        Model,
        ExportVolume,
        ExportSlices,
        CropBox,
        CropPlane,
        GapSwitch,
        Exit,
        KeepPreview,
        RemovePreview,
        Submit,
        SubmitBlock
    };

    bool DispatchCrop(
        HostCommandKind command,
        const HostCropViewRequest& request,
        HostCropPreviewMode previewMode = HostCropPreviewMode::KeepInside) const;
    bool DispatchCrop(HostCommandKind command) const;
    bool DispatchGap(
        HostCommandKind command,
        const HostGapViewRequest& request) const;
    bool DispatchGap(HostCommandKind command) const;
    bool DispatchFeatureExit() const;
    bool LoadVolume(
        const InitialVolumeLoadConfig& initialVolume,
        std::function<void(bool isSuccess)> loadComplete) const;
    bool ExportData(
        const HostDataExportConfig& dataExportConfig,
        std::function<void(bool isSuccess)> onComplete) const;
    bool SetViewConfig(const HostViewConfig& viewConfig) const;
    bool GetHotkeyExitReady(
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext) const;
    bool AttachHotkeys(
        const HostContextInput& renderContextInput,
        const HostDataExportConfig& dataExportConfig,
        const HostCommandInputConfig& commandInput,
        const HostHotkeyBindings& hotkeys) const;
    InteractionResult OnHotkey(
        const InteractionEvent& event,
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext,
        HostRenderViewRole role,
        const HostDataExportConfig& dataExportConfig,
        const HostCommandInputConfig& commandInput,
        const HostHotkeyBindings& hotkeys,
        bool& isCropDown,
        bool& isPlaneDown,
        bool& isGapDown,
        bool& isSubmitDown) const;
    bool SendHotkeyPress(
        HotkeyAction action,
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext,
        HostRenderViewRole role,
        const HostDataExportConfig& dataExportConfig,
        const HostCommandInputConfig& commandInput,
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
    bool SendHotkeyCommand(
        HotkeyAction action,
        bool hasContext,
        HostRenderViewRole role,
        const HostDataExportConfig& dataExportConfig,
        const HostCommandInputConfig& commandInput) const;
    HotkeyAction GetHotkeyAction(
        const InteractionEvent& event,
        bool hasFeature,
        bool hasContext,
        const HostHotkeyBindings& hotkeys) const;
    bool AttachViews(
        std::vector<const HostRenderViewRuntime*>& target,
        const std::vector<const HostRenderViewRuntime*>& source) const;
    bool GetViewFound(
        const std::vector<const HostRenderViewRuntime*>& views,
        const HostRenderViewRuntime* view) const;
    bool GetCharKeyMatched(const InteractionEvent& event, char key) const;

    const HostCoreServices* m_core = nullptr;
    const HostRenderViewSet* m_renderViews = nullptr;
    std::weak_ptr<HostFeatureBindings> m_featureBindings;
    mutable std::vector<std::weak_ptr<StdRenderContext>> m_hotkeyContexts;
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
    // 输入 handler 捕获 Impl 地址，必须在 Impl 销毁前解除所有仍存活的回调。
    for (const auto& context : m_hotkeyContexts) {
        if (const auto lockedContext = context.lock()) {
            lockedContext->ClearInputHandler();
        }
    }
}

bool HostCommandRouter::Impl::GetCharKeyMatched(
    const InteractionEvent& event,
    char key) const
{
    if (key == 0) {
        return false;
    }

    const char upperKey = (key >= 'a' && key <= 'z')
        ? static_cast<char>(key - 'a' + 'A')
        : key;
    const std::string keySymbol(1, key);
    const std::string upperKeySymbol(1, upperKey);
    return event.keyCode == key
        || event.keyCode == upperKey
        || event.keySym == keySymbol
        || event.keySym == upperKeySymbol;
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
        return AttachHotkeys(
            request.renderContextInput,
            request.dataExportConfig,
            request.commandInput,
            request.hotkeys);
    case HostCommandKind::ViewConfig:
        return SetViewConfig(request.viewConfig);
    case HostCommandKind::CropStart:
    case HostCommandKind::CropBox:
    case HostCommandKind::CropPlane:
    case HostCommandKind::CropPreview:
    case HostCommandKind::CropApply:
        return DispatchCrop(
            request.command,
            request.cropViewRequest,
            request.cropPreviewMode);
    case HostCommandKind::CropExit:
        return DispatchCrop(request.command);
    case HostCommandKind::FeatureExit:
        return DispatchFeatureExit();
    case HostCommandKind::GapStart:
    case HostCommandKind::GapSwitch:
        return DispatchGap(
            request.command,
            request.gapViewRequest);
    case HostCommandKind::GapOverlay:
    case HostCommandKind::GapExit:
        return DispatchGap(request.command);
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

bool HostCommandRouter::Impl::DispatchCrop(
    HostCommandKind command,
    const HostCropViewRequest& request,
    HostCropPreviewMode previewMode) const
{
    const auto bindings = m_featureBindings.lock();
    if (!bindings) {
        std::cerr << "[Host] Feature command dispatch skipped: feature bindings are not ready." << std::endl;
        return false;
    }

    switch (command) {
    case HostCommandKind::CropStart:
        return bindings->StartCrop(request);
    case HostCommandKind::CropBox:
        return bindings->SwitchCropBox(request);
    case HostCommandKind::CropPlane:
        return bindings->SwitchCropPlane(request);
    case HostCommandKind::CropPreview:
        return bindings->SwitchCropView(
            request,
            previewMode);
    case HostCommandKind::CropApply:
        return bindings->SendCrop(request);
    default:
        return false;
    }
}

bool HostCommandRouter::Impl::DispatchCrop(HostCommandKind command) const
{
    const auto bindings = m_featureBindings.lock();
    if (!bindings) {
        std::cerr << "[Host] Feature command dispatch skipped: feature bindings are not ready." << std::endl;
        return false;
    }

    switch (command) {
    case HostCommandKind::CropExit:
        return bindings->ExitCrop();
    default:
        return false;
    }
}

bool HostCommandRouter::Impl::DispatchGap(
    HostCommandKind command,
    const HostGapViewRequest& request) const
{
    const auto bindings = m_featureBindings.lock();
    if (!bindings) {
        std::cerr << "[Host] Feature command dispatch skipped: feature bindings are not ready." << std::endl;
        return false;
    }

    switch (command) {
    case HostCommandKind::GapStart:
        return bindings->StartGapView(request);
    case HostCommandKind::GapSwitch:
        return bindings->SwitchGapView(request);
    default:
        return false;
    }
}

bool HostCommandRouter::Impl::DispatchGap(HostCommandKind command) const
{
    const auto bindings = m_featureBindings.lock();
    if (!bindings) {
        std::cerr << "[Host] Feature command dispatch skipped: feature bindings are not ready." << std::endl;
        return false;
    }

    switch (command) {
    case HostCommandKind::GapOverlay:
        return bindings->SwitchGapLayer();
    case HostCommandKind::GapExit:
        return bindings->ExitGapView();
    default:
        return false;
    }
}

bool HostCommandRouter::Impl::DispatchFeatureExit() const
{
    const auto bindings = m_featureBindings.lock();
    if (!bindings) {
        std::cerr << "[Host] Feature command dispatch skipped: feature bindings are not ready." << std::endl;
        return false;
    }
    return bindings->ExitFeature();
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

    if (dataExportConfig.sliceOutputDir.empty()) {
        std::cerr << "[Host] Slice image export skipped: output directory was not configured." << std::endl;
        return false;
    }

    const auto* exportView = m_renderViews->GetViewBySelector(
        dataExportConfig.sliceSourceViewId,
        dataExportConfig.isSliceRoleUsed,
        dataExportConfig.sliceSourceRole);

    if (!exportView || !exportView->service) {
        std::cerr << "[Host] Slice image export skipped: source render view is missing." << std::endl;
        return false;
    }
    if (!m_renderViews->GetRoleIsSliceView(exportView->config.role)) {
        std::cerr << "[Host] Slice image export skipped: source render view is not a slice view." << std::endl;
        return false;
    }

    exportView->service->ExportSlicesAsync(
        dataExportConfig.sliceOutputDir,
        dataExportConfig.sliceAngleDeg,
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

    const auto* targetView = m_renderViews->GetViewBySelector(
        viewConfig.viewId,
        viewConfig.isViewRoleUsed,
        viewConfig.viewRole);

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

InteractionResult HostCommandRouter::Impl::OnHotkey(
    const InteractionEvent& event,
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext,
    HostRenderViewRole role,
    const HostDataExportConfig& dataExportConfig,
    const HostCommandInputConfig& commandInput,
    const HostHotkeyBindings& hotkeys,
    bool& isCropDown,
    bool& isPlaneDown,
    bool& isGapDown,
    bool& isSubmitDown) const
{
    const HotkeyAction action = GetHotkeyAction(
        event,
        hasFeature,
        hasContext,
        hotkeys);
    const bool isManaged = action != HotkeyAction::None
        && (action != HotkeyAction::Exit
            || GetHotkeyExitReady(context, hasFeature, hasContext));

    if (event.vtkEventId == vtkCommand::CharEvent && isManaged) {
        return { true, true };
    }

    if (event.vtkEventId == vtkCommand::KeyPressEvent && isManaged) {
        if (SendHotkeyPress(
            action,
            context,
            hasFeature,
            hasContext,
            role,
            dataExportConfig,
            commandInput,
            isCropDown,
            isPlaneDown,
            isGapDown,
            isSubmitDown)) {
            return { true, true };
        }
        return { true, true };
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

HostCommandRouter::Impl::HotkeyAction HostCommandRouter::Impl::GetHotkeyAction(
    const InteractionEvent& event,
    bool hasFeature,
    bool hasContext,
    const HostHotkeyBindings& hotkeys) const
{
    if (hasContext && GetCharKeyMatched(event, hotkeys.modelSwitchKey)) {
        return HotkeyAction::Model;
    }
    if (hasContext && GetCharKeyMatched(event, hotkeys.saveTransformedDataKey)) {
        return HotkeyAction::ExportVolume;
    }
    if (hasContext && GetCharKeyMatched(event, hotkeys.saveSliceImagesKey)) {
        return HotkeyAction::ExportSlices;
    }
    if (hasFeature && GetCharKeyMatched(event, hotkeys.cropSwitchKey)) {
        return HotkeyAction::CropBox;
    }
    if (hasFeature && GetCharKeyMatched(event, hotkeys.planarSwitchKey)) {
        return HotkeyAction::CropPlane;
    }
    if (hasFeature && GetCharKeyMatched(event, hotkeys.gapSwitchKey)) {
        return HotkeyAction::GapSwitch;
    }
    if (!hotkeys.exitKeySym.empty() && event.keySym == hotkeys.exitKeySym) {
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

bool HostCommandRouter::Impl::SendHotkeyPress(
    HotkeyAction action,
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext,
    HostRenderViewRole role,
    const HostDataExportConfig& dataExportConfig,
    const HostCommandInputConfig& commandInput,
    bool& isCropDown,
    bool& isPlaneDown,
    bool& isGapDown,
    bool& isSubmitDown) const
{
    switch (action) {
    case HotkeyAction::Model: {
        auto renderContext = context.lock();
        if (!renderContext) {
            return false;
        }
        renderContext->SetToolMode(
            renderContext->GetToolMode() == ToolMode::ModelTransform
                ? ToolMode::Navigation
                : ToolMode::ModelTransform);
        return true;
    }
    case HotkeyAction::ExportVolume:
    case HotkeyAction::ExportSlices:
        return SendHotkeyCommand(
            action,
            hasContext,
            role,
            dataExportConfig,
            commandInput);
    case HotkeyAction::CropBox:
        if (isCropDown) {
            return false;
        }
        isCropDown = true;
        return SendHotkeyCommand(
            action,
            hasContext,
            role,
            dataExportConfig,
            commandInput);
    case HotkeyAction::CropPlane:
        if (isPlaneDown) {
            return false;
        }
        isPlaneDown = true;
        return SendHotkeyCommand(
            action,
            hasContext,
            role,
            dataExportConfig,
            commandInput);
    case HotkeyAction::GapSwitch:
        if (isGapDown) {
            return false;
        }
        isGapDown = true;
        return SendHotkeyCommand(
            action,
            hasContext,
            role,
            dataExportConfig,
            commandInput);
    case HotkeyAction::Exit:
        if (hasFeature && SendHotkeyCommand(
            HotkeyAction::Exit,
            hasContext,
            role,
            dataExportConfig,
            commandInput)) {
            return true;
        }
        if (!hasContext) {
            return false;
        }
        if (auto renderContext = context.lock()) {
            if (renderContext->GetToolMode() == ToolMode::ModelTransform) {
                renderContext->SetToolMode(ToolMode::Navigation);
                return true;
            }
        }
        return false;
    case HotkeyAction::KeepPreview:
    case HotkeyAction::RemovePreview:
        return SendHotkeyCommand(
            action,
            hasContext,
            role,
            dataExportConfig,
            commandInput);
    case HotkeyAction::Submit:
        if (isSubmitDown) {
            return false;
        }
        isSubmitDown = true;
        return SendHotkeyCommand(
            action,
            hasContext,
            role,
            dataExportConfig,
            commandInput);
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
    case HotkeyAction::GapSwitch:
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

bool HostCommandRouter::Impl::GetHotkeyExitReady(
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
    const auto renderContext = hasContext ? context.lock() : nullptr;
    const bool hasModelActive = renderContext
        && renderContext->GetToolMode() == ToolMode::ModelTransform;
    return hasFeatureActive || hasModelActive;
}

bool HostCommandRouter::Impl::SendHotkeyCommand(
    HotkeyAction action,
    bool hasContext,
    HostRenderViewRole role,
    const HostDataExportConfig& dataExportConfig,
    const HostCommandInputConfig& commandInput) const
{
    HostCommandRouterRequest request;

    switch (action) {
    case HotkeyAction::ExportVolume:
        request.command = HostCommandKind::Export;
        request.dataExportConfig = dataExportConfig;
        request.dataExportConfig.hasVolumeExport = true;
        request.dataExportConfig.hasSliceExport = false;
        break;
    case HotkeyAction::ExportSlices:
        if (!hasContext) {
            break;
        }
        request.command = HostCommandKind::Export;
        request.dataExportConfig = dataExportConfig;
        request.dataExportConfig.hasVolumeExport = false;
        request.dataExportConfig.hasSliceExport = true;
        if (request.dataExportConfig.sliceSourceViewId.empty()
            && !request.dataExportConfig.isSliceRoleUsed) {
            request.dataExportConfig.isSliceRoleUsed = true;
            request.dataExportConfig.sliceSourceRole = role;
        }
        break;
    case HotkeyAction::CropBox:
        request.command = HostCommandKind::CropBox;
        request.cropViewRequest = commandInput.cropViewRequest;
        break;
    case HotkeyAction::CropPlane:
        request.command = HostCommandKind::CropPlane;
        request.cropViewRequest = commandInput.cropViewRequest;
        break;
    case HotkeyAction::GapSwitch:
        request.command = HostCommandKind::GapSwitch;
        request.gapViewRequest = commandInput.gapViewRequest;
        break;
    case HotkeyAction::KeepPreview:
        request.command = HostCommandKind::CropPreview;
        request.cropViewRequest = commandInput.cropViewRequest;
        request.cropPreviewMode = HostCropPreviewMode::KeepInside;
        break;
    case HotkeyAction::RemovePreview:
        request.command = HostCommandKind::CropPreview;
        request.cropViewRequest = commandInput.cropViewRequest;
        request.cropPreviewMode = HostCropPreviewMode::RemoveInside;
        break;
    case HotkeyAction::Submit:
        request.command = HostCommandKind::CropApply;
        request.cropViewRequest = commandInput.cropViewRequest;
        break;
    case HotkeyAction::Exit:
        request.command = HostCommandKind::FeatureExit;
        break;
    case HotkeyAction::None:
    case HotkeyAction::Model:
    case HotkeyAction::SubmitBlock:
    default:
        break;
    }

    if (request.command == HostCommandKind::None) {
        return false;
    }
    return DispatchCommand(std::move(request));
}

bool HostCommandRouter::Impl::AttachHotkeys(
    const HostContextInput& renderContextInput,
    const HostDataExportConfig& dataExportConfig,
    const HostCommandInputConfig& commandInput,
    const HostHotkeyBindings& hotkeys) const
{
    if (!m_renderViews) {
        return false;
    }
    if (!commandInput.isHotkeyEnabled
        && !renderContextInput.isHotkeyEnabled) {
        return true;
    }

    std::vector<const HostRenderViewRuntime*> featureTargetViews;
    if (commandInput.isHotkeyEnabled) {
        featureTargetViews = m_renderViews->GetViewsByIdsAndRoles(
            commandInput.targetViewIds,
            commandInput.targetViewRoles);
        if (featureTargetViews.empty()) {
            std::cerr << "[Host] Standalone feature hotkeys skipped: no target render view was requested." << std::endl;
        }
    }

    std::vector<const HostRenderViewRuntime*> renderContextTargetViews;
    if (renderContextInput.isHotkeyEnabled) {
        renderContextTargetViews = m_renderViews->GetViewsByIdsAndRoles(
            renderContextInput.targetViewIds,
            renderContextInput.targetViewRoles);
        if (renderContextTargetViews.empty()) {
            std::cerr << "[Host] Standalone render context hotkeys skipped: no target render view was requested." << std::endl;
        }
    }

    std::vector<const HostRenderViewRuntime*> keyTargetViews;
    const bool hasFeatureView = AttachViews(keyTargetViews, featureTargetViews);
    const bool hasContextView = AttachViews(keyTargetViews, renderContextTargetViews);
    if (!hasFeatureView && !hasContextView) {
        return false;
    }

    for (const auto* view : keyTargetViews) {
        if (!view || !view->context) {
            continue;
        }

        const std::weak_ptr<StdRenderContext> context = view->context;
        m_hotkeyContexts.push_back(context);
        const bool hasFeature = GetViewFound(featureTargetViews, view);
        const bool hasContext = GetViewFound(renderContextTargetViews, view);
        const HostRenderViewRole role = view->config.role;
        view->context->SetInputHandler(
            [
                impl = this,
                context,
                hasFeature,
                hasContext,
                role,
                dataExportConfig,
                commandInput,
                hotkeys,
                isCropDown = false,
                isPlaneDown = false,
                isGapDown = false,
                isSubmitDown = false
            ](const InteractionEvent& event) mutable {
                return impl->OnHotkey(
                    event,
                    context,
                    hasFeature,
                    hasContext,
                    role,
                    dataExportConfig,
                    commandInput,
                    hotkeys,
                    isCropDown,
                    isPlaneDown,
                    isGapDown,
                    isSubmitDown);
            },
            std::vector<unsigned long>{
                vtkCommand::KeyPressEvent,
                vtkCommand::KeyReleaseEvent,
                vtkCommand::CharEvent
            });
    }
    return true;
}

bool HostCommandRouter::Impl::AttachViews(
    std::vector<const HostRenderViewRuntime*>& target,
    const std::vector<const HostRenderViewRuntime*>& source) const
{
    bool hasAttachedView = false;
    for (const auto* view : source) {
        if (!view || GetViewFound(target, view)) {
            continue;
        }
        target.push_back(view);
        hasAttachedView = true;
    }
    return hasAttachedView;
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
