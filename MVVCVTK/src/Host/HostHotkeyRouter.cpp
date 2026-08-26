#include "Host/HostHotkeyRouter.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostFeature.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/Types/HostRequestTypes.h"
#include "Interaction/AbstractViewContext.h"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class HostHotkeyRouter::Impl final {
public:
    class InputPort final : public HostInputPort {
    public:
        explicit InputPort(Impl& owner)
            : m_owner(owner)
        {
        }

        bool AttachInput(HostInputBinding binding) override
        {
            return m_owner.AttachInput(std::move(binding));
        }

        bool DetachInput(const std::string_view featureId) override
        {
            return m_owner.DetachInput(featureId);
        }

    private:
        Impl& m_owner;
    };

    Impl(
        std::weak_ptr<IHostViewDirectory> directory,
        std::weak_ptr<HostCommandRouter> commandRouter)
        : m_directory(std::move(directory))
        , m_commandRouter(std::move(commandRouter))
        , m_inputPort(*this)
    {
    }

    ~Impl()
    {
        // 外壳仅在 ClearHotkeys 成功后删除 Impl。
    }

    bool AttachHotkeys(const HostHotkeyConfig& config);
    bool ClearHotkeys();
    HostInputPort& GetInputPort() { return m_inputPort; }

private:
    enum class HotkeyAction {
        None,
        Model,
        ExportData,
        ExportSlices,
        Exit,
        Count
    };

    bool AttachInput(HostInputBinding binding);
    bool DetachInput(std::string_view featureId);
    bool AttachContexts();
    bool ClearContexts();
    InteractionResult OnInput(
        const InteractionEvent& event,
        const std::string& viewId,
        HostRenderViewRole role,
        bool hasCommand,
        bool hasContext);
    InteractionResult SendFeatureInput(
        const InteractionEvent& event,
        const std::string& viewId,
        HostRenderViewRole role);
    HotkeyAction GetHotkeyAction(
        const InteractionEvent& event) const;
    bool SendRequest(
        HotkeyAction action,
        HostRenderViewRole role) const;
    bool GetCharMatched(
        const InteractionEvent& event,
        char key) const;
    bool GetTargetMatched(
        const HostViewTargets& targets,
        const std::string& viewId,
        HostRenderViewRole role) const;
    bool GetRouteFound(
        const std::vector<HostInputRoute>& routes,
        const HostInputRoute& route) const;
    bool SetActionDown(HotkeyAction action, bool isDown);

    std::weak_ptr<IHostViewDirectory> m_directory;
    std::weak_ptr<HostCommandRouter> m_commandRouter;
    std::vector<HostInputBinding> m_inputBindings;
    std::vector<std::weak_ptr<AbstractViewContext>> m_contexts;
    HostHotkeyConfig m_config;
    InputPort m_inputPort;
    std::array<bool, static_cast<std::size_t>(HotkeyAction::Count)>
        m_isDown{};
    bool m_isConfigured = false;
};

bool HostHotkeyRouter::Impl::GetCharMatched(
    const InteractionEvent& event,
    const char key) const
{
    if (key == 0) {
        return false;
    }
    const char upper = key >= 'a' && key <= 'z'
        ? static_cast<char>(key - 'a' + 'A')
        : key;
    return event.keyCode == key
        || event.keyCode == upper
        || event.keySym == std::string(1, key)
        || event.keySym == std::string(1, upper);
}

bool HostHotkeyRouter::Impl::GetTargetMatched(
    const HostViewTargets& targets,
    const std::string& viewId,
    const HostRenderViewRole role) const
{
    return std::find(
        targets.viewIds.begin(),
        targets.viewIds.end(),
        viewId) != targets.viewIds.end()
        || std::find(
            targets.viewRoles.begin(),
            targets.viewRoles.end(),
            role) != targets.viewRoles.end();
}

bool HostHotkeyRouter::Impl::GetRouteFound(
    const std::vector<HostInputRoute>& routes,
    const HostInputRoute& route) const
{
    return std::find_if(
        routes.begin(), routes.end(),
        [&route](const HostInputRoute& current) {
            const auto currentContext = current.context.lock();
            const auto routeContext = route.context.lock();
            return currentContext
                && currentContext == routeContext;
        }) != routes.end();
}

HostHotkeyRouter::Impl::HotkeyAction
HostHotkeyRouter::Impl::GetHotkeyAction(
    const InteractionEvent& event) const
{
    if (GetCharMatched(event, m_config.modelSwitchKey)) {
        return HotkeyAction::Model;
    }
    if (GetCharMatched(event, m_config.dataExportKey)) {
        return HotkeyAction::ExportData;
    }
    if (GetCharMatched(event, m_config.sliceExportKey)) {
        return HotkeyAction::ExportSlices;
    }
    if (!m_config.exitKeySym.empty()
        && event.keySym == m_config.exitKeySym) {
        return HotkeyAction::Exit;
    }
    return HotkeyAction::None;
}

