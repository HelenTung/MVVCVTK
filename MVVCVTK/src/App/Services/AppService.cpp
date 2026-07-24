#include "AppService.h"
#include "AppDataExportTaskService.h"
#include "AppDataLoadTaskService.h"
#include "AppState.h"
#include "CompositeStrategy.h"
#include "DataManager.h"
#include "InteractionComputeService.h"
#include "IsoSurfaceStrategy.h"
#include "SliceStrategy.h"
#include "VolumeStrategy.h"
#include <vtkSmartPointer.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

// ─────────────────────────────────────────────────────────────────────
// 构造 / 析构
// ─────────────────────────────────────────────────────────────────────
// 单个视图的应用服务实现：共享 DataManager/State，拥有该视图的 strategy/overlay 与异步任务槽。
// worker 只准备 pending 数据或导出结果；current 提交、VTK 管线切换和业务 callback 统一由 host tick 收口。
class VizService::Impl final
    : public std::enable_shared_from_this<Impl> {
public:
    Impl(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state,
        std::shared_ptr<IStateEventSource> stateEventSource,
        VizService::TaskStart taskStart);
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
    bool LoadFileAsync(std::string path,
        VolumeLayout layout,
        std::function<void(bool isSuccess)> onComplete);
    bool ReloadFromBufferAsync(
        VolumeBuffer buffer,
        std::function<void(bool isSuccess)> onComplete);
    void ExportDataAsync(const std::string& path,
        std::function<void(bool isSuccess)> onComplete);
    void ExportSlicesAsync(const std::string& path,
        std::optional<double> rotationAngleDeg,
        std::function<void(bool isSuccess)> onComplete);
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
    bool SendReloadUpdate();
    bool GetDirty() const;
    void SetDirty();
    bool ResetDirty();
    void SetCurrentStrategy(
        std::shared_ptr<AbstractVisualStrategy> newStrategy,
        bool isRendererAttached = false);
    void AttachOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy);
    void RemoveOverlayStrategy(
        std::shared_ptr<AbstractVisualStrategy> strategy) noexcept;
    void ClearOverlayStrategies();
    RenderInputStamp GetRenderInputStamp() const;
    bool AttachRenderEffect(std::shared_ptr<RenderEffect> effect);
    bool DetachRenderEffect(const RenderEffect* effect);

private:
    struct LoadNotice {
        LoadEventKind kind = LoadEventKind::None; // 区分 File/Reload，决定提交 pending 与失败清场策略。
        bool isSucceeded = false; // worker/共享状态最终结论，可在管线构建失败时降级为 false。
        bool isStateSet = false; // true 表示共享终态已发布，仅等待 owner 释放 admission。
    };

    struct ActiveTask final {
        LoadEventKind loadKind = LoadEventKind::None; // None 用于普通导出，File/Reload 需要加载事务收尾。
        std::future<bool> result; // worker 只返回准备结果，不直接触碰 VTK 管线。
        std::thread worker; // SendTasks 发现 future ready 后负责 join。
        std::function<void(bool)> callback; // 延迟到主线程完成提交/管线同步后执行。
    };

    struct Completion final {
        bool isSuccess = false; // 已包含 worker、pending 提交和 BuildPipeline 的综合结果。
        std::function<void(bool)> callback; // SendCompletions 在内部锁外调用，允许安全发起下一事务。
    };

    void SetRenderBinding(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren);
    void SetStateObserver();
    void SendStateFlags(UpdateFlags flags);
    void SendTasks();
    void SendCompletions();
    void SetTaskResult(ActiveTask task, bool isSuccess);
    void SetLoadResult(ActiveTask task, bool isSuccess);
    void SetCompletion(bool isSuccess, std::function<void(bool)> callback);
    bool CreateLoadNotice(
        LoadEventKind loadEventKind,
        bool isSucceeded,
        bool isStateSet = false);
    bool RemoveLoadNotice(LoadNotice& loadNotice);
    bool GetOwnedLoad(LoadEventKind loadEventKind) const;
    bool SetOwnedLoad(LoadEventKind loadEventKind);
    bool ResetOwnedLoad(LoadEventKind loadEventKind);
    bool BuildPipeline();
    void SetStrategyState();
    void ClearLoadFail(LoadEventKind loadEventKind);
    RenderParams GetRenderParams(UpdateFlags flags) const;
    std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode);
    std::shared_ptr<AbstractVisualStrategy> CreateStrategy(VizMode mode);
    void SetRendererBg();
    void ClearStrategyCache();
    void SetCursorCenter();
    void SetSyncNeeded();
    void SetPendingFlags(UpdateFlags flags);
    void SetDataRefresh();
    bool StartTask(VizService::TaskWork task,
        LoadEventKind loadKind,
        std::function<void(bool)> callback);

    // Service 共享持有会话数据源；File/Reload worker 都只 staging pending，
    // 再由 SendUpdates 所在线程的 owner 提交为 current。
    std::shared_ptr<AbstractDataManager> m_dataManager;
    // 当前主渲染策略的共享 owner；替换前从旧 renderer 脱离，随后由缓存决定是否继续保留。
    std::shared_ptr<AbstractVisualStrategy> m_currentStrategy;
    // Service 只弱观察可选效果根；Feature 是效果的唯一业务 owner，
    // Strategy 只强持有由该 root 构造的单目标 binding。
    std::weak_ptr<RenderEffect> m_renderEffect;
    // RenderContext 注入的 VTK 强引用；策略/overlay 的 Attach、Detach 和相机更新均以它为目标。
    vtkSmartPointer<vtkRenderer> m_renderer;
    // RenderContext 注入的窗口强引用；仅用于渲染节奏与窗口级操作，不拥有当前 Strategy。
    vtkSmartPointer<vtkRenderWindow> m_renderWindow;
    // 状态合并、外部请求或主线程管线操作均可置位；Timer 在 SendUpdates() 后取走并清零。
    std::atomic<bool> m_isDirty{ false };
    // 普通状态事件、overlay 挂接与管线重建置位；主线程 SetStrategyState() 用 CAS 领取。
    std::atomic<bool> m_hasSyncNeed{ false };
    // 状态事件与 overlay 挂接以按位 OR 合并；主线程 exchange(0)，加载失败清场也会清零。
    std::atomic<int> m_pendingFlags{ static_cast<int>(UpdateFlags::All) };
    // 已挂载 overlay 的共享 owner 集合；renderer 另持 VTK prop 引用，Remove/Clear 负责先解除挂载。
    std::vector<std::shared_ptr<AbstractVisualStrategy>> m_overlayStrategies;
    // 任务 builder 只准备 bool 结果，不越界发布终态或 callback。
    std::shared_ptr<AppDataLoadTaskService> m_dataLoadTaskService;
    std::shared_ptr<AppDataExportTaskService> m_dataExportTaskService;
    // 按 VizMode 强持有已构建 Strategy；清缓存时先 Detach，避免同模式反复创建 VTK pipeline。
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;
    // 本 service 持有 DataManager 当前批次 owner；各 view 共享只读 image/scalars，旧批次随最后一个 owner 释放。
    ImageSnapshot m_renderSnapshot;
    // observer 把 kind/result 作为一个完整终态 payload 入队；锁只保护队列，不覆盖 VTK 或 callback 调用。
    std::deque<LoadNotice> m_loadNotices;
    mutable std::mutex m_loadNoticeMutex;
    // 只有实际启动异步 load/reload 的 service 持有 owner；Host 同步事务不写入该槽。
    std::atomic<int> m_ownedLoadKind{ static_cast<int>(LoadEventKind::None) };
    // 会话运行态的共享 owner 与单一事实源；前处理、交互和渲染参数都通过它发布/读取。
    std::shared_ptr<SharedInteractionState> m_sharedState;
    // 状态广播源的共享 owner；observer 只 weak-lock Impl，避免事件源反向延长 Service 生命周期。
    std::shared_ptr<IStateEventSource> m_stateEventSource;

    // SetVizMode/SetVisualConfig 写入最新快照；管线重建、切片交互和导出读取但不清零。
    std::atomic<int> m_pendingVizModeInt{ static_cast<int>(VizMode::IsoSurface) };
    // DataReady、Spacing 或模式变化置位；主线程重建前 exchange(false)，失败清场也会清零。
    std::atomic<bool> m_hasDataRefreshNeed{ false };
    VizService::TaskStart m_taskStart;
    std::list<ActiveTask> m_activeTasks;
    mutable std::mutex m_activeTaskMutex;
    std::deque<Completion> m_completions;
    mutable std::mutex m_completionMutex;
    std::function<void(bool)> m_ownedCallback;
    std::atomic<bool> m_isAccepting{ true };
};

