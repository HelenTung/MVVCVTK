#include "AppService.h"
#include "AppDataExportTaskService.h"
#include "AppDataLoadTaskService.h"
#include "AppState.h"
#include "AppStateSyncStrategy.h"
#include "DataManager.h"
#include "InteractionComputeService.h"
#include "StrategyFactory.h"
#include <vtkSmartPointer.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

// ─────────────────────────────────────────────────────────────────────
// 构造 / 析构
// ─────────────────────────────────────────────────────────────────────
class VizService::Impl final
    : public IAppStateSyncTarget
    , public std::enable_shared_from_this<Impl> {
public:
    Impl(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state,
        std::shared_ptr<IStateEventSource> stateEventSource);
    ~Impl();

    void SetRenderContext(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren);
    void SetVizMode(VizMode mode);
    void SetMaterial(const MaterialParams& mat);
    void SetOpacity(double opacity);
    void SetTransferFunction(const std::vector<TFNode>& nodes);
    void SetIsoThreshold(double val);
    void SetBackground(const BackgroundColor& bg);
    void SetSpacing(double sx, double sy, double sz);
    void SetWindowLevel(double ww, double wc);
    void SetVisualConfig(const PreInitConfig& cfg);
    LoadState GetFileLoadState() const;
    LoadState GetReloadLoadState() const;
    void LoadFileAsync(const std::string& path,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool isSuccess)> onComplete);
    bool ReloadFromBufferAsync(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool isSuccess)> onComplete);
    void ExportDataAsync(const std::string& path,
        std::function<void(bool isSuccess)> onComplete);
    void ExportSlicesAsync(const std::string& path,
        std::optional<double> rotationAngleDeg,
        std::function<void(bool isSuccess)> onComplete);
    void StopFileLoad();
    void SetSliceScroll(int delta);
    void SetCursorWorldPosition(double worldPos[3], int axis);
    std::array<double, 3> GetCursorWorld();
    void SetInteracting(bool isInteracting);
    int GetPlaneAxis(vtkActor* actor);
    vtkProp3D* GetMainProp();
    void SetModelMatrix(vtkMatrix4x4* modelToWorldMatrix);
    std::array<double, 16> GetModelMatrix();
    WindowLevelParams GetWindowLevel() const;
    int GetNavigationAxis() const;
    void SetElementVisible(uint32_t flagBit, bool isVisible);
    void SetWindowLevelDrag(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC);
    void SetModelTransform(double translate[3], double rotate[3], double scale[3]);
    void SetModelTransformReset();
    void GetModelPositionFromWorld(const double w[3], double m[3]) const;
    void GetWorldPositionFromModel(const double m[3], double w[3]) const;
    void SendUpdates();
    bool GetDirty() const;
    void SetDirty();
    bool ResetDirty();
    void SetCurrentStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy);
    void AttachOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy);
    void RemoveOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy);
    void ClearOverlayStrategies();

private:
    void SetRenderBinding(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren);
    void SetStateObserver();
    void SendPendingImage();
    void SendExportCallback();
    bool SetLoadFailureState(bool& hasLoadEvent);
    bool SetDataRefreshState(bool& hasLoadEvent);
    void SendLoadCallbacks();
    void BuildPipeline();
    void SetStrategyState();
    void ClearLoadFail();
    RenderParams GetRenderParams(UpdateFlags flags) const;
    std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode);
    void SetRendererBg();
    void SetStrategyClear() override;
    void ClearStrategyCache();
    void SetCursorCenter() override;
    void SetSyncNeeded() override;
    void SetPendingFlags(UpdateFlags flags) override;
    void SetDataRefresh() override;
    void SetLoadFailed() override;
    void StartRun(std::packaged_task<void()> task,
        bool hasActiveLoadFuture);

    std::shared_ptr<AbstractDataManager> m_dataManager; // 当前体数据源，加载成功后由主线程提交到渲染管线
    std::shared_ptr<AbstractVisualStrategy> m_currentStrategy; // 当前主渲染策略
    vtkSmartPointer<vtkRenderer> m_renderer; // 当前渲染器，所有 VTK 节点挂接都收口到主线程
    vtkSmartPointer<vtkRenderWindow> m_renderWindow; // 当前渲染窗口，用于交互帧率控制
    std::atomic<bool> m_isDirty{ false }; // 渲染脏位：只驱动下一帧 Render
    std::atomic<bool> m_hasSyncNeed{ false }; // 同步闸门：状态事件只置位，主线程统一消费
    std::atomic<int> m_pendingFlags{ static_cast<int>(UpdateFlags::All) }; // 增量更新位图
    std::vector<std::shared_ptr<AbstractVisualStrategy>> m_overlayStrategies; // 图层叠加策略列表
    std::shared_ptr<AppDataLoadTaskService> m_dataLoadTaskService; // 加载任务构建和加载回调状态
    std::shared_ptr<AppDataExportTaskService> m_dataExportTaskService; // 导出任务构建和保存回调状态
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache; // 已构建过的 Strategy 缓存，避免同模式反复创建渲染对象
    AppStateSyncStrategy m_stateSyncStrategy; // 把状态广播翻译成“重建/清理/增量同步”动作的策略对象
    LoadEventKind m_pendingLoadEventKind = LoadEventKind::None; // 当前 service 本地待消费的 load 事件来源，避免多个窗口抢同一个 Share pending 标记
    std::shared_ptr<SharedInteractionState> m_sharedState; // 运行期单一状态真源，前处理与交互都回写到这里
    std::shared_ptr<IStateEventSource> m_stateEventSource; // 状态广播源，Service 通过它订阅 SharedState 的增量事件

    std::atomic<int> m_pendingVizModeInt{ static_cast<int>(VizMode::IsoSurface) }; // 模式切换快照：前处理阶段可先写入，主线程重建管线时再一次性读取
    std::atomic<bool> m_hasDataRefreshNeed{ false }; // 管线重建请求：DataReady/Spacing 这类结构性变化只置位，不直接重建
    std::atomic<bool> m_hasCacheClearNeed{ false }; // 缓存清理请求：Strategy Detach 涉及渲染对象，必须推迟到主线程处理
    std::atomic<bool> m_hasLoadFailure{ false }; // 失败收敛请求：把后台失败信号汇总到主线程做统一清场

    std::future<void> m_activeLoadFuture; // 当前活动加载任务的 future，用于析构时等待后台线程结束
    mutable std::mutex m_activeLoadMutex; // 保护 m_activeLoadFuture
};

