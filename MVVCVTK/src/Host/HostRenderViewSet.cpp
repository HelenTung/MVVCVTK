#include "Host/HostRenderViewSet.h"

#include "AppInterfaces.h"
#include "AppService.h"
#include "AppStateEvents.h"
#include "DataManager.h"
#include "StdRenderContext.h"

#include <algorithm>
#include <iostream>
#include <utility>

class HostRenderViewSet::Impl final {
public:
    void Build(
        const HostCoreServices& core,
        const std::vector<HostRenderViewConfig>& configs);
    const std::vector<HostRenderViewRuntime>& GetViews() const;
    const HostRenderViewRuntime* GetViewById(const std::string& id) const;
    const HostRenderViewRuntime* GetFirstViewByRole(HostRenderViewRole role) const;
    const HostRenderViewRuntime* GetPrimaryView() const;
    const HostRenderViewRuntime* GetStandaloneStartView() const;
    std::vector<const HostRenderViewRuntime*> GetViewsByIdsAndRoles(
        const std::vector<std::string>& ids,
        const std::vector<HostRenderViewRole>& roles) const;
    std::vector<const HostRenderViewRuntime*> GetCropPreviewViews() const;
    std::vector<const HostRenderViewRuntime*> GetGapOverlayViews() const;
    std::vector<std::shared_ptr<InteractiveService>> BuildServices(
        const std::vector<const HostRenderViewRuntime*>& views) const;
    void SetInitialVisibility() const;
    void SendRenderAll() const;
    void SetInteractorsReady() const;
    std::vector<HostRenderViewEndpoint> BuildEndpoints() const;
    bool GetRoleIs3DView(HostRenderViewRole role) const;
    bool GetRoleIsSliceView(HostRenderViewRole role) const;
    bool GetRoleIsGapOverlayRole(HostRenderViewRole role) const;

private:
    std::pair<std::shared_ptr<VizService>, std::shared_ptr<StdRenderContext>> BuildViewPair(
        const WindowConfig& cfg,
        vtkSmartPointer<vtkRenderWindow> renderWindow,
        std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> sharedState,
        std::shared_ptr<IStateEventSource> stateEventSource) const;
    bool GetIdIncluded(
        const std::vector<std::string>& selectedIds,
        const std::string& id) const;
    bool GetRoleIncluded(
        const std::vector<HostRenderViewRole>& selectedRoles,
        HostRenderViewRole role) const;

    std::vector<HostRenderViewRuntime> m_views;
};

std::pair<std::shared_ptr<VizService>, std::shared_ptr<StdRenderContext>> HostRenderViewSet::Impl::BuildViewPair(
    const WindowConfig& cfg,
    vtkSmartPointer<vtkRenderWindow> renderWindow,
    std::shared_ptr<AbstractDataManager> dataMgr,
    std::shared_ptr<SharedInteractionState> sharedState,
    std::shared_ptr<IStateEventSource> stateEventSource) const
{
    // 1. service 绑定共享数据和状态，负责业务渲染能力。
    // 2. context 绑定 VTK window/interactor，负责单窗口渲染生命周期。
    // 3. 二者在这里组装，是因为 HostRenderViewSet 知道窗口拓扑，但不把 topology 写进 StdRenderContext。
    auto service = std::make_shared<VizService>(
        std::move(dataMgr),
        std::move(sharedState),
        std::move(stateEventSource));
    auto context = std::make_shared<StdRenderContext>();

    if (renderWindow) {
        // 外部窗口必须先注入，再绑定 service；否则 service 会先拿到默认 window，后续 overlay 目标会短暂错位。
        context->SetRenderWindow(std::move(renderWindow));
    }
    context->SetServiceBound(service);
    service->SetVisualConfig(cfg.preInitCfg);
    context->SetWindowTitle(cfg.title);
    context->SetWindowSize(cfg.width, cfg.height);
    context->SetWindowPosition(cfg.posX, cfg.posY);
    context->SetCameraStyle(cfg.preInitCfg.vizMode);
    if (cfg.isAxesVisible) {
        context->SetOrientationAxesVisible(true);
    }

    if (cfg.preInitCfg.hasBgColor) {
        service->SetBackground(cfg.preInitCfg.bgColor);
    }

    return std::make_pair(service, context);
}