bool HostHotkeyRouter::Impl::SetActionDown(
    const HotkeyAction action,
    const bool isDown)
{
    const auto index = static_cast<std::size_t>(action);
    if (action == HotkeyAction::None || index >= m_isDown.size()) {
        return false;
    }
    const bool wasDown = m_isDown[index];
    m_isDown[index] = isDown;
    return wasDown != isDown;
}

bool HostHotkeyRouter::Impl::SendRequest(
    const HotkeyAction action,
    const HostRenderViewRole role) const
{
    const auto router = m_commandRouter.lock();
    if (!router) {
        return false;
    }

    switch (action) {
    case HotkeyAction::Model: {
        HostToolSwitchRequest request;
        request.targetView = { "", true, role };
        return router->Dispatch(std::move(request));
    }
    case HotkeyAction::ExportData: {
        HostDataExportRequest request;
        request.outputPath = m_config.dataExportPath.empty()
            ? "." : m_config.dataExportPath;
        request.format = m_config.dataExportFormat;
        request.sourceView = m_config.dataSourceView;
        // 显式格式已经决定底层 writer；只有缺省格式才需要触发窗口参与模式推断。
        if (!request.format
            && request.sourceView.viewId.empty()
            && !request.sourceView.isViewRoleUsed) {
            request.sourceView = { "", true, role };
        }
        return router->Dispatch(std::move(request));
    }
    case HotkeyAction::ExportSlices: {
        HostSliceExportRequest request;
        request.outputDir = m_config.sliceExportDir.empty()
            ? "." : m_config.sliceExportDir;
        request.sourceView = m_config.sliceSourceView;
        request.angleDeg = m_config.sliceAngleDeg;
        if (request.sourceView.viewId.empty()
            && !request.sourceView.isViewRoleUsed) {
            request.sourceView = { "", true, role };
        }
        return router->Dispatch(std::move(request));
    }
    case HotkeyAction::Exit: {
        HostToolSetRequest request;
        request.targetView = { "", true, role };
        request.toolMode = HostToolMode::Navigation;
        return router->Dispatch(std::move(request));
    }
    case HotkeyAction::None:
    case HotkeyAction::Count:
        return false;
    }
    return false;
}

InteractionResult HostHotkeyRouter::Impl::SendFeatureInput(
    const InteractionEvent& event,
    const std::string& viewId,
    const HostRenderViewRole role)
{
    InteractionResult result;
    for (const auto& binding : m_inputBindings) {
        if (!binding.onInput
            || !GetTargetMatched(
                binding.targetViews, viewId, role)) {
            continue;
        }
        InteractionResult current;
        try {
            current = binding.onInput(event);
        }
        catch (...) {
            std::cerr
                << "[Host] Feature input failed: "
                << binding.featureId << '\n';
            continue;
        }
        result.isHandled = result.isHandled || current.isHandled;
        result.isPropagationStopped =
            result.isPropagationStopped
            || current.isPropagationStopped;
        if (result.isPropagationStopped) {
            break;
        }
    }
    return result;
}

InteractionResult HostHotkeyRouter::Impl::OnInput(
    const InteractionEvent& event,
    const std::string& viewId,
    const HostRenderViewRole role,
    const bool hasCommand,
    const bool hasContext)
{
    const auto featureResult = SendFeatureInput(
        event, viewId, role);
    if (featureResult.isPropagationStopped) {
        return featureResult;
    }

    const auto action = GetHotkeyAction(event);
    if (action == HotkeyAction::None) {
        return featureResult;
    }
    const bool hasPermission = action == HotkeyAction::Model
        ? hasContext : hasCommand;
    if (!hasPermission) {
        return featureResult;
    }
    if (event.eventKind == InteractionEventKind::KeyRelease) {
        (void)SetActionDown(action, false);
        return { true, true };
    }
    if (event.eventKind == InteractionEventKind::TextInput) {
        return { true, true };
    }
    if (event.eventKind != InteractionEventKind::KeyPress) {
        return featureResult;
    }
    if (!SetActionDown(action, true)) {
        return { true, true };
    }
    (void)SendRequest(action, role);
    return { true, true };
}

bool HostHotkeyRouter::Impl::AttachInput(HostInputBinding binding)
{
    if (binding.featureId.empty()
        || !binding.onInput
        || (binding.targetViews.viewIds.empty()
            && binding.targetViews.viewRoles.empty())) {
        return false;
    }
    const auto duplicate = std::find_if(
        m_inputBindings.begin(),
        m_inputBindings.end(),
        [&binding](const HostInputBinding& current) {
            return current.featureId == binding.featureId;
        });
    if (duplicate != m_inputBindings.end()) {
        return false;
    }
    m_inputBindings.push_back(std::move(binding));
    if (!m_isConfigured || AttachContexts()) {
        return true;
    }
    m_inputBindings.pop_back();
    (void)AttachContexts();
    return false;
}