VizService::Impl::Impl(
    std::shared_ptr<AbstractDataManager> dataMgr,
    std::shared_ptr<SharedInteractionState> state,
    std::shared_ptr<IStateEventSource> stateEventSource)
    : m_dataManager(std::move(dataMgr))
    , m_sharedState(std::move(state))
    , m_stateEventSource(std::move(stateEventSource))
{
    m_dataLoadTaskService = std::make_shared<AppDataLoadTaskService>(m_dataManager, m_sharedState);
    m_dataExportTaskService = std::make_shared<AppDataExportTaskService>(m_dataManager, m_sharedState);
}

VizService::Impl::~Impl()
{
    std::lock_guard<std::mutex> lk(m_activeLoadMutex);
    if (m_activeLoadFuture.valid())
        m_activeLoadFuture.wait();
}

bool VizService::Impl::GetDirty() const
{
    return m_isDirty;
}

void VizService::Impl::SetDirty()
{
    m_isDirty = true;
}

bool VizService::Impl::ResetDirty()
{
    return m_isDirty.exchange(false);
}

void VizService::Impl::SetPendingFlags(UpdateFlags flags)
{
    int old = m_pendingFlags.load();
    // compare_exchange_weak 在竞争下允许伪失败，但循环体很小，适合这里做位图 OR 合并。
    // 这样多个线程/回调同时上报更新时，只会不断把新位并进同一个原子整数，不会丢标志。
    while (!m_pendingFlags.compare_exchange_weak(
        old, old | static_cast<int>(flags))) {
    }
}

void VizService::Impl::SetSyncNeeded()
{
    // 这里只声明“下一帧需要把状态推给 Strategy”，不直接同步，保持所有渲染改动都经由 Timer 主循环收口。
    m_hasSyncNeed = true;
    m_isDirty = true;
}

void VizService::Impl::SetCurrentStrategy(
    std::shared_ptr<AbstractVisualStrategy> newStrategy)
{
    if (m_currentStrategy == newStrategy) {
        if (m_currentStrategy && m_renderer) {
            m_currentStrategy->SetCamera(m_renderer);
            m_isDirty = true;
        }
        return;
    }

    if (m_currentStrategy && m_renderer)
        m_currentStrategy->DetachRenderer(m_renderer);

    m_currentStrategy = std::move(newStrategy);

    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->AttachRenderer(m_renderer);
        m_currentStrategy->SetCamera(m_renderer);
    }
    if (m_renderer)
        m_renderer->ResetCamera();
    m_isDirty = true;
}

void VizService::Impl::AttachOverlayStrategy(
    std::shared_ptr<AbstractVisualStrategy> strategy)
{
    if (!strategy) return;

    const auto sameStrategy = std::find_if(m_overlayStrategies.begin(), m_overlayStrategies.end(),
        [strategy](const std::shared_ptr<AbstractVisualStrategy>& current) {
            return current.get() == strategy.get();
        });
    if (sameStrategy != m_overlayStrategies.end()) {
        return;
    }

    m_overlayStrategies.push_back(strategy);
    if (m_renderer) {
        strategy->AttachRenderer(m_renderer);
    }

    m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
    m_hasSyncNeed = true;
    m_isDirty = true;
}

void VizService::Impl::RemoveOverlayStrategy(
    std::shared_ptr<AbstractVisualStrategy> strategy)
{
    if (!strategy) return;

    const auto it = std::find_if(m_overlayStrategies.begin(), m_overlayStrategies.end(),
        [strategy](const std::shared_ptr<AbstractVisualStrategy>& current) {
            return current.get() == strategy.get();
        });
    if (it == m_overlayStrategies.end()) {
        return;
    }

    if (m_renderer) {
        strategy->DetachRenderer(m_renderer);
    }
    m_overlayStrategies.erase(it);
    m_isDirty = true;
}

