#include "Host/HostHotkeyRouter.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostInputRegistry.h"
#include "Host/Types/HostRequestTypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

class HostHotkeyRouter::Impl final
    : public std::enable_shared_from_this<HostHotkeyRouter::Impl>
{
public:
    Impl(
        HostInputRegistry& inputRegistry,
        std::weak_ptr<HostCommandRouter> commandRouter)
        : m_inputRegistry(inputRegistry)
        , m_commandRouter(std::move(commandRouter))
    {
    }

    bool AttachHotkeys(const HostHotkeyConfig& config);
    bool ClearHotkeys();

private:
    enum class HotkeyAction {
        None,
        Model,
        ExportData,
        ExportSlices,
        Exit,
        Count
    };

    InteractionResult OnInput(
        const InteractionEvent& event,
        const std::string& viewId,
        HostRenderViewRole role);
    HotkeyAction GetHotkeyAction(const InteractionEvent& event) const;
    bool SendRequest(HotkeyAction action, HostRenderViewRole role) const;
    bool GetCharMatched(const InteractionEvent& event, char key) const;
    bool GetTargetMatched(
        const HostViewTargets& targets,
        const std::string& viewId,
        HostRenderViewRole role) const;
    bool SetActionDown(HotkeyAction action, bool isDown);
    static HostViewTargets GetTargets(const HostHotkeyConfig& config);

    HostInputRegistry& m_inputRegistry;
    std::weak_ptr<HostCommandRouter> m_commandRouter;
    HostHotkeyConfig m_config;
    std::array<bool, static_cast<std::size_t>(HotkeyAction::Count)>
        m_isDown{};
};

bool HostHotkeyRouter::Impl::GetCharMatched(
    const InteractionEvent& event,
    const char key) const
{
    if (key == 0) return false;
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
        targets.viewIds.begin(), targets.viewIds.end(), viewId)
            != targets.viewIds.end()
        || std::find(
            targets.viewRoles.begin(), targets.viewRoles.end(), role)
            != targets.viewRoles.end();
}

HostViewTargets HostHotkeyRouter::Impl::GetTargets(
    const HostHotkeyConfig& config)
{
    HostViewTargets targets;
    const auto append = [&targets](const HostViewTargets& source) {
        for (const auto& id : source.viewIds) {
            if (std::find(
                    targets.viewIds.begin(), targets.viewIds.end(), id)
                == targets.viewIds.end()) {
                targets.viewIds.push_back(id);
            }
        }
        for (const auto role : source.viewRoles) {
            if (std::find(
                    targets.viewRoles.begin(), targets.viewRoles.end(), role)
                == targets.viewRoles.end()) {
                targets.viewRoles.push_back(role);
            }
        }
    };
    if (config.isContextInputEnabled) append(config.contextInputViews);
    if (config.isCommandInputEnabled) append(config.commandInputViews);
    return targets;
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
    if (!router) return false;

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

InteractionResult HostHotkeyRouter::Impl::OnInput(
    const InteractionEvent& event,
    const std::string& viewId,
    const HostRenderViewRole role)
{
    const auto action = GetHotkeyAction(event);
    if (action == HotkeyAction::None) return {};
    const bool hasPermission = action == HotkeyAction::Model
        ? m_config.isContextInputEnabled
            && GetTargetMatched(m_config.contextInputViews, viewId, role)
        : m_config.isCommandInputEnabled
            && GetTargetMatched(m_config.commandInputViews, viewId, role);
    if (!hasPermission) return {};

    if (event.eventKind == InteractionEventKind::KeyRelease) {
        (void)SetActionDown(action, false);
        return { true, true };
    }
    if (event.eventKind == InteractionEventKind::TextInput) {
        return { true, true };
    }
    if (event.eventKind != InteractionEventKind::KeyPress) {
        return {};
    }
    if (!SetActionDown(action, true)) {
        return { true, true };
    }
    (void)SendRequest(action, role);
    return { true, true };
}

bool HostHotkeyRouter::Impl::AttachHotkeys(
    const HostHotkeyConfig& config)
{
    HostHotkeyConfig nextConfig;
    HostViewTargets targets;
    try {
        nextConfig = config;
        targets = GetTargets(nextConfig);
    }
    catch (...) {
        return false;
    }

    const bool hasInput = nextConfig.isContextInputEnabled
        || nextConfig.isCommandInputEnabled;
    if (!hasInput) {
        if (!m_inputRegistry.ClearHostInput()) return false;
        m_config = std::move(nextConfig);
        m_isDown.fill(false);
        return true;
    }

    const std::weak_ptr<Impl> weakOwner = shared_from_this();
    if (!m_inputRegistry.SetHostInput(
            std::move(targets),
            [weakOwner](
                const InteractionEvent& event,
                const std::string& viewId,
                const HostRenderViewRole role) {
                const auto owner = weakOwner.lock();
                return owner
                    ? owner->OnInput(event, viewId, role)
                    : InteractionResult{};
            })) {
        return false;
    }
    m_config = std::move(nextConfig);
    m_isDown.fill(false);
    return true;
}

bool HostHotkeyRouter::Impl::ClearHotkeys()
{
    if (!m_inputRegistry.ClearHostInput()) return false;
    m_config = {};
    m_isDown.fill(false);
    return true;
}

HostHotkeyRouter::HostHotkeyRouter(
    HostInputRegistry& inputRegistry,
    std::weak_ptr<HostCommandRouter> commandRouter)
    : m_impl(std::make_shared<Impl>(
        inputRegistry, std::move(commandRouter)))
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
