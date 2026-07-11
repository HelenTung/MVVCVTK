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
#include <array>
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
    // 一个 action 对应一个逻辑按键状态，而不是一个窗口状态；同一物理键跨 view
    // press/release 时仍命中同一槽。Count 只用于固定数组长度，不参与命令分发。
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
        SubmitBlock,
        Count
    };

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
        const HostHotkeyBindings& hotkeys) const;
    bool SendHotkeyPress(
        HotkeyAction action,
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext,
        HostRenderViewRole role,
        const HostDataExportConfig& dataExportConfig,
        const HostCommandInputConfig& commandInput) const;
    bool SetHotkeyDown(
        HotkeyAction action,
        bool isDown) const;
    bool ClearHotkeyDown(
        const InteractionEvent& event,
        const HostHotkeyBindings& hotkeys) const;
    bool ClearHotkeys() const;
    bool SendHotkeyCommand(
        HotkeyAction action,
        HostRenderViewRole role,
        const HostDataExportConfig& dataExportConfig,
        const HostCommandInputConfig& commandInput) const;
    HotkeyAction GetHotkeyAction(
        const InteractionEvent& event,
        bool hasFeature,
        bool hasContext,
        const HostHotkeyBindings& hotkeys) const;
    bool GetHotkeyMatched(
        const InteractionEvent& event,
        const HostHotkeyBindings& hotkeys,
        HotkeyAction action) const;
    bool GetHotkeyDown(
        const InteractionEvent& event,
        const HostHotkeyBindings& hotkeys) const;
    bool AttachViews(
        std::vector<const HostRenderViewRuntime*>& target,
        const std::vector<const HostRenderViewRuntime*>& source) const;
    bool GetViewFound(
        const std::vector<const HostRenderViewRuntime*>& views,
        const HostRenderViewRuntime* view) const;
    bool GetCharKeyMatched(const InteractionEvent& event, char key) const;

    // 非拥有引用：core 和 view set 由 session 持有，声明顺序保证它们晚于 router 析构。
    const HostCoreServices* m_core = nullptr;
    const HostRenderViewSet* m_renderViews = nullptr;
    // 弱引用避免 router 延长 feature 生命周期；命令入口必须允许绑定层已卸载时 lock 失败。
    std::weak_ptr<HostFeatureBindings> m_featureBindings;
    // 记录已安装 input handler 的 context，仅用于重配/析构时撤销回调，不延长窗口生命周期。
    mutable std::vector<std::weak_ptr<StdRenderContext>> m_hotkeyContexts;
    // 首次 KeyPress 置位，匹配的物理 KeyRelease 清零；repeat/Char 在此期间持续被同一 action 消费。
    mutable std::array<bool, static_cast<std::size_t>(HotkeyAction::Count)> m_isHotkeyDown = {};
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
    ClearHotkeys();
}

