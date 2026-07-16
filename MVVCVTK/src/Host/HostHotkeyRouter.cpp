#include "Host/HostHotkeyRouter.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "Interaction/InteractionTypes.h"
#include "StdRenderContext.h"

#include <vtkCommand.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class HostHotkeyRouter::Impl final {
public:
    Impl(
        const HostRenderViewSet& renderViews,
        std::weak_ptr<HostFeatureBindings> featureBindings,
        std::weak_ptr<HostCommandRouter> commandRouter);
    ~Impl();

    bool AttachHotkeys(
        const HostContextInput& renderContextInput,
        const HostDataExportConfig& dataExportConfig,
        const HostCommandInputConfig& commandInput,
        const HostHotkeyBindings& hotkeys);
    bool ClearHotkeys();

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
    bool GetHotkeyExitReady(
        const std::weak_ptr<StdRenderContext>& context,
        bool hasFeature,
        bool hasContext) const;
    bool GetHotkeyMatched(
        const InteractionEvent& event,
        const HostHotkeyBindings& hotkeys,
        HotkeyAction action) const;
    bool GetHotkeyDown(
        const InteractionEvent& event,
        const HostHotkeyBindings& hotkeys) const;
    bool SetHotkeyDown(HotkeyAction action, bool isDown) const;
    bool ClearHotkeyDown(
        const InteractionEvent& event,
        const HostHotkeyBindings& hotkeys) const;
    bool AttachViews(
        std::vector<const HostRenderViewRuntime*>& target,
        const std::vector<const HostRenderViewRuntime*>& source) const;
    bool GetViewFound(
        const std::vector<const HostRenderViewRuntime*>& views,
        const HostRenderViewRuntime* view) const;
    bool GetCharKeyMatched(const InteractionEvent& event, char key) const;

    const HostRenderViewSet* m_renderViews = nullptr;
    std::weak_ptr<HostFeatureBindings> m_featureBindings;
    std::weak_ptr<HostCommandRouter> m_commandRouter;
    // 记录已安装 handler 的 context；只用于重配/析构时撤销回调，不延长窗口生命周期。
    std::vector<std::weak_ptr<StdRenderContext>> m_hotkeyContexts;
    // 首次 KeyPress 置位，匹配的物理 KeyRelease 清零；repeat/Char 只消费，不重复分发。
    mutable std::array<bool, static_cast<std::size_t>(HotkeyAction::Count)> m_isHotkeyDown = {};
};

HostHotkeyRouter::Impl::Impl(
    const HostRenderViewSet& renderViews,
    std::weak_ptr<HostFeatureBindings> featureBindings,
    std::weak_ptr<HostCommandRouter> commandRouter)
    : m_renderViews(&renderViews)
    , m_featureBindings(std::move(featureBindings))
    , m_commandRouter(std::move(commandRouter))
{
}

HostHotkeyRouter::Impl::~Impl()
{
    ClearHotkeys();
}

bool HostHotkeyRouter::Impl::ClearHotkeys()
{
    // handler 捕获 Impl 地址；销毁或重配前必须先解除所有仍存活的回调。
    for (const auto& context : m_hotkeyContexts) {
        if (const auto lockedContext = context.lock()) {
            lockedContext->ClearInputHandler();
        }
    }
    m_hotkeyContexts.clear();
    m_isHotkeyDown.fill(false);
    return true;
}

bool HostHotkeyRouter::Impl::GetCharKeyMatched(
    const InteractionEvent& event,
    char key) const
{
    if (key == 0) {
        return false;
    }
    const char upperKey = (key >= 'a' && key <= 'z')
        ? static_cast<char>(key - 'a' + 'A')
        : key;
    return event.keyCode == key
        || event.keyCode == upperKey
        || event.keySym == std::string(1, key)
        || event.keySym == std::string(1, upperKey);
}

