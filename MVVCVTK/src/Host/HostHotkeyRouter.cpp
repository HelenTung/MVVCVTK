#include "Host/HostHotkeyRouter.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "Interaction/InteractionTypes.h"
#include "StdRenderContext.h"

#include <vtkCommand.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

    const HostRenderViewSet* m_renderViews = nullptr;
    std::weak_ptr<HostFeatureBindings> m_featureBindings;
    std::weak_ptr<HostCommandRouter> m_commandRouter;
    std::vector<std::weak_ptr<StdRenderContext>> m_contexts;
    mutable std::array<bool, static_cast<std::size_t>(HotkeyAction::Count)> m_isDown{};
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
    const HotkeyAction action = GetHotkeyAction(event, config);
    if (action == HotkeyAction::None) return {};
    if ((action == HotkeyAction::Model && !hasContext)
        || (action != HotkeyAction::Model && !hasFeature)) return {};

    if (event.vtkEventId == vtkCommand::CharEvent) return { true, true };
    if (event.vtkEventId == vtkCommand::KeyReleaseEvent) {
        SetActionDown(action, false);
        return { true, true };
    }
    if (event.vtkEventId != vtkCommand::KeyPressEvent) return {};
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
    ClearHotkeys();
    if (!m_renderViews) return false;
    if (!config.isContextInputEnabled && !config.isCommandInputEnabled) return true;

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
            { vtkCommand::KeyPressEvent, vtkCommand::KeyReleaseEvent, vtkCommand::CharEvent });
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