bool HostHotkeyRouter::Impl::DetachInput(
    const std::string_view featureId)
{
    const auto binding = std::find_if(
        m_inputBindings.begin(),
        m_inputBindings.end(),
        [featureId](const HostInputBinding& current) {
            return current.featureId == featureId;
        });
    if (binding == m_inputBindings.end()) {
        return false;
    }
    const auto bindingIndex = std::distance(
        m_inputBindings.begin(), binding);
    auto removed = std::move(*binding);
    m_inputBindings.erase(binding);
    if (!m_isConfigured || AttachContexts()) {
        return true;
    }
    m_inputBindings.insert(
        m_inputBindings.begin() + bindingIndex,
        std::move(removed));
    (void)AttachContexts();
    return false;
}

bool HostHotkeyRouter::Impl::ClearContexts()
{
    bool isCleared = true;
    for (const auto& context : m_contexts) {
        if (const auto value = context.lock()) {
            if (!value->ClearInputHandler()) {
                isCleared = false;
            }
        }
    }
    // 任一 context 拒绝时保留完整弱集合供 owner thread 重试。
    if (!isCleared) {
        return false;
    }
    m_contexts.clear();
    m_isDown.fill(false);
    return true;
}

bool HostHotkeyRouter::Impl::AttachContexts()
{
    if (!ClearContexts()) {
        return false;
    }
    const auto directory = m_directory.lock();
    if (!directory) {
        return false;
    }
    const auto commandViews = m_config.isCommandInputEnabled
        ? directory->GetInputRoutes(m_config.commandInputViews)
        : std::vector<HostInputRoute>{};
    const auto contextViews = m_config.isContextInputEnabled
        ? directory->GetInputRoutes(m_config.contextInputViews)
        : std::vector<HostInputRoute>{};
    std::vector<HostInputRoute> views = commandViews;
    for (const auto& view : contextViews) {
        if (!GetRouteFound(views, view)) {
            views.push_back(view);
        }
    }
    for (const auto& binding : m_inputBindings) {
        for (const auto& view :
            directory->GetInputRoutes(binding.targetViews)) {
            if (!GetRouteFound(views, view)) {
                views.push_back(view);
            }
        }
    }
    if (views.empty()) {
        return !m_config.isCommandInputEnabled
            && !m_config.isContextInputEnabled
            && m_inputBindings.empty();
    }

    for (const auto& view : views) {
        const auto context = view.context.lock();
        if (!context) continue;
        const bool hasCommand = GetRouteFound(commandViews, view);
        const bool hasContext = GetRouteFound(contextViews, view);
        const auto viewId = view.id;
        const auto role = view.role;
        if (!context->SetInputHandler(
            [this, viewId, role, hasCommand, hasContext](
                const InteractionEvent& event) {
                return OnInput(
                    event,
                    viewId,
                    role,
                    hasCommand,
                    hasContext);
            },
            { InteractionEventKind::KeyPress,
              InteractionEventKind::KeyRelease,
              InteractionEventKind::TextInput })) {
            (void)ClearContexts();
            return false;
        }
        m_contexts.push_back(context);
    }
    return !m_contexts.empty();
}

bool HostHotkeyRouter::Impl::AttachHotkeys(
    const HostHotkeyConfig& config)
{
    const auto oldConfig = m_config;
    const bool wasConfigured = m_isConfigured;
    m_config = config;
    m_isConfigured = true;
    if (AttachContexts()) {
        return true;
    }

    m_config = oldConfig;
    m_isConfigured = wasConfigured;
    (void)AttachContexts();
    return false;
}

bool HostHotkeyRouter::Impl::ClearHotkeys()
{
    if (!ClearContexts()) {
        return false;
    }
    m_isConfigured = false;
    m_config = {};
    return true;
}

HostHotkeyRouter::HostHotkeyRouter(
    std::weak_ptr<IHostViewDirectory> directory,
    std::weak_ptr<HostCommandRouter> commandRouter)
    : m_impl(std::make_unique<Impl>(
        std::move(directory),
        std::move(commandRouter)))
{
}

HostHotkeyRouter::~HostHotkeyRouter()
{
    if (m_impl && !m_impl->ClearHotkeys()) {
        std::cerr
            << "[Host] Hotkey router destroyed before owner-thread cleanup.\n";
    }
}

bool HostHotkeyRouter::AttachHotkeys(
    const HostHotkeyConfig& config)
{
    return m_impl && m_impl->AttachHotkeys(config);
}

bool HostHotkeyRouter::ClearHotkeys()
{
    return m_impl && m_impl->ClearHotkeys();
}

HostInputPort& HostHotkeyRouter::GetInputPort()
{
    return m_impl->GetInputPort();
}
