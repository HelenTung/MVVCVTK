#include "Host/HostViewRuntimeRegistry.h"
#include "Host/Internal/HostRenderViewRuntime.h"
#include "Host/Internal/HostFrameRuntime.h"
#include "Host/Internal/HostTransferCodec.h"

#include "App/AppStateEvents.h"
#include "App/AppState.h"
#include "Data/DataPayloads.h"
#include "App/AppTypes.h"
#include "App/Services/AppPorts.h"
#include "App/Services/AppServiceFactory.h"
#include "DataConverters.h"
#include "Host/LoadCommitCoordinator.h"
#include "Interaction/AbstractViewContext.h"
#include "Interaction/InteractionPorts.h"
#include "Interaction/ViewContextFactory.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Contracts/RenderBindPort.h"

#include <vtkRenderer.h>
#include <vtkRenderWindow.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

class HostViewRuntimeRegistry::Impl final {
    bool m_isHostDriven = false;
public:
    Impl();
    ~Impl();

    bool Build(
        const HostCoreServices& core,
        const std::vector<HostRenderViewConfig>& configs);
    const std::vector<HostRenderViewRuntime>& GetViews() const;
    const HostRenderViewRuntime* GetViewById(
        const std::string& id) const;
    const HostRenderViewRuntime* GetFirstViewByRole(
        HostRenderViewRole role) const;
    const HostRenderViewRuntime* GetViewBySelector(
        const HostViewTarget& target) const;
    std::optional<HostRenderViewState> GetViewState(
        const HostViewTarget& target) const;
    std::vector<HostRenderViewState> GetViewStates() const;
    std::optional<HostSceneViewState> GetSceneViewState(
        const HostViewTarget& target);
    std::vector<HostSceneViewState> GetSceneViewStates();
    std::optional<HostDataRoute> GetDataRoute(
        const HostViewTarget& target) const;
    std::optional<HostViewRoute> GetViewRoute(
        const HostViewTarget& target) const;
    std::weak_ptr<AppSessionPort> GetSessionPort() const;
    bool StopView(std::string_view viewId);
    FeatureDataTransitionState StartDataTransition(std::uint64_t ownerId, FeatureDataTransitionRequest request);
    FeatureDataTransitionState SetDataTransition(std::uint64_t ownerId, std::uint64_t requestId);
    FeatureDataTransitionState StopDataTransition(std::uint64_t ownerId, std::uint64_t requestId);
    LoadCommitResult SetLoadCommit(
        LoadEventKind loadKind,
        std::uint64_t transactionRevision,
        const VtkImageGridSnapshot& pending);
    LoadCommitResult SetLoadCancelled(
        std::uint64_t transactionRevision,
        LoadCommitFailure failureReason);
    std::vector<HostInputRoute> GetInputRouteValues(
        const HostViewTargets& targets) const;
    const HostRenderViewRuntime* GetPrimaryView() const;
    const HostRenderViewRuntime* GetStandaloneStartView() const;
    std::vector<const HostRenderViewRuntime*> GetViewsByTargets(
        const HostViewTargets& targets) const;
    std::vector<HostFeatureView> GetFeatureViews(
        const HostViewTargets& targets) const;
    std::shared_ptr<FeatureViewService> GetFeaturePort(
        const std::string& viewId) const;
    std::shared_ptr<OverlayService> GetOverlayPort(
        const std::string& viewId) const;
    std::optional<HostInputView> GetInputView(
        const HostViewTarget& target) const;
    bool SetFeatureViews(
        const std::string& featureId,
        const std::vector<std::string>& viewIds);
    std::vector<std::string> GetFeatureViewIds(
        const std::string& featureId) const;
    bool SetViewStatus(
        const std::vector<std::string>& viewIds,
        const std::string& status) const;
    bool SetViewWindow(
        const std::string& viewId,
        vtkSmartPointer<vtkRenderWindow> renderWindow);
    bool SetTimerHandler(
        const HostViewTarget& target,
        std::function<void()> handler) const;
    bool ClearTimerHandler(const HostViewTarget& target) const;
    bool SetFrameHandlers(std::function<void()> handler) const;
    bool SetInputsEnabled(bool isEnabled) const;
    bool SetFrameGeneration(std::uint64_t sessionGeneration);
    bool SetFrameIntents(
        const std::vector<HostFrameIntent>& intents);
    bool CollectFrameUpdates();
    bool ApplyFrameUpdates();
    HostFrameStageStatus BuildFrameStage(std::uint64_t nextEpoch);
    void SetFrameCommit(std::uint64_t epoch) noexcept;
    bool SendFrameRender(std::uint64_t epoch);
    HostRenderResult SendFrameRender(const HostRenderRequest& request,
        const std::function<bool()>& getIsRunning);
    std::vector<std::string> GetRenderViewIds() const;
    bool GetFrameRenderPending() const noexcept;
    void SendFrameCompletions() noexcept;
    void ClearFrameStage() noexcept;
    bool SetModelMatrix(
        const HostViewTarget& target,
        const std::array<double, 16>& modelToWorld) const;
    bool SendViewUpdates(const HostViewTarget& target) const;
    bool StartStandaloneView() const;
    std::shared_ptr<AppTaskExecutor> GetTaskExecutor() const;
    bool StopLease();
    bool StopRoutes();
    bool SetInitialVisibility() const;
    bool SendRenderAll() const;
    bool SetInteractorsReady() const;
    std::vector<HostRenderViewEndpoint> BuildEndpoints() const;
    bool GetRoleIs3DView(HostRenderViewRole role) const;
    bool GetRoleIsSliceView(HostRenderViewRole role) const;
    std::weak_ptr<IHostViewDirectory> GetViewDirectory() const;

private:
    class ViewDirectory final : public IHostViewDirectory {
    public:
        explicit ViewDirectory(Impl* owner)
            : m_owner(owner)
        {
        }

        std::optional<HostDataRoute> GetDataRoute(
            const HostViewTarget& target) const override;
        std::optional<HostViewRoute> GetViewRoute(
            const HostViewTarget& target) const override;
        std::weak_ptr<AppSessionPort> GetSessionPort() const override;
        std::vector<HostInputRoute> GetInputRoutes(
            const HostViewTargets& targets) const override;
        bool SetOwnerThread(std::thread::id ownerThread);
        bool StopView(std::string_view viewId) override;
        LoadCommitResult SetLoadCommit(
            LoadEventKind loadKind,
            std::uint64_t transactionRevision,
            const VtkImageGridSnapshot& pending);
        LoadCommitResult SetLoadCancelled(
            std::uint64_t transactionRevision,
            LoadCommitFailure failureReason);
        bool StopOwner();

    private:
        mutable std::mutex m_mutex;
        Impl* m_owner = nullptr;
        std::thread::id m_ownerThread;
        bool m_hasOwnerThread = false;
    };

    class FeatureLeasePort final : public FeatureViewService {
    public:
        FeatureLeasePort(
            std::weak_ptr<FeatureViewService> port,
            std::weak_ptr<const FeatureViewLease> lease)
            : m_port(std::move(port))
            , m_lease(std::move(lease))
        {
        }

        bool SetInteracting(
            const InteractionSource& source,
            bool isInteracting) override;
        std::optional<std::array<double, 16>>
            GetModelToWorld() const override;
        std::optional<std::array<double, 3>> GetWorldPosition(
            const std::array<double, 3>& modelPosition) const override;
        std::optional<RenderInputStamp>
            GetRenderInputStamp() const override;
        bool AttachRenderEffect(
            std::shared_ptr<RenderEffect> effect) override;
        bool DetachRenderEffect(
            const RenderEffect* effect) override;
        bool SetRenderNeeded() override;

    private:
        std::shared_ptr<FeatureViewService> GetPort() const;

        std::weak_ptr<FeatureViewService> m_port;
        std::weak_ptr<const FeatureViewLease> m_lease;
    };

    class OverlayLeasePort final : public OverlayService {
    public:
        OverlayLeasePort(
            std::weak_ptr<OverlayService> port,
            std::weak_ptr<const FeatureViewLease> lease)
            : m_port(std::move(port))
            , m_lease(std::move(lease))
        {
        }

        bool AttachOverlay(
            std::shared_ptr<FeatureOverlay> overlay) override;
        void RemoveOverlay(
            std::shared_ptr<FeatureOverlay> overlay)
            noexcept override;
        void ClearOverlays() noexcept override;

    private:
        std::shared_ptr<OverlayService> GetPort() const;

        std::weak_ptr<OverlayService> m_port;
        std::weak_ptr<const FeatureViewLease> m_lease;
    };

    struct ViewLeasePorts final {
        std::shared_ptr<FeatureViewService> feature;
        std::shared_ptr<OverlayService> overlay;
    };

    std::optional<HostRenderViewRuntime> BuildView(
        const HostCoreServices& core,
        HostRenderViewConfig config,
        const std::shared_ptr<AppTaskExecutor>& taskExecutor,
        const std::shared_ptr<HistogramConverter>& histogram,
        const std::shared_ptr<RenderStrategyServices>& renderServices);
    bool SetViewWindow(
        HostRenderViewRuntime& view,
        vtkSmartPointer<vtkRenderWindow> renderWindow);
    std::optional<VizMode> GetAppViewMode(
        HostRenderMode mode) const;

    std::optional<PreInitConfig> BuildAppInit(
        const HostViewInitConfig& config) const;