bool HostRenderViewSet::Impl::GetIdIncluded(
    const std::vector<std::string>& selectedIds,
    const std::string& id) const
{
    return std::find(selectedIds.begin(), selectedIds.end(), id) != selectedIds.end();
}

bool HostRenderViewSet::Impl::GetRoleIncluded(
    const std::vector<HostRenderViewRole>& selectedRoles,
    HostRenderViewRole role) const
{
    return std::find(selectedRoles.begin(), selectedRoles.end(), role) != selectedRoles.end();
}

void HostRenderViewSet::Impl::Build(
    const HostCoreServices& core,
    const std::vector<HostRenderViewConfig>& configs)
{
    // Build 是窗口拓扑进入 session 的唯一入口；调用方传多少 configs，就创建/接管多少窗口。
    // 这里不补默认五窗口；standalone 调试拓扑由 main 显式传入，真实上位机也走同一条配置链路。
    m_views.clear();
    m_views.reserve(configs.size());
    for (const auto& requestedConfig : configs) {
        HostRenderViewConfig config = requestedConfig;
        if (config.id.empty()) {
            // 空 id 不再按 index 造假稳定标识；上位机若需要按 id 寻址，必须在 HostRenderViewConfig 中显式提供。
            std::cerr << "[Host] Render view config has empty id; this view can only be targeted by role." << std::endl;
        }
        auto pair = BuildViewPair(
            config.window,
            config.renderWindow,
            core.sharedDataMgr,
            core.sharedState,
            core.sharedStateBroadcaster);

        HostRenderViewRuntime view;
        view.config = std::move(config);
        view.service = std::move(pair.first);
        view.context = std::move(pair.second);
        m_views.push_back(std::move(view));
    }
}

const std::vector<HostRenderViewRuntime>& HostRenderViewSet::Impl::GetViews() const
{
    return m_views;
}

const HostRenderViewRuntime* HostRenderViewSet::Impl::GetViewById(const std::string& id) const
{
    for (const auto& view : m_views) {
        if (view.config.id == id) {
            return &view;
        }
    }
    return nullptr;
}

const HostRenderViewRuntime* HostRenderViewSet::Impl::GetFirstViewByRole(HostRenderViewRole role) const
{
    for (const auto& view : m_views) {
        if (view.config.role == role) {
            return &view;
        }
    }
    return nullptr;
}

const HostRenderViewRuntime* HostRenderViewSet::Impl::GetPrimaryView() const
{
    // 先选明确 Primary3D，再退到任意 3D 视图；非标准窗口拓扑也要给裁切和初始加载一个可解释的参考视图。
    if (const auto* view = GetFirstViewByRole(HostRenderViewRole::Primary3D)) {
        return view;
    }

    for (const auto& view : m_views) {
        if (GetRoleIs3DView(view.config.role)) {
            return &view;
        }
    }

    return m_views.empty() ? nullptr : &m_views.front();
}

const HostRenderViewRuntime* HostRenderViewSet::Impl::GetStandaloneStartView() const
{
    // 独立 VTK host 只能由一个 interactor 进入阻塞主循环；Qt host 不调用 Start，因此不会被这里约束。
    for (const auto& view : m_views) {
        if (view.config.isEventLoopEnabled) {
            return &view;
        }
    }
    return m_views.empty() ? nullptr : &m_views.front();
}