VizService::Impl::Impl(
    std::shared_ptr<AbstractDataManager> dataMgr,
    std::shared_ptr<SharedInteractionState> state,
    std::shared_ptr<IStateEventSource> stateEventSource,
    VizService::TaskStart taskStart)
    : m_dataManager(std::move(dataMgr))
    , m_sharedState(std::move(state))
    , m_stateEventSource(std::move(stateEventSource))
    , m_taskStart(std::move(taskStart))
{
    if (!m_taskStart) {
        m_taskStart = [](VizService::TaskWork task) {
            return std::thread(std::move(task));
        };
    }
    m_dataLoadTaskService = std::make_shared<AppDataLoadTaskService>(m_dataManager);
    m_dataExportTaskService = std::make_shared<AppDataExportTaskService>(m_dataManager, m_sharedState);
}

VizService::Impl::~Impl()
{
    m_isAccepting = false;
    std::list<ActiveTask> activeTasks;
    {
        std::lock_guard<std::mutex> lock(m_activeTaskMutex);
        activeTasks.splice(activeTasks.end(), m_activeTasks);
    }
    for (auto& task : activeTasks) {
        if (task.result.valid()) {
            try { (void)task.result.get(); }
            catch (...) {}
        }
        if (task.worker.joinable()) task.worker.join();
    }

    const auto ownedLoadKind = static_cast<LoadEventKind>(
        m_ownedLoadKind.exchange(static_cast<int>(LoadEventKind::None)));
    if (m_sharedState && ownedLoadKind != LoadEventKind::None) {
        // Reload worker 可能已发布 pending、但 Timer 尚未提交；先销毁 payload，再发布失败终态，
        // 防止共享 DataManager 在下一事务中提交旧批次。
        if (m_dataManager) m_dataManager->ClearPending();
        if (ownedLoadKind == LoadEventKind::Reload) {
            if (m_sharedState->GetReloadLoadState() == LoadState::Loading) {
                m_sharedState->SetReloadLoadFailed();
            }
        }
        else if (ownedLoadKind == LoadEventKind::File
            && m_sharedState->GetFileLoadState() == LoadState::Loading) {
            m_sharedState->SetFileLoadFailed();
        }
        // Service 销毁后不会再消费 callback，但不能让它已接纳的全局事务永久占用 admission。
        m_sharedState->ResetLoad(ownedLoadKind);
    }
}

bool VizService::Impl::GetDirty() const
{
    return m_isDirty;
}

void VizService::Impl::SetDirty()
{
    // 外部显式请求只置门铃，实际 Render 仍由 Timer 决定。
    m_isDirty = true;
}