    const HostRenderViewRuntime* GetSceneViewBySelector(
        const HostViewTarget& target) const;
    std::vector<std::string> GetActiveFeatureIds(
        const HostRenderViewRuntime& view) const;

    const ViewLeasePorts* GetLeasePorts(
        const HostRenderViewRuntime* view) const;

    std::vector<HostRenderViewRuntime> m_views;
    std::vector<ViewLeasePorts> m_leasePorts;
    std::shared_ptr<FeatureViewLease> m_lease;
    std::map<std::string, std::vector<std::shared_ptr<AppFeaturePort>>>
        m_featureViews;
    std::shared_ptr<ViewDirectory> m_directory;
    std::unique_ptr<LoadCommitCoordinator> m_loadCommit;
    struct FeatureTransition final {
        std::uint64_t ownerId = 0;
        FeatureDataTransitionRequest request;
        LoadCommitRequest load;
        DataReadyState ready;
        FeatureDataTransitionState state;
        std::shared_ptr<const DataResourceLease> sourceLease;
        bool isAdvancing = false;
    };
    std::shared_ptr<FeatureTransition> m_featureTransition;
    std::weak_ptr<AbstractDataManager> m_transitionData;
    std::weak_ptr<SharedInteractionState> m_transitionState;
    std::shared_ptr<AppTaskExecutor> m_taskExecutor;
    std::shared_ptr<RenderStrategyServices> m_renderServices;
    std::unique_ptr<HostFrameRuntime> m_frameRuntime;
};

HostViewRuntimeRegistry::Impl::Impl()
    : m_directory(std::make_shared<ViewDirectory>(this))
    , m_frameRuntime(std::make_unique<HostFrameRuntime>(m_views, m_lease,
        [this](const HostRenderViewRuntime& view) { return GetActiveFeatureIds(view); }))
{
}

std::optional<HostDataRoute>
HostViewRuntimeRegistry::Impl::ViewDirectory::GetDataRoute(
    const HostViewTarget& target) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_owner && m_hasOwnerThread
        && m_ownerThread == std::this_thread::get_id()
        ? m_owner->GetDataRoute(target)
        : std::optional<HostDataRoute>{};
}

std::optional<HostViewRoute>
HostViewRuntimeRegistry::Impl::ViewDirectory::GetViewRoute(
    const HostViewTarget& target) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_owner && m_hasOwnerThread
        && m_ownerThread == std::this_thread::get_id()
        ? m_owner->GetViewRoute(target)
        : std::optional<HostViewRoute>{};
}

std::weak_ptr<AppSessionPort>
HostViewRuntimeRegistry::Impl::ViewDirectory::GetSessionPort() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_owner && m_hasOwnerThread
        && m_ownerThread == std::this_thread::get_id()
        ? m_owner->GetSessionPort()
        : std::weak_ptr<AppSessionPort>{};
}

std::vector<HostInputRoute>
HostViewRuntimeRegistry::Impl::ViewDirectory::GetInputRoutes(
    const HostViewTargets& targets) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_owner && m_hasOwnerThread
        && m_ownerThread == std::this_thread::get_id()
        ? m_owner->GetInputRouteValues(targets)
        : std::vector<HostInputRoute>{};
}

bool HostViewRuntimeRegistry::Impl::ViewDirectory::SetOwnerThread(
    const std::thread::id ownerThread)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_owner
        || (m_hasOwnerThread && m_ownerThread != ownerThread)) {
        return false;
    }
    m_ownerThread = ownerThread;
    m_hasOwnerThread = true;
    return true;
}

bool HostViewRuntimeRegistry::Impl::ViewDirectory::StopView(
    const std::string_view viewId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_owner && m_hasOwnerThread
        && m_ownerThread == std::this_thread::get_id()
        && m_owner->StopView(viewId);
}

LoadCommitResult
HostViewRuntimeRegistry::Impl::ViewDirectory::SetLoadCommit(
    const LoadEventKind loadKind,
    const std::uint64_t transactionRevision,
    const VtkImageGridSnapshot& pending)
{
    Impl* owner = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_owner || !m_hasOwnerThread
            || m_ownerThread != std::this_thread::get_id()) {
            return {};
        }
        owner = m_owner;
    }
    return owner->SetLoadCommit(
        loadKind, transactionRevision, pending);
}

LoadCommitResult
HostViewRuntimeRegistry::Impl::ViewDirectory::SetLoadCancelled(
    const std::uint64_t transactionRevision,
    const LoadCommitFailure failureReason)
{
    Impl* owner = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_owner || !m_hasOwnerThread
            || m_ownerThread != std::this_thread::get_id()) {
            return {};
        }
        owner = m_owner;
    }
    return owner->SetLoadCancelled(
        transactionRevision, failureReason);
}

bool HostViewRuntimeRegistry::Impl::ViewDirectory::StopOwner()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_owner = nullptr;
    return true;
}

std::weak_ptr<IHostViewDirectory>
HostViewRuntimeRegistry::Impl::GetViewDirectory() const
{
    return m_directory;
}

std::shared_ptr<FeatureViewService>
HostViewRuntimeRegistry::Impl::FeatureLeasePort::GetPort() const
{
    const auto lease = m_lease.lock();
    if (!lease
        || !lease->GetIsActive()
        || !lease->GetIsOwnerThread()) {
        return nullptr;
    }
    return m_port.lock();
}

bool HostViewRuntimeRegistry::Impl::FeatureLeasePort::SetInteracting(
    const InteractionSource& source,
    const bool isInteracting)
{
    const auto port = GetPort();
    return port && port->SetInteracting(source, isInteracting);
}

std::optional<std::array<double, 16>>
HostViewRuntimeRegistry::Impl::FeatureLeasePort::GetModelToWorld() const
{
    const auto port = GetPort();
    return port ? port->GetModelToWorld() : std::nullopt;
}

std::optional<std::array<double, 3>>
HostViewRuntimeRegistry::Impl::FeatureLeasePort::GetWorldPosition(
    const std::array<double, 3>& modelPosition) const
{
    const auto port = GetPort();
    return port
        ? port->GetWorldPosition(modelPosition)
        : std::nullopt;
}

std::optional<RenderInputStamp>
HostViewRuntimeRegistry::Impl::FeatureLeasePort::GetRenderInputStamp() const
{
    const auto port = GetPort();
    return port ? port->GetRenderInputStamp() : std::nullopt;
}

bool HostViewRuntimeRegistry::Impl::FeatureLeasePort::AttachRenderEffect(
    std::shared_ptr<RenderEffect> effect)
{
    const auto port = GetPort();
    return port && port->AttachRenderEffect(std::move(effect));
}

bool HostViewRuntimeRegistry::Impl::FeatureLeasePort::DetachRenderEffect(
    const RenderEffect* effect)
{
    const auto port = GetPort();
    return port && port->DetachRenderEffect(effect);
}

bool HostViewRuntimeRegistry::Impl::FeatureLeasePort::SetRenderNeeded()
{
    const auto port = GetPort();
    return port && port->SetRenderNeeded();
}

std::shared_ptr<OverlayService>
HostViewRuntimeRegistry::Impl::OverlayLeasePort::GetPort() const
{
    const auto lease = m_lease.lock();
    if (!lease
        || !lease->GetIsActive()
        || !lease->GetIsOwnerThread()) {
        return nullptr;
    }
    return m_port.lock();
}

bool HostViewRuntimeRegistry::Impl::OverlayLeasePort::AttachOverlay(
    std::shared_ptr<FeatureOverlay> overlay)
{
    const auto port = GetPort();
    if (!port) return false;
    return port->AttachOverlay(std::move(overlay));
}

void HostViewRuntimeRegistry::Impl::OverlayLeasePort::RemoveOverlay(
    std::shared_ptr<FeatureOverlay> overlay) noexcept
{
    const auto port = GetPort();
    if (port) port->RemoveOverlay(std::move(overlay));
}

void HostViewRuntimeRegistry::Impl::OverlayLeasePort::ClearOverlays() noexcept
{
    const auto port = GetPort();
    if (port) {
        try { port->ClearOverlays(); } catch (...) {}
    }
}

bool HostViewRuntimeRegistry::Impl::SetViewWindow(
    HostRenderViewRuntime& view,
    vtkSmartPointer<vtkRenderWindow> renderWindow)
{
    if (!view.context || !view.renderBind) return false;
    const bool hasActiveFeature = std::any_of(
        m_featureViews.begin(), m_featureViews.end(),
        [&view](const auto& featureViews) {
            return std::find(
                featureViews.second.begin(),
                featureViews.second.end(),
                view.app.feature) != featureViews.second.end();
        });
    if (hasActiveFeature) return false;
    if (!renderWindow) {
        renderWindow = view.context->GetRenderWindow();
    }
    if (!renderWindow) return false;

    const bool wasAvailable = view.isAvailable;
    vtkSmartPointer<vtkRenderWindow> oldWindow =
        view.context->GetRenderWindow();
    vtkSmartPointer<vtkRenderer> oldRenderer =
        view.context->GetRenderer();

    if (!view.context->SetRenderWindow(renderWindow)) {
        const bool isRolledBack = oldWindow
            && view.context->SetRenderWindow(oldWindow);
        view.isAvailable = wasAvailable && isRolledBack;
        return false;
    }

    vtkSmartPointer<vtkRenderWindow> nextWindow =
        view.context->GetRenderWindow();
    vtkSmartPointer<vtkRenderer> nextRenderer =
        view.context->GetRenderer();
    if (nextWindow && nextRenderer
        && view.renderBind->SetRenderTarget(
            nextWindow, nextRenderer)) {
        view.isAvailable = true;
        return true;
    }

    // Render port 承诺失败时保留旧绑定；Host 再回滚 Context。
    const bool isContextRolledBack = oldWindow
        && view.context->SetRenderWindow(oldWindow);
    const bool isRenderRolledBack = !wasAvailable
        || (isContextRolledBack
            && oldRenderer
            && view.renderBind->SetRenderTarget(
                oldWindow, oldRenderer));
    view.isAvailable = wasAvailable
        && isContextRolledBack
        && isRenderRolledBack;
    return false;
}

