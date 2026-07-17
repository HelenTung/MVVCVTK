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

// Impl 依次拥有会话配置、共享 core、窗口运行时、feature/router 和输入适配器；
// 成员声明顺序也约束析构顺序，使 hotkey/router 先于 renderViews/core 释放。
class VtkAppHostSession::Impl final {
public:
    explicit Impl(HostSessionConfig sessionConfig)
        : config(std::move(sessionConfig)),
          featureBindings(std::make_shared<HostFeatureBindings>()) {}

    bool BuildSession();
    bool SendCommand(HostCommand command);

    HostSessionConfig config;                         // 本会话窗口拓扑与外部 window 注入配置。
    HostCoreServices core;                           // 会话级 Data/State/Gap/Crop 共享 owner 集合。
    HostRenderViewSet renderViews;                   // service/context/window 的实际所有者。
    std::shared_ptr<HostFeatureBindings> featureBindings; // 把 core 能力绑定到 renderViews。
    std::shared_ptr<HostCommandRouter> commandRouter;      // Host 请求到 App/feature 的统一入口。
    std::vector<HostRenderViewEndpoint> endpoints;   // 面向外部的非拥有 VTK 观察句柄快照。
    std::unique_ptr<HostHotkeyRouter> hotkeyRouter;  // standalone 输入 observer 的 RAII owner。
    bool isBuilt = false;                            // 完整组装成功后才置位。
    bool isStarted = false;                          // standalone 事件循环只允许启动一次。

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
    // 1. 已构建会话直接复用；没有窗口配置时拒绝创建无拓扑 core。
    if (isBuilt) return true;
    if (config.renderViews.empty()) return false;

    // 2. 创建共享 core，再让每个窗口构建 service/context；任一步失败都不发布 isBuilt。
    core = BuildCore();
    if (!renderViews.Build(core, config.renderViews)) return false;
    // 3. feature 取得 core owner 与窗口拓扑，命令 router 随后才能把 Host 请求路由到两者。
    featureBindings->AttachFeatures(core, renderViews);
    commandRouter = std::make_shared<HostCommandRouter>(core, renderViews, featureBindings);
    // 4. 所有 context/service 就绪后统一应用初始可见性、初始化 interactor，并导出 endpoint。
    renderViews.SetInitialVisibility();
    renderViews.SetInteractorsReady();
    endpoints = renderViews.BuildEndpoints();
    // 5. hotkey router 最后创建，确保其安装 handler 时目标 context 与 command router 已有效。
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
    // standalone 启动路径：必须先完成构建，并且只能选择配置中唯一允许事件循环的 context。
    if (!BuildSession() || m_impl->isStarted) return false;
    const auto* view = m_impl->renderViews.GetStandaloneStartView();
    if (!view || !view->context) return false;
    // 先让全部窗口完成首帧，再发布 started 并进入阻塞式 VTK event loop。
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