void VizService::Impl::ClearOverlayStrategies()
{
    if (m_renderer) {
        for (auto& strategy : m_overlayStrategies) {
            strategy->DetachRenderer(m_renderer);
        }
    }
    m_overlayStrategies.clear();
    m_isDirty = true;
}

void VizService::Impl::SetStrategyClear()
{
    m_hasCacheClearNeed = true;
}

void VizService::Impl::ClearStrategyCache()
{
    // 清缓存时先把当前 Strategy 从 renderer 上摘掉，再清 overlay 和缓存表，
    // 这样可以保证下一次重建拿到的是一套完全干净的渲染节点。
    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->DetachRenderer(m_renderer);
        m_currentStrategy = nullptr;
    }

    ClearOverlayStrategies();
    m_strategyCache.clear();
}

VizService::VizService(
    std::shared_ptr<AbstractDataManager> dataMgr,
    std::shared_ptr<SharedInteractionState> state,
    std::shared_ptr<IStateEventSource> stateEventSource)
    : m_impl(std::make_shared<VizService::Impl>(
        std::move(dataMgr),
        std::move(state),
        std::move(stateEventSource)))
{
}

VizService::~VizService() = default;

void VizService::SetRenderContext(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer> ren)
{
    m_impl->SetRenderContext(std::move(win), std::move(ren));
}

void VizService::SetVizMode(VizMode mode)
{
    m_impl->SetVizMode(mode);
}

void VizService::SetMaterial(const MaterialParams& mat)
{
    m_impl->SetMaterial(mat);
}

void VizService::SetOpacity(double opacity)
{
    m_impl->SetOpacity(opacity);
}

void VizService::SetTransferFunction(const std::vector<TFNode>& nodes)
{
    m_impl->SetTransferFunction(nodes);
}

void VizService::SetIsoThreshold(double val)
{
    m_impl->SetIsoThreshold(val);
}

void VizService::SetBackground(const BackgroundColor& bg)
{
    m_impl->SetBackground(bg);
}

void VizService::SetSpacing(double sx, double sy, double sz)
{
    m_impl->SetSpacing(sx, sy, sz);
}

void VizService::SetWindowLevel(double ww, double wc)
{
    m_impl->SetWindowLevel(ww, wc);
}

void VizService::SetVisualConfig(const PreInitConfig& cfg)
{
    m_impl->SetVisualConfig(cfg);
}

LoadState VizService::GetFileLoadState() const
{
    return m_impl->GetFileLoadState();
}

LoadState VizService::GetReloadLoadState() const
{
    return m_impl->GetReloadLoadState();
}

void VizService::LoadFileAsync(
    const std::string& path,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool isSuccess)> onComplete)
{
    m_impl->LoadFileAsync(path, spacing, origin, std::move(onComplete));
}

bool VizService::ReloadFromBufferAsync(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool isSuccess)> onComplete)
{
    return m_impl->ReloadFromBufferAsync(data, dims, spacing, origin, std::move(onComplete));
}

void VizService::ExportDataAsync(
    const std::string& path,
    std::function<void(bool isSuccess)> onComplete)
{
    m_impl->ExportDataAsync(path, std::move(onComplete));
}

void VizService::ExportSlicesAsync(
    const std::string& path,
    std::optional<double> rotationAngleDeg,
    std::function<void(bool isSuccess)> onComplete)
{
    m_impl->ExportSlicesAsync(path, rotationAngleDeg, std::move(onComplete));
}

void VizService::StopFileLoad()
{
    m_impl->StopFileLoad();
}

void VizService::SetSliceScroll(int delta)
{
    m_impl->SetSliceScroll(delta);
}

void VizService::SetCursorWorldPosition(double worldPos[3], int axis)
{
    m_impl->SetCursorWorldPosition(worldPos, axis);
}

std::array<double, 3> VizService::GetCursorWorld()
{
    return m_impl->GetCursorWorld();
}

void VizService::SetInteracting(bool isInteracting)
{
    m_impl->SetInteracting(isInteracting);
}

int VizService::GetPlaneAxis(vtkActor* actor)
{
    return m_impl->GetPlaneAxis(actor);
}

vtkProp3D* VizService::GetMainProp()
{
    return m_impl->GetMainProp();
}

void VizService::SetModelMatrix(vtkMatrix4x4* modelToWorldMatrix)
{
    m_impl->SetModelMatrix(modelToWorldMatrix);
}

std::array<double, 16> VizService::GetModelMatrix()
{
    return m_impl->GetModelMatrix();
}

WindowLevelParams VizService::GetWindowLevel() const
{
    return m_impl->GetWindowLevel();
}

int VizService::GetNavigationAxis() const
{
    return m_impl->GetNavigationAxis();
}

void VizService::SetElementVisible(uint32_t flagBit, bool isVisible)
{
    m_impl->SetElementVisible(flagBit, isVisible);
}

void VizService::SetWindowLevelDrag(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC)
{
    m_impl->SetWindowLevelDrag(totalDx, totalDy, viewWidth, viewHeight, startWW, startWC);
}