std::optional<HostRenderViewRuntime>
HostViewRuntimeRegistry::Impl::BuildView(
    const HostCoreServices& core,
    HostRenderViewConfig config,
    const std::shared_ptr<AppTaskExecutor>& taskExecutor,
    const std::shared_ptr<HistogramConverter>& histogram,
    const std::shared_ptr<RenderStrategyServices>& renderServices)
{
    const auto appInit = BuildAppInit(config.window.viewInit);
    if (!appInit) return std::nullopt;

    AppServiceArgs args;
    args.dataManager = core.sharedDataMgr;
    args.onWorkAvailable = core.onWorkAvailable;
    args.interactionState = core.sharedState;
    args.eventSource = core.sharedStateBroadcaster;
    args.taskExecutor = taskExecutor;
    args.histogram = histogram;
    args.renderServices = renderServices;
    const std::weak_ptr<ViewDirectory> directory = m_directory;
    args.setLoadCommit = [directory](
        const LoadEventKind loadKind,
        const std::uint64_t transactionRevision,
        const VtkImageGridSnapshot& pending) {
        const auto current = directory.lock();
        return current
            ? current->SetLoadCommit(
                loadKind, transactionRevision, pending)
            : LoadCommitResult{};
    };
    args.setLoadCancelled = [directory](
        const std::uint64_t transactionRevision,
        const LoadCommitFailure failureReason) {
        const auto current = directory.lock();
        return current
            ? current->SetLoadCancelled(
                transactionRevision, failureReason)
            : LoadCommitResult{};
    };
    auto ports = CreateAppPorts(std::move(args));
    if (!ports.app.data || !ports.app.view || !ports.app.session
        || !ports.app.feature
        || !ports.dataStage
        || !ports.interaction.update || !ports.interaction.state
        || !ports.interaction.slice || !ports.interaction.model
        || !ports.renderBind || !ports.featureView
        || !ports.overlay || !ports.taskControl) {
        return std::nullopt;
    }

    auto context = CreateViewContext(
        ports.interaction,
        config.inputMode == HostInputMode::HostInjected, core.isHostDriven);
    if (!context) return std::nullopt;

    HostRenderViewRuntime view;
    view.config = std::move(config);
    view.app = std::move(ports.app);
    view.dataStage = std::move(ports.dataStage);
    view.interaction = std::move(ports.interaction);
    view.renderBind = std::move(ports.renderBind);
    view.featureView = std::move(ports.featureView);
    view.overlay = std::move(ports.overlay);
    view.taskControl = std::move(ports.taskControl);
    view.context = std::move(context);

    // 首次绑定与后续 Qt rebind 共享同一事务入口。
    if (!SetViewWindow(view, view.config.renderWindow)
        || !view.app.view->SetViewConfig(*appInit)
        || (!(core.isHostDriven && view.config.renderWindow)
            && (!view.context->SetWindowTitle(view.config.window.title)
                || !view.context->SetWindowSize(
                    view.config.window.width, view.config.window.height)
                || !view.context->SetWindowPosition(
                    view.config.window.posX, view.config.window.posY)))
        || !view.context->SetCameraStyle(appInit->vizMode)
        || !view.context->SetOrientationAxesVisible(
            view.config.window.isAxesVisible)) {
        view.isAvailable = false;
        return std::nullopt;
    }
    return view;
}

std::optional<VizMode>
HostViewRuntimeRegistry::Impl::GetAppViewMode(
    const HostRenderMode mode) const
{
    switch (mode) {
    case HostRenderMode::Volume: return VizMode::Volume;
    case HostRenderMode::IsoSurface: return VizMode::IsoSurface;
    case HostRenderMode::SliceTopDown: return VizMode::SliceTop_down;
    case HostRenderMode::SliceFrontBack:
        return VizMode::SliceFront_back;
    case HostRenderMode::SliceLeftRight:
        return VizMode::SliceLeft_right;
    case HostRenderMode::CompositeVolume:
        return VizMode::CompositeVolume;
    case HostRenderMode::CompositeIsoSurface:
        return VizMode::CompositeIsoSurface;
    }
    return std::nullopt;
}

std::vector<std::string>
HostViewRuntimeRegistry::Impl::GetActiveFeatureIds(
    const HostRenderViewRuntime& view) const
{
    std::vector<std::string> ids;
    if (!view.app.feature) return ids;

    for (const auto& [featureId, ports] : m_featureViews) {
        if (std::find(ports.begin(), ports.end(), view.app.feature)
            != ports.end()) {
            ids.push_back(featureId);
        }
    }
    return ids;
}

std::optional<PreInitConfig>
HostViewRuntimeRegistry::Impl::BuildAppInit(
    const HostViewInitConfig& config) const
{
    const auto mode = GetAppViewMode(config.viewMode);
    const auto isFinite = [](const double value) {
        return std::isfinite(value);
    };
    const auto isUnit = [&isFinite](const double value) {
        return isFinite(value) && value >= 0.0 && value <= 1.0;
    };
    if (!mode || !isUnit(config.material.ambient)
        || !isUnit(config.material.diffuse)
        || !isUnit(config.material.specular)
        || !isFinite(config.material.specularPower)
        || config.material.specularPower < 0.0
        || !isUnit(config.material.opacity)
        || (config.hasIso && !isFinite(config.isoThreshold))
        || (config.hasBackground
            && (!isUnit(config.background.r)
                || !isUnit(config.background.g)
                || !isUnit(config.background.b)))
        || (config.hasWindowLevel
            && (!isFinite(config.windowLevel.windowWidth)
                || config.windowLevel.windowWidth <= 0.0
                || !isFinite(config.windowLevel.windowCenter)))) {
        return std::nullopt;
    }

    PreInitConfig result;
    result.vizMode = *mode;
    result.material = {
        config.material.ambient,
        config.material.diffuse,
        config.material.specular,
        config.material.specularPower,
        config.material.opacity,
        config.material.isShadeOn
    };
    result.isoThreshold = config.isoThreshold;
    result.bgColor = {
        config.background.r,
        config.background.g,
        config.background.b
    };
    result.windowLevel = {
        config.windowLevel.windowWidth,
        config.windowLevel.windowCenter
    };
    result.hasVolumeTransferFunction =
        config.hasVolumeTransferFunction;
    result.hasIso = config.hasIso;
    result.hasBgColor = config.hasBackground;
    result.hasWindowLevel = config.hasWindowLevel;
    if (config.hasVolumeTransferFunction) {
        const auto function =
            HostTransferCodec::BuildVolumeTransferFunction(
                config.volumeTransferFunction);
        if (!function) return std::nullopt;
        result.volumeTransferFunction = *function;
    }
    return result;
}

bool HostViewRuntimeRegistry::Impl::Build(
    const HostCoreServices& core,
    const std::vector<HostRenderViewConfig>& configs)
{
    std::vector<std::string> viewIds;
    std::map<std::string, HostViewSyncPolicy> policies;
    viewIds.reserve(configs.size());
    for (const auto& config : configs) {
        if (config.syncPolicy != HostViewSyncPolicy::SamePrimary
            && config.syncPolicy != HostViewSyncPolicy::ExplicitInputs) return false;
        const auto policy = policies.find(config.synchronizationGroup);
        if (policy != policies.end() && policy->second != config.syncPolicy) return false;
        policies[config.synchronizationGroup] = config.syncPolicy;
        if (config.id.empty()
            || std::find(viewIds.begin(), viewIds.end(), config.id)
                != viewIds.end()) {
            return false;
        }
        viewIds.push_back(config.id);
    }

    if (!m_directory
        || !m_directory->SetOwnerThread(std::this_thread::get_id())) {
        return false;
    }
    if (m_lease && !m_featureViews.empty()) {
        return false;
    }

    auto nextLease = std::make_shared<FeatureViewLease>(
        std::this_thread::get_id());
    std::shared_ptr<AppTaskExecutor> nextTaskExecutor;
    std::shared_ptr<HistogramConverter> nextHistogram;
    std::shared_ptr<RenderStrategyServices> nextRenderServices;
    try {
        if (!configs.empty()) {
            nextTaskExecutor = CreateAppTaskExecutor({}, core.onWorkAvailable);
            nextHistogram = std::make_shared<HistogramConverter>();
            const std::weak_ptr<AppTaskExecutor> weakExecutor =
                nextTaskExecutor;
            nextRenderServices =
                std::make_shared<RenderStrategyServices>();
            nextRenderServices->isHostDriven = core.isHostDriven;
            nextRenderServices->resources =
                std::make_shared<RenderResourceCoordinator>(
                    [weakExecutor](RenderLaneWork work) {
                        return SendRenderTask(
                            weakExecutor.lock(), std::move(work));
                    }, core.sharedDataMgr);
        }
    }
    catch (...) {
        return false;
    }
    std::vector<HostRenderViewRuntime> nextViews;
    std::vector<ViewLeasePorts> nextLeasePorts;
    nextViews.reserve(configs.size());
    nextLeasePorts.reserve(configs.size());

    for (const auto& requestedConfig : configs) {
        auto config = requestedConfig;
        auto view = BuildView(
            core,
            std::move(config),
            nextTaskExecutor,
            nextHistogram,
            nextRenderServices);
        if (!view) return false;
        nextViews.push_back(std::move(*view));

        ViewLeasePorts leasePorts;
        leasePorts.feature = std::make_shared<FeatureLeasePort>(
            nextViews.back().featureView, nextLease);
        leasePorts.overlay = std::make_shared<OverlayLeasePort>(
            nextViews.back().overlay, nextLease);
        nextLeasePorts.push_back(std::move(leasePorts));
    }

    // 候选拓扑全部成功后才停止旧 lease；停止失败则候选在 owner thread 自动释放，旧拓扑保留。
    if (m_lease && !StopLease()) return false;
    m_featureViews.clear();
    m_views = std::move(nextViews);
    m_leasePorts = std::move(nextLeasePorts);
    m_lease = std::move(nextLease);
    m_taskExecutor = std::move(nextTaskExecutor);
    m_isHostDriven = core.isHostDriven;
    m_frameRuntime->SetDriveMode(core.isHostDriven
        ? HostDriveMode::HostDriven : HostDriveMode::Native);
    m_renderServices = std::move(nextRenderServices);
    if (m_renderServices && m_renderServices->resources) {
        (void)m_renderServices->resources->AdvanceTopologyRevision();
    }
    m_loadCommit = std::make_unique<LoadCommitCoordinator>(
        core.sharedDataMgr);
    m_transitionData = core.sharedDataMgr;
    m_transitionState = core.sharedState;
    m_frameRuntime->BuildSceneStates();
    m_frameRuntime->SetDataRead(core.sharedDataMgr);
    return true;
}