bool VizService::Impl::ResetDirty()
{
    // Timer 在本帧同步完成后领取一次请求；领取之后的新请求留给下一帧。
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
    std::shared_ptr<AbstractVisualStrategy> newStrategy,
    const bool isRendererAttached)
{
    if (m_currentStrategy == newStrategy) {
        if (m_currentStrategy && m_renderer) {
            m_currentStrategy->SetCamera(m_renderer);
            m_isDirty = true;
        }
        return;
    }

    auto effect = m_renderEffect.lock();
    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->DetachRenderer(m_renderer);
    }
    if (m_currentStrategy && effect) {
        (void)m_currentStrategy->DetachRenderEffect(effect.get());
    }

    m_currentStrategy = std::move(newStrategy);

    if (m_currentStrategy) {
        (void)m_currentStrategy->SetRenderInputStamp(
            GetRenderInputStamp());
        if (effect) {
            const auto state = m_currentStrategy->GetRenderEffectState();
            const bool hasBinding =
                state.failureReason != RenderEffectFailure::Unsupported;
            if (hasBinding) {
                (void)m_currentStrategy->SetRenderEffectUse(
                    RenderBindingUse::Current);
            }
            else {
                (void)m_currentStrategy->AttachRenderEffect(
                    effect, RenderBindingUse::Current);
            }
        }
        if (m_renderer && !isRendererAttached) {
            m_currentStrategy->AttachRenderer(m_renderer);
        }
        if (m_renderer) {
            m_currentStrategy->SetCamera(m_renderer);
        }
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
    std::shared_ptr<AbstractVisualStrategy> strategy) noexcept
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
        // renderer 是外部策略边界；即使自定义策略清理抛出，service 也必须移除自身登记，
        // 让上层 Start/Clear 事务不会暴露异常或永久保留强 owner。
        try {
            strategy->DetachRenderer(m_renderer);
        }
        catch (...) {
        }
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

RenderInputStamp VizService::Impl::GetRenderInputStamp() const
{
    RenderInputStamp stamp;
    if (m_renderSnapshot) {
        stamp.identity = m_renderSnapshot->image.GetPointer();
        stamp.version = m_renderSnapshot->version;
    }
    return stamp;
}

bool VizService::Impl::AttachRenderEffect(
    std::shared_ptr<RenderEffect> effect)
{
    if (!effect || !m_renderEffect.expired()) {
        return false;
    }
    m_renderEffect = effect;
    if (!m_currentStrategy) {
        return true;
    }
    if (!m_currentStrategy->SetRenderInputStamp(GetRenderInputStamp())
        || !m_currentStrategy->AttachRenderEffect(
            std::move(effect), RenderBindingUse::Current)) {
        m_renderEffect.reset();
        return false;
    }
    m_isDirty = true;
    return true;
}

bool VizService::Impl::DetachRenderEffect(const RenderEffect* effect)
{
    auto currentEffect = m_renderEffect.lock();
    if (!effect || currentEffect.get() != effect) {
        return false;
    }
    if (m_currentStrategy
        && !m_currentStrategy->DetachRenderEffect(effect)) {
        return false;
    }
    m_renderEffect.reset();
    m_isDirty = true;
    return true;
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
    std::shared_ptr<IStateEventSource> stateEventSource,
    TaskStart taskStart)
    : m_impl(std::make_shared<VizService::Impl>(
        std::move(dataMgr),
        std::move(state),
        std::move(stateEventSource),
        std::move(taskStart)))
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

bool VizService::LoadFileAsync(
    std::string path,
    VolumeLayout layout,
    std::function<void(bool isSuccess)> onComplete)
{
    return m_impl->LoadFileAsync(
        std::move(path), std::move(layout), std::move(onComplete));
}

bool VizService::ReloadFromBufferAsync(
    VolumeBuffer buffer,
    std::function<void(bool isSuccess)> onComplete)
{
    return m_impl->ReloadFromBufferAsync(
        std::move(buffer), std::move(onComplete));
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

void VizService::RemoveOverlayStrategy(
    std::shared_ptr<AbstractVisualStrategy> strategy) noexcept
{
    m_impl->RemoveOverlayStrategy(std::move(strategy));
}

void VizService::ClearOverlayStrategies()
{
    m_impl->ClearOverlayStrategies();
}

RenderInputStamp VizService::GetRenderInputStamp() const
{
    return m_impl->GetRenderInputStamp();
}

bool VizService::AttachRenderEffect(std::shared_ptr<RenderEffect> effect)
{
    return m_impl->AttachRenderEffect(std::move(effect));
}

bool VizService::DetachRenderEffect(const RenderEffect* effect)
{
    return m_impl->DetachRenderEffect(effect);
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
    auto oldWindow = m_renderWindow;
    const bool isBindingChanged =
        oldRenderer.GetPointer() != ren.GetPointer()
        || oldWindow.GetPointer() != win.GetPointer();
    if (oldRenderer && isBindingChanged) {
        if (m_currentStrategy) {
            m_currentStrategy->DetachRenderer(oldRenderer);
        }
        for (auto& overlay : m_overlayStrategies) {
            if (overlay) overlay->DetachRenderer(oldRenderer);
        }
    }

    m_renderWindow = std::move(win);
    m_renderer = std::move(ren);

    if (m_renderer && isBindingChanged) {
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

            const UpdateFlags loadFlags = UpdateFlags::DataReady | UpdateFlags::LoadFailed;
            if ((flags & loadFlags) != UpdateFlags::None) {
                LoadEventKind loadEventKind = LoadEventKind::None;
                if ((flags & UpdateFlags::FileLoad) != UpdateFlags::None) {
                    loadEventKind = LoadEventKind::File;
                }
                else if ((flags & UpdateFlags::ReloadLoad) != UpdateFlags::None) {
                    loadEventKind = LoadEventKind::Reload;
                }
                if (loadEventKind != LoadEventKind::None) {
                    self->CreateLoadNotice(
                        loadEventKind,
                        (flags & UpdateFlags::DataReady)
                            != UpdateFlags::None);
                }
            }

            // 广播在状态发布线程同步进入：这里只登记主线程工作，不读取 VTK image 或重建 Strategy。
            self->SendStateFlags(flags);
        }
    );
}