void VizService::SetModelTransform(double translate[3], double rotate[3], double scale[3])
{
    m_impl->SetModelTransform(translate, rotate, scale);
}

void VizService::SetModelTransformReset()
{
    m_impl->SetModelTransformReset();
}

void VizService::GetModelPositionFromWorld(const double worldPos[3], double modelPos[3]) const
{
    m_impl->GetModelPositionFromWorld(worldPos, modelPos);
}

void VizService::GetWorldPositionFromModel(const double modelPos[3], double worldPos[3]) const
{
    m_impl->GetWorldPositionFromModel(modelPos, worldPos);
}

void VizService::SendUpdates()
{
    m_impl->SendUpdates();
}

bool VizService::GetDirty() const
{
    return m_impl->GetDirty();
}

void VizService::SetDirty()
{
    m_impl->SetDirty();
}

bool VizService::ResetDirty()
{
    return m_impl->ResetDirty();
}

void VizService::SetCurrentStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy)
{
    m_impl->SetCurrentStrategy(std::move(newStrategy));
}

void VizService::AttachOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy)
{
    m_impl->AttachOverlayStrategy(std::move(strategy));
}

void VizService::RemoveOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy)
{
    m_impl->RemoveOverlayStrategy(std::move(strategy));
}

void VizService::ClearOverlayStrategies()
{
    m_impl->ClearOverlayStrategies();
}

// ─────────────────────────────────────────────────────────────────────
// RenderContext 绑定
// ─────────────────────────────────────────────────────────────────────
void VizService::Impl::SetRenderContext(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer> ren)
{
    SetRenderBinding(std::move(win), std::move(ren));
    SetRendererBg();
    if (!m_stateEventSource) return;

    SetStateObserver();
}

void VizService::Impl::SetRenderBinding(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer> ren)
{
    auto oldRenderer = m_renderer;
    if (oldRenderer && oldRenderer != ren) {
        if (m_currentStrategy) {
            m_currentStrategy->DetachRenderer(oldRenderer);
        }
        for (auto& overlay : m_overlayStrategies) {
            if (overlay) overlay->DetachRenderer(oldRenderer);
        }
    }

    m_renderWindow = std::move(win);
    m_renderer = std::move(ren);

    if (m_renderer) {
        if (m_currentStrategy) {
            m_currentStrategy->AttachRenderer(m_renderer);
            m_currentStrategy->SetCamera(m_renderer);
        }
        for (auto& overlay : m_overlayStrategies) {
            if (overlay) overlay->AttachRenderer(m_renderer);
        }
        m_isDirty = true;
    }
}

void VizService::Impl::SetRendererBg()
{
    if (!m_renderer || !m_sharedState) {
        return;
    }

    const auto bg = m_sharedState->GetBackground();
    m_renderer->SetBackground(bg.r, bg.g, bg.b);
}

void VizService::Impl::SetStateObserver()
{
    std::shared_ptr<Impl> self = shared_from_this();
    std::weak_ptr<Impl> weakSelf = self;

    m_stateEventSource->SetObserver(std::move(self),
        [weakSelf](UpdateFlags flags)
        {
            auto self = weakSelf.lock();
            if (!self) return;

            if (GetFlagOn(flags, UpdateFlags::DataReady) && self->m_sharedState) {
                self->m_pendingLoadEventKind = self->m_sharedState->GetPendingLoadEventKind();
            }

            // 广播回调只做“事件翻译”为主线程可消费的原子标志，
            // 不在这里直接碰 VTK/Strategy，避免后台线程或任意调用线程破坏渲染线程边界。
            self->m_stateSyncStrategy.SendFlags(flags, *self);
        }
    );
}

// ─────────────────────────────────────────────────────────────────────
// 视觉配置 — 前处理 / 运行期配置意图
// ─────────────────────────────────────────────────────────────────────
void VizService::Impl::SetVizMode(VizMode mode)
{
    m_pendingVizModeInt.store(static_cast<int>(mode));
}

void VizService::Impl::SetMaterial(const MaterialParams& mat)
{
    m_sharedState->SetMaterial(mat);
}

void VizService::Impl::SetOpacity(double opacity)
{
    auto mat = m_sharedState->GetMaterial();
    mat.opacity = opacity;
    m_sharedState->SetMaterial(mat);
}

void VizService::Impl::SetTransferFunction(const std::vector<TFNode>& nodes)
{
    m_sharedState->SetTFNodes(nodes);
}

void VizService::Impl::SetIsoThreshold(double val)
{
    m_sharedState->SetIsoValue(val);
}

void VizService::Impl::SetBackground(const BackgroundColor& bg)
{
    m_sharedState->SetBackground(bg);
}

void VizService::Impl::SetSpacing(double sx, double sy, double sz)
{
    m_sharedState->SetSpacing(sx, sy, sz);
}

void VizService::Impl::SetWindowLevel(double ww, double wc)
{
    m_sharedState->SetWindowLevel(ww, wc);
}

void VizService::Impl::SetVisualConfig(const PreInitConfig& cfg)
{
    m_pendingVizModeInt.store(static_cast<int>(cfg.vizMode));
    m_sharedState->SetPreInitConfig(cfg);
}