HostViewRuntimeRegistry::Impl::~Impl()
{
    // 外壳只在 StopLease 成功后删除 Impl；失败时保留完整 runtime，
    // 避免成员析构在错误线程触达 VTK。
}

const std::vector<HostRenderViewRuntime>&
HostViewRuntimeRegistry::Impl::GetViews() const
{
    return m_views;
}

const HostRenderViewRuntime*
HostViewRuntimeRegistry::Impl::GetViewById(
    const std::string& id) const
{
    for (const auto& view : m_views) {
        if (view.isAvailable && view.config.id == id) return &view;
    }
    return nullptr;
}

const HostRenderViewRuntime*
HostViewRuntimeRegistry::Impl::GetFirstViewByRole(
    const HostRenderViewRole role) const
{
    for (const auto& view : m_views) {
        if (view.isAvailable && view.config.role == role) {
            return &view;
        }
    }
    return nullptr;
}

const HostRenderViewRuntime*
HostViewRuntimeRegistry::Impl::GetViewBySelector(
    const HostViewTarget& target) const
{
    if (!target.viewId.empty()) return GetViewById(target.viewId);
    if (target.isViewRoleUsed) {
        return GetFirstViewByRole(target.viewRole);
    }
    return nullptr;
}

const HostRenderViewRuntime*
HostViewRuntimeRegistry::Impl::GetSceneViewBySelector(
    const HostViewTarget& target) const
{
    if (!target.viewId.empty()) {
        const auto view = std::find_if(
            m_views.begin(), m_views.end(),
            [&target](const HostRenderViewRuntime& current) {
                return current.config.id == target.viewId;
            });
        return view != m_views.end() ? &*view : nullptr;
    }
    if (!target.isViewRoleUsed) return nullptr;
    return GetFirstViewByRole(target.viewRole);
}

std::optional<HostRenderViewState>
HostViewRuntimeRegistry::Impl::GetViewState(
    const HostViewTarget& target) const
{
    const auto* view = GetViewBySelector(target);
    return view
        ? std::optional<HostRenderViewState>(view->BuildViewState())
        : std::nullopt;
}

std::vector<HostRenderViewState>
HostViewRuntimeRegistry::Impl::GetViewStates() const
{
    std::vector<HostRenderViewState> states;
    states.reserve(m_views.size());
    for (const auto& view : m_views) {
        if (view.isAvailable) states.push_back(view.BuildViewState());
    }
    return states;
}

std::optional<HostSceneViewState>
HostViewRuntimeRegistry::Impl::GetSceneViewState(
    const HostViewTarget& target)
{
    if (!m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return std::nullopt;
    }
    const auto* view = GetSceneViewBySelector(target);
    if (!view) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(view - m_views.data());
    return m_frameRuntime->GetSceneState(index);
}

std::vector<HostSceneViewState>
HostViewRuntimeRegistry::Impl::GetSceneViewStates()
{
    if (!m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return {};
    }
    return m_frameRuntime->GetSceneStates();
}

std::optional<HostDataRoute>
HostViewRuntimeRegistry::Impl::GetDataRoute(
    const HostViewTarget& target) const
{
    const bool hasTarget = !target.viewId.empty()
        || target.isViewRoleUsed;
    const auto* view = hasTarget
        ? GetViewBySelector(target)
        : GetPrimaryView();
    if (!view || !view->app.data || !view->app.view) {
        return std::nullopt;
    }

    const auto mode = HostRenderViewRuntime::GetHostViewMode(
        view->app.view->GetViewState().mode);
    if (!mode) return std::nullopt;

    HostDataRoute route;
    route.id = view->config.id;
    route.role = view->config.role;
    route.mode = *mode;
    route.data = view->app.data;
    return route;
}

std::optional<HostViewRoute>
HostViewRuntimeRegistry::Impl::GetViewRoute(
    const HostViewTarget& target) const
{
    const auto* view = GetViewBySelector(target);
    if (!view || !view->app.view
        || !view->interaction.update || !view->context) {
        return std::nullopt;
    }

    HostViewRoute route;
    route.id = view->config.id;
    route.role = view->config.role;
    route.view = view->app.view;
    route.update = view->interaction.update;
    route.context = view->context;
    const std::weak_ptr<IHostViewDirectory> directory = m_directory;
    const std::string viewId = view->config.id;
    route.stopView = [directory, viewId]() {
        const auto current = directory.lock();
        return current && current->StopView(viewId);
    };
    return route;
}

std::weak_ptr<AppSessionPort>
HostViewRuntimeRegistry::Impl::GetSessionPort() const
{
    const auto* view = GetPrimaryView();
    return view && view->isAvailable
        ? std::weak_ptr<AppSessionPort>(view->app.session)
        : std::weak_ptr<AppSessionPort>{};
}

bool HostViewRuntimeRegistry::Impl::StopView(
    const std::string_view viewId)
{
    const auto found = std::find_if(
        m_views.begin(),
        m_views.end(),
        [viewId](const HostRenderViewRuntime& view) {
            return view.config.id.size() == viewId.size()
                && std::equal(
                    view.config.id.begin(),
                    view.config.id.end(),
                    viewId.begin());
        });
    if (found == m_views.end() || !found->isAvailable) return false;

    if (found->context && !found->context->StopInput()) {
        return false;
    }
    found->isAvailable = false;
    const auto index = static_cast<std::size_t>(found - m_views.begin());
    m_frameRuntime->SetViewUnavailable(index);
    return true;
}

LoadCommitResult HostViewRuntimeRegistry::Impl::SetLoadCommit(
    const LoadEventKind loadKind,
    const std::uint64_t transactionRevision,
    const VtkImageGridSnapshot& pending)
{
    if (!m_loadCommit || m_views.empty()) {
        return {};
    }

    // 显式文件加载取消尚未发布的 Feature 输入候选；晚到的 Feature poll 不重新启动它。
    if (m_featureTransition) {
        (void)StopDataTransition(m_featureTransition->ownerId, m_featureTransition->request.requestId);
    }
    LoadCommitRequest request;
    request.loadKind = loadKind;
    request.transactionRevision = transactionRevision;
    request.sourceRevision = pending && pending->data
        ? pending->data->self : DataRevisionRef{};
    request.pending = pending;
    request.stages.reserve(m_views.size());
    for (const auto& view : m_views) {
        if (!view.isAvailable || !view.dataStage) {
            return {};
        }
        request.stages.push_back(view.dataStage);
    }
    request.stopViews = [this]() {
        bool isStopped = true;
        for (auto& view : m_views) {
            view.isAvailable = false;
            if (view.context) {
                isStopped = view.context->StopInput() && isStopped;
            }
        }
        return isStopped;
    };
    return m_loadCommit->SetLoadCommit(request);
}