void VizService::Impl::SendStateFlags(UpdateFlags flags)
{
    // 把跨层状态事件收敛为主线程邮箱；结构事件与普通增量使用不同消费路径。
    if ((flags & UpdateFlags::LoadFailed) != UpdateFlags::None
        || ((flags & UpdateFlags::DataReady) != UpdateFlags::None
            && ((flags & UpdateFlags::FileLoad) != UpdateFlags::None
                || (flags & UpdateFlags::ReloadLoad) != UpdateFlags::None))) {
        // load 终态的完整语义由主线程从 payload 队列消费，不能再拆回独立布尔门铃。
        return;
    }

    if ((flags & (UpdateFlags::Spacing | UpdateFlags::DataReady))
        != UpdateFlags::None) {
        // spacing 改变输入几何，不能只做 Strategy 增量写入，必须走结构重建。
        SetPendingFlags(UpdateFlags::All);
        SetDataRefresh();
        return;
    }

    // 其余状态可合并为增量位图，由下一次 Timer 心跳统一下发。
    SetPendingFlags(flags);
    SetSyncNeeded();
}

// ─────────────────────────────────────────────────────────────────────
// 视觉配置 — 前处理 / 运行期配置意图
// ─────────────────────────────────────────────────────────────────────
void VizService::Impl::SetVizMode(VizMode mode)
{
    const int nextMode = static_cast<int>(mode);
    // 模式快照保留最新值且不清零；同值写入不产生重复重建请求。
    if (m_pendingVizModeInt.exchange(nextMode) == nextMode) {
        return;
    }
    // 模式变化会更换 Strategy 或输入方向，因此进入结构重建路径并请求下一帧 Render。
    m_hasDataRefreshNeed = true;
    m_isDirty = true;
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
    if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz)
        || sx <= 0.0 || sy <= 0.0 || sz <= 0.0) {
        return;
    }
    if (m_dataManager && !m_dataManager->SetSpacing({ sx, sy, sz })) {
        return;
    }
    m_sharedState->SetSpacing(sx, sy, sz);
}

bool VizService::SendReloadUpdate()
{
    return m_impl->SendReloadUpdate();
}

void VizService::Impl::SetWindowLevel(double ww, double wc)
{
    m_sharedState->SetWindowLevel(ww, wc);
}

void VizService::Impl::SetVisualConfig(const PreInitConfig& cfg)
{
    // 先更新供 BuildPipeline/交互读取的模式快照，再由 SharedState 广播具体配置差异。
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

bool VizService::Impl::StartTask(
    VizService::TaskWork task,
    LoadEventKind loadKind,
    std::function<void(bool)> callback)
{
    // 1. admission 关闭或 packaged_task 无效时不创建线程，但仍把失败 callback 排入主线程完成队列。
    if (!m_isAccepting || !task.valid()) {
        SetCompletion(false, std::move(callback));
        return false;
    }

    // 2. 先在 active 列表建立 future/callback 槽，再让 taskStart 返回可 join 的 worker owner。
    std::lock_guard<std::mutex> lock(m_activeTaskMutex);
    m_activeTasks.emplace_back();
    auto entry = std::prev(m_activeTasks.end());
    try {
        entry->loadKind = loadKind;
        entry->result = task.get_future();
        entry->callback = std::move(callback);
        auto worker = m_taskStart(std::move(task));
        if (!worker.joinable()) {
            auto failedCallback = std::move(entry->callback);
            m_activeTasks.erase(entry);
            SetCompletion(false, std::move(failedCallback));
            return false;
        }
        entry->worker = std::move(worker);
        return true;
    }
    catch (...) {
        // 3. 启动异常与不可 join 线程都撤销 active 槽，并统一按异步失败完成，避免遗留半任务。
        auto failedCallback = std::move(entry->callback);
        m_activeTasks.erase(entry);
        SetCompletion(false, std::move(failedCallback));
        return false;
    }
}

bool VizService::Impl::LoadFileAsync(
    std::string path,
    VolumeLayout layout,
    std::function<void(bool isSuccess)> onComplete)
{
    // File 链：构造任务 -> 领取全局 Load admission -> 清 pending -> 登记 owner -> 启动 worker。
    // 任一后置步骤失败都会发布 FileFailed、释放 admission/owner，并把 callback 排入完成队列。
    if (!m_isAccepting || !m_dataLoadTaskService) {
        SetCompletion(false, std::move(onComplete));
        return false;
    }
    auto task = m_dataLoadTaskService->BuildLoadFileTask(
        std::move(path), std::move(layout));
    if (!task || !m_sharedState
        || !m_sharedState->StartLoad(LoadEventKind::File)) {
        SetCompletion(false, std::move(onComplete));
        return false;
    }
    if (!m_dataManager || !m_dataManager->ClearPending()
        || !SetOwnedLoad(LoadEventKind::File)) {
        if (m_dataManager) m_dataManager->ClearPending();
        m_sharedState->SetFileLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::File);
        ResetOwnedLoad(LoadEventKind::File);
        SetCompletion(false, std::move(onComplete));
        return false;
    }
    // StartTask 接管 callback 后也负责所有启动失败完成通知；移交后外层只回滚 load 状态。
    if (!StartTask(std::move(*task), LoadEventKind::File,
        std::move(onComplete))) {
        m_dataManager->ClearPending();
        m_sharedState->SetFileLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::File);
        ResetOwnedLoad(LoadEventKind::File);
        return false;
    }
    return true;
}

bool VizService::Impl::ReloadFromBufferAsync(
    VolumeBuffer buffer,
    std::function<void(bool isSuccess)> onComplete)
{
    // Reload 使用与 File 相同的 admission/owner 协议，但失败状态允许 SharedState 保留旧可信数据。
    if (!m_isAccepting || !m_dataLoadTaskService) {
        SetCompletion(false, std::move(onComplete));
        return false;
    }
    auto task = m_dataLoadTaskService->BuildReloadTask(std::move(buffer));
    if (!task || !m_sharedState
        || !m_sharedState->StartLoad(LoadEventKind::Reload)) {
        SetCompletion(false, std::move(onComplete));
        return false;
    }
    if (!m_dataManager || !m_dataManager->ClearPending()
        || !SetOwnedLoad(LoadEventKind::Reload)) {
        if (m_dataManager) m_dataManager->ClearPending();
        m_sharedState->SetReloadLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::Reload);
        ResetOwnedLoad(LoadEventKind::Reload);
        SetCompletion(false, std::move(onComplete));
        return false;
    }
    // callback 的失败完成由 StartTask 统一排队，避免在所有权移交后再次访问移动源。
    if (!StartTask(std::move(*task), LoadEventKind::Reload,
        std::move(onComplete))) {
        m_dataManager->ClearPending();
        m_sharedState->SetReloadLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::Reload);
        ResetOwnedLoad(LoadEventKind::Reload);
        return false;
    }
    return true;
}

