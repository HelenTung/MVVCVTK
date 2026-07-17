#include "Host/HostHotkeyRouter.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "Interaction/InteractionTypes.h"
#include "StdRenderContext.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// 热键状态只记录“某 action 是否按下”，用于把操作系统/VTK 的重复 KeyPress 压缩成一次命令边沿；
// 业务目标和参数全部来自 HostHotkeyConfig/Templates，不在输入层推断算法配置。
class HostHotkeyRouter::Impl final {
public:
    Impl(const HostRenderViewSet& renderViews,
        std::weak_ptr<HostFeatureBindings> featureBindings,
        std::weak_ptr<HostCommandRouter> commandRouter)
        : m_renderViews(&renderViews),
          m_featureBindings(std::move(featureBindings)),
          m_commandRouter(std::move(commandRouter)) {}
    ~Impl() { ClearHotkeys(); }

    bool AttachHotkeys(const HostHotkeyConfig& config, HostHotkeyTemplates templates);
    bool ClearHotkeys();

private:
    enum class HotkeyAction {
        // Model 只属于 context 工具输入；其余可执行项属于 command/feature 输入权限。
        None, Model, ExportVolume, ExportSlices, CropBox, CropPlane,
        GapSwitch, Exit, KeepPreview, RemovePreview, Submit, Count
    };

    InteractionResult OnHotkey(const InteractionEvent& event,
        HostRenderViewRole role, bool hasFeature, bool hasContext,
        const HostHotkeyConfig& config, const HostHotkeyTemplates& templates) const;
    HotkeyAction GetHotkeyAction(
        const InteractionEvent& event, const HostHotkeyConfig& config) const;
    bool GetCharMatched(const InteractionEvent& event, char key) const;
    bool SendCommand(HotkeyAction action, HostRenderViewRole role,
        const HostHotkeyTemplates& templates) const;
    bool SetActionDown(HotkeyAction action, bool isDown) const;
    bool GetViewFound(const std::vector<const HostRenderViewRuntime*>& views,
        const HostRenderViewRuntime* view) const;

    const HostRenderViewSet* m_renderViews = nullptr; // 非拥有；解析配置中的 id/role 目标。
    std::weak_ptr<HostFeatureBindings> m_featureBindings; // 查询 Crop/Gap 当前模式，不延长 feature 生命周期。
    std::weak_ptr<HostCommandRouter> m_commandRouter;     // 命令投递能力，失效时热键静默拒绝。
    std::vector<std::weak_ptr<StdRenderContext>> m_contexts; // 已安装 handler 的 context，用于对称清理。
    mutable std::array<bool, static_cast<std::size_t>(HotkeyAction::Count)> m_isDown{}; // action 级按下去重状态。
};

bool HostHotkeyRouter::Impl::GetCharMatched(
    const InteractionEvent& event, char key) const
{
    if (key == 0) return false;
    const char upper = key >= 'a' && key <= 'z'
        ? static_cast<char>(key - 'a' + 'A') : key;
    return event.keyCode == key || event.keyCode == upper
        || event.keySym == std::string(1, key)
        || event.keySym == std::string(1, upper);
}

HostHotkeyRouter::Impl::HotkeyAction HostHotkeyRouter::Impl::GetHotkeyAction(
    const InteractionEvent& event, const HostHotkeyConfig& config) const
{
    if (GetCharMatched(event, config.modelSwitchKey)) return HotkeyAction::Model;
    if (GetCharMatched(event, config.saveTransformedDataKey)) return HotkeyAction::ExportVolume;
    if (GetCharMatched(event, config.saveSliceImagesKey)) return HotkeyAction::ExportSlices;
    if (GetCharMatched(event, config.cropSwitchKey)) return HotkeyAction::CropBox;
    if (GetCharMatched(event, config.planarSwitchKey)) return HotkeyAction::CropPlane;
    if (GetCharMatched(event, config.gapSwitchKey)) return HotkeyAction::GapSwitch;
    if (!config.exitKeySym.empty() && event.keySym == config.exitKeySym) return HotkeyAction::Exit;
    if (GetCharMatched(event, config.keepInsidePreviewKey)) return HotkeyAction::KeepPreview;
    if (GetCharMatched(event, config.removeInsidePreviewKey)) return HotkeyAction::RemovePreview;
    if (GetCharMatched(event, config.submitKey)) return HotkeyAction::Submit;
    return HotkeyAction::None;
}