FeatureDataTransitionState HostViewRuntimeRegistry::Impl::StartDataTransition(
    const std::uint64_t ownerId, FeatureDataTransitionRequest request)
{
    const auto data = m_transitionData.lock();
    const auto state = m_transitionState.lock();
    if (!m_lease || !m_lease->GetIsOwnerThread() || !m_lease->GetIsActive()
        || !ownerId || !request.requestId || !request.input || !request.input->data
        || !request.input->binding || !request.commit || !m_loadCommit || m_loadCommit->GetIsPending()
        || m_featureTransition || !data || !state || m_views.empty()) return {};
    // primary 是 Session 级输入，必须准备全部必要 View，不能只切 Feature 的局部目标。
    const auto primary = std::find_if(request.transaction.bindings.begin(), request.transaction.bindings.end(),
        [](const auto& binding) { return binding.binding == primaryVolumeBinding; });
    if (request.transaction.policy != DataPublishPolicy::RequireCurrentInputs
        || primary == request.transaction.bindings.end() || !primary->isTargetChecked
        || primary->target != std::optional<DataRevisionRef>{request.input->data->self}
        || request.input->binding->name != primaryVolumeBinding
        || request.input->binding->target != primary->target
        || primary->expectedRevision == std::numeric_limits<DataBindingRevision>::max()
        || request.input->binding->revision != primary->expectedRevision + 1) return {};
    auto transition = std::make_shared<FeatureTransition>();
    transition->ownerId = ownerId;
    transition->request = std::move(request);
    const auto canonical = data->GetData(data->GetDataGraph(), transition->request.input->data->self);
    if (!canonical || canonical->payload != transition->request.input->data->payload) return {};
    if (GetDataEntityIdValid(canonical->lifetimeScope)) {
        const auto lifetime = canonical->lifetime.lock();
        transition->sourceLease = lifetime ? lifetime->StartResourceUse(canonical->self, "input-transition") : nullptr;
        if (!transition->sourceLease) return {FeatureRunStatus::Failed, DataCommitFailure::ResultRetired, {}};
    }
    const auto payload = std::dynamic_pointer_cast<const ImageGrid3DPayload>(transition->request.input->data->payload);
    if (!payload || !payload->GetValid()) return {};
    transition->ready.dataRevision = transition->request.input->data->self;
    transition->ready.bindingRevision = transition->request.input->binding->revision;
    transition->ready.scalarRange = payload->GetScalarRange();
    transition->ready.spacing = payload->GetGeometry().spacing;
    transition->ready.cursorWorld = state->GetCursorWorld();
    transition->load.ownerId = ownerId;
    transition->load.transactionRevision = transition->request.requestId;
    transition->load.sourceRevision = transition->request.input->data->self;
    transition->load.pending = transition->request.input;
    for (const auto& view : m_views) {
        if (!view.isAvailable || !view.dataStage) return {};
        transition->load.stages.push_back(view.dataStage);
    }
    transition->load.stopViews = [this] {
        bool stopped = true;
        for (auto& view : m_views) {
            view.isAvailable = false;
            if (view.context) stopped = view.context->StopInput() && stopped;
        }
        return stopped;
    };
    const std::weak_ptr<FeatureTransition> weak = transition;
    transition->load.onPublish = [weak, data, state] {
        const auto active = weak.lock();
        if (!active) return false;
        auto result = data->SetDataCommit(std::move(active->request.transaction));
        active->state.commitFailure = result.failureReason;
        active->state.blockers = std::move(result.blockers);
        if (result.status != DataCommitStatus::Succeeded) return false;
        active->request.commit->SetCommit();
        state->SetImageDataReady(active->ready);
        return true;
    };
    transition->state = {FeatureRunStatus::Preparing, DataCommitFailure::None, {}};
    m_featureTransition = transition;
    transition->isAdvancing = true;
    const auto result = m_loadCommit->SetLoadCommit(transition->load);
    transition->isAdvancing = false;
    if (result.status != LoadCommitStatus::Preparing) {
        m_featureTransition.reset();
        return {};
    }
    return transition->state;
}

FeatureDataTransitionState HostViewRuntimeRegistry::Impl::SetDataTransition(
    const std::uint64_t ownerId, const std::uint64_t requestId)
{
    if (!m_lease || !m_lease->GetIsOwnerThread() || !m_lease->GetIsActive() || !m_loadCommit || !m_featureTransition
        || m_featureTransition->ownerId != ownerId || m_featureTransition->request.requestId != requestId) {
        return {FeatureRunStatus::Cancelled, DataCommitFailure::None, {}};
    }
    const auto active = m_featureTransition;
    if (active->isAdvancing) return {FeatureRunStatus::Running, DataCommitFailure::None, {}};
    struct AdvanceGuard final {
        bool& value;
        explicit AdvanceGuard(bool& flag) : value(flag) { value = true; }
        ~AdvanceGuard() { value = false; }
    } guard(active->isAdvancing);
    const auto data = m_transitionData.lock();
    if (!data) return {};
    auto batch = data->StartDataChanges();
    if (!batch) return {};
    const auto result = m_loadCommit->SetLoadCommit(active->load);
    if (result.status == LoadCommitStatus::Preparing) return active->state;
    active->state.status = result.status == LoadCommitStatus::Succeeded ? FeatureRunStatus::Succeeded
        : result.status == LoadCommitStatus::Cancelled ? FeatureRunStatus::Cancelled : FeatureRunStatus::Failed;
    m_featureTransition.reset();
    // participant 已在 graph 交换之后、此 batch 的通知之前无失败接管状态。
    return std::move(active->state);
}

FeatureDataTransitionState HostViewRuntimeRegistry::Impl::StopDataTransition(
    const std::uint64_t ownerId, const std::uint64_t requestId)
{
    if (!m_lease || !m_lease->GetIsOwnerThread()) return {};
    if (!m_featureTransition || m_featureTransition->ownerId != ownerId
        || m_featureTransition->request.requestId != requestId) {
        return {FeatureRunStatus::Cancelled, DataCommitFailure::None, {}};
    }
    if (!m_loadCommit) return {};
    if (m_featureTransition->isAdvancing) return {FeatureRunStatus::Running, DataCommitFailure::None, {}};
    (void)m_loadCommit->SetLoadCancelled(requestId, LoadCommitFailure::Cancelled, ownerId);
    m_featureTransition.reset();
    return {FeatureRunStatus::Cancelled, DataCommitFailure::None, {}};
}

LoadCommitResult HostViewRuntimeRegistry::Impl::SetLoadCancelled(
    const std::uint64_t transactionRevision,
    const LoadCommitFailure failureReason)
{
    return m_loadCommit
        ? m_loadCommit->SetLoadCancelled(
            transactionRevision, failureReason)
        : LoadCommitResult{};
}

std::vector<HostInputRoute>
HostViewRuntimeRegistry::Impl::GetInputRouteValues(
    const HostViewTargets& targets) const
{
    std::vector<HostInputRoute> routes;
    const auto views = GetViewsByTargets(targets);
    routes.reserve(views.size());
    for (const auto* view : views) {
        if (!view || !view->context) continue;
        HostInputRoute route;
        route.id = view->config.id;
        route.role = view->config.role;
        route.inputMode = view->config.inputMode;
        route.context = view->context;
        routes.push_back(std::move(route));
    }
    return routes;
}

const HostRenderViewRuntime*
HostViewRuntimeRegistry::Impl::GetPrimaryView() const
{
    if (const auto* view = GetFirstViewByRole(
            HostRenderViewRole::Primary3D)) {
        return view;
    }
    for (const auto& view : m_views) {
        if (view.isAvailable
            && GetRoleIs3DView(view.config.role)) {
            return &view;
        }
    }
    for (const auto& view : m_views) {
        if (view.isAvailable) return &view;
    }
    return nullptr;
}

const HostRenderViewRuntime*
HostViewRuntimeRegistry::Impl::GetStandaloneStartView() const
{
    for (const auto& view : m_views) {
        if (view.isAvailable
            && view.config.isEventLoopEnabled) {
            return &view;
        }
    }
    return nullptr;
}

std::vector<const HostRenderViewRuntime*>
HostViewRuntimeRegistry::Impl::GetViewsByTargets(
    const HostViewTargets& targets) const
{
    std::vector<const HostRenderViewRuntime*> selected;
    selected.reserve(m_views.size());
    for (const auto& view : m_views) {
        if (!view.isAvailable) continue;
        const bool isIdSelected = !targets.viewIds.empty()
            && std::find(
                targets.viewIds.begin(),
                targets.viewIds.end(),
                view.config.id) != targets.viewIds.end();
        const bool isRoleSelected = !targets.viewRoles.empty()
            && std::find(
                targets.viewRoles.begin(),
                targets.viewRoles.end(),
                view.config.role) != targets.viewRoles.end();
        if (isIdSelected || isRoleSelected) {
            selected.push_back(&view);
        }
    }
    return selected;
}

const HostViewRuntimeRegistry::Impl::ViewLeasePorts*
HostViewRuntimeRegistry::Impl::GetLeasePorts(
    const HostRenderViewRuntime* view) const
{
    if (!view) return nullptr;
    for (std::size_t index = 0; index < m_views.size(); ++index) {
        if (&m_views[index] == view
            && index < m_leasePorts.size()) {
            return &m_leasePorts[index];
        }
    }
    return nullptr;
}

std::vector<HostFeatureView>
HostViewRuntimeRegistry::Impl::GetFeatureViews(
    const HostViewTargets& targets) const
{
    std::vector<HostFeatureView> result;
    if (!m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return result;
    }

    const auto views = GetViewsByTargets(targets);
    result.reserve(views.size());
    for (const auto* view : views) {
        if (!view || view->config.id.empty()) {
            continue;
        }
        HostFeatureView value;
        value.id = view->config.id;
        value.role = view->config.role;
        result.push_back(std::move(value));
    }
    return result;
}