std::vector<const HostRenderViewRuntime*> HostRenderViewSet::Impl::GetViewsByIdsAndRoles(
    const std::vector<std::string>& ids,
    const std::vector<HostRenderViewRole>& roles) const
{
    std::vector<const HostRenderViewRuntime*> selectedViews;
    selectedViews.reserve(m_views.size());

    // 空 ids/roles 表示宿主没有声明作用域，返回空而不是全选；这样 feature 激活不会因为漏配目标而接管所有窗口。
    for (const auto& view : m_views) {
        const bool isIdSelected = !ids.empty() && GetIdIncluded(ids, view.config.id);
        const bool isRoleSelected = !roles.empty() && GetRoleIncluded(roles, view.config.role);
        if (isIdSelected || isRoleSelected) {
            selectedViews.push_back(&view);
        }
    }

    return selectedViews;
}

std::vector<const HostRenderViewRuntime*> HostRenderViewSet::Impl::GetCropPreviewViews() const
{
    // isCropPreviewIncluded 是 host 对“配置默认 preview 集合”的声明；只有请求明确允许 fallback 时才会走到这里。
    std::vector<const HostRenderViewRuntime*> selectedViews;
    selectedViews.reserve(m_views.size());
    for (const auto& view : m_views) {
        if (view.config.isCropPreviewIncluded) {
            selectedViews.push_back(&view);
        }
    }
    return selectedViews;
}

std::vector<const HostRenderViewRuntime*> HostRenderViewSet::Impl::GetGapOverlayViews() const
{
    // 默认孔隙 overlay 只按 role 判断可显示能力，不按窗口序号或历史五窗口布局判断。
    std::vector<const HostRenderViewRuntime*> selectedViews;
    selectedViews.reserve(m_views.size());
    for (const auto& view : m_views) {
        if (GetRoleIsGapOverlayRole(view.config.role)) {
            selectedViews.push_back(&view);
        }
    }
    return selectedViews;
}

std::vector<std::shared_ptr<InteractiveService>> HostRenderViewSet::Impl::BuildServices(
    const std::vector<const HostRenderViewRuntime*>& views) const
{
    // 裁切 bridge 只需要刷新/overlay 这种交互服务接口；这里降到抽象类型，防止 feature 依赖 HostRenderViewRuntime。
    std::vector<std::shared_ptr<InteractiveService>> services;
    services.reserve(views.size());
    for (const auto* view : views) {
        if (view && view->service) {
            services.push_back(view->service);
        }
    }
    return services;
}

void HostRenderViewSet::Impl::SetInitialVisibility() const
{
    // 初始可见性是视图角色策略，不是窗口编号策略；新增窗口只要声明 role 即可进入正确默认状态。
    for (const auto& view : m_views) {
        if (!view.service) {
            continue;
        }

        if (GetRoleIs3DView(view.config.role)) {
            view.service->SetElementVisible(VisFlags::Planes3D, false);
            view.service->SetElementVisible(VisFlags::Ruler, false);
        }

        if (GetRoleIsSliceView(view.config.role)) {
            view.service->SetElementVisible(VisFlags::Crosshair, true);
        }
    }
}

void HostRenderViewSet::Impl::SendRenderAll() const
{
    // 初始化末尾先渲染一次，让外部 host 获取 endpoint 后看到的是已创建 renderer/interactor 的窗口。
    for (const auto& view : m_views) {
        if (view.context) {
            view.context->SendRender();
        }
    }
}

void HostRenderViewSet::Impl::SetInteractorsReady() const
{
    // VTK interactor 需要在 endpoint 暴露前完成初始化；Qt host 注入的 window 也沿用同一顺序。
    for (const auto& view : m_views) {
        if (view.context) {
            view.context->SetInteractorReady();
        }
    }
}

std::vector<HostRenderViewEndpoint> HostRenderViewSet::Impl::BuildEndpoints() const
{
    std::vector<HostRenderViewEndpoint> endpoints;
    endpoints.reserve(m_views.size());
    for (const auto& view : m_views) {
        // endpoint 是非拥有观察句柄，给外部 host 做 widget 映射；service 仍留在 session 内部维持边界。
        HostRenderViewEndpoint endpoint;
        endpoint.id = view.config.id;
        endpoint.role = view.config.role;
        endpoint.renderer = view.context ? view.context->GetRenderer() : nullptr;
        endpoint.renderWindow = view.context ? view.context->GetRenderWindow() : nullptr;
        endpoint.interactor = view.context ? view.context->GetInteractor() : nullptr;
        endpoints.push_back(endpoint);
    }
    return endpoints;
}

