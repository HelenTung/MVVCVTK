#include "Host/HostHotkeyRouter.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <algorithm>
#include <array>
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
        const HostRenderViewSet& renderViews,
        std::weak_ptr<HostFeatureBindings> featureBindings,
        std::weak_ptr<HostCommandRouter> commandRouter)
        : m_renderViews(&renderViews)
        , m_featureBindings(std::move(featureBindings))
        , m_commandRouter(std::move(commandRouter))
        , m_inputPort(*this)
    {
    }

    ~Impl()
    {
        ClearContexts();
    }

    bool AttachHotkeys(
        const HostHotkeyConfig& config,
        HostHotkeyTemplates templates);
    bool ClearHotkeys();
    HostInputPort& GetInputPort() { return m_inputPort; }

private:
    enum class HotkeyAction {
        None,
        Model,
        ExportVolume,
        ExportSlices,
        GapSwitch,
        Exit,
        Count
    };

    bool AttachInput(HostInputBinding binding);
    bool DetachInput(std::string_view featureId);
    bool AttachContexts();
    void ClearContexts();
    InteractionResult OnInput(
        const InteractionEvent& event,
        const HostRenderViewRuntime& view,
        bool hasCommand,
        bool hasContext);
    InteractionResult SendFeatureInput(
        const InteractionEvent& event,
        const HostRenderViewRuntime& view);
    HotkeyAction GetHotkeyAction(
        const InteractionEvent& event) const;
    bool SendCommand(
        HotkeyAction action,
        HostRenderViewRole role) const;
    bool GetCharMatched(
        const InteractionEvent& event,
        char key) const;
    bool GetTargetMatched(
        const HostViewTargets& targets,
        const HostRenderViewRuntime& view) const;
    bool GetViewFound(
        const std::vector<const HostRenderViewRuntime*>& views,
        const HostRenderViewRuntime* view) const;
    bool SetActionDown(HotkeyAction action, bool isDown);

    const HostRenderViewSet* m_renderViews = nullptr;
    std::weak_ptr<HostFeatureBindings> m_featureBindings;
    std::weak_ptr<HostCommandRouter> m_commandRouter;
    std::vector<HostInputBinding> m_inputBindings;
    std::vector<std::weak_ptr<StdRenderContext>> m_contexts;
    HostHotkeyConfig m_config;
    HostHotkeyTemplates m_templates;
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
    const HostRenderViewRuntime& view) const
{
    return std::find(
        targets.viewIds.begin(),
        targets.viewIds.end(),
        view.config.id) != targets.viewIds.end()
        || std::find(
            targets.viewRoles.begin(),
            targets.viewRoles.end(),
            view.config.role) != targets.viewRoles.end();
}

bool HostHotkeyRouter::Impl::GetViewFound(
    const std::vector<const HostRenderViewRuntime*>& views,
    const HostRenderViewRuntime* view) const
{
    return std::find(views.begin(), views.end(), view) != views.end();
}