// ─────────────────────────────────────────────────────────────────────
// 数据加载 / 数据导出
// ─────────────────────────────────────────────────────────────────────
LoadState VizService::Impl::GetFileLoadState() const
{
    return m_sharedState ? m_sharedState->GetFileLoadState() : LoadState::Idle;
}

LoadState VizService::Impl::GetReloadLoadState() const
{
    return m_sharedState ? m_sharedState->GetReloadLoadState() : LoadState::Idle;
}

void VizService::Impl::StartRun(
    std::packaged_task<void()> task,
    bool hasActiveLoadFuture)
{
    if (hasActiveLoadFuture) {
        std::lock_guard<std::mutex> lk(m_activeLoadMutex);
        m_activeLoadFuture = task.get_future(); // 当前活动加载任务的 future，用于析构时等待后台线程结束
    }
    // 线程统一 detach，说明 Service 只关心生命周期托管和结果回收，不在调用点阻塞等待后台任务。
    std::thread(std::move(task)).detach();
}

void VizService::Impl::LoadFileAsync(
    const std::string& path,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool isSuccess)> onComplete)
{
    if (!m_dataLoadTaskService) {
        return;
    }

    auto task = m_dataLoadTaskService->BuildLoadFileTask(
        path,
        spacing,
        origin,
        std::move(onComplete));
    if (task) {
        StartRun(std::move(*task), true);
    }
}

bool VizService::Impl::ReloadFromBufferAsync(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool isSuccess)> onComplete)
{
    if (!m_dataLoadTaskService) {
        return false;
    }

    auto task = m_dataLoadTaskService->BuildReloadFromBufferTask(
        data,
        dims,
        spacing,
        origin,
        std::move(onComplete));
    if (!task) {
        return false;
    }

    StartRun(std::move(*task), true);
    return true;
}

void VizService::Impl::ExportDataAsync(
    const std::string& path,
    std::function<void(bool isSuccess)> onComplete)
{
    if (!m_dataExportTaskService) {
        return;
    }

    auto task = m_dataExportTaskService->BuildDataTask(path, std::move(onComplete));
    if (task) {
        StartRun(std::move(*task), false);
    }
}

void VizService::Impl::ExportSlicesAsync(
    const std::string& path,
    std::optional<double> rotationAngleDeg,
    std::function<void(bool isSuccess)> onComplete)
{
    if (!m_dataExportTaskService) {
        return;
    }

    const VizMode currentMode = static_cast<VizMode>(m_pendingVizModeInt.load());
    auto task = m_dataExportTaskService->BuildSlicesTask(
        path,
        rotationAngleDeg,
        currentMode,
        std::move(onComplete));
    if (task) {
        StartRun(std::move(*task), false);
    }
}

void VizService::Impl::StopFileLoad()
{
    // 当前加载流程尚未接入可中断检查，先保留上层控制入口。
}

// ─────────────────────────────────────────────────────────────────────
// InteractiveService — 交互接口
// ─────────────────────────────────────────────────────────────────────
void VizService::Impl::SetSliceScroll(int delta)
{
    if (!m_sharedState || !m_dataManager || !m_dataManager->GetVtkImage()) return;
    const VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    const int axis = InteractionComputeService::GetSliceAxis(mode); // 当前切片滚动应推进的模型坐标轴
    if (axis < 0)
		return;

    double space[3] = { 0.0 };
    auto img = m_dataManager->GetVtkImage();
    img->GetSpacing(space);

    auto cursorWorld = m_sharedState->GetCursorWorld();
    double cursorModel[3] = { 0.0 };
    GetModelPositionFromWorld(cursorWorld.data(), cursorModel);

    double bounds[6] = { 0.0 };
    img->GetBounds(bounds);
    double nextModel[3] = { 0.0, 0.0, 0.0 }; // 滚轮推进并钳制后的模型坐标
    InteractionComputeService::GetScrolledModelPosition(cursorModel, axis, delta, space, bounds, nextModel);

    double newCursorWorld[3] = { 0.0, 0.0, 0.0 };
    GetWorldPositionFromModel(nextModel, newCursorWorld);
    m_sharedState->SetCursorRawWorld(newCursorWorld[0], newCursorWorld[1], newCursorWorld[2]);
    m_sharedState->SetCursorAxis(axis);
    m_sharedState->SetCursorWorld(newCursorWorld[0], newCursorWorld[1], newCursorWorld[2]);
    SetSyncNeeded();
}

void VizService::Impl::SetCursorWorldPosition(double worldPos[3], int axis)
{
    if (!m_dataManager || !m_dataManager->GetVtkImage() || !m_sharedState) return;
    auto currentPos = m_sharedState->GetCursorWorld();
    m_sharedState->SetCursorRawWorld(worldPos[0], worldPos[1], worldPos[2]);
    m_sharedState->SetCursorAxis(axis);

    // 0 1 2 分别表示 SliceLeft_right SliceFront_back SliceTop_down
    if (axis == -1)
    {
        m_sharedState->SetCursorWorld(worldPos[0], worldPos[1], worldPos[2]);
    }
    else {
        double newPos[3] = { worldPos[0], worldPos[1], worldPos[2] };
        newPos[axis] = currentPos[axis];
        m_sharedState->SetCursorWorld(newPos[0], newPos[1], newPos[2]);
    }
}