bool HostCommandRouter::Impl::ClearHotkeys() const
{
    // 输入 handler 捕获 Impl 地址；销毁或重新配置前必须先解除所有仍存活的回调。
    for (const auto& context : m_hotkeyContexts) {
        if (const auto lockedContext = context.lock()) {
            lockedContext->ClearInputHandler();
        }
    }
    m_hotkeyContexts.clear();
    m_isHotkeyDown.fill(false);
    return true;
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

bool HostCommandRouter::Impl::GetHotkeyMatched(
    const InteractionEvent& event,
    const HostHotkeyBindings& hotkeys,
    HotkeyAction action) const
{
    switch (action) {
    case HotkeyAction::Model:
        return GetCharKeyMatched(event, hotkeys.modelSwitchKey);
    case HotkeyAction::ExportVolume:
        return GetCharKeyMatched(event, hotkeys.saveTransformedDataKey);
    case HotkeyAction::ExportSlices:
        return GetCharKeyMatched(event, hotkeys.saveSliceImagesKey);
    case HotkeyAction::CropBox:
        return GetCharKeyMatched(event, hotkeys.cropSwitchKey);
    case HotkeyAction::CropPlane:
        return GetCharKeyMatched(event, hotkeys.planarSwitchKey);
    case HotkeyAction::GapSwitch:
        return GetCharKeyMatched(event, hotkeys.gapSwitchKey);
    case HotkeyAction::Exit:
        return !hotkeys.exitKeySym.empty() && event.keySym == hotkeys.exitKeySym;
    case HotkeyAction::KeepPreview:
        return GetCharKeyMatched(event, hotkeys.keepInsidePreviewKey);
    case HotkeyAction::RemovePreview:
        return GetCharKeyMatched(event, hotkeys.removeInsidePreviewKey);
    case HotkeyAction::Submit:
    case HotkeyAction::SubmitBlock:
        return GetCharKeyMatched(event, hotkeys.submitKey);
    case HotkeyAction::None:
    case HotkeyAction::Count:
    default:
        return false;
    }
}

bool HostCommandRouter::Impl::GetHotkeyDown(
    const InteractionEvent& event,
    const HostHotkeyBindings& hotkeys) const
{
    for (std::size_t index = 1;
        index < static_cast<std::size_t>(HotkeyAction::Count);
        ++index) {
        const auto action = static_cast<HotkeyAction>(index);
        const auto stateAction = action == HotkeyAction::SubmitBlock
            ? HotkeyAction::Submit
            : action;
        if (GetHotkeyMatched(event, hotkeys, action)
            && m_isHotkeyDown[static_cast<std::size_t>(stateAction)]) {
            return true;
        }
    }
    return false;
}

bool HostCommandRouter::Impl::ClearHotkeyDown(
    const InteractionEvent& event,
    const HostHotkeyBindings& hotkeys) const
{
    bool isCleared = false;
    for (std::size_t index = 1;
        index < static_cast<std::size_t>(HotkeyAction::Count);
        ++index) {
        const auto action = static_cast<HotkeyAction>(index);
        if (GetHotkeyMatched(event, hotkeys, action)
            && SetHotkeyDown(action, false)) {
            isCleared = true;
        }
    }
    return isCleared;
}

bool HostCommandRouter::Impl::DispatchCommand(HostCommandRouterRequest request) const
{
    // request 是按 command 解释的命令快照；只有对应分支的 payload 有效。
    // Load/Export 的 callback 在此移动给异步服务，feature 命令则先锁定弱引用再进入绑定层。
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
    case HostCommandKind::CropExit:
    case HostCommandKind::FeatureExit:
    case HostCommandKind::GapStart:
    case HostCommandKind::GapSwitch:
    case HostCommandKind::GapOverlay:
    case HostCommandKind::GapExit: {
        const auto bindings = m_featureBindings.lock();
        if (!bindings) {
            std::cerr << "[Host] Feature command dispatch skipped: feature bindings are not ready." << std::endl;
            return false;
        }

        switch (request.command) {
        case HostCommandKind::CropStart:
            return bindings->StartCrop(request.cropViewRequest);
        case HostCommandKind::CropBox:
            return bindings->SwitchCropBox(request.cropViewRequest);
        case HostCommandKind::CropPlane:
            return bindings->SwitchCropPlane(request.cropViewRequest);
        case HostCommandKind::CropPreview:
            return bindings->SwitchCropView(
                request.cropViewRequest,
                request.cropPreviewMode);
        case HostCommandKind::CropApply:
            return bindings->SendCrop(request.cropViewRequest);
        case HostCommandKind::CropExit:
            return bindings->ExitCrop();
        case HostCommandKind::FeatureExit:
            return bindings->ExitFeature();
        case HostCommandKind::GapStart:
            return bindings->StartGapView(request.gapViewRequest);
        case HostCommandKind::GapSwitch:
            return bindings->SwitchGapView(request.gapViewRequest);
        case HostCommandKind::GapOverlay:
            return bindings->SwitchGapLayer();
        case HostCommandKind::GapExit:
            return bindings->ExitGapView();
        default:
            return false;
        }
    }
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
    // optional 字段组成一次命令快照；先完成目标和输入校验，保证拒绝路径没有部分写入。
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
        // mode 同时属于 service 管线意图和 context 输入/相机策略；必须先确认两个 owner 都可写。
        if (!targetView->context) {
            std::cerr << "[Host] View mode config skipped: render context is missing." << std::endl;
            return false;
        }
        service->SetVizMode(*viewConfig.mode);
        targetView->context->SetCameraStyle(*viewConfig.mode);
    }
    // 其余 optional 字段只有 service 一个 owner，可按请求中实际携带的值独立分发。
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
    return true; // 配置已分发；service 的实际管线同步由后续主线程 tick 消费。
}