void VizService::Impl::ExportDataAsync(
    const std::string& path,
    std::function<void(bool isSuccess)> onComplete)
{
    auto task = m_dataExportTaskService
        ? m_dataExportTaskService->BuildDataTask(path) : std::nullopt;
    if (!task) {
        SetCompletion(false, std::move(onComplete));
        return;
    }
    // StartTask 从这里开始独占 callback，并负责启动失败通知。
    StartTask(std::move(*task), LoadEventKind::None, std::move(onComplete));
}

void VizService::Impl::ExportSlicesAsync(
    const std::string& path,
    std::optional<double> rotationAngleDeg,
    std::function<void(bool isSuccess)> onComplete)
{
    const VizMode currentMode = static_cast<VizMode>(m_pendingVizModeInt.load());
    auto task = m_dataExportTaskService
        ? m_dataExportTaskService->BuildSlicesTask(
            path, rotationAngleDeg, currentMode) : std::nullopt;
    if (!task) {
        SetCompletion(false, std::move(onComplete));
        return;
    }
    // StartTask 从这里开始独占 callback，并负责启动失败通知。
    StartTask(std::move(*task), LoadEventKind::None, std::move(onComplete));
}

// ─────────────────────────────────────────────────────────────────────
// InteractiveService — 交互接口
// ─────────────────────────────────────────────────────────────────────
void VizService::Impl::SetSliceScroll(int delta)
{
    if (!m_sharedState) return;
    auto img = m_renderSnapshot ? m_renderSnapshot->image : nullptr;
    if (!img) return;
    const VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    const int axis = InteractionComputeService::GetSliceAxis(mode); // 当前切片滚动应推进的模型坐标轴
    if (axis < 0)
		return;

    double space[3] = { 0.0 };
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
    if (!m_sharedState || !m_renderSnapshot || !m_renderSnapshot->image) return;
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
    auto img = m_renderSnapshot ? m_renderSnapshot->image : nullptr;
    if (!img) return;
    // DataReady 后把联动光标重置到新体数据中心，避免沿用旧数据上的 cursor 位置导致切片落在无效区域。
    double imgCenter[3] = { 0.0 };
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
    // 更新入口按固定阶段收敛 pending/current、完成回调和渲染变更；常规由主线程 Timer 驱动，
    // 外部 reload handler 也会在调用线程同步进入，因此本函数不提供线程切换保证。
    // 1. 先领取所有 ready 任务并 join worker，load 的 pending 只由 owner 提交。
    SendTasks();

    // 2. load 终态按完整 payload 顺序消费；队列锁只覆盖弹出，VTK 与 callback 始终在锁外。
    LoadNotice loadNotice;
    while (RemoveLoadNotice(loadNotice)) {
        if (!loadNotice.isStateSet) {
            if (loadNotice.isSucceeded) {
                ClearStrategyCache();
                SetPendingFlags(UpdateFlags::All);
                if (!BuildPipeline()) {
                    loadNotice.isSucceeded = false;
                    ClearLoadFail(loadNotice.kind);
                }
            }
            else {
                ClearLoadFail(loadNotice.kind);
            }
        }

        // 非发起 view 只同步自己的最终显示；只有 owner 能释放全局 admission 和准备业务 callback。
        if (!GetOwnedLoad(loadNotice.kind)) {
            continue;
        }
        if (!m_sharedState || !m_sharedState->ResetLoad(loadNotice.kind)) {
            CreateLoadNotice(loadNotice.kind, loadNotice.isSucceeded, true);
            break;
        }
        ResetOwnedLoad(loadNotice.kind);
        SetCompletion(loadNotice.isSucceeded, std::move(m_ownedCallback));
    }

    // 3. spacing / mode 等非 load 结构变化继续使用独立门铃，不与 load 终态混槽。
    if (m_hasDataRefreshNeed.exchange(false)) {
        if (!BuildPipeline()) m_hasDataRefreshNeed = true;
    }
    SetStrategyState();

    // 4. owner 已释放 admission 后再执行回调，允许业务方安全重入下一次 load。
    SendCompletions();
}

bool VizService::Impl::SendReloadUpdate()
{
    SetPendingFlags(UpdateFlags::All);
    return BuildPipeline();
}

void VizService::Impl::SendTasks()
{
    // 1. 锁内只把 ready future 从 active 列表 splice 到局部列表；未完成任务继续由 service 持有。
    std::list<ActiveTask> readyTasks;
    {
        std::lock_guard<std::mutex> lock(m_activeTaskMutex);
        for (auto entry = m_activeTasks.begin(); entry != m_activeTasks.end();) {
            const bool isReady = entry->result.valid()
                && entry->result.wait_for(std::chrono::seconds(0))
                    == std::future_status::ready;
            if (!isReady) {
                ++entry;
                continue;
            }
            const auto ready = entry++;
            readyTasks.splice(readyTasks.end(), m_activeTasks, ready);
        }
    }
    // 2. 锁外读取结果并 join worker，再区分普通任务 callback 与 Load/Reload 提交链。
    for (auto& task : readyTasks) {
        bool isSuccess = false;
        try { isSuccess = task.result.get(); }
        catch (...) { isSuccess = false; }
        if (task.worker.joinable()) task.worker.join();
        SetTaskResult(std::move(task), isSuccess);
    }
}

void VizService::Impl::SetTaskResult(ActiveTask task, bool isSuccess)
{
    if (task.loadKind == LoadEventKind::None) {
        SetCompletion(isSuccess, std::move(task.callback));
        return;
    }
    SetLoadResult(std::move(task), isSuccess);
}