std::array<double, 3> VizService::Impl::GetCursorWorld()
{
    return m_sharedState->GetCursorWorld();
}

void VizService::Impl::SetInteracting(bool isInteracting)
{
    m_sharedState->SetInteracting(isInteracting);
}

int VizService::Impl::GetPlaneAxis(vtkActor* actor)
{
    return m_currentStrategy ? m_currentStrategy->GetPlaneAxis(actor) : -1;
}

vtkProp3D* VizService::Impl::GetMainProp()
{
    return m_currentStrategy ? m_currentStrategy->GetMainProp() : nullptr;
}

void VizService::Impl::SetModelMatrix(vtkMatrix4x4* modelToWorldMatrix)
{
    if (!modelToWorldMatrix) return;

    std::array<double, 16> matData = { 0 }; // 当前模型矩阵快照，回写 SharedState 使用
    std::memcpy(matData.data(), modelToWorldMatrix->GetData(), 16 * sizeof(double));
    if (m_sharedState) {
        m_sharedState->SetModelMatrix(matData);
    }
}

std::array<double, 16> VizService::Impl::GetModelMatrix()
{
    return m_sharedState ? m_sharedState->GetModelMatrix()
        : std::array<double, 16>{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
}

WindowLevelParams VizService::Impl::GetWindowLevel() const
{
    if (m_sharedState)
        return m_sharedState->GetWindowLevel();

    WindowLevelParams p = {};
    return p;
}

int VizService::Impl::GetNavigationAxis() const
{
    return m_currentStrategy ? m_currentStrategy->GetNavigationAxis() : -1;
}

void VizService::Impl::SetElementVisible(uint32_t flagBit, bool isVisible)
{
    m_sharedState->SetElementVisible(flagBit, isVisible);
}

void VizService::Impl::SetWindowLevelDrag(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC)
{
    if (!m_sharedState) return;

    const WindowLevelParams windowLevel = InteractionComputeService::GetWindowLevel(
        totalDx,
        totalDy,
        viewWidth,
        viewHeight,
        startWW,
        startWC); // 当前拖拽结束后应写回状态的窗宽窗位

    m_sharedState->SetWindowLevel(windowLevel.windowWidth, windowLevel.windowCenter);
    SetSyncNeeded();
}

void VizService::Impl::SetModelTransform(
    double translate[3], double rotate[3], double scale[3])
{
    auto currentModelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // SharedState 当前模型矩阵快照，作为 TRS 叠加基准
    currentModelToWorldMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        currentModelToWorldMatrix->DeepCopy(matrixData.data());
    }

    auto nextModelToWorldMatrix = InteractionComputeService::GetModelMatrix(
        currentModelToWorldMatrix, translate, rotate, scale);
    std::array<double, 16> matData = { 0 }; // TRS 叠加后的模型矩阵快照，供状态同步使用
    std::memcpy(matData.data(), nextModelToWorldMatrix->GetData(), 16 * sizeof(double));
    if (m_sharedState) {
        m_sharedState->SetModelMatrix(matData);
    }
}

void VizService::Impl::SetModelTransformReset()
{
    std::array<double, 16> identity = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    }; // 模型重置后的单位矩阵
    if (m_sharedState) {
        m_sharedState->SetModelMatrix(identity);
    }
}

void VizService::Impl::GetModelPositionFromWorld(const double w[3], double m[3]) const
{
    auto worldToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // 由 SharedState 现算的逆矩阵，避免维护第二份真相
    worldToModelMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        worldToModelMatrix->DeepCopy(matrixData.data());
        worldToModelMatrix->Invert();
    }

    InteractionComputeService::GetModelPositionFromWorld(worldToModelMatrix, w, m);
}

void VizService::Impl::GetWorldPositionFromModel(const double m[3], double w[3]) const
{
    auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // SharedState 当前模型矩阵快照，保证 world/model 换算只依赖单一真源
    modelToWorldMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        modelToWorldMatrix->DeepCopy(matrixData.data());
    }

    InteractionComputeService::GetWorldPositionFromModel(modelToWorldMatrix, m, w);
}