bool HostHotkeyRouter::Impl::SetActionDown(HotkeyAction action, bool isDown) const
{
    const auto index = static_cast<std::size_t>(action);
    if (action == HotkeyAction::None || index >= m_isDown.size()) return false;
    const bool wasDown = m_isDown[index];
    m_isDown[index] = isDown;
    return wasDown != isDown;
}

bool HostHotkeyRouter::Impl::SendCommand(
    HotkeyAction action, HostRenderViewRole role,
    const HostHotkeyTemplates& templates) const
{
    // 所有分支先锁定统一 router，随后只构造 Host 层 request：
    // A. Model：以触发窗口 role 为目标，切换 Navigation/ModelTransform。
    // B. Export：复用模板；切片模板未指定 source 时回退触发窗口 role。
    // C. Crop：Box/Plane/Submit 复用 target，Preview 额外写 KeepInside/RemoveInside。
    // D. Gap：已进入显示模式时切 overlay，否则用模板启动分析。
    // E. Exit：按 Crop -> Gap -> 当前窗口 Navigation 的优先级退出最内层模式。
    const auto router = m_commandRouter.lock();
    if (!router) return false;

    HostCommand command;
    switch (action) {
    case HotkeyAction::Model: {
        HostToolRequest request;
        request.action = HostToolAction::Switch;
        request.payload = HostToolSwitchRequest{
            HostViewTarget{ "", true, role } };
        command = HostToolCommand{ std::move(request) };
        break;
    }
    case HotkeyAction::ExportVolume: {
        HostDataRequest request;
        request.action = HostDataAction::ExportVolume;
        request.payload = templates.volumeExportRequest;
        command = HostDataCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::ExportSlices: {
        auto value = templates.sliceExportRequest;
        if (value.sourceView.viewId.empty() && !value.sourceView.isViewRoleUsed) {
            value.sourceView = { "", true, role };
        }
        HostDataRequest request;
        request.action = HostDataAction::ExportSlices;
        request.payload = std::move(value);
        command = HostDataCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::CropBox:
    case HotkeyAction::CropPlane:
    case HotkeyAction::Submit: {
        HostCropRequest request;
        request.action = action == HotkeyAction::CropBox ? HostCropAction::Box
            : action == HotkeyAction::CropPlane ? HostCropAction::Plane
            : HostCropAction::Submit;
        request.payload = templates.cropTarget;
        command = HostCropCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::KeepPreview:
    case HotkeyAction::RemovePreview: {
        HostCropRequest request;
        request.action = HostCropAction::Preview;
        request.payload = HostCropPreviewRequest{ templates.cropTarget,
            action == HotkeyAction::KeepPreview
                ? HostCropPreviewMode::KeepInside
                : HostCropPreviewMode::RemoveInside };
        command = HostCropCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::GapSwitch: {
        HostGapRequest request;
        const auto bindings = m_featureBindings.lock();
        if (bindings && bindings->GetGapView()) {
            request.action = HostGapAction::Overlay;
            request.payload = std::monostate{};
        } else {
            request.action = HostGapAction::Start;
            request.payload = templates.gapStart;
        }
        command = HostGapCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::Exit: {
        const auto bindings = m_featureBindings.lock();
        if (bindings && bindings->GetCropActive()) {
            HostCropRequest request;
            request.action = HostCropAction::Exit;
            request.payload = std::monostate{};
            command = HostCropCommand{ std::move(request), nullptr };
        } else if (bindings && bindings->GetGapView()) {
            HostGapRequest request;
            request.action = HostGapAction::Exit;
            request.payload = std::monostate{};
            command = HostGapCommand{ std::move(request), nullptr };
        } else {
            HostToolRequest request;
            request.action = HostToolAction::Set;
            request.payload = HostToolSetRequest{
                HostViewTarget{ "", true, role }, HostToolMode::Navigation };
            command = HostToolCommand{ std::move(request) };
        }
        break;
    }
    case HotkeyAction::None:
    case HotkeyAction::Count:
        return false;
    }
    return router->DispatchCommand(std::move(command));
}

InteractionResult HostHotkeyRouter::Impl::OnHotkey(
    const InteractionEvent& event, HostRenderViewRole role,
    bool hasFeature, bool hasContext, const HostHotkeyConfig& config,
    const HostHotkeyTemplates& templates) const
{
    // 1. 解析 action 并校验输入作用域：Model 需要 context 权限，其余 action 需要 command 权限。
    const HotkeyAction action = GetHotkeyAction(event, config);
    if (action == HotkeyAction::None) return {};
    if ((action == HotkeyAction::Model && !hasContext)
        || (action != HotkeyAction::Model && !hasFeature)) return {};

    // 2. TextInput 只消费底层默认行为；Release 清除按下态；其它事件不参与命令。
    if (event.eventKind == InteractionEventKind::TextInput) return { true, true };
    if (event.eventKind == InteractionEventKind::KeyRelease) {
        SetActionDown(action, false);
        return { true, true };
    }
    if (event.eventKind != InteractionEventKind::KeyPress) return {};
    // 3. Submit 额外要求 Ctrl；KeyPress 只有从 up->down 的首次边沿真正发命令，重复事件仍被消费。
    if (action == HotkeyAction::Submit && !event.isCtrlDown) return { true, true };
    if (!SetActionDown(action, true)) return { true, true };
    SendCommand(action, role, templates);
    return { true, true };
}

bool HostHotkeyRouter::Impl::GetViewFound(
    const std::vector<const HostRenderViewRuntime*>& views,
    const HostRenderViewRuntime* view) const
{
    return std::find(views.begin(), views.end(), view) != views.end();
}

bool HostHotkeyRouter::Impl::AttachHotkeys(
    const HostHotkeyConfig& config, HostHotkeyTemplates templates)
{
    // 1. 重绑前先卸载全部旧 handler；两个输入域都关闭时，清理成功即视为有效配置。
    ClearHotkeys();
    if (!m_renderViews) return false;
    if (!config.isContextInputEnabled && !config.isCommandInputEnabled) return true;

    // 2. command/context 两组目标独立解析，再按 runtime 指针去重合并；空目标不解释为全选。
    const auto featureViews = config.isCommandInputEnabled
        ? m_renderViews->GetViewsByTargets(config.commandInputViews)
        : std::vector<const HostRenderViewRuntime*>{};
    const auto contextViews = config.isContextInputEnabled
        ? m_renderViews->GetViewsByTargets(config.contextInputViews)
        : std::vector<const HostRenderViewRuntime*>{};
    std::vector<const HostRenderViewRuntime*> views = featureViews;
    for (const auto* view : contextViews) {
        if (!GetViewFound(views, view)) views.push_back(view);
    }
    if (views.empty()) return false;

    // 3. 每个 context 安装同一组语义键盘事件，但捕获各自 role 与两类权限快照。
    for (const auto* view : views) {
        if (!view || !view->context) continue;
        m_contexts.push_back(view->context);
        const bool hasFeature = GetViewFound(featureViews, view);
        const bool hasContext = GetViewFound(contextViews, view);
        const auto role = view->config.role;
        view->context->SetInputHandler(
            [impl = this, role, hasFeature, hasContext, config, templates](
                const InteractionEvent& event) {
                return impl->OnHotkey(
                    event, role, hasFeature, hasContext, config, templates);
            },
            { InteractionEventKind::KeyPress,
              InteractionEventKind::KeyRelease,
              InteractionEventKind::TextInput });
    }
    return true;
}

bool HostHotkeyRouter::Impl::ClearHotkeys()
{
    for (const auto& context : m_contexts) {
        if (const auto value = context.lock()) value->ClearInputHandler();
    }
    m_contexts.clear();
    m_isDown.fill(false);
    return true;
}

HostHotkeyRouter::HostHotkeyRouter(const HostRenderViewSet& renderViews,
    std::weak_ptr<HostFeatureBindings> featureBindings,
    std::weak_ptr<HostCommandRouter> commandRouter)
    : m_impl(std::make_unique<Impl>(renderViews,
        std::move(featureBindings), std::move(commandRouter))) {}

HostHotkeyRouter::~HostHotkeyRouter() = default;

bool HostHotkeyRouter::AttachHotkeys(
    const HostHotkeyConfig& config, HostHotkeyTemplates templates)
{
    return m_impl && m_impl->AttachHotkeys(config, std::move(templates));
}

bool HostHotkeyRouter::ClearHotkeys()
{
    return m_impl && m_impl->ClearHotkeys();
}