bool HostHotkeyRouter::Impl::GetHotkeyMatched(
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

bool HostHotkeyRouter::Impl::GetHotkeyDown(
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

bool HostHotkeyRouter::Impl::SetHotkeyDown(
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

bool HostHotkeyRouter::Impl::ClearHotkeyDown(
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

HostHotkeyRouter::Impl::HotkeyAction HostHotkeyRouter::Impl::GetHotkeyAction(
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

bool HostHotkeyRouter::Impl::GetHotkeyExitReady(
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext) const
{
    bool hasFeatureActive = false;
    if (hasFeature) {
        if (const auto bindings = m_featureBindings.lock()) {
            hasFeatureActive = bindings->GetCropActive() || bindings->GetGapView();
        }
    }
    const auto renderContext = hasContext ? context.lock() : nullptr;
    return hasFeatureActive
        || (renderContext && renderContext->GetToolMode() == ToolMode::ModelTransform);
}

InteractionResult HostHotkeyRouter::Impl::OnHotkey(
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
    const auto action = GetHotkeyAction(event, hasFeature, hasContext, hotkeys);
    const bool hasHotkeyDown = GetHotkeyDown(event, hotkeys);
    const bool isActionReady = action != HotkeyAction::None
        && (action != HotkeyAction::Exit
            || GetHotkeyExitReady(context, hasFeature, hasContext));
    // 已接管的物理键在 release 前持续消费 repeat/Char；fresh idle Escape 仍然放行。
    const bool isManaged = hasHotkeyDown || isActionReady;
    if (event.vtkEventId == vtkCommand::CharEvent && isManaged) {
        return { true, true };
    }
    if (event.vtkEventId == vtkCommand::KeyPressEvent && isManaged) {
        // 业务失败也不能把同一次物理按键交给后续 handler；消费与命令成功是两个维度。
        if (!hasHotkeyDown) {
            (void)SendHotkeyPress(
                action, context, hasFeature, hasContext, role,
                dataExportConfig, commandInput);
        }
        return { true, true };
    }
    return {};
}

bool HostHotkeyRouter::Impl::SendHotkeyPress(
    HotkeyAction action,
    const std::weak_ptr<StdRenderContext>& context,
    bool hasFeature,
    bool hasContext,
    HostRenderViewRole role,
    const HostDataExportConfig& dataExportConfig,
    const HostCommandInputConfig& commandInput) const
{
    // 所有 view 共享 action 级 down 状态，避免自动重复和跨 view press/release 分叉。
    if (!SetHotkeyDown(action, true)) {
        return false;
    }
    switch (action) {
    case HotkeyAction::Model: {
        const auto renderContext = context.lock();
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
        return SendHotkeyCommand(action, role, dataExportConfig, commandInput);
    case HotkeyAction::Exit:
        if (hasFeature && SendHotkeyCommand(action, role, dataExportConfig, commandInput)) {
            return true;
        }
        if (!hasContext) {
            return false;
        }
        if (const auto renderContext = context.lock()) {
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

bool HostHotkeyRouter::Impl::SendHotkeyCommand(
    HotkeyAction action,
    HostRenderViewRole role,
    const HostDataExportConfig& dataExportConfig,
    const HostCommandInputConfig& commandInput) const
{
    HostCommand command;
    switch (action) {
    case HotkeyAction::ExportVolume: {
        auto request = dataExportConfig;
        request.hasVolumeExport = true;
        request.hasSliceExport = false;
        command = HostExportCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::ExportSlices: {
        auto request = dataExportConfig;
        request.hasVolumeExport = false;
        request.hasSliceExport = true;
        if (request.sliceSourceViewId.empty() && !request.isSliceRoleUsed) {
            request.isSliceRoleUsed = true;
            request.sliceSourceRole = role;
        }
        command = HostExportCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::CropBox:
        command = HostFeatureCommand{ HostCropCommand{
            HostCropAction::Box, commandInput.cropViewRequest } };
        break;
    case HotkeyAction::CropPlane:
        command = HostFeatureCommand{ HostCropCommand{
            HostCropAction::Plane, commandInput.cropViewRequest } };
        break;
    case HotkeyAction::GapSwitch:
        command = HostFeatureCommand{ HostGapCommand{
            HostGapAction::Switch, commandInput.gapViewRequest } };
        break;
    case HotkeyAction::KeepPreview:
        command = HostFeatureCommand{ HostCropCommand{
            HostCropAction::Preview,
            commandInput.cropViewRequest,
            HostCropPreviewMode::KeepInside } };
        break;
    case HotkeyAction::RemovePreview:
        command = HostFeatureCommand{ HostCropCommand{
            HostCropAction::Preview,
            commandInput.cropViewRequest,
            HostCropPreviewMode::RemoveInside } };
        break;
    case HotkeyAction::Submit:
        command = HostFeatureCommand{ HostCropCommand{
            HostCropAction::Submit, commandInput.cropViewRequest } };
        break;
    case HotkeyAction::Exit:
        command = HostFeatureCommand{ HostExitCommand{} };
        break;
    case HotkeyAction::None:
    case HotkeyAction::Model:
    case HotkeyAction::SubmitBlock:
    case HotkeyAction::Count:
    default:
        return false;
    }
    const auto router = m_commandRouter.lock();
    return router && router->DispatchCommand(std::move(command));
}

bool HostHotkeyRouter::Impl::AttachViews(
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

bool HostHotkeyRouter::Impl::GetViewFound(
    const std::vector<const HostRenderViewRuntime*>& views,
    const HostRenderViewRuntime* view) const
{
    return std::find(views.begin(), views.end(), view) != views.end();
}

bool HostHotkeyRouter::Impl::AttachHotkeys(
    const HostContextInput& renderContextInput,
    const HostDataExportConfig& dataExportConfig,
    const HostCommandInputConfig& commandInput,
    const HostHotkeyBindings& hotkeys)
{
    // Attach 接收完整配置快照；重配前先撤销旧目标，避免旧 view 保留旁路 handler。
    ClearHotkeys();
    if (!m_renderViews) {
        return false;
    }
    if (!commandInput.isHotkeyEnabled && !renderContextInput.isHotkeyEnabled) {
        return true;
    }

    std::vector<const HostRenderViewRuntime*> featureViews;
    if (commandInput.isHotkeyEnabled) {
        featureViews = m_renderViews->GetViewsByIdsAndRoles(
            commandInput.targetViewIds, commandInput.targetViewRoles);
        if (featureViews.empty()) {
            std::cerr << "[Host] Standalone feature hotkeys skipped: no target render view was requested." << std::endl;
        }
    }
    std::vector<const HostRenderViewRuntime*> contextViews;
    if (renderContextInput.isHotkeyEnabled) {
        contextViews = m_renderViews->GetViewsByIdsAndRoles(
            renderContextInput.targetViewIds, renderContextInput.targetViewRoles);
        if (contextViews.empty()) {
            std::cerr << "[Host] Standalone render context hotkeys skipped: no target render view was requested." << std::endl;
        }
    }
    std::vector<const HostRenderViewRuntime*> targetViews;
    const bool hasFeatureView = AttachViews(targetViews, featureViews);
    const bool hasContextView = AttachViews(targetViews, contextViews);
    if (!hasFeatureView && !hasContextView) {
        return false;
    }

    for (const auto* view : targetViews) {
        if (!view || !view->context) {
            continue;
        }
        const std::weak_ptr<StdRenderContext> context = view->context;
        m_hotkeyContexts.push_back(context);
        const bool hasFeature = GetViewFound(featureViews, view);
        const bool hasContext = GetViewFound(contextViews, view);
        const auto role = view->config.role;
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
                    event, context, hasFeature, hasContext, role,
                    dataExportConfig, commandInput, hotkeys);
            },
            std::vector<unsigned long>{
                vtkCommand::KeyPressEvent,
                vtkCommand::KeyReleaseEvent,
                vtkCommand::CharEvent
            });
    }
    return true;
}

HostHotkeyRouter::HostHotkeyRouter(
    const HostRenderViewSet& renderViews,
    std::weak_ptr<HostFeatureBindings> featureBindings,
    std::weak_ptr<HostCommandRouter> commandRouter)
    : m_impl(std::make_unique<Impl>(
        renderViews,
        std::move(featureBindings),
        std::move(commandRouter)))
{
}

HostHotkeyRouter::~HostHotkeyRouter() = default;

bool HostHotkeyRouter::AttachHotkeys(
    const HostContextInput& renderContextInput,
    const HostDataExportConfig& dataExportConfig,
    const HostCommandInputConfig& commandInput,
    const HostHotkeyBindings& hotkeys)
{
    return m_impl->AttachHotkeys(
        renderContextInput,
        dataExportConfig,
        commandInput,
        hotkeys);
}

bool HostHotkeyRouter::ClearHotkeys()
{
    return m_impl->ClearHotkeys();
}