void VizService::Impl::SetCursorCenter()
{
    if (!m_dataManager || !m_dataManager->GetVtkImage()) return;
    // DataReady 后把联动光标重置到新体数据中心，避免沿用旧数据上的 cursor 位置导致切片落在无效区域。
    double imgCenter[3] = { 0.0 };
    auto img = m_dataManager->GetVtkImage();
    img->GetCenter(imgCenter);

    double imgCenterWorld[3];
    GetWorldPositionFromModel(imgCenter, imgCenterWorld);
    m_sharedState->SetCursorRawWorld(imgCenterWorld[0], imgCenterWorld[1], imgCenterWorld[2]);
    m_sharedState->SetCursorAxis(-1);
    m_sharedState->SetCursorWorld(imgCenterWorld[0], imgCenterWorld[1], imgCenterWorld[2]);
}

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService — 主线程后处理入口
// ─────────────────────────────────────────────────────────────────────
void VizService::Impl::SendUpdates()
{
    // 统一主线程后处理链路：
    // 1. pending image 提交为 current data
    // 2. 导出回调
    // 3. 策略缓存清理
    // 4. 加载失败或 DataReady 管线重建
    // 5. 普通增量同步
    // 6. 文件加载 / reload 回调

    SendPendingImage();
    SendExportCallback();

    // 延迟缓存清理（Detach 必须在主线程）
    if (m_hasCacheClearNeed.exchange(false))
        ClearStrategyCache();

    bool hasLoadEvent = false;
    const bool hasLoadFailure = SetLoadFailureState(hasLoadEvent);
    if (!hasLoadFailure) {
        SetDataRefreshState(hasLoadEvent);
        SetStrategyState();
    }

    SendLoadCallbacks();
    if (hasLoadEvent) {
        m_pendingLoadEventKind = LoadEventKind::None;
    }
}

void VizService::Impl::SendPendingImage()
{
    // 数据源只有一份；pending image 由 DataManager 自己保证只被消费一次。
    if (!m_dataManager || !m_dataManager->SetCurrentFromPending()) {
        return;
    }

    // 走和文件加载成功相同的后处理路径。
    auto img = m_dataManager->GetVtkImage();
    if (!img) {
        m_sharedState->SetReloadLoadFailed();
        return;
    }

    const auto range = m_dataManager->GetScalarRange();
    const auto spacing = m_dataManager->GetSpacing();
    m_sharedState->SetReloadDataReady(range[0], range[1], spacing);
}

void VizService::Impl::SendExportCallback()
{
    // 导出异步任务回调触发。
    if (m_dataExportTaskService && m_dataExportTaskService->ResetSaveCallback()) {
        m_dataExportTaskService->SendSaveCallback();
    }
}

bool VizService::Impl::SetLoadFailureState(bool& hasLoadEvent)
{
    if (!m_hasLoadFailure.exchange(false)) {
        return false;
    }

    const LoadEventKind loadEventKind = m_pendingLoadEventKind;
    ClearLoadFail();
    if (m_dataLoadTaskService && loadEventKind == LoadEventKind::File) {
        m_dataLoadTaskService->SetFileLoadCallbackReady(false);
    }
    else if (m_dataLoadTaskService && loadEventKind == LoadEventKind::Reload) {
        m_dataLoadTaskService->SetReloadReady(false);
    }
    hasLoadEvent = true;
    return true;
}

bool VizService::Impl::SetDataRefreshState(bool& hasLoadEvent)
{
    if (!m_hasDataRefreshNeed.exchange(false)) {
        return false;
    }

    const LoadEventKind loadEventKind = m_pendingLoadEventKind;
    BuildPipeline();
    if (m_dataLoadTaskService && loadEventKind == LoadEventKind::File) {
        m_dataLoadTaskService->SetFileLoadCallbackReady(true);
    }
    else if (m_dataLoadTaskService && loadEventKind == LoadEventKind::Reload) {
        m_dataLoadTaskService->SetReloadReady(true);
    }
    hasLoadEvent = true;
    return true;
}

void VizService::Impl::SetDataRefresh()
{
    m_hasDataRefreshNeed = true;
}

void VizService::Impl::SetLoadFailed()
{
    m_hasLoadFailure = true;
    m_isDirty = true;
}

void VizService::Impl::SendLoadCallbacks()
{
    // 文件加载 / 重载回调延后到策略同步之后，保证上层 submit 回调看到的是主线程收敛后的最终状态。
    if (m_dataLoadTaskService && m_dataLoadTaskService->ResetFileCallback()) {
        m_dataLoadTaskService->SendFileLoadCallback();
    }
    if (m_dataLoadTaskService && m_dataLoadTaskService->ResetReloadCallback()) {
        m_dataLoadTaskService->SendReloadCallback();
    }
}

// ─────────────────────────────────────────────────────────────────────
// 私有辅助
// ─────────────────────────────────────────────────────────────────────
void VizService::Impl::BuildPipeline()
{
    // 这一步只处理“结构性变化后的管线重建”：选对 Strategy、喂入最新图像、重新挂接渲染器。
    // 具体材质、TF、窗宽窗位等参数同步故意留到后续增量同步阶段再做。
    VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    auto strategy = GetStrategy(mode);
    if (!strategy) {
        std::cerr << "[BuildPipeline] StrategyFactory returned null for mode "
            << static_cast<int>(mode) << "\n";
        return;
    }

    auto img = m_dataManager ? m_dataManager->GetVtkImage() : nullptr;
    if (img) {
        const auto spacing = m_sharedState->GetSpacing();
        double currentSpacing[3] = { 1.0, 1.0, 1.0 };
        img->GetSpacing(currentSpacing);
        if (std::abs(currentSpacing[0] - spacing[0]) > 1e-6 ||
            std::abs(currentSpacing[1] - spacing[1]) > 1e-6 ||
            std::abs(currentSpacing[2] - spacing[2]) > 1e-6)
        {
            img->SetSpacing(spacing[0], spacing[1], spacing[2]);
        }

        int dims[3] = { 0, 0, 0 };
        img->GetDimensions(dims);
        if (dims[0] > 0 && dims[1] > 0 && dims[2] > 0) {
            strategy->SetInputData(img);
        }
        else {
            std::cerr << "[BuildPipeline] Image has zero dimension, skipping SetInputData.\n";
            return;
        }
    }
    else {
        std::cerr << "[BuildPipeline] DataManager has no valid image.\n";
        return;
    }

    SetCurrentStrategy(strategy);
    SetRendererBg();
    SetSyncNeeded(); // 重建后触发一次全量参数同步
}