void VizService::Impl::SetLoadResult(ActiveTask task, bool isSuccess)
{
    // worker 成功只表示 pending 已准备；这里还必须把 pending 提交为 current，提交失败统一降级为失败。
    bool hasPending = false;
    if (isSuccess && m_dataManager) {
        isSuccess = m_dataManager->SetCurrentFromPending(hasPending) && hasPending;
    }
    if (!isSuccess && m_dataManager) m_dataManager->ClearPending();

    // callback 暂存到 owner 槽，待共享终态广播、各视图管线同步和 admission 释放后再执行。
    m_ownedCallback = std::move(task.callback);
    if (!m_sharedState || !m_dataManager) return;
    if (isSuccess) {
        const auto range = m_dataManager->GetScalarRange();
        const auto spacing = m_dataManager->GetSpacing();
        if (task.loadKind == LoadEventKind::File) {
            m_sharedState->SetFileDataReady(range[0], range[1], spacing);
        }
        else {
            m_sharedState->SetReloadDataReady(range[0], range[1], spacing);
        }
    }
    else if (task.loadKind == LoadEventKind::File) {
        m_sharedState->SetFileLoadFailed();
    }
    else {
        m_sharedState->SetReloadLoadFailed();
    }
}

void VizService::Impl::SetCompletion(
    bool isSuccess,
    std::function<void(bool)> callback)
{
    if (!callback) return;
    std::lock_guard<std::mutex> lock(m_completionMutex);
    m_completions.push_back({ isSuccess, std::move(callback) });
}

void VizService::Impl::SendCompletions()
{
    // 整批 swap 后在锁外调用，允许 callback 重入下一任务，也隔离单个 callback 异常。
    std::deque<Completion> completions;
    {
        std::lock_guard<std::mutex> lock(m_completionMutex);
        completions.swap(m_completions);
    }
    for (auto& completion : completions) {
        try { completion.callback(completion.isSuccess); }
        catch (const std::exception& error) {
            std::cerr << "[VizService] Completion failed: " << error.what() << '\n';
        }
        catch (...) {
            std::cerr << "[VizService] Completion failed with an unknown exception.\n";
        }
    }
}

bool VizService::Impl::CreateLoadNotice(
    LoadEventKind loadEventKind,
    bool isSucceeded,
    bool isStateSet)
{
    if (loadEventKind != LoadEventKind::File
        && loadEventKind != LoadEventKind::Reload) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_loadNoticeMutex);
    const auto ownedLoadKind = static_cast<LoadEventKind>(m_ownedLoadKind.load());
    if (ownedLoadKind != loadEventKind) {
        // 非 owner view 不需要重放每个历史终态；保留最新 payload 即可恢复最终显示，并避免停更 view 无界增长。
        m_loadNotices.clear();
    }
    m_loadNotices.push_back({ loadEventKind, isSucceeded, isStateSet });
    return true;
}

bool VizService::Impl::RemoveLoadNotice(LoadNotice& loadNotice)
{
    std::lock_guard<std::mutex> lock(m_loadNoticeMutex);
    if (m_loadNotices.empty()) {
        return false;
    }
    loadNotice = m_loadNotices.front();
    m_loadNotices.pop_front();
    return true;
}

bool VizService::Impl::SetOwnedLoad(LoadEventKind loadEventKind)
{
    if (loadEventKind != LoadEventKind::File
        && loadEventKind != LoadEventKind::Reload) {
        return false;
    }
    int expectedKind = static_cast<int>(LoadEventKind::None);
    const bool isOwned = m_ownedLoadKind.compare_exchange_strong(
        expectedKind,
        static_cast<int>(loadEventKind));
    if (isOwned) {
        std::lock_guard<std::mutex> lock(m_loadNoticeMutex);
        // 上一事务的广播已经结束；新 owner 不得让同 kind 的旧 notice 通过 ABA 释放新事务。
        m_loadNotices.clear();
    }
    return isOwned;
}

bool VizService::Impl::GetOwnedLoad(LoadEventKind loadEventKind) const
{
    return m_ownedLoadKind.load() == static_cast<int>(loadEventKind);
}

bool VizService::Impl::ResetOwnedLoad(LoadEventKind loadEventKind)
{
    int expectedKind = static_cast<int>(loadEventKind);
    return m_ownedLoadKind.compare_exchange_strong(
        expectedKind,
        static_cast<int>(LoadEventKind::None));
}

void VizService::Impl::SetDataRefresh()
{
    // 状态发布线程只置结构重建门铃，不在回调栈内修改渲染管线。
    m_hasDataRefreshNeed = true;
}