std::shared_ptr<FeatureViewService>
HostViewRuntimeRegistry::Impl::GetFeaturePort(
    const std::string& viewId) const
{
    if (viewId.empty() || !m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return nullptr;
    }
    const auto* ports = GetLeasePorts(GetViewById(viewId));
    return ports ? ports->feature : nullptr;
}

std::shared_ptr<OverlayService>
HostViewRuntimeRegistry::Impl::GetOverlayPort(
    const std::string& viewId) const
{
    if (viewId.empty() || !m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return nullptr;
    }
    const auto* ports = GetLeasePorts(GetViewById(viewId));
    return ports ? ports->overlay : nullptr;
}

std::optional<HostInputView>
HostViewRuntimeRegistry::Impl::GetInputView(
    const HostViewTarget& target) const
{
    if (!m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return std::nullopt;
    }
    const auto* view = GetViewBySelector(target);
    if (!view || !view->context || view->config.id.empty()) {
        return std::nullopt;
    }

    HostInputView input;
    input.view.id = view->config.id;
    input.view.role = view->config.role;
    input.renderer = view->context->GetRenderer();
    input.interactor = view->context->GetInteractor();
    input.lease = m_lease;
    if (!input.renderer || !input.interactor) {
        return std::nullopt;
    }
    return input;
}

bool HostViewRuntimeRegistry::Impl::SetFeatureViews(
    const std::string& featureId,
    const std::vector<std::string>& viewIds)
{
    if (featureId.empty() || !m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }

    std::vector<std::shared_ptr<AppFeaturePort>> nextViews;
    nextViews.reserve(viewIds.size());
    for (const auto& viewId : viewIds) {
        const auto* view = GetViewById(viewId);
        if (viewId.empty() || !view || !view->app.feature) {
            return false;
        }
        if (std::find(
                nextViews.begin(),
                nextViews.end(),
                view->app.feature) == nextViews.end()) {
            nextViews.push_back(view->app.feature);
        }
    }

    const auto current = m_featureViews.find(featureId);
    const auto currentViews = current == m_featureViews.end()
        ? std::vector<std::shared_ptr<AppFeaturePort>>{}
        : current->second;
    auto activeViews = currentViews;

    // 先取得所有受影响 View 的 frame admission，再改变 Feature active 状态。
    // 后续状态事务失败只会留下一个无害的额外 dirty，不会出现“返回 false
    // 但 active 状态已经部分提交且没有重绘门铃”。
    for (auto& view : m_views) {
        const bool wasActive = std::find(
            currentViews.begin(), currentViews.end(), view.app.feature)
            != currentViews.end();
        const bool willBeActive = std::find(
            nextViews.begin(), nextViews.end(), view.app.feature)
            != nextViews.end();
        if (wasActive != willBeActive
            && (!view.interaction.update
                || !view.interaction.update->SetRenderNeeded())) {
            return false;
        }
    }

    const FeatureSource source{ featureId };
    std::vector<std::shared_ptr<AppFeaturePort>> removedViews;
    std::vector<std::shared_ptr<AppFeaturePort>> addedViews;
    const auto setStoredViews = [this, &featureId](
        std::vector<std::shared_ptr<AppFeaturePort>> views) {
        if (views.empty()) {
            m_featureViews.erase(featureId);
        }
        else {
            m_featureViews[featureId] = std::move(views);
        }
    };

    for (const auto& port : currentViews) {
        if (!port
            || std::find(nextViews.begin(), nextViews.end(), port)
                != nextViews.end()) {
            continue;
        }
        if (!port->SetFeatureActive(source, false)) {
            // 回滚也可能被端口拒绝；只记录已确认成功的状态转换，
            // 保证注册表始终反映端口的最后已知状态，供后续重试。
            for (auto removed = removedViews.rbegin();
                removed != removedViews.rend(); ++removed) {
                if ((*removed)->SetFeatureActive(source, true)
                    && std::find(
                        activeViews.begin(),
                        activeViews.end(),
                        *removed) == activeViews.end()) {
                    activeViews.push_back(*removed);
                }
            }
            setStoredViews(std::move(activeViews));
            return false;
        }
        activeViews.erase(
            std::remove(activeViews.begin(), activeViews.end(), port),
            activeViews.end());
        removedViews.push_back(port);
    }
    for (const auto& port : nextViews) {
        if (!port
            || std::find(currentViews.begin(), currentViews.end(), port)
                != currentViews.end()) {
            continue;
        }
        if (!port->SetFeatureActive(source, true)) {
            for (auto added = addedViews.rbegin();
                added != addedViews.rend(); ++added) {
                if ((*added)->SetFeatureActive(source, false)) {
                    activeViews.erase(
                        std::remove(
                            activeViews.begin(),
                            activeViews.end(),
                            *added),
                        activeViews.end());
                }
            }
            for (auto removed = removedViews.rbegin();
                removed != removedViews.rend(); ++removed) {
                if ((*removed)->SetFeatureActive(source, true)
                    && std::find(
                        activeViews.begin(),
                        activeViews.end(),
                        *removed) == activeViews.end()) {
                    activeViews.push_back(*removed);
                }
            }
            setStoredViews(std::move(activeViews));
            return false;
        }
        activeViews.push_back(port);
        addedViews.push_back(port);
    }

    setStoredViews(std::move(nextViews));
    return true;
}

std::vector<std::string>
HostViewRuntimeRegistry::Impl::GetFeatureViewIds(
    const std::string& featureId) const
{
    std::vector<std::string> viewIds;
    if (featureId.empty() || !m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return viewIds;
    }
    const auto current = m_featureViews.find(featureId);
    if (current == m_featureViews.end()) return viewIds;

    viewIds.reserve(current->second.size());
    for (const auto& port : current->second) {
        const auto view = std::find_if(
            m_views.begin(), m_views.end(),
            [&port](const HostRenderViewRuntime& currentView) {
                return currentView.app.feature == port;
            });
        if (view != m_views.end()) {
            viewIds.push_back(view->config.id);
        }
    }
    return viewIds;
}

bool HostViewRuntimeRegistry::Impl::SetViewStatus(
    const std::vector<std::string>& viewIds,
    const std::string& status) const
{
    if (viewIds.empty() || !m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }

    std::vector<const HostRenderViewRuntime*> views;
    views.reserve(viewIds.size());
    for (const auto& viewId : viewIds) {
        const auto* view = GetViewById(viewId);
        if (viewId.empty() || !view || !view->context) {
            return false;
        }
        if (std::find(views.begin(), views.end(), view)
            == views.end()) {
            views.push_back(view);
        }
    }
    for (const auto* view : views) {
        if (m_isHostDriven && view->config.renderWindow) continue;
        std::string title = view->config.window.title;
        if (!status.empty()) {
            if (!title.empty()) title += " | ";
            title += status;
        }
        if (!view->context->SetWindowTitle(title)) return false;
    }
    return true;
}

bool HostViewRuntimeRegistry::Impl::SetViewWindow(
    const std::string& viewId,
    vtkSmartPointer<vtkRenderWindow> renderWindow)
{
    if (viewId.empty() || !renderWindow || !m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }
    for (auto& view : m_views) {
        if (view.config.id == viewId) {
            return view.interaction.update
                && view.interaction.update->SetRenderNeeded()
                && SetViewWindow(view, std::move(renderWindow));
        }
    }
    return false;
}

bool HostViewRuntimeRegistry::Impl::SetTimerHandler(
    const HostViewTarget& target,
    std::function<void()> handler) const
{
    if (!handler || !m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }
    const auto* view = GetViewBySelector(target);
    return view && view->isAvailable && view->context
        && view->context->SetTimerHandler(std::move(handler));
}

bool HostViewRuntimeRegistry::Impl::ClearTimerHandler(
    const HostViewTarget& target) const
{
    if (!m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }
    const auto* view = GetViewBySelector(target);
    return view && view->context
        && view->context->ClearTimerHandler();
}

bool HostViewRuntimeRegistry::Impl::SetFrameHandlers(
    std::function<void()> handler) const
{
    if (!handler || !m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }

    std::vector<const HostRenderViewRuntime*> installed;
    installed.reserve(m_views.size());
    try {
        for (const auto& view : m_views) {
            if (!view.isAvailable || !view.context
                || !view.context->SetTimerHandler(handler)) {
                for (auto current = installed.rbegin();
                    current != installed.rend(); ++current) {
                    (void)(*current)->context->ClearTimerHandler();
                }
                return false;
            }
            installed.push_back(&view);
        }
    }
    catch (...) {
        for (auto current = installed.rbegin();
            current != installed.rend(); ++current) {
            (void)(*current)->context->ClearTimerHandler();
        }
        return false;
    }
    return installed.size() == m_views.size();
}

bool HostViewRuntimeRegistry::Impl::SetInputsEnabled(
    const bool isEnabled) const
{
    if (!m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }
    bool isSet = true;
    for (const auto& view : m_views) {
        try {
            if (!view.context) {
                if (view.isAvailable) isSet = false;
                continue;
            }
            // unavailable View 已由 StopView 关闭业务输入；开启阶段跳过，
            // 关闭阶段仍重复关门以保持 Stop 幂等。
            if (!view.isAvailable && isEnabled) continue;
            if (!view.context->SetInputEnabled(isEnabled)) {
                isSet = false;
            }
        }
        catch (...) {
            isSet = false;
        }
    }
    if (!isSet && isEnabled) {
        // 任一 View 开启失败时，对全部 View 做补偿关闭；失败的实现也可能
        // 在返回 false/抛异常前发生了局部副作用，不能只回滚成功前缀。
        for (const auto& view : m_views) {
            try {
                if (view.context) {
                    (void)view.context->SetInputEnabled(false);
                }
            }
            catch (...) {
            }
        }
    }
    return isSet;
}