InteractionResult HostCommandRouter::Impl::OnHotkey(
    const InteractionEvent& event,
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext,
    HostRenderViewRole role,
    const HostDataExportConfig& dataExportConfig,
    const HostCommandInputConfig& commandInput,
    const HostHotkeyBindings& hotkeys) const
{
    // release 不依赖当前 view 的 feature/context 能力；按物理键清理所有匹配 action，避免跨窗口状态卡住。
    if (event.vtkEventId == vtkCommand::KeyReleaseEvent) {
        return ClearHotkeyDown(event, hotkeys)
            ? InteractionResult{ true, true }
            : InteractionResult{};
    }

    const HotkeyAction action = GetHotkeyAction(
        event,
        hasFeature,
        hasContext,
        hotkeys);
    const bool hasHotkeyDown = GetHotkeyDown(event, hotkeys);
    const bool isActionReady = action != HotkeyAction::None
        && (action != HotkeyAction::Exit
            || GetHotkeyExitReady(context, hasFeature, hasContext));
    // 已经接管的物理按键在 release 前持续消费 repeat KeyPress / Char；fresh idle Escape 仍然放行。
    const bool isManaged = hasHotkeyDown || isActionReady;

    if (event.vtkEventId == vtkCommand::CharEvent && isManaged) {
        return { true, true };
    }

    if (event.vtkEventId == vtkCommand::KeyPressEvent && isManaged) {
        // 业务执行失败也不能把同一次物理按键交给后续 handler；按键是否消费与命令是否成功是两个维度。
        if (!hasHotkeyDown) {
            (void)SendHotkeyPress(
                action,
                context,
                hasFeature,
                hasContext,
                role,
                dataExportConfig,
                commandInput);
        }
        return { true, true };
    }

    return {};
}

HostCommandRouter::Impl::HotkeyAction HostCommandRouter::Impl::GetHotkeyAction(
    const InteractionEvent& event,
    bool hasFeature,
    bool hasContext,
    const HostHotkeyBindings& hotkeys) const
{
    if (hasContext && GetHotkeyMatched(event, hotkeys, HotkeyAction::Model)) {
        return HotkeyAction::Model;
    }
    if (hasContext && GetHotkeyMatched(event, hotkeys, HotkeyAction::ExportVolume)) {
        return HotkeyAction::ExportVolume;
    }
    if (hasContext && GetHotkeyMatched(event, hotkeys, HotkeyAction::ExportSlices)) {
        return HotkeyAction::ExportSlices;
    }
    if (hasFeature && GetHotkeyMatched(event, hotkeys, HotkeyAction::CropBox)) {
        return HotkeyAction::CropBox;
    }
    if (hasFeature && GetHotkeyMatched(event, hotkeys, HotkeyAction::CropPlane)) {
        return HotkeyAction::CropPlane;
    }
    if (hasFeature && GetHotkeyMatched(event, hotkeys, HotkeyAction::GapSwitch)) {
        return HotkeyAction::GapSwitch;
    }
    if (GetHotkeyMatched(event, hotkeys, HotkeyAction::Exit)) {
        return HotkeyAction::Exit;
    }
    if (hasFeature && GetHotkeyMatched(event, hotkeys, HotkeyAction::KeepPreview)) {
        return HotkeyAction::KeepPreview;
    }
    if (hasFeature && GetHotkeyMatched(event, hotkeys, HotkeyAction::RemovePreview)) {
        return HotkeyAction::RemovePreview;
    }
    if (hasFeature && GetHotkeyMatched(event, hotkeys, HotkeyAction::Submit)) {
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
    const HostCommandInputConfig& commandInput) const
{
    // 所有 action 共享 router 级 down 状态，避免按键自动重复和跨 view press/release 分叉。
    if (!SetHotkeyDown(action, true)) {
        return false;
    }

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
    case HotkeyAction::CropBox:
    case HotkeyAction::CropPlane:
    case HotkeyAction::GapSwitch:
    case HotkeyAction::KeepPreview:
    case HotkeyAction::RemovePreview:
    case HotkeyAction::Submit:
        return SendHotkeyCommand(
            action,
            role,
            dataExportConfig,
            commandInput);
    case HotkeyAction::Exit:
        if (hasFeature && SendHotkeyCommand(
            HotkeyAction::Exit,
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
    case HotkeyAction::SubmitBlock:
        return true;
    case HotkeyAction::None:
    case HotkeyAction::Count:
    default:
        return false;
    }
}

bool HostCommandRouter::Impl::SetHotkeyDown(
    HotkeyAction action,
    bool isDown) const
{
    // Submit 与 SubmitBlock 来自同一物理键；release 时 Ctrl 可能已先松开，因此必须共享状态槽。
    if (action == HotkeyAction::SubmitBlock) {
        action = HotkeyAction::Submit;
    }
    if (action == HotkeyAction::None || action == HotkeyAction::Count) {
        return false;
    }

    bool& isActionDown = m_isHotkeyDown[static_cast<std::size_t>(action)];
    if (isActionDown == isDown) {
        return false;
    }
    isActionDown = isDown;
    return true;
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
    case HotkeyAction::Count:
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
    // Hotkeys request 是完整配置快照；重复分发时先撤销旧目标，避免旧 view 继续保留旁路 handler。
    ClearHotkeys();
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
                hotkeys
            ](const InteractionEvent& event) {
                return impl->OnHotkey(
                    event,
                    context,
                    hasFeature,
                    hasContext,
                    role,
                    dataExportConfig,
                    commandInput,
                    hotkeys);
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