// ─────────────────────────────────────────────────────────────────────
// 私有辅助
// ─────────────────────────────────────────────────────────────────────
bool VizService::Impl::BuildPipeline()
{
    // 这一步只处理“结构性变化后的管线重建”：选对 Strategy、喂入最新图像、重新挂接渲染器。
    // 具体材质、TF、窗宽窗位等参数同步故意留到后续增量同步阶段再做。
    // 读取最新模式快照但不清除；后续交互和导出仍需使用同一模式。
    const auto oldSnapshot = m_renderSnapshot;
    const auto oldStrategy = m_currentStrategy;
    std::shared_ptr<AbstractVisualStrategy> candidateStrategy;
    auto renderEffect = m_renderEffect.lock();
    bool isCandidateAttached = false;
    try {
        if (!m_dataManager || !m_sharedState || !m_renderer) {
            return false;
        }
        const VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
        const auto currentSnapshot = m_dataManager->GetImageSnapshot();
        if (!currentSnapshot || !currentSnapshot->image) return false;
        int dims[3] = { 0, 0, 0 };
        currentSnapshot->image->GetDimensions(dims);
        if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) return false;

        // 同模式输入换代时不能原位改写当前可见 Strategy。新建候选，让 image、
        // mask、stamp 与 effect 在不可见对象上完整收敛后再交换，消除无 mask 中间帧。
        const bool hasInputChange =
            oldStrategy
            && oldSnapshot
            && currentSnapshot != oldSnapshot;
        if (hasInputChange && renderEffect && oldStrategy) {
            const auto effectState =
                oldStrategy->GetRenderEffectState();
            if (effectState.status
                    == RenderEffectStatus::Staged
                || effectState.status
                    == RenderEffectStatus::Ready) {
                // binding 换代不能穿过未完成的 effect 事务；否则 Feature root
                // 会把旧 staged target 丢失判定为 ContextLost。
                std::cout
                    << "[Render][InputSwap] deferred"
                    << " oldVersion=" << oldSnapshot->version
                    << " nextVersion="
                    << currentSnapshot->version
                    << " status="
                    << static_cast<int>(effectState.status)
                    << " revision="
                    << effectState.stagedRevision
                    << '\n';
                return false;
            }
        }
        candidateStrategy = hasInputChange
            ? CreateStrategy(mode)
            : GetStrategy(mode);
        if (!candidateStrategy) return false;
        if (hasInputChange) {
            std::cout
                << "[Render][InputSwap] candidate"
                << " oldVersion=" << oldSnapshot->version
                << " nextVersion=" << currentSnapshot->version
                << " oldStrategy="
                << static_cast<const void*>(oldStrategy.get())
                << " nextStrategy="
                << static_cast<const void*>(candidateStrategy.get())
                << '\n';
        }

        // 候选 strategy 先完成输入校验，成功后才替换当前渲染真源。
        candidateStrategy->SetInputData(currentSnapshot->image);
        candidateStrategy->SetInputMask(
            currentSnapshot->validityMask);
        const RenderInputStamp inputStamp = {
            currentSnapshot->image.GetPointer(),
            currentSnapshot->version
        };
        if (!candidateStrategy->SetRenderInputStamp(inputStamp)) {
            return false;
        }
        const bool isStrategyChanged = candidateStrategy != oldStrategy;
        if (isStrategyChanged && renderEffect) {
            if (!m_renderWindow
                || !candidateStrategy->AttachRenderEffect(
                    renderEffect, RenderBindingUse::Candidate)) {
                return false;
            }

            candidateStrategy->AttachRenderer(m_renderer);
            isCandidateAttached = true;
            candidateStrategy->SetCamera(m_renderer);
            candidateStrategy->SetVisualState(
                GetRenderParams(UpdateFlags::All),
                UpdateFlags::All);

            // 2. 旧 strategy 保持可见；关闭 swap 后在同一 context 的背缓冲预热候选，
            //    Render 只用于 texture upload/shader realization，不向窗口发布未裁切帧。
            const vtkTypeBool isSwapEnabled = m_renderWindow->GetSwapBuffers();
            m_renderWindow->SwapBuffersOff();
            try {
                for (int renderCount = 0; renderCount < 2; ++renderCount) {
                    m_renderWindow->Render();
                    if (candidateStrategy->GetRenderEffectState().status
                        != RenderEffectStatus::Staged) {
                        break;
                    }
                }
            }
            catch (...) {
                m_renderWindow->SetSwapBuffers(isSwapEnabled);
                throw;
            }
            m_renderWindow->SetSwapBuffers(isSwapEnabled);

            // 3. Candidate 只接收 effect root 重放的 committed revision。
            //    Ready 表示 GPU 预热成功，此时提交同一个 revision 后才可切 Current。
            const auto effectState =
                candidateStrategy->GetRenderEffectState();
            const bool isReadyCommitted =
                effectState.status == RenderEffectStatus::Ready
                && effectState.stagedRevision != 0
                && candidateStrategy->SetRenderEffectCommit(
                    effectState.stagedRevision);
            const bool hasNoReplay =
                effectState.status == RenderEffectStatus::Idle;
            const bool isAlreadyCommitted =
                effectState.status == RenderEffectStatus::Committed;
            if ((!isReadyCommitted && !hasNoReplay && !isAlreadyCommitted)
                || !candidateStrategy->SetRenderEffectUse(
                    RenderBindingUse::Current)) {
                (void)candidateStrategy->ClearRenderEffectStage(
                    effectState.stagedRevision);
                candidateStrategy->DetachRenderer(m_renderer);
                (void)candidateStrategy->DetachRenderEffect(
                    renderEffect.get());
                isCandidateAttached = false;
                return false;
            }
        }

        // 4. Commit 与替换之间没有 Render；下一帧只能看见旧 active 或候选已提交的 active。
        m_renderSnapshot = currentSnapshot;
        SetCurrentStrategy(candidateStrategy, isCandidateAttached);
        isCandidateAttached = false;
        SetCursorCenter();
        SetRendererBg();
        SetSyncNeeded();
        if (hasInputChange) {
            // 输入换代后只保留新 current mode。其它 mode 的隐藏 Strategy 仍强持有
            // 旧 image/mask，若继续缓存会让历次物化对象无法释放。
            std::map<VizMode,
                std::shared_ptr<AbstractVisualStrategy>>
                nextCache;
            nextCache.emplace(mode, candidateStrategy);
            const std::size_t retiredCount =
                m_strategyCache.size();
            m_strategyCache.swap(nextCache);
            std::cout
                << "[Render][InputSwap] complete"
                << " version=" << currentSnapshot->version
                << " currentStrategy="
                << static_cast<const void*>(candidateStrategy.get())
                << " retiredCache=" << retiredCount
                << '\n';
        }
        return true;
    }
    catch (const std::exception& error) {
        std::cerr << "[BuildPipeline] Failed: " << error.what() << '\n';
    }
    catch (...) {
        std::cerr << "[BuildPipeline] Failed with an unknown exception.\n";
    }

    if (isCandidateAttached && candidateStrategy && m_renderer) {
        candidateStrategy->DetachRenderer(m_renderer);
        if (renderEffect) {
            (void)candidateStrategy->DetachRenderEffect(
                renderEffect.get());
        }
    }

    // 回滚只恢复上一个已提交的 snapshot/strategy，不反写 DataManager current。
    try {
        if (m_currentStrategy != oldStrategy && m_currentStrategy && m_renderer) {
            m_currentStrategy->DetachRenderer(m_renderer);
        }
        m_currentStrategy = oldStrategy;
        if (m_currentStrategy && m_renderer) {
            m_currentStrategy->AttachRenderer(m_renderer);
            m_currentStrategy->SetCamera(m_renderer);
        }
    }
    catch (...) {
        m_currentStrategy = oldStrategy;
    }
    m_renderSnapshot = oldSnapshot;
    return false;
}