void VizService::Impl::SetStrategyState()
{
    bool isExpected = true;
    if (!m_hasSyncNeed.compare_exchange_strong(isExpected, false)) return;
    if (!m_currentStrategy) return;

    // 用 exchange(0) 取走当前整包增量标志，相当于把这一帧前累计的状态改动做一次原子快照。
    // 后续新来的事件会写入新的 m_pendingFlags，留到下一帧继续消费，不会和本次同步互相覆盖。
    int flagsInt = m_pendingFlags.exchange(0);
    UpdateFlags flags = static_cast<UpdateFlags>(flagsInt);

    if (flags == UpdateFlags::None) {
        m_hasSyncNeed = false;
        return;
    }

    // 交互状态控制帧率
    if (GetFlagOn(flags, UpdateFlags::Interaction) && m_renderWindow) {
        const bool isInteracting = m_sharedState->GetIsInteracting();
        m_renderWindow->SetDesiredUpdateRate(isInteracting ? 15.0 : 0.001);
    }

    // 背景色同步（数据无关，直接写渲染器）
    if (GetFlagOn(flags, UpdateFlags::Background) && m_renderer) {
        SetRendererBg();
        flags = static_cast<UpdateFlags>(
            static_cast<int>(flags) & ~static_cast<int>(UpdateFlags::Background));
    }
	// 判断取了背景色标志后剩下的位，如果没有了才标脏，否则继续走 Strategy 同步剩余状态
    // 避免每次背景色改动都重置全局脏标志导致不必要的渲染刷新。
    if (flags == UpdateFlags::None) {
        m_isDirty = true;
        return;
    }

    RenderParams params = GetRenderParams(flags);
    m_currentStrategy->SetVisualState(params, flags);

    for (auto& overlay : m_overlayStrategies) {
        overlay->SetVisualState(params, flags);
    }

    m_isDirty = true;
    // m_hasSyncNeed = false;
}

void VizService::Impl::ClearLoadFail()
{
    std::cerr << "[ClearLoadFail] Load failed; clearing pipeline state.\n";

    // 清理策略缓存（无有效数据，不应保留旧 Strategy）
    ClearStrategyCache();

    // 重置刷新标记，防止残留 DataReady 触发错误重建
    m_hasDataRefreshNeed = false;

	// 重置交互状态
    m_pendingFlags = 0;

    // 标脏使渲染器刷新空场景
    m_isDirty = true;
}

RenderParams VizService::Impl::GetRenderParams(UpdateFlags flags) const
{
    RenderParams p;

    // RenderParams 是当前这一帧需要下发给 Strategy 的“最小快照”，
    // 只按 flags 拿必要字段，避免每次同步都把全部状态搬运一遍。

    if (GetFlagOn(flags, UpdateFlags::Cursor) || GetFlagOn(flags, UpdateFlags::Transform)) {
        auto pos = m_sharedState->GetCursorWorld();
        auto rawPos = m_sharedState->GetCursorRawWorld();
        p.cursor = { pos[0], pos[1], pos[2] };
        p.cursorRaw = { rawPos[0], rawPos[1], rawPos[2] };
        p.cursorAxis = m_sharedState->GetCursorAxis();
        p.modelMatrix = m_sharedState->GetModelMatrix();
    }
    if (GetFlagOn(flags, UpdateFlags::TF)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        m_sharedState->GetTFNodes(p.tfNodes);
    }

    if (GetFlagOn(flags, UpdateFlags::WindowLevel)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.windowLevel = m_sharedState->GetWindowLevel();
    }

    if (GetFlagOn(flags, UpdateFlags::Material)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.material = m_sharedState->GetMaterial();
        m_sharedState->GetTFNodes(p.tfNodes);
    }

    if (GetFlagOn(flags, UpdateFlags::Interaction))
        p.isInteracting = m_sharedState->GetIsInteracting();

    if (GetFlagOn(flags, UpdateFlags::IsoValue))
        p.isoValue = m_sharedState->GetIsoValue();

    if (GetFlagOn(flags, UpdateFlags::Visibility))
		p.visibilityMask = m_sharedState->GetVisibilityMask();

    return p;
}

std::shared_ptr<AbstractVisualStrategy> VizService::Impl::GetStrategy(VizMode mode)
{
    // Strategy 以 VizMode 为键缓存，说明模式切换是“复用既有渲染对象”而不是“每次全新构建”。
    auto it = m_strategyCache.find(mode);
    if (it != m_strategyCache.end()) return it->second;

    auto s = StrategyFactory::GetStrategy(mode);
    if (s) m_strategyCache[mode] = s;
    return s;
}