bool HostRenderViewSet::Impl::GetRoleIs3DView(HostRenderViewRole role) const
{
    return role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D;
}

bool HostRenderViewSet::Impl::GetRoleIsSliceView(HostRenderViewRole role) const
{
    return role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice;
}

bool HostRenderViewSet::Impl::GetRoleIsGapOverlayRole(HostRenderViewRole role) const
{
    // 当前 overlay 分为 3D mesh 和 2D label 两类；其他辅助窗口默认不接收，除非后续扩展新 role 语义。
    return GetRoleIs3DView(role) || GetRoleIsSliceView(role);
}

HostRenderViewSet::HostRenderViewSet()
    : m_impl(std::make_unique<HostRenderViewSet::Impl>())
{
}

HostRenderViewSet::~HostRenderViewSet() = default;

HostRenderViewSet::HostRenderViewSet(HostRenderViewSet&&) noexcept = default;

HostRenderViewSet& HostRenderViewSet::operator=(HostRenderViewSet&&) noexcept = default;

void HostRenderViewSet::Build(
    const HostCoreServices& core,
    const std::vector<HostRenderViewConfig>& configs)
{
    m_impl->Build(core, configs);
}

const std::vector<HostRenderViewRuntime>& HostRenderViewSet::GetViews() const
{
    return m_impl->GetViews();
}

const HostRenderViewRuntime* HostRenderViewSet::GetViewById(const std::string& id) const
{
    return m_impl->GetViewById(id);
}

const HostRenderViewRuntime* HostRenderViewSet::GetFirstViewByRole(HostRenderViewRole role) const
{
    return m_impl->GetFirstViewByRole(role);
}

const HostRenderViewRuntime* HostRenderViewSet::GetPrimaryView() const
{
    return m_impl->GetPrimaryView();
}

const HostRenderViewRuntime* HostRenderViewSet::GetStandaloneStartView() const
{
    return m_impl->GetStandaloneStartView();
}

std::vector<const HostRenderViewRuntime*> HostRenderViewSet::GetViewsByIdsAndRoles(
    const std::vector<std::string>& ids,
    const std::vector<HostRenderViewRole>& roles) const
{
    return m_impl->GetViewsByIdsAndRoles(ids, roles);
}

std::vector<const HostRenderViewRuntime*> HostRenderViewSet::GetCropPreviewViews() const
{
    return m_impl->GetCropPreviewViews();
}

std::vector<const HostRenderViewRuntime*> HostRenderViewSet::GetGapOverlayViews() const
{
    return m_impl->GetGapOverlayViews();
}

std::vector<std::shared_ptr<InteractiveService>> HostRenderViewSet::BuildServices(
    const std::vector<const HostRenderViewRuntime*>& views) const
{
    return m_impl->BuildServices(views);
}

void HostRenderViewSet::SetInitialVisibility() const
{
    m_impl->SetInitialVisibility();
}

void HostRenderViewSet::SendRenderAll() const
{
    m_impl->SendRenderAll();
}

void HostRenderViewSet::SetInteractorsReady() const
{
    m_impl->SetInteractorsReady();
}

std::vector<HostRenderViewEndpoint> HostRenderViewSet::BuildEndpoints() const
{
    return m_impl->BuildEndpoints();
}

bool HostRenderViewSet::GetRoleIs3DView(HostRenderViewRole role) const
{
    return m_impl && m_impl->GetRoleIs3DView(role);
}

bool HostRenderViewSet::GetRoleIsSliceView(HostRenderViewRole role) const
{
    return m_impl && m_impl->GetRoleIsSliceView(role);
}

bool HostRenderViewSet::GetRoleIsGapOverlayRole(HostRenderViewRole role) const
{
    return m_impl && m_impl->GetRoleIsGapOverlayRole(role);
}