void VizService::Impl::SetStrategyState()
{
    bool isExpected = true;
    // CAS 同时领取并清掉同步闸门；没有请求时不触碰 pending 位图。
    if (!m_hasSyncNeed.compare_exchange_strong(isExpected, false)) return;
    // 当前无 Strategy 时位图仍保留；只有后续再次置同步闸门才会消费。
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
    if (((flags & UpdateFlags::Interaction) != UpdateFlags::None) && m_renderWindow) {
        const bool isInteracting = m_sharedState->GetIsInteracting();
        m_renderWindow->SetDesiredUpdateRate(isInteracting ? 15.0 : 0.001);
    }

    // 背景色同步（数据无关，直接写渲染器）
    if (((flags & UpdateFlags::Background) != UpdateFlags::None) && m_renderer) {
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

    // Strategy 已消费本次快照，发布本帧 Render 请求；Timer 随后用 ResetDirty() 领取。
    m_isDirty = true;
}

void VizService::Impl::ClearLoadFail(LoadEventKind loadEventKind)
{
    if (loadEventKind == LoadEventKind::Reload
        && m_sharedState
        && m_sharedState->GetDataTrustedState() == LoadState::Succeeded) {
        // Reload 失败不替换 current；保留旧 snapshot/strategy/overlay，使可信数据继续可见。
        m_isDirty = true;
        return;
    }

    std::cerr << "[ClearLoadFail] Load failed; clearing pipeline state.\n";

    // 清理策略缓存（无有效数据，不应保留旧 Strategy）
    ClearStrategyCache();
    m_renderSnapshot.reset();

    // 失败终态取消尚未消费的重建与增量，防止旧 DataReady 在后续帧恢复管线。
    m_hasDataRefreshNeed = false;
    m_pendingFlags = 0;

    // 标脏使渲染器刷新空场景
    m_isDirty = true;
}

RenderParams VizService::Impl::GetRenderParams(UpdateFlags flags) const
{
    RenderParams p;

    // RenderParams 是当前这一帧需要下发给 Strategy 的“最小快照”，
    // 只按 flags 拿必要字段，避免每次同步都把全部状态搬运一遍。

    if (((flags & UpdateFlags::Cursor) != UpdateFlags::None) || ((flags & UpdateFlags::Transform) != UpdateFlags::None)) {
        auto pos = m_sharedState->GetCursorWorld();
        auto rawPos = m_sharedState->GetCursorRawWorld();
        p.cursor = { pos[0], pos[1], pos[2] };
        p.cursorRaw = { rawPos[0], rawPos[1], rawPos[2] };
        p.cursorAxis = m_sharedState->GetCursorAxis();
        p.modelMatrix = m_sharedState->GetModelMatrix();
    }
    if (((flags & UpdateFlags::TF) != UpdateFlags::None)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        m_sharedState->GetTFNodes(p.tfNodes);
    }

    if (((flags & UpdateFlags::WindowLevel) != UpdateFlags::None)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.windowLevel = m_sharedState->GetWindowLevel();
    }

    if (((flags & UpdateFlags::Material) != UpdateFlags::None)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.material = m_sharedState->GetMaterial();
        m_sharedState->GetTFNodes(p.tfNodes);
    }

    if (((flags & UpdateFlags::Interaction) != UpdateFlags::None))
        p.isInteracting = m_sharedState->GetIsInteracting();

    if (((flags & UpdateFlags::IsoValue) != UpdateFlags::None))
        p.isoValue = m_sharedState->GetIsoValue();

    if (((flags & UpdateFlags::Visibility) != UpdateFlags::None))
		p.visibilityMask = m_sharedState->GetVisibilityMask();

    return p;
}

std::shared_ptr<AbstractVisualStrategy> VizService::Impl::GetStrategy(VizMode mode)
{
    // Strategy 以 VizMode 为键缓存，说明模式切换是“复用既有渲染对象”而不是“每次全新构建”。
    auto it = m_strategyCache.find(mode);
    if (it != m_strategyCache.end()) return it->second;

    auto strategy = CreateStrategy(mode);
    if (strategy) {
        m_strategyCache[mode] = strategy;
    }
    return strategy;
}

std::shared_ptr<AbstractVisualStrategy>
VizService::Impl::CreateStrategy(const VizMode mode)
{
    std::shared_ptr<AbstractVisualStrategy> strategy;
    switch (mode) {
    case VizMode::Volume:
        strategy = std::make_shared<VolumeStrategy>();
        break;
    case VizMode::IsoSurface:
        strategy = std::make_shared<IsoSurfaceStrategy>();
        break;
    case VizMode::SliceTop_down:
        strategy = std::make_shared<SliceStrategy>(Orientation::Top_down);
        break;
    case VizMode::SliceFront_back:
        strategy = std::make_shared<SliceStrategy>(Orientation::Front_back);
        break;
    case VizMode::SliceLeft_right:
        strategy = std::make_shared<SliceStrategy>(Orientation::Left_right);
        break;
    case VizMode::CompositeVolume:
        strategy = std::make_shared<CompositeStrategy>(VizMode::CompositeVolume);
        break;
    case VizMode::CompositeIsoSurface:
        strategy = std::make_shared<CompositeStrategy>(VizMode::CompositeIsoSurface);
        break;
    default:
        return nullptr;
    }
    return strategy;
}