HostHotkeyRouter::Impl::HotkeyAction
HostHotkeyRouter::Impl::GetHotkeyAction(
    const InteractionEvent& event) const
{
    if (GetCharMatched(event, m_config.modelSwitchKey)) {
        return HotkeyAction::Model;
    }
    if (GetCharMatched(event, m_config.saveTransformedDataKey)) {
        return HotkeyAction::ExportVolume;
    }
    if (GetCharMatched(event, m_config.saveSliceImagesKey)) {
        return HotkeyAction::ExportSlices;
    }
    if (GetCharMatched(event, m_config.gapSwitchKey)) {
        return HotkeyAction::GapSwitch;
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

bool HostHotkeyRouter::Impl::SendCommand(
    const HotkeyAction action,
    const HostRenderViewRole role) const
{
    const auto router = m_commandRouter.lock();
    if (!router) {
        return false;
    }

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
        request.payload = m_templates.volumeExportRequest;
        command = HostDataCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::ExportSlices: {
        auto value = m_templates.sliceExportRequest;
        if (value.sourceView.viewId.empty()
            && !value.sourceView.isViewRoleUsed) {
            value.sourceView = { "", true, role };
        }
        HostDataRequest request;
        request.action = HostDataAction::ExportSlices;
        request.payload = std::move(value);
        command = HostDataCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::GapSwitch: {
        HostGapRequest request;
        const auto bindings = m_featureBindings.lock();
        if (bindings && bindings->GetGapView()) {
            request.action = HostGapAction::Overlay;
            request.payload = std::monostate{};
        }
        else {
            request.action = HostGapAction::Start;
            request.payload = m_templates.gapStart;
        }
        command = HostGapCommand{ std::move(request), nullptr };
        break;
    }
    case HotkeyAction::Exit: {
        const auto bindings = m_featureBindings.lock();
        if (bindings && bindings->GetGapView()) {
            HostGapRequest request;
            request.action = HostGapAction::Exit;
            request.payload = std::monostate{};
            command = HostGapCommand{ std::move(request), nullptr };
        }
        else {
            HostToolRequest request;
            request.action = HostToolAction::Set;
            request.payload = HostToolSetRequest{
                HostViewTarget{ "", true, role },
                HostToolMode::Navigation };
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

InteractionResult HostHotkeyRouter::Impl::SendFeatureInput(
    const InteractionEvent& event,
    const HostRenderViewRuntime& view)
{
    InteractionResult result;
    for (const auto& binding : m_inputBindings) {
        if (!binding.onInput
            || !GetTargetMatched(binding.targetViews, view)) {
            continue;
        }
        const auto current = binding.onInput(event);
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
    const HostRenderViewRuntime& view,
    const bool hasCommand,
    const bool hasContext)
{
    const auto featureResult = SendFeatureInput(event, view);
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
    (void)SendCommand(action, view.config.role);
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

void HostHotkeyRouter::Impl::ClearContexts()
{
    for (const auto& context : m_contexts) {
        if (const auto value = context.lock()) {
            value->ClearInputHandler();
        }
    }
    m_contexts.clear();
    m_isDown.fill(false);
}

bool HostHotkeyRouter::Impl::AttachContexts()
{
    ClearContexts();
    if (!m_renderViews) {
        return false;
    }
    const auto commandViews = m_config.isCommandInputEnabled
        ? m_renderViews->GetViewsByTargets(m_config.commandInputViews)
        : std::vector<const HostRenderViewRuntime*>{};
    const auto contextViews = m_config.isContextInputEnabled
        ? m_renderViews->GetViewsByTargets(m_config.contextInputViews)
        : std::vector<const HostRenderViewRuntime*>{};
    std::vector<const HostRenderViewRuntime*> views = commandViews;
    for (const auto* view : contextViews) {
        if (!GetViewFound(views, view)) {
            views.push_back(view);
        }
    }
    for (const auto& binding : m_inputBindings) {
        for (const auto* view :
            m_renderViews->GetViewsByTargets(binding.targetViews)) {
            if (!GetViewFound(views, view)) {
                views.push_back(view);
            }
        }
    }
    if (views.empty()) {
        return !m_config.isCommandInputEnabled
            && !m_config.isContextInputEnabled
            && m_inputBindings.empty();
    }

    for (const auto* view : views) {
        if (!view || !view->context) {
            continue;
        }
        m_contexts.push_back(view->context);
        const bool hasCommand = GetViewFound(commandViews, view);
        const bool hasContext = GetViewFound(contextViews, view);
        view->context->SetInputHandler(
            [this, view, hasCommand, hasContext](
                const InteractionEvent& event) {
                return OnInput(
                    event, *view, hasCommand, hasContext);
            },
            { InteractionEventKind::KeyPress,
              InteractionEventKind::KeyRelease,
              InteractionEventKind::TextInput });
    }
    return !m_contexts.empty();
}

bool HostHotkeyRouter::Impl::AttachHotkeys(
    const HostHotkeyConfig& config,
    HostHotkeyTemplates templates)
{
    m_config = config;
    m_templates = std::move(templates);
    m_isConfigured = true;
    return AttachContexts();
}

bool HostHotkeyRouter::Impl::ClearHotkeys()
{
    ClearContexts();
    m_isConfigured = false;
    m_config = {};
    m_templates = {};
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
    const HostHotkeyConfig& config,
    HostHotkeyTemplates templates)
{
    return m_impl
        && m_impl->AttachHotkeys(config, std::move(templates));
}

bool HostHotkeyRouter::ClearHotkeys()
{
    return m_impl && m_impl->ClearHotkeys();
}

HostInputPort& HostHotkeyRouter::GetInputPort()
{
    return m_impl->GetInputPort();
}