bool HostViewRuntimeRegistry::Impl::SetFrameGeneration(std::uint64_t generation)
{
    return m_frameRuntime->SetFrameGeneration(generation);
}

bool HostViewRuntimeRegistry::Impl::SetFrameIntents(const std::vector<HostFrameIntent>& intents)
{
    return m_frameRuntime->SetFrameIntents(intents);
}

bool HostViewRuntimeRegistry::Impl::CollectFrameUpdates()
{
    return m_frameRuntime->CollectFrameUpdates();
}

bool HostViewRuntimeRegistry::Impl::ApplyFrameUpdates()
{
    return m_frameRuntime->ApplyFrameUpdates();
}

HostFrameStageStatus HostViewRuntimeRegistry::Impl::BuildFrameStage(std::uint64_t epoch)
{
    return m_frameRuntime->BuildFrameStage(epoch);
}

void HostViewRuntimeRegistry::Impl::SetFrameCommit(std::uint64_t epoch) noexcept
{
    m_frameRuntime->SetFrameCommit(epoch);
}

bool HostViewRuntimeRegistry::Impl::SendFrameRender(std::uint64_t epoch)
{
    return m_frameRuntime->SendFrameRender(epoch);
}


std::vector<std::string>
HostViewRuntimeRegistry::Impl::GetRenderViewIds() const
{
    return m_frameRuntime->GetRenderViewIds();
}

HostRenderResult HostViewRuntimeRegistry::Impl::SendFrameRender(
    const HostRenderRequest& request,
    const std::function<bool()>& getIsRunning)
{
    return m_frameRuntime->SendFrameRender(request, getIsRunning);
}

bool HostViewRuntimeRegistry::Impl::GetFrameRenderPending() const noexcept
{
    return m_frameRuntime->GetFrameRenderPending();
}

void HostViewRuntimeRegistry::Impl::SendFrameCompletions() noexcept
{
    m_frameRuntime->SendFrameCompletions();
}

void HostViewRuntimeRegistry::Impl::ClearFrameStage() noexcept
{
    m_frameRuntime->ClearFrameStage();
}

bool HostViewRuntimeRegistry::Impl::SetModelMatrix(
    const HostViewTarget& target,
    const std::array<double, 16>& modelToWorld) const
{
    if (!m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }
    const auto* view = GetViewBySelector(target);
    return view && view->isAvailable && view->interaction.model
        && view->interaction.model->SetModelMatrix(modelToWorld);
}

bool HostViewRuntimeRegistry::Impl::SendViewUpdates(
    const HostViewTarget& target) const
{
    if (!m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }
    const auto* view = GetViewBySelector(target);
    return view && view->isAvailable && view->interaction.update
        && view->interaction.update->SendUpdates();
}

bool HostViewRuntimeRegistry::Impl::StartStandaloneView() const
{
    if (!m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }
    const auto* view = GetStandaloneStartView();
    return !view || (view->context && view->context->Start());
}

std::shared_ptr<AppTaskExecutor>
HostViewRuntimeRegistry::Impl::GetTaskExecutor() const
{
    return m_lease && m_lease->GetIsActive()
        && m_lease->GetIsOwnerThread()
        ? m_taskExecutor : nullptr;
}

bool HostViewRuntimeRegistry::Impl::StopLease()
{
    if (m_lease && !m_lease->GetIsOwnerThread()) return false;
    if (m_frameRuntime->GetIsBusy()) return false;
    if (!m_lease) {
        m_frameRuntime->Clear();
        m_leasePorts.clear();
        m_views.clear();
        m_loadCommit.reset();
        m_renderServices.reset();
        m_taskExecutor.reset();
        return true;
    }
    if (!m_featureViews.empty()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }

    if (m_featureTransition) {
        (void)StopDataTransition(m_featureTransition->ownerId, m_featureTransition->request.requestId);
    }
    // 所有 lane 先同时收到取消，再共用同一个绝对 deadline；超时保留整个 aggregate。
    constexpr auto taskStopLimit = std::chrono::seconds(2);
    const auto taskDeadline =
        std::chrono::steady_clock::now() + taskStopLimit;
    bool areTasksStopping = !m_renderServices
        || !m_renderServices->resources
        || m_renderServices->resources->StartStop();
    for (auto& view : m_views) {
        areTasksStopping = view.taskControl
            && view.taskControl->SetTaskStopping()
            && areTasksStopping;
    }
    if (!areTasksStopping) return false;
    bool areTasksStopped = true;
    for (auto& view : m_views) {
        areTasksStopped = view.taskControl->StopTasks(
            taskDeadline) && areTasksStopped;
    }
    if (m_renderServices && m_renderServices->resources) {
        areTasksStopped = m_renderServices->resources->Stop(
            taskDeadline) && areTasksStopped;
    }
    if (!areTasksStopped) return false;

    // StopTasks 已把所有未派发 completion 降级为取消终态；在 runtime
    // 与 callback 槽销毁前完成最后一次 owner-thread drain。
    SendFrameCompletions();

    bool areInputsStopped = true;
    for (auto& view : m_views) {
        areInputsStopped = view.context
            && view.context->StopInput()
            && areInputsStopped;
    }
    // StopPending 绝不恢复输入；全部 View 都已收到关闭 gate 与清理请求，
    // 失败项由同一 owner thread 重试完整 StopLease。
    if (!areInputsStopped) return false;

    // 先确认 lease 已停止，再提交 wrapper/runtime 释放；即使以后
    // StopLease 增加新的失败条件，也不会留下半清理状态。
    if (!m_lease->StopLease()) return false;
    m_frameRuntime->Clear();
    m_leasePorts.clear();
    m_lease.reset();

    // 释放顺序固定为 context → App/Interaction/Render/Feature adapters。
    for (auto& view : m_views) view.context.reset();
    for (auto& view : m_views) view.dataStage.reset();
    for (auto& view : m_views) view.app = {};
    for (auto& view : m_views) view.interaction = {};
    for (auto& view : m_views) view.renderBind.reset();
    for (auto& view : m_views) view.featureView.reset();
    for (auto& view : m_views) view.overlay.reset();
    for (auto& view : m_views) view.taskControl.reset();
    m_views.clear();
    m_loadCommit.reset();
    m_renderServices.reset();
    m_taskExecutor.reset();

    return true;
}

bool HostViewRuntimeRegistry::Impl::StopRoutes()
{
    return !m_directory || m_directory->StopOwner();
}

bool HostViewRuntimeRegistry::Impl::SetInitialVisibility() const
{
    bool isSet = true;
    for (const auto& view : m_views) {
        if (!view.isAvailable || !view.app.view) {
            isSet = false;
            continue;
        }

        AppVisibilityUpdate visibility;
        if (GetRoleIs3DView(view.config.role)) {
            visibility.isPlanes3DVisible = false;
            visibility.isRulerVisible = false;
        }
        if (GetRoleIsSliceView(view.config.role)) {
            visibility.isCrosshairVisible = true;
        }
        AppViewUpdate update;
        update.visibility = visibility;
        if (!view.app.view->SendViewUpdate(update)) isSet = false;
    }
    return isSet;
}

bool HostViewRuntimeRegistry::Impl::SendRenderAll() const
{
    bool isSent = true;
    for (const auto& view : m_views) {
        if (!view.isAvailable || !view.context
            || !view.context->SendRender()) {
            isSent = false;
        }
    }
    return isSent;
}

bool HostViewRuntimeRegistry::Impl::SetInteractorsReady() const
{
    bool isReady = true;
    for (const auto& view : m_views) {
        if (!view.isAvailable || !view.context
            || !view.context->SetInteractorReady()) {
            isReady = false;
        }
    }
    return isReady;
}

std::vector<HostRenderViewEndpoint>
HostViewRuntimeRegistry::Impl::BuildEndpoints() const
{
    std::vector<HostRenderViewEndpoint> endpoints;
    endpoints.reserve(m_views.size());
    for (const auto& view : m_views) {
        if (!view.isAvailable || !view.context) continue;
        HostRenderViewEndpoint endpoint;
        endpoint.id = view.config.id;
        endpoint.role = view.config.role;
        endpoint.renderer = view.context->GetRenderer();
        endpoint.renderWindow = view.context->GetRenderWindow();
        endpoint.interactor = view.context->GetInteractor();
        if (endpoint.renderer && endpoint.renderWindow
            && endpoint.interactor) {
            endpoints.push_back(endpoint);
        }
    }
    return endpoints;
}

bool HostViewRuntimeRegistry::Impl::GetRoleIs3DView(
    const HostRenderViewRole role) const
{
    return role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D;
}

bool HostViewRuntimeRegistry::Impl::GetRoleIsSliceView(
    const HostRenderViewRole role) const
{
    return role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice;
}

HostViewRuntimeRegistry::HostViewRuntimeRegistry()
    : m_impl(std::make_unique<Impl>())
{
}

HostViewRuntimeRegistry::~HostViewRuntimeRegistry()
{
    if (!m_impl) return;
    (void)m_impl->StopRoutes();
    if (!m_impl->StopLease()) {
        std::cerr
            << "[Host] ViewSet destroyed without owner-thread StopLease.\n";
    }
}
HostViewRuntimeRegistry::HostViewRuntimeRegistry(
    HostViewRuntimeRegistry&&) noexcept = default;
