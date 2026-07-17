#include "Host/VtkAppHostSession.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostHotkeyRouter.h"
#include "Host/HostRenderViewSet.h"

#include "AppState.h"
#include "DataManager.h"
#include "Interaction/CropBridge.h"
#include "Services/GapAnalysisService.h"
#include "StdRenderContext.h"

#include <memory>
#include <utility>
#include <vector>

class VtkAppHostSession::Impl final {
public:
    explicit Impl(HostSessionConfig sessionConfig)
        : config(std::move(sessionConfig)),
          featureBindings(std::make_shared<HostFeatureBindings>()) {}

    bool BuildSession();
    bool SendCommand(HostCommand command);

    HostSessionConfig config;
    HostCoreServices core;
    HostRenderViewSet renderViews;
    std::shared_ptr<HostFeatureBindings> featureBindings;
    std::shared_ptr<HostCommandRouter> commandRouter;
    std::vector<HostRenderViewEndpoint> endpoints;
    std::unique_ptr<HostHotkeyRouter> hotkeyRouter;
    bool isBuilt = false;
    bool isStarted = false;

private:
    static HostCoreServices BuildCore();
};

HostCoreServices VtkAppHostSession::Impl::BuildCore()
{
    HostCoreServices value;
    value.sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    value.sharedStateBroadcaster = std::make_shared<SharedStateBroadcaster>();
    value.sharedState = std::make_shared<SharedInteractionState>(value.sharedStateBroadcaster);
    value.gapAnalysis = std::make_shared<GapAnalysisService>();
    value.orthogonalCropBridge = std::make_shared<CropBridge>();
    return value;
}

bool VtkAppHostSession::Impl::BuildSession()
{
    if (isBuilt) return true;
    if (config.renderViews.empty()) return false;

    core = BuildCore();
    if (!renderViews.Build(core, config.renderViews)) return false;
    featureBindings->AttachFeatures(core, renderViews);
    commandRouter = std::make_shared<HostCommandRouter>(core, renderViews, featureBindings);
    renderViews.SetInitialVisibility();
    renderViews.SetInteractorsReady();
    endpoints = renderViews.BuildEndpoints();
    hotkeyRouter = std::make_unique<HostHotkeyRouter>(
        renderViews, featureBindings, commandRouter);
    isBuilt = true;
    return true;
}

bool VtkAppHostSession::Impl::SendCommand(HostCommand command)
{
    return BuildSession() && commandRouter
        && commandRouter->DispatchCommand(std::move(command));
}

VtkAppHostSession::VtkAppHostSession(HostSessionConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

VtkAppHostSession::~VtkAppHostSession() = default;
VtkAppHostSession::VtkAppHostSession(VtkAppHostSession&&) noexcept = default;
VtkAppHostSession& VtkAppHostSession::operator=(VtkAppHostSession&&) noexcept = default;

bool VtkAppHostSession::BuildSession()
{
    return m_impl && m_impl->BuildSession();
}

bool VtkAppHostSession::AttachTimer(const HostTimerConfig& config)
{
    return BuildSession() && m_impl->featureBindings
        && m_impl->featureBindings->AttachHostTimer(config);
}

bool VtkAppHostSession::AttachHotkeys(
    const HostHotkeyConfig& config,
    HostHotkeyTemplates templates)
{
    return BuildSession() && m_impl->hotkeyRouter
        && m_impl->hotkeyRouter->AttachHotkeys(config, std::move(templates));
}

bool VtkAppHostSession::Start()
{
    if (!BuildSession() || m_impl->isStarted) return false;
    const auto* view = m_impl->renderViews.GetStandaloneStartView();
    if (!view || !view->context) return false;
    m_impl->renderViews.SendRenderAll();
    m_impl->isStarted = true;
    view->context->Start();
    return true;
}

bool VtkAppHostSession::SendData(
    HostDataRequest request, HostCompleteCallback onComplete)
{
    return m_impl->SendCommand(HostCommand{ HostDataCommand{
        std::move(request), std::move(onComplete) } });
}

bool VtkAppHostSession::SendView(HostViewRequest request)
{
    return m_impl->SendCommand(HostCommand{ HostViewCommand{ std::move(request) } });
}

bool VtkAppHostSession::SendTool(HostToolRequest request)
{
    return m_impl->SendCommand(HostCommand{ HostToolCommand{ std::move(request) } });
}

bool VtkAppHostSession::SendCrop(
    HostCropRequest request, HostCompleteCallback onComplete)
{
    return m_impl->SendCommand(HostCommand{ HostCropCommand{
        std::move(request), std::move(onComplete) } });
}

bool VtkAppHostSession::SendGap(
    HostGapRequest request, HostCompleteCallback onComplete)
{
    return m_impl->SendCommand(HostCommand{ HostGapCommand{
        std::move(request), std::move(onComplete) } });
}

const std::vector<HostRenderViewEndpoint>& VtkAppHostSession::GetRenderViewEndpoints()
{
    BuildSession();
    return m_impl->endpoints;
}

const HostRenderViewEndpoint* VtkAppHostSession::GetRenderViewEndpoint(
    const std::string& viewId)
{
    BuildSession();
    for (const auto& endpoint : m_impl->endpoints) {
        if (endpoint.id == viewId) return &endpoint;
    }
    return nullptr;
}

const HostRenderViewEndpoint* VtkAppHostSession::GetPrimaryEndpoint()
{
    BuildSession();
    for (const auto& endpoint : m_impl->endpoints) {
        if (endpoint.role == HostRenderViewRole::Primary3D) return &endpoint;
    }
    for (const auto& endpoint : m_impl->endpoints) {
        if (endpoint.role == HostRenderViewRole::Composite3D) return &endpoint;
    }
    return m_impl->endpoints.empty() ? nullptr : &m_impl->endpoints.front();
}