HostViewRuntimeRegistry& HostViewRuntimeRegistry::operator=(
    HostViewRuntimeRegistry&& other) noexcept
{
    if (this == &other) return *this;
    if (m_impl) {
        (void)m_impl->StopRoutes();
        if (!m_impl->StopLease()) {
            std::cerr
                << "[Host] ViewSet move replaced a runtime before StopLease.\n";
        }
    }
    m_impl = std::move(other.m_impl);
    return *this;
}

bool HostViewRuntimeRegistry::Build(
    const HostCoreServices& core,
    const std::vector<HostRenderViewConfig>& configs)
{
    return m_impl && m_impl->Build(core, configs);
}

std::optional<HostRenderViewState>
HostViewRuntimeRegistry::GetViewState(
    const HostViewTarget& target) const
{
    return m_impl
        ? m_impl->GetViewState(target)
        : std::nullopt;
}

std::vector<HostRenderViewState>
HostViewRuntimeRegistry::GetViewStates() const
{
    return m_impl
        ? m_impl->GetViewStates()
        : std::vector<HostRenderViewState>{};
}

std::optional<HostSceneViewState>
HostViewRuntimeRegistry::GetSceneViewState(
    const HostViewTarget& target)
{
    return m_impl
        ? m_impl->GetSceneViewState(target)
        : std::nullopt;
}

std::vector<HostSceneViewState>
HostViewRuntimeRegistry::GetSceneViewStates() const
{
    return m_impl
        ? m_impl->GetSceneViewStates()
        : std::vector<HostSceneViewState>{};
}

std::weak_ptr<IHostViewDirectory>
HostViewRuntimeRegistry::GetViewDirectory() const
{
    return m_impl
        ? m_impl->GetViewDirectory()
        : std::weak_ptr<IHostViewDirectory>{};
}

std::vector<HostFeatureView>
HostViewRuntimeRegistry::GetFeatureViews(
    const HostViewTargets& targets) const
{
    return m_impl
        ? m_impl->GetFeatureViews(targets)
        : std::vector<HostFeatureView>{};
}

std::shared_ptr<FeatureViewService>
HostViewRuntimeRegistry::GetFeaturePort(
    const std::string& viewId) const
{
    return m_impl
        ? m_impl->GetFeaturePort(viewId)
        : nullptr;
}

std::shared_ptr<OverlayService>
HostViewRuntimeRegistry::GetOverlayPort(
    const std::string& viewId) const
{
    return m_impl
        ? m_impl->GetOverlayPort(viewId)
        : nullptr;
}

std::optional<HostInputView>
HostViewRuntimeRegistry::GetInputView(
    const HostViewTarget& target) const
{
    return m_impl
        ? m_impl->GetInputView(target)
        : std::nullopt;
}

bool HostViewRuntimeRegistry::SetFeatureViews(
    const std::string& featureId,
    const std::vector<std::string>& viewIds)
{
    return m_impl
        && m_impl->SetFeatureViews(featureId, viewIds);
}

std::vector<std::string>
HostViewRuntimeRegistry::GetFeatureViewIds(
    const std::string& featureId) const
{
    return m_impl
        ? m_impl->GetFeatureViewIds(featureId)
        : std::vector<std::string>{};
}

bool HostViewRuntimeRegistry::SetViewStatus(
    const std::vector<std::string>& viewIds,
    const std::string& status) const
{
    return m_impl
        && m_impl->SetViewStatus(viewIds, status);
}

bool HostViewRuntimeRegistry::SetViewWindow(
    const std::string& viewId,
    vtkSmartPointer<vtkRenderWindow> renderWindow)
{
    return m_impl
        && m_impl->SetViewWindow(
            viewId, std::move(renderWindow));
}

bool HostViewRuntimeRegistry::SetTimerHandler(
    const HostViewTarget& target,
    std::function<void()> handler) const
{
    return m_impl
        && m_impl->SetTimerHandler(target, std::move(handler));
}

bool HostViewRuntimeRegistry::ClearTimerHandler(
    const HostViewTarget& target) const
{
    return m_impl && m_impl->ClearTimerHandler(target);
}

bool HostViewRuntimeRegistry::SetFrameHandlers(
    std::function<void()> handler) const
{
    return m_impl
        && m_impl->SetFrameHandlers(std::move(handler));
}

bool HostViewRuntimeRegistry::SetInputsEnabled(
    const bool isEnabled) const
{
    return m_impl && m_impl->SetInputsEnabled(isEnabled);
}

bool HostViewRuntimeRegistry::SetFrameGeneration(
    const std::uint64_t sessionGeneration)
{
    return m_impl
        && m_impl->SetFrameGeneration(sessionGeneration);
}

bool HostViewRuntimeRegistry::SetFrameIntents(
    const std::vector<HostFrameIntent>& intents)
{
    return m_impl && m_impl->SetFrameIntents(intents);
}

HostFrameStageStatus HostViewRuntimeRegistry::BuildFrameStage(
    const std::uint64_t nextEpoch)
{
    return m_impl
        ? m_impl->BuildFrameStage(nextEpoch)
        : HostFrameStageStatus::Failed;
}

bool HostViewRuntimeRegistry::ApplyFrameUpdates()
{
    return m_impl && m_impl->ApplyFrameUpdates();
}

bool HostViewRuntimeRegistry::CollectFrameUpdates()
{
    return m_impl && m_impl->CollectFrameUpdates();
}

void HostViewRuntimeRegistry::SetFrameCommit(
    const std::uint64_t epoch) noexcept
{
    if (!m_impl) std::terminate();
    m_impl->SetFrameCommit(epoch);
}

bool HostViewRuntimeRegistry::SendFrameRender(
    const std::uint64_t epoch)
{
    return m_impl && m_impl->SendFrameRender(epoch);
}

bool HostViewRuntimeRegistry::GetFrameRenderPending() const noexcept
{
    return m_impl && m_impl->GetFrameRenderPending();
}

void HostViewRuntimeRegistry::SendFrameCompletions() noexcept
{
    if (m_impl) m_impl->SendFrameCompletions();
}

void HostViewRuntimeRegistry::ClearFrameStage() noexcept
{
    if (m_impl) m_impl->ClearFrameStage();
}

bool HostViewRuntimeRegistry::SetModelMatrix(
    const HostViewTarget& target,
    const std::array<double, 16>& modelToWorld) const
{
    return m_impl
        && m_impl->SetModelMatrix(target, modelToWorld);
}

bool HostViewRuntimeRegistry::SendViewUpdates(
    const HostViewTarget& target) const
{
    return m_impl && m_impl->SendViewUpdates(target);
}

bool HostViewRuntimeRegistry::StartStandaloneView() const
{
    return m_impl && m_impl->StartStandaloneView();
}

std::shared_ptr<AppTaskExecutor>
HostViewRuntimeRegistry::GetTaskExecutor() const
{
    return m_impl ? m_impl->GetTaskExecutor() : nullptr;
}

bool HostViewRuntimeRegistry::StopLease()
{
    return m_impl && m_impl->StopLease();
}

bool HostViewRuntimeRegistry::SetInitialVisibility() const
{
    return m_impl && m_impl->SetInitialVisibility();
}

bool HostViewRuntimeRegistry::SendRenderAll() const
{
    return m_impl && m_impl->SendRenderAll();
}

bool HostViewRuntimeRegistry::SetInteractorsReady() const
{
    return m_impl && m_impl->SetInteractorsReady();
}

std::vector<HostRenderViewEndpoint>
HostViewRuntimeRegistry::BuildEndpoints() const
{
    return m_impl
        ? m_impl->BuildEndpoints()
        : std::vector<HostRenderViewEndpoint>{};
}

bool HostViewRuntimeRegistry::GetRoleIs3DView(
    const HostRenderViewRole role) const
{
    return m_impl && m_impl->GetRoleIs3DView(role);
}

bool HostViewRuntimeRegistry::GetRoleIsSliceView(
    const HostRenderViewRole role) const
{
    return m_impl && m_impl->GetRoleIsSliceView(role);
}

HostRenderResult HostViewRuntimeRegistry::SendFrameRender(
    const HostRenderRequest& request,
    const std::function<bool()>& getIsRunning)
{
    return m_impl ? m_impl->SendFrameRender(request, getIsRunning) : HostRenderResult{};
}

std::vector<std::string> HostViewRuntimeRegistry::GetRenderViewIds() const
{
    return m_impl ? m_impl->GetRenderViewIds() : std::vector<std::string>{};
}

FeatureDataTransitionState HostViewRuntimeRegistry::StartDataTransition(
    std::uint64_t ownerId, FeatureDataTransitionRequest request)
{
    return m_impl ? m_impl->StartDataTransition(ownerId, std::move(request)) : FeatureDataTransitionState{};
}
FeatureDataTransitionState HostViewRuntimeRegistry::SetDataTransition(std::uint64_t ownerId, std::uint64_t requestId)
{
    return m_impl ? m_impl->SetDataTransition(ownerId, requestId) : FeatureDataTransitionState{};
}
FeatureDataTransitionState HostViewRuntimeRegistry::StopDataTransition(std::uint64_t ownerId, std::uint64_t requestId)
{
    return m_impl ? m_impl->StopDataTransition(ownerId, requestId) : FeatureDataTransitionState{};
}
