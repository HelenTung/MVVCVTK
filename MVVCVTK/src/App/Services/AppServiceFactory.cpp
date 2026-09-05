#include "App/Services/AppServiceFactory.h"
#include "App/Services/AppPorts.h"
#include "App/Services/FeatureViewService.h"
#include "AppDataExportTaskService.h"
#include "AppDataLoadTaskService.h"
#include "AppState.h"
#include "DataConverters.h"
#include "DataManager.h"
#include "InteractionComputeService.h"
#include "Interaction/InteractionPorts.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Contracts/RenderBindPort.h"
#include "Render/Contracts/RenderEffect.h"
#include "Render/Contracts/RenderStrategyFactory.h"
#include "Render/Contracts/VisualStrategy.h"
#include <vtkActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkProp3D.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
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
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────
// 构造 / 析构
// ─────────────────────────────────────────────────────────────────────
// 单个视图的应用服务实现：共享 DataManager/State，拥有该视图的 strategy/overlay 与异步任务槽。
// worker 只准备 pending 数据或导出结果；current 提交、VTK 管线切换和业务 callback 统一由 host tick 收口。
class AppRuntime final {
public:
    using TaskWork = AppTaskWork;
    using WorkerStart = AppWorkerStart;

    explicit AppRuntime(AppServiceArgs args);
    ~AppRuntime();

    bool SetRenderContext(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren);
    void SetVizMode(VizMode mode);
    VizMode GetVizMode() const;
    void SetMaterial(const MaterialParams& mat);
    MaterialParams GetMaterial() const;
    void SetOpacity(double opacity);
    double GetOpacity() const;
    bool SetVolumeTransferFunction(
        const VolumeTransferFunction& function);
    bool SetAutoTransfer(
        const VolumeTransferFunction& function);
    bool ResetTransfer(
        const VolumeTransferFunction& function);
    VolumeTransferFunction GetVolumeTransferFunction() const;
    bool GetTransferAuto() const;
    void SetIsoThreshold(double val);
    double GetIsoThreshold() const;
    void SetBackground(const BackgroundColor& bg);
    BackgroundColor GetBackground() const;
    bool SetSpacing(double sx, double sy, double sz);
    std::array<double, 3> GetSpacing() const;
    void SetWindowLevel(double ww, double wc);
    bool ResetWindowLevel();
    bool ResetWindowLevel(const WindowLevelParams& windowLevel);
    WindowLevelMode GetWindowLevelMode() const;
    void SetVisualConfig(const PreInitConfig& cfg);
    PreInitConfig GetVisualConfig() const;
    std::array<double, 2> GetScalarRange() const;
    bool SetVolumeQuality(VolumeQuality quality);
    VolumeQuality GetVolumeQuality() const;
    bool SetFeatureActive(
        const FeatureSource& source,
        bool isActive);
    bool GetIsFeatureActive() const;
    bool SetGradientOpacity(const std::vector<GradientOpacityNode>& nodes);
    std::vector<GradientOpacityNode> GetGradientOpacity() const;
    bool SetDenoiseOn(bool isDenoiseOn);
    bool GetDenoiseOn() const;
    LoadState GetFileLoadState() const;
    LoadState GetReloadLoadState() const;
    TaskAdmissionResult LoadFileAsync(std::string path,
        VolumeLayout layout,
        std::function<void(bool isSuccess)> onComplete);
    TaskAdmissionResult ReloadFromBufferAsync(
        VolumeBuffer buffer,
        std::function<void(bool isSuccess)> onComplete);
    TaskAdmissionResult ExportDataAsync(
        std::string outputDir,
        std::string extension,
        std::function<void(bool isSuccess)> onComplete);
    TaskAdmissionResult ExportSlicesAsync(const std::string& path,
        std::optional<double> rotationAngleDeg,
        std::function<void(bool isSuccess)> onComplete);
    void SetSliceScroll(int delta);
    void SetCursorWorldPosition(double worldPos[3], int axis);
    bool SetCursorState(
        const std::array<double, 3>& worldPosition,
        AppCursorAxis axis);
    std::array<double, 3> GetCursorWorld();
    AppCursorAxis GetCursorAxis() const;
    bool SetInteracting(
        const InteractionSource& source,
        bool isInteracting);
    bool GetIsInteracting() const;
    int GetPlaneAxis(vtkActor* actor);
    vtkProp3D* GetMainProp();
    void SetModelMatrix(vtkMatrix4x4* modelToWorldMatrix);
    std::array<double, 16> GetModelMatrix();
    WindowLevelParams GetWindowLevel() const;
    int GetNavigationAxis() const;
    void SetElementVisible(uint32_t flagBit, bool isVisible);
    uint32_t GetVisibilityMask() const;
    void SetWindowLevelDrag(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC);
    void SetModelTransform(double translate[3], double rotate[3], double scale[3]);
    void SetModelTransformReset();
    void GetModelPositionFromWorld(const double w[3], double m[3]) const;
    void GetWorldPositionFromModel(const double m[3], double w[3]) const;
    bool SendUpdates();
    bool SendReloadUpdate();
    bool GetDirty() const;
    void SetDirty();
    bool ResetDirty();
    void SetCurrentStrategy(
        std::shared_ptr<AbstractVisualStrategy> newStrategy,
        VizMode mode,
        bool isRendererAttached = false);
    bool AttachOverlay(std::shared_ptr<FeatureOverlay> overlay);
    void RemoveOverlay(
        std::shared_ptr<FeatureOverlay> overlay) noexcept;
    void ClearOverlays() noexcept;
    RenderInputStamp GetRenderInputStamp() const;
    bool AttachRenderEffect(std::shared_ptr<RenderEffect> effect);
    bool DetachRenderEffect(const RenderEffect* effect);
    bool BuildDataStage(const VtkImageGridSnapshot& snapshot);
    bool SetViewStage(const VtkImageGridSnapshot& snapshot);
    bool ResetViewStage();
    bool ClearDataStage();
    void SetDataStageComplete() noexcept;
    bool StartViewUpdate();
    bool SetViewUpdateCommit(
        bool isCommitted,
        UpdateFlags& pendingFlags);
    void SendViewUpdateFlags(UpdateFlags flags) noexcept;
    bool SendSessionUpdate(const AppSessionUpdate& update);
    bool SetTaskStopping();
    bool StopTasks(
        std::chrono::steady_clock::time_point deadline);

private:
    friend class ViewPortAdapter;

    // 相机快照只参与单视图管线事务，不进入 App/Host 公共值类型。
    struct CameraState final {
        bool isValid = false;
        std::array<double, 3> position = { 0.0, 0.0, 1.0 };
        std::array<double, 3> focalPoint = { 0.0, 0.0, 0.0 };
        std::array<double, 3> viewUp = { 0.0, 1.0, 0.0 };
        std::array<double, 2> clippingRange = { 0.1, 1000.0 };
        double parallelScale = 1.0;
        double viewAngle = 30.0;
        bool isParallelProjection = false;
    };

    struct DataStage final {
        VtkImageGridSnapshot oldSnapshot;
        VtkImageGridSnapshot nextSnapshot;
        std::shared_ptr<AbstractVisualStrategy> oldStrategy;
        std::shared_ptr<AbstractVisualStrategy> nextStrategy;
        std::optional<VizMode> oldMode;
        CameraState oldCamera;
        DataReadyState readyState;
        RenderParams nextParams;
        WindowLevelParams autoWindowLevel;
        VizMode mode = VizMode::Volume;
        bool isEffectAttached = false;
        bool hasDefaultTransfer = false;
        bool isCommitted = false;
    };

    class ObserverGate final : public IStateEventSink {
    public:
        explicit ObserverGate(AppRuntime* owner)
            : m_owner(owner)
        {
        }

        void SendFlags(UpdateFlags flags) override;
        bool StopOwner();

    private:
        bool StopCall();

        std::mutex m_mutex;
        std::condition_variable m_stopped;
        AppRuntime* m_owner = nullptr;
        std::size_t m_callCount = 0;
    };

    struct LoadNotice {
        LoadEventKind kind = LoadEventKind::None; // 区分 File/Reload，决定提交 pending 与失败清场策略。
        bool isSucceeded = false; // worker/共享状态最终结论，可在管线构建失败时降级为 false。
        bool isStateSet = false; // true 表示共享终态已发布，仅等待 owner 释放 admission。
    };

    struct ActiveTask final {
        LoadEventKind loadKind = LoadEventKind::None; // None 用于普通导出，File/Reload 需要加载事务收尾。
        std::future<bool> result; // worker 只返回准备结果，不直接触碰 VTK 管线。
        std::function<void(bool)> callback; // 延迟到主线程完成提交/管线同步后执行。
    };

    struct Completion final {
        bool isSuccess = false; // 已包含 worker、pending 提交和 BuildPipeline 的综合结果。
        std::function<void(bool)> callback; // SendCompletions 在内部锁外调用，允许安全发起下一事务。
    };

    struct ActiveFeature final {
        FeatureSource source;
    };

    bool SetRenderBinding(vtkSmartPointer<vtkRenderWindow> win,
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
    std::optional<VolumeTransferFunction> GetDefaultVolumeTransfer(
        const VtkImageGridSnapshot& snapshot);
    bool GetVolumeTransferValid(
        const VolumeTransferFunction& function) const;
    bool GetTransferRangeValid(
        const VolumeTransferFunction& function,
        const std::array<double, 2>& scalarRange) const;
    double GetRenderRate(bool isInteracting) const noexcept;
    VolumeQuality GetTargetQuality() const;
    bool SetStrategyState();
    void ClearLoadFail(LoadEventKind loadEventKind);
    RenderParams GetRenderParams(UpdateFlags flags) const;
    std::shared_ptr<AbstractVisualStrategy> CreateStrategy(VizMode mode);
    CameraState GetCameraState() const;
    bool SetCameraState(const CameraState& state);
    bool SetModeCamera(VizMode mode, const VtkImageGridSnapshot& snapshot);
    bool SetCameraCenter(
        const std::array<double, 16>& modelToWorld,
        const VtkImageGridSnapshot& snapshot);
    void SetRendererBg();
    void ClearStrategies();
    bool GetDataReadyState(
        const VtkImageGridSnapshot& snapshot,
        DataReadyState& state) const;
    std::optional<WindowLevelParams> GetAutoWindowLevel(
        const std::array<double, 2>& scalarRange) const;
    void SetSyncNeeded();
    void SetPendingFlags(UpdateFlags flags);
    void SetDataRefresh();
    TaskAdmissionResult StartTask(AppRuntime::TaskWork task,
        LoadEventKind loadKind,
        std::function<void(bool)> callback);

    // Service 共享持有会话数据源；File/Reload worker 都只 staging pending，
    // 再由 SendUpdates 所在线程的 owner 提交为 current。
    std::shared_ptr<AbstractDataManager> m_dataManager;
    // 当前主渲染策略的共享 owner；替换前从旧 renderer 脱离，随后由缓存决定是否继续保留。
    std::shared_ptr<AbstractVisualStrategy> m_currentStrategy;
    // 只有 BuildPipeline 成功提交后才更新；renderer 重绑和相机恢复禁止读取 pending mode。
    std::optional<VizMode> m_currentMode;
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
    std::vector<std::shared_ptr<FeatureOverlay>> m_overlays;
    // 任务 builder 只准备 bool 结果，不越界发布终态或 callback。
    std::shared_ptr<AppDataLoadTaskService> m_dataLoadTaskService;
    std::shared_ptr<AppDataExportTaskService> m_dataExportTaskService;
    // 构造期冻结 Strategy 创建入口；生产默认进入 Render 层唯一工厂，测试可注入失败路径。
    StrategyCreate m_strategyCreate;
    // 本 service 持有 DataManager 当前批次 owner；各 view 共享只读 image/scalars，旧批次随最后一个 owner 释放。
    VtkImageGridSnapshot m_renderSnapshot;
    // observer 把 kind/result 作为一个完整终态 payload 入队；锁只保护队列，不覆盖 VTK 或 callback 调用。
    std::deque<LoadNotice> m_loadNotices;
    mutable std::mutex m_loadNoticeMutex;
    // 只有实际启动异步 load/reload 的 service 持有 owner；Host 同步事务不写入该槽。
    std::atomic<int> m_ownedLoadKind{ static_cast<int>(LoadEventKind::None) };
    // 会话运行态的共享 owner 与单一事实源；前处理、交互和渲染参数都通过它发布/读取。
    std::shared_ptr<SharedInteractionState> m_sharedState;
    // 状态广播源的共享 owner；observer 只 weak-lock 回调门禁，不反向延长 Impl 生命周期。
    std::shared_ptr<IStateEventSource> m_stateEventSource;
    // 广播只共享该门禁；本 runtime 析构先阻断新回调并等待已进入回调退出。
    std::shared_ptr<ObserverGate> m_observerGate;
    // 每个 runtime 独占展示状态；Session 共享状态只保留数据坐标、spacing 与 cursor。
    std::shared_ptr<ViewPresentationState> m_viewState;

    // SetVizMode/SetVisualConfig 写入最新快照；管线重建、切片交互和导出读取但不清零。
    std::atomic<int> m_pendingVizModeInt{ static_cast<int>(VizMode::IsoSurface) };
    // DataReady、Spacing 或模式变化置位；主线程重建前 exchange(false)，失败清场也会清零。
    std::atomic<bool> m_hasDataRefreshNeed{ false };
    mutable std::mutex m_viewConfigMutex;
    VolumeQuality m_requestedQuality = VolumeQuality::Auto;
    VolumeQuality m_appliedQuality = VolumeQuality::Auto;
    std::vector<ActiveFeature> m_activeFeatures;
    std::vector<GradientOpacityNode> m_gradientOpacity;
    bool m_isDenoiseOn = false;
    // Host 多 View 共享频率缓存，但每个 View 仍独立保存最终 TF 值快照。
    std::shared_ptr<HistogramConverter> m_histogram;
    std::shared_ptr<AppTaskExecutor> m_taskExecutor;
    std::function<bool(LoadEventKind)> m_setLoadCommit;
    std::optional<DataStage> m_dataStage;
    // Host 多 View 全部清除已提交 stage 后，load owner 取走同版本候选并做一次共享提交。
    std::optional<DataReadyState> m_readyState;
    std::list<ActiveTask> m_activeTasks;
    mutable std::mutex m_activeTaskMutex;
    std::deque<Completion> m_completions;
    mutable std::mutex m_completionMutex;
    std::function<void(bool)> m_ownedCallback;
    std::atomic<bool> m_isAccepting{ true };
    // RenderContext 绑定线程，也是本 service 访问 VTK 对象的唯一归属线程。
    // 未绑定时不存在合法提交者，状态可以累积，但不能下发到 Strategy 或 VTK。
    mutable std::mutex m_ownerMutex;
    std::thread::id m_ownerThread;
    bool m_hasOwnerThread = false;

    bool GetIsOwnerThread() const
    {
        const std::lock_guard<std::mutex> lock(m_ownerMutex);
        return m_hasOwnerThread
            && m_ownerThread == std::this_thread::get_id();
    }
};

// 每个 AppRuntime 只创建固定 worker；任务本身由 future 回传，worker 不捕获 runtime。
class AppTaskExecutor final {
    class TaskLane final {
        struct QueuedTask final {
            std::uint64_t id = 0;
            AppTaskWork work;
            TaskStopSource stopSource;
        };

    public:
        TaskLane(
            std::size_t workerCount,
            std::size_t taskLimit,
            const AppWorkerStart& workerStart)
            : m_taskLimit(taskLimit)
            , m_workerStart(workerStart)
        {
            if (workerCount == 0 || taskLimit == 0
                || !m_workerStart) {
                throw std::invalid_argument(
                    "TaskLane requires workers and capacity.");
            }
            CreateWorkers(workerCount);
        }

        ~TaskLane()
        {
            (void)Stop(
                std::chrono::steady_clock::time_point::max());
        }

        TaskLane(const TaskLane&) = delete;
        TaskLane& operator=(const TaskLane&) = delete;

        TaskAdmissionResult SendTask(AppTaskWork work)
        {
            if (!work.valid()) {
                return TaskAdmissionResult::InvalidRequest;
            }

            std::uint64_t taskId = 0;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_isAccepting) {
                    return TaskAdmissionResult::Stopping;
                }
                if (m_stopSources.size() >= m_taskLimit) {
                    return TaskAdmissionResult::QueueFull;
                }
                if (m_nextId == 0
                    || m_nextId
                        == std::numeric_limits<std::uint64_t>::max()) {
                    if (!m_stopSources.empty()) {
                        return TaskAdmissionResult::QueueFull;
                    }
                    m_nextId = 1;
                }
                taskId = m_nextId++;
                TaskStopSource stopSource;
                try {
                    const auto inserted = m_stopSources.emplace(
                        taskId, stopSource);
                    try {
                        m_tasks.push_back({
                            taskId,
                            std::move(work),
                            std::move(stopSource)
                        });
                    }
                    catch (...) {
                        m_stopSources.erase(inserted.first);
                        return TaskAdmissionResult::Unavailable;
                    }
                }
                catch (...) {
                    return TaskAdmissionResult::Unavailable;
                }
            }
            m_ready.notify_one();
            return TaskAdmissionResult::Accepted;
        }

        bool StartStop()
        {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_isAccepting = false;
                m_isStopping = true;
                for (auto& entry : m_stopSources) {
                    (void)entry.second.Stop();
                }
                if (m_liveWorkers == 0) {
                    m_isStopped = true;
                }
            }
            m_ready.notify_all();
            m_stopped.notify_all();
            return true;
        }

        bool Stop(
            const std::chrono::steady_clock::time_point deadline)
        {
            (void)StartStop();
            bool isStopped = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                const auto stopped = [this] { return m_isStopped; };
                if (deadline
                    == std::chrono::steady_clock::time_point::max()) {
                    m_stopped.wait(lock, stopped);
                    isStopped = true;
                }
                else {
                    isStopped = m_stopped.wait_until(
                        lock, deadline, stopped);
                }
            }
            if (!isStopped) return false;

            const std::lock_guard<std::mutex> joinLock(m_joinMutex);
            for (auto& worker : m_workers) {
                if (worker.joinable()) worker.join();
            }
            return true;
        }

    private:
        void CreateWorkers(const std::size_t workerCount)
        {
            try {
                m_workers.reserve(workerCount);
                for (std::size_t index = 0;
                    index < workerCount; ++index) {
                    auto worker = m_workerStart(
                        [this] { OnWorker(); });
                    if (!worker.joinable()) {
                        throw std::runtime_error(
                            "Task worker did not start.");
                    }
                    {
                        const std::lock_guard<std::mutex> lock(m_mutex);
                        ++m_liveWorkers;
                    }
                    m_workers.push_back(std::move(worker));
                }
            }
            catch (...) {
                (void)StartStop();
                for (auto& worker : m_workers) {
                    if (worker.joinable()) worker.join();
                }
                throw;
            }
        }

        void OnWorker()
        {
            while (true) {
                QueuedTask task;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_ready.wait(lock, [this] {
                        return m_isStopping || !m_tasks.empty();
                    });
                    if (m_tasks.empty()) {
                        if (!m_isStopping) continue;
                        if (m_liveWorkers > 0) --m_liveWorkers;
                        if (m_liveWorkers == 0) {
                            m_isStopped = true;
                            lock.unlock();
                            m_stopped.notify_all();
                        }
                        return;
                    }
                    task = std::move(m_tasks.front());
                    m_tasks.pop_front();
                }

                try {
                    task.work(task.stopSource.GetToken());
                }
                catch (...) {
                    // packaged_task 已把业务异常写入 future；这里只隔离无效调用等框架异常。
                }

                {
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    m_stopSources.erase(task.id);
                }
            }
        }

        const std::size_t m_taskLimit;
        AppWorkerStart m_workerStart;
        std::mutex m_mutex;
        std::condition_variable m_ready;
        std::condition_variable m_stopped;
        std::deque<QueuedTask> m_tasks;
        std::map<std::uint64_t, TaskStopSource> m_stopSources;
        std::vector<std::thread> m_workers;
        std::mutex m_joinMutex;
        std::uint64_t m_nextId = 1;
        std::size_t m_liveWorkers = 0;
        bool m_isAccepting = true;
        bool m_isStopping = false;
        bool m_isStopped = false;
    };

public:
    explicit AppTaskExecutor(AppWorkerStart workerStart)
        : m_workerStart(GetWorkerStart(std::move(workerStart)))
        , m_loadLane(1, 1, m_workerStart)
        , m_exportLane(2, 2, m_workerStart)
    {
    }

    TaskAdmissionResult SendTask(
        AppTaskWork work,
        const LoadEventKind loadKind)
    {
        return loadKind == LoadEventKind::None
            ? m_exportLane.SendTask(std::move(work))
            : m_loadLane.SendTask(std::move(work));
    }

    bool StartStop()
    {
        const bool isLoadSet = m_loadLane.StartStop();
        const bool isExportSet = m_exportLane.StartStop();
        return isLoadSet && isExportSet;
    }

    bool Stop(
        const std::chrono::steady_clock::time_point deadline)
    {
        (void)StartStop();
        const bool isLoadStopped = m_loadLane.Stop(deadline);
        const bool isExportStopped = m_exportLane.Stop(deadline);
        return isLoadStopped && isExportStopped;
    }

private:
    static AppWorkerStart GetWorkerStart(
        AppWorkerStart workerStart)
    {
        if (workerStart) return workerStart;
        return [](AppWorkerWork work) {
            return std::thread(std::move(work));
        };
    }

    AppWorkerStart m_workerStart;
    TaskLane m_loadLane;
    TaskLane m_exportLane;
};

std::shared_ptr<AppTaskExecutor> CreateAppTaskExecutor(
    AppWorkerStart workerStart)
{
    return std::make_shared<AppTaskExecutor>(
        std::move(workerStart));
}

TaskAdmissionResult SendReadTask(
    const std::shared_ptr<AppTaskExecutor>& executor,
    AppTaskWork work)
{
    return executor
        ? executor->SendTask(
            std::move(work), LoadEventKind::None)
        : TaskAdmissionResult::Unavailable;
}

AppRuntime::AppRuntime(AppServiceArgs args)
    : m_dataManager(std::move(args.dataManager))
    , m_histogram(args.histogram
        ? std::move(args.histogram)
        : std::make_shared<HistogramConverter>())
    , m_strategyCreate(std::move(args.strategyCreate))
    , m_sharedState(std::move(args.interactionState))
    , m_stateEventSource(std::move(args.eventSource))
    , m_observerGate(std::make_shared<ObserverGate>(this))
    , m_viewState(std::make_shared<ViewPresentationState>(m_observerGate))
    , m_setLoadCommit(std::move(args.setLoadCommit))
{
    if (!m_strategyCreate) {
        m_strategyCreate = [](const VizMode mode) {
            return CreateRenderStrategy(mode);
        };
    }
    m_taskExecutor = args.taskExecutor
        ? std::move(args.taskExecutor)
        : CreateAppTaskExecutor(std::move(args.workerStart));
    m_dataLoadTaskService = std::make_shared<AppDataLoadTaskService>(m_dataManager);
    m_dataExportTaskService = std::make_shared<AppDataExportTaskService>(
        m_dataManager, m_sharedState, m_viewState);
}

AppRuntime::~AppRuntime()
{
    if (m_observerGate) {
        (void)m_observerGate->StopOwner();
    }
    (void)SetTaskStopping();
    (void)StopTasks(
        std::chrono::steady_clock::time_point::max());
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
    }

    const auto ownedLoadKind = static_cast<LoadEventKind>(
        m_ownedLoadKind.exchange(static_cast<int>(LoadEventKind::None)));
    if (m_sharedState && ownedLoadKind != LoadEventKind::None) {
        // Reload worker 可能已发布 pending、但 Timer 尚未提交；先销毁 payload，再发布失败终态，
        // 防止共享 DataManager 在下一事务中提交旧批次。
        if (m_dataManager) m_dataManager->ClearLoadStage();
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

bool AppRuntime::SetTaskStopping()
{
    m_isAccepting = false;
    return !m_taskExecutor || m_taskExecutor->StartStop();
}

bool AppRuntime::StopTasks(
    const std::chrono::steady_clock::time_point deadline)
{
    (void)SetTaskStopping();
    return !m_taskExecutor || m_taskExecutor->Stop(deadline);
}

bool AppRuntime::GetDirty() const
{
    return m_isDirty;
}

void AppRuntime::SetDirty()
{
    // 外部显式请求只置门铃，实际 Render 仍由 Timer 决定。
    m_isDirty = true;
}

bool AppRuntime::ResetDirty()
{
    // Timer 在本帧同步完成后领取一次请求；领取之后的新请求留给下一帧。
    return m_isDirty.exchange(false);
}

void AppRuntime::SetPendingFlags(UpdateFlags flags)
{
    int old = m_pendingFlags.load();
    // compare_exchange_weak 在竞争下允许伪失败，但循环体很小，适合这里做位图 OR 合并。
    // 这样多个线程/回调同时上报更新时，只会不断把新位并进同一个原子整数，不会丢标志。
    while (!m_pendingFlags.compare_exchange_weak(
        old, old | static_cast<int>(flags))) {
    }
}

void AppRuntime::SetSyncNeeded()
{
    // 这里只声明“下一帧需要把状态推给 Strategy”，不直接同步，保持所有渲染改动都经由 Timer 主循环收口。
    m_hasSyncNeed = true;
    m_isDirty = true;
}

AppRuntime::CameraState AppRuntime::GetCameraState() const
{
    CameraState state;
    if (!m_renderer || !m_renderer->GetActiveCamera()) {
        return state;
    }

    vtkCamera* camera = m_renderer->GetActiveCamera();
    const double* position = camera->GetPosition();
    const double* focalPoint = camera->GetFocalPoint();
    const double* viewUp = camera->GetViewUp();
    const double* clippingRange = camera->GetClippingRange();
    if (!position || !focalPoint || !viewUp || !clippingRange) {
        return state;
    }

    std::copy_n(position, state.position.size(), state.position.begin());
    std::copy_n(focalPoint, state.focalPoint.size(), state.focalPoint.begin());
    std::copy_n(viewUp, state.viewUp.size(), state.viewUp.begin());
    std::copy_n(
        clippingRange,
        state.clippingRange.size(),
        state.clippingRange.begin());
    state.parallelScale = camera->GetParallelScale();
    state.viewAngle = camera->GetViewAngle();
    state.isParallelProjection = camera->GetParallelProjection() != 0;
    state.isValid = true;
    return state;
}

bool AppRuntime::SetCameraState(const CameraState& state)
{
    if (!state.isValid || !m_renderer
        || !m_renderer->GetActiveCamera()) {
        return false;
    }

    vtkCamera* camera = m_renderer->GetActiveCamera();
    camera->SetPosition(
        state.position[0], state.position[1], state.position[2]);
    camera->SetFocalPoint(
        state.focalPoint[0], state.focalPoint[1], state.focalPoint[2]);
    camera->SetViewUp(
        state.viewUp[0], state.viewUp[1], state.viewUp[2]);
    camera->SetClippingRange(
        state.clippingRange[0], state.clippingRange[1]);
    camera->SetParallelScale(state.parallelScale);
    camera->SetViewAngle(state.viewAngle);
    camera->SetParallelProjection(
        state.isParallelProjection ? 1 : 0);
    return true;
}

bool AppRuntime::SetModeCamera(
    const VizMode mode,
    const VtkImageGridSnapshot& snapshot)
{
    if (!m_renderer || !m_renderer->GetActiveCamera()) {
        return false;
    }

    vtkCamera* camera = m_renderer->GetActiveCamera();
    switch (mode) {
    case VizMode::Volume:
    case VizMode::IsoSurface:
    case VizMode::CompositeVolume:
    case VizMode::CompositeIsoSurface:
        camera->ParallelProjectionOff();
        m_renderer->ResetCamera();
        m_renderer->ResetCameraClippingRange();
        return true;
    case VizMode::SliceTop_down:
    case VizMode::SliceFront_back:
    case VizMode::SliceLeft_right:
        break;
    default:
        return false;
    }

    camera->ParallelProjectionOn();
    if (!snapshot || !snapshot->image) {
        return false;
    }

    double imageCenter[3] = { 0.0, 0.0, 0.0 };
    snapshot->image->GetCenter(imageCenter);
    const double distance = (std::max)(camera->GetDistance(), 1e-6);
    camera->SetFocalPoint(imageCenter);
    if (mode == VizMode::SliceTop_down) {
        camera->SetPosition(
            imageCenter[0], imageCenter[1], imageCenter[2] + distance);
        camera->SetViewUp(0.0, 1.0, 0.0);
    }
    else if (mode == VizMode::SliceFront_back) {
        camera->SetPosition(
            imageCenter[0], imageCenter[1] + distance, imageCenter[2]);
        camera->SetViewUp(0.0, 0.0, 1.0);
    }
    else {
        camera->SetPosition(
            imageCenter[0] + distance, imageCenter[1], imageCenter[2]);
        camera->SetViewUp(0.0, 0.0, 1.0);
    }
    m_renderer->ResetCamera();
    m_renderer->ResetCameraClippingRange();
    return true;
}

bool AppRuntime::SetCameraCenter(
    const std::array<double, 16>& modelToWorld,
    const VtkImageGridSnapshot& snapshot)
{
    if (!m_renderer || !m_renderer->GetActiveCamera()
        || !snapshot || !snapshot->image) {
        return false;
    }

    double modelCenter[3] = { 0.0, 0.0, 0.0 };
    snapshot->image->GetCenter(modelCenter);
    const double sourceCenter[4] = {
        modelCenter[0], modelCenter[1], modelCenter[2], 1.0
    };
    double targetCenter[4] = { 0.0, 0.0, 0.0, 1.0 };
    auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    modelToWorldMatrix->DeepCopy(modelToWorld.data());
    modelToWorldMatrix->MultiplyPoint(sourceCenter, targetCenter);

    const double invW = std::abs(targetCenter[3]) > 1e-12
        ? 1.0 / targetCenter[3]
        : 1.0;
    const double worldCenter[3] = {
        targetCenter[0] * invW,
        targetCenter[1] * invW,
        targetCenter[2] * invW
    };

    vtkCamera* camera = m_renderer->GetActiveCamera();
    const double* oldFocalPoint = camera->GetFocalPoint();
    const double* oldPosition = camera->GetPosition();
    if (!oldFocalPoint || !oldPosition) {
        return false;
    }
    const double offset[3] = {
        oldPosition[0] - oldFocalPoint[0],
        oldPosition[1] - oldFocalPoint[1],
        oldPosition[2] - oldFocalPoint[2]
    };
    camera->SetFocalPoint(worldCenter);
    camera->SetPosition(
        worldCenter[0] + offset[0],
        worldCenter[1] + offset[1],
        worldCenter[2] + offset[2]);
    m_renderer->ResetCameraClippingRange();
    return true;
}

void AppRuntime::SetCurrentStrategy(
    std::shared_ptr<AbstractVisualStrategy> newStrategy,
    const VizMode mode,
    const bool isRendererAttached)
{
    if (m_currentStrategy == newStrategy) {
        if (!m_currentStrategy) {
            m_currentMode.reset();
            return;
        }
        bool isCameraSet = true;
        if (m_renderer
            && m_renderer->GetActiveCamera()) {
            const bool isSlice = mode == VizMode::SliceTop_down
                || mode == VizMode::SliceFront_back
                || mode == VizMode::SliceLeft_right;
            if (isSlice) {
                isCameraSet = SetModeCamera(mode, m_renderSnapshot);
            }
            else {
                m_renderer->GetActiveCamera()->ParallelProjectionOff();
            }
            m_isDirty = true;
        }
        if (isCameraSet) {
            m_currentMode = mode;
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
        if (m_renderer
            && !SetModeCamera(mode, m_renderSnapshot)) {
            throw std::runtime_error(
                "Committed mode camera initialization failed.");
        }
        m_currentMode = mode;
    }
    else {
        m_currentMode.reset();
    }
    m_isDirty = true;
}

bool AppRuntime::AttachOverlay(
    std::shared_ptr<FeatureOverlay> overlay)
{
    if (!overlay) return false;

    const auto sameOverlay = std::find_if(
        m_overlays.begin(), m_overlays.end(),
        [overlay](const std::shared_ptr<FeatureOverlay>& current) {
            return current.get() == overlay.get();
        });
    if (sameOverlay != m_overlays.end()) {
        return false;
    }

    bool isStored = false;
    try {
        m_overlays.push_back(overlay);
        isStored = true;
        if (m_renderer) {
            overlay->AttachRenderer(m_renderer);
        }
    }
    catch (...) {
        if (m_renderer) {
            try { overlay->DetachRenderer(m_renderer); }
            catch (...) {}
        }
        if (isStored) m_overlays.pop_back();
        return false;
    }

    m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
    m_hasSyncNeed = true;
    m_isDirty = true;
    return true;
}

void AppRuntime::RemoveOverlay(
    std::shared_ptr<FeatureOverlay> overlay) noexcept
{
    if (!overlay) return;

    const auto it = std::find_if(
        m_overlays.begin(), m_overlays.end(),
        [overlay](const std::shared_ptr<FeatureOverlay>& current) {
            return current.get() == overlay.get();
        });
    if (it == m_overlays.end()) {
        return;
    }

    if (m_renderer) {
        // renderer 是外部策略边界；即使自定义策略清理抛出，service 也必须移除自身登记，
        // 让上层 Start/Clear 事务不会暴露异常或永久保留强 owner。
        try {
            overlay->DetachRenderer(m_renderer);
        }
        catch (...) {
        }
    }
    m_overlays.erase(it);
    m_isDirty = true;
}

void AppRuntime::ClearOverlays() noexcept
{
    if (m_renderer) {
        for (auto& overlay : m_overlays) {
            try { overlay->DetachRenderer(m_renderer); }
            catch (...) {}
        }
    }
    m_overlays.clear();
    m_isDirty = true;
}

RenderInputStamp AppRuntime::GetRenderInputStamp() const
{
    RenderInputStamp stamp;
    if (m_renderSnapshot && m_renderSnapshot->data) {
        stamp.dataRevision = m_renderSnapshot->data->self;
    }
    return stamp;
}

bool AppRuntime::AttachRenderEffect(
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

bool AppRuntime::DetachRenderEffect(const RenderEffect* effect)
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

void AppRuntime::ClearStrategies()
{
    // 当前 Strategy 从 renderer/effect 摘除后直接释放；每次重建使用全新候选，
    // 不再缓存带可变 VTK 状态的对象。
    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->DetachRenderer(m_renderer);
    }
    if (m_currentStrategy) {
        if (auto effect = m_renderEffect.lock()) {
            (void)m_currentStrategy->DetachRenderEffect(effect.get());
        }
    }
    m_currentStrategy.reset();
    m_currentMode.reset();

    ClearOverlays();
}

bool AppRuntime::SetRenderContext(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer> ren)
{
    // 首次绑定确定 VTK 归属线程；后续只能由同一线程更新绑定，
    // 防止外部线程通过重新注入 RenderContext 绕过提交门。
    {
        const std::lock_guard<std::mutex> lock(m_ownerMutex);
        const auto currentThread = std::this_thread::get_id();
        if (m_hasOwnerThread && m_ownerThread != currentThread) {
            return false;
        }
    }
    if (!SetRenderBinding(std::move(win), std::move(ren))) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(m_ownerMutex);
        m_ownerThread = std::this_thread::get_id();
        m_hasOwnerThread = true;
    }
    SetRendererBg();
    if (!m_stateEventSource) return true;

    SetStateObserver();
    return true;
}

bool AppRuntime::SetRenderBinding(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer> ren)
{
    auto oldRenderer = m_renderer;
    auto oldWindow = m_renderWindow;
    const bool isBindingChanged =
        oldRenderer.GetPointer() != ren.GetPointer()
        || oldWindow.GetPointer() != win.GetPointer();
    if (!isBindingChanged) {
        return true;
    }
    // SetRenderContext 不是解绑 API；无效目标不得先卸载已提交的旧 binding。
    if (!win || !ren) {
        return false;
    }

    const CameraState oldCamera = GetCameraState();
    try {
        if (oldRenderer) {
            if (m_currentStrategy) {
                m_currentStrategy->DetachRenderer(oldRenderer);
            }
            for (auto& overlay : m_overlays) {
                if (overlay) overlay->DetachRenderer(oldRenderer);
            }
        }

        m_renderWindow = std::move(win);
        m_renderer = std::move(ren);
        if (m_currentStrategy) {
            m_currentStrategy->AttachRenderer(m_renderer);
        }
        for (auto& overlay : m_overlays) {
            if (overlay) overlay->AttachRenderer(m_renderer);
        }

        if (oldCamera.isValid) {
            if (!SetCameraState(oldCamera)) {
                throw std::runtime_error(
                    "Renderer rebind camera restore failed.");
            }
            if (m_currentMode && m_renderer->GetActiveCamera()) {
                const bool isSlice = *m_currentMode == VizMode::SliceTop_down
                    || *m_currentMode == VizMode::SliceFront_back
                    || *m_currentMode == VizMode::SliceLeft_right;
                m_renderer->GetActiveCamera()->SetParallelProjection(
                    isSlice ? 1 : 0);
                m_renderer->ResetCameraClippingRange();
            }
        }
        else if (m_currentMode) {
            if (!SetModeCamera(*m_currentMode, m_renderSnapshot)) {
                throw std::runtime_error(
                    "Renderer rebind mode camera initialization failed.");
            }
        }
        m_isDirty = true;
        return true;
    }
    catch (...) {
        // 重绑是单视图事务；任一步失败都尽力卸载新目标并恢复旧 binding/camera。
    }

    try {
        if (m_renderer && m_renderer != oldRenderer) {
            if (m_currentStrategy) {
                m_currentStrategy->DetachRenderer(m_renderer);
            }
            for (auto& overlay : m_overlays) {
                if (overlay) overlay->DetachRenderer(m_renderer);
            }
        }
    }
    catch (...) {
    }

    m_renderWindow = std::move(oldWindow);
    m_renderer = std::move(oldRenderer);
    try {
        if (m_renderer) {
            if (m_currentStrategy) {
                m_currentStrategy->AttachRenderer(m_renderer);
            }
            for (auto& overlay : m_overlays) {
                if (overlay) overlay->AttachRenderer(m_renderer);
            }
            (void)SetCameraState(oldCamera);
        }
    }
    catch (...) {
    }
    return false;
}

void AppRuntime::SetRendererBg()
{
    if (!m_renderer || !m_viewState) {
        return;
    }

    const auto bg = m_viewState->GetBackground();
    m_renderer->SetBackground(bg.r, bg.g, bg.b);
}

void AppRuntime::SetStateObserver()
{
    const std::weak_ptr<ObserverGate> weakGate = m_observerGate;
    m_stateEventSource->SetObserver(m_observerGate,
        [weakGate](UpdateFlags flags)
        {
            const auto gate = weakGate.lock();
            if (gate) {
                gate->SendFlags(flags);
            }
        }
    );
}

void AppRuntime::ObserverGate::SendFlags(const UpdateFlags flags)
{
    AppRuntime* owner = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_owner) {
            return;
        }
        owner = m_owner;
        ++m_callCount;
    }

    try {
        const UpdateFlags loadFlags =
            UpdateFlags::DataReady | UpdateFlags::LoadFailed;
        if ((flags & loadFlags) != UpdateFlags::None) {
            LoadEventKind loadEventKind = LoadEventKind::None;
            if ((flags & UpdateFlags::FileLoad) != UpdateFlags::None) {
                loadEventKind = LoadEventKind::File;
            }
            else if ((flags & UpdateFlags::ReloadLoad) != UpdateFlags::None) {
                loadEventKind = LoadEventKind::Reload;
            }
            if (loadEventKind != LoadEventKind::None) {
                owner->CreateLoadNotice(
                    loadEventKind,
                    (flags & UpdateFlags::DataReady) != UpdateFlags::None);
            }
        }

        // 广播线程只登记主线程邮箱；VTK 与 Strategy 仍由 owner tick 消费。
        owner->SendStateFlags(flags);
    }
    catch (...) {
        (void)StopCall();
        throw;
    }
    (void)StopCall();
}

bool AppRuntime::ObserverGate::StopCall()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_callCount == 0) {
        return false;
    }
    --m_callCount;
    if (m_callCount == 0) {
        m_stopped.notify_all();
    }
    return true;
}

bool AppRuntime::ObserverGate::StopOwner()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_owner = nullptr;
    m_stopped.wait(lock, [this]() { return m_callCount == 0; });
    return true;
}

void AppRuntime::SendStateFlags(UpdateFlags flags)
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
void AppRuntime::SetVizMode(VizMode mode)
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

VizMode AppRuntime::GetVizMode() const
{
    return static_cast<VizMode>(m_pendingVizModeInt.load());
}

void AppRuntime::SetMaterial(const MaterialParams& mat)
{
    m_viewState->SetMaterial(mat);
}

MaterialParams AppRuntime::GetMaterial() const
{
    return m_viewState->GetMaterial();
}

void AppRuntime::SetOpacity(double opacity)
{
    auto mat = m_viewState->GetMaterial();
    mat.opacity = opacity;
    m_viewState->SetMaterial(mat);
}

double AppRuntime::GetOpacity() const
{
    return m_viewState->GetMaterial().opacity;
}

bool AppRuntime::GetVolumeTransferValid(
    const VolumeTransferFunction& function) const
{
    const auto isUnit = [](const double value) {
        return std::isfinite(value)
            && value >= 0.0 && value <= 1.0;
    };
    if (function.colorNodes.size() < 2
        || function.opacityNodes.size() < 2) {
        return false;
    }

    double previousScalar = function.colorNodes.front().scalar;
    for (std::size_t index = 0;
        index < function.colorNodes.size(); ++index) {
        const auto& node = function.colorNodes[index];
        if (!std::isfinite(node.scalar)
            || !isUnit(node.r) || !isUnit(node.g) || !isUnit(node.b)
            || (index > 0 && node.scalar <= previousScalar)) {
            return false;
        }
        previousScalar = node.scalar;
    }

    previousScalar = function.opacityNodes.front().scalar;
    for (std::size_t index = 0;
        index < function.opacityNodes.size(); ++index) {
        const auto& node = function.opacityNodes[index];
        if (!std::isfinite(node.scalar)
            || !isUnit(node.opacity)
            || (index > 0 && node.scalar <= previousScalar)) {
            return false;
        }
        previousScalar = node.scalar;
    }
    return true;
}

bool AppRuntime::GetTransferRangeValid(
    const VolumeTransferFunction& function,
    const std::array<double, 2>& scalarRange) const
{
    if (!GetVolumeTransferValid(function)
        || !std::isfinite(scalarRange[0])
        || !std::isfinite(scalarRange[1])
        || scalarRange[1] < scalarRange[0]) {
        return false;
    }
    const auto hasOverlap = [&scalarRange](
        const double lower,
        const double upper) {
        return upper >= scalarRange[0]
            && lower <= scalarRange[1];
    };
    return hasOverlap(
            function.colorNodes.front().scalar,
            function.colorNodes.back().scalar)
        && hasOverlap(
            function.opacityNodes.front().scalar,
            function.opacityNodes.back().scalar);
}

bool AppRuntime::SetVolumeTransferFunction(
    const VolumeTransferFunction& function)
{
    const bool hasData = m_renderSnapshot
        && m_renderSnapshot->image;
    const bool hasStage = m_dataStage.has_value();
    return m_viewState
        && GetVolumeTransferValid(function)
        && (!hasData
            || (m_sharedState
                && GetTransferRangeValid(
                    function,
                    m_sharedState->GetScalarRange())))
        && (!hasStage
            || GetTransferRangeValid(
                function,
                m_dataStage->readyState.scalarRange))
        && m_viewState->SetVolumeTransferFunction(function);
}

bool AppRuntime::SetAutoTransfer(
    const VolumeTransferFunction& function)
{
    const bool isEmpty = function.colorNodes.empty()
        && function.opacityNodes.empty();
    return m_viewState
        && (isEmpty || GetVolumeTransferValid(function))
        && m_viewState->SetAutoTransfer(function);
}

bool AppRuntime::ResetTransfer(
    const VolumeTransferFunction& function)
{
    const bool isEmpty = function.colorNodes.empty()
        && function.opacityNodes.empty();
    return m_viewState
        && (isEmpty || GetVolumeTransferValid(function))
        && m_viewState->ResetTransfer(function);
}

VolumeTransferFunction AppRuntime::GetVolumeTransferFunction() const
{
    return m_viewState
        ? m_viewState->GetVolumeTransferFunction()
        : VolumeTransferFunction{};
}

bool AppRuntime::GetTransferAuto() const
{
    return !m_viewState || m_viewState->GetTransferAuto();
}

void AppRuntime::SetIsoThreshold(double val)
{
    m_viewState->SetIsoValue(val);
}

double AppRuntime::GetIsoThreshold() const
{
    return m_viewState->GetIsoValue();
}

void AppRuntime::SetBackground(const BackgroundColor& bg)
{
    m_viewState->SetBackground(bg);
}

BackgroundColor AppRuntime::GetBackground() const
{
    return m_viewState->GetBackground();
}

bool AppRuntime::SetSpacing(double sx, double sy, double sz)
{
    if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz)
        || sx <= 0.0 || sy <= 0.0 || sz <= 0.0) {
        return false;
    }
    if (!m_sharedState) return false;
    const std::weak_ptr<AbstractDataManager> weakData =
        m_dataManager;
    return m_sharedState->SetSpacingData(
        { sx, sy, sz },
        [weakData](const std::array<double, 3>& spacing) {
            const auto data = weakData.lock();
            DataReadyState committed;
            committed.spacing = spacing;
            if (!data) {
                return std::optional<DataReadyState>{ std::move(committed) };
            }
            if (!data->SetSpacing(spacing)) {
                return std::optional<DataReadyState>{};
            }
            const auto primary = data->GetPrimaryImage();
            if (primary && primary->data && primary->binding) {
                committed.dataRevision = primary->data->self;
                committed.bindingRevision = primary->binding->revision;
                committed.scalarRange = data->GetScalarRange();
                committed.spacing = data->GetSpacing();
            }
            return std::optional<DataReadyState>{ std::move(committed) };
        });
}

std::array<double, 3> AppRuntime::GetSpacing() const
{
    return m_sharedState->GetSpacing();
}

void AppRuntime::SetWindowLevel(double ww, double wc)
{
    m_viewState->SetWindowLevel(ww, wc);
}

bool AppRuntime::ResetWindowLevel()
{
    if (!m_sharedState || !m_viewState) return false;
    const auto windowLevel = GetAutoWindowLevel(
        m_sharedState->GetScalarRange());
    return windowLevel
        && m_viewState->ResetWindowLevel(*windowLevel);
}

bool AppRuntime::ResetWindowLevel(
    const WindowLevelParams& windowLevel)
{
    return m_viewState
        && std::isfinite(windowLevel.windowWidth)
        && windowLevel.windowWidth > 0.0
        && std::isfinite(windowLevel.windowCenter)
        && m_viewState->ResetWindowLevel(windowLevel);
}

WindowLevelMode AppRuntime::GetWindowLevelMode() const
{
    return m_viewState
        ? m_viewState->GetWindowLevelMode()
        : WindowLevelMode::Auto;
}

void AppRuntime::SetVisualConfig(const PreInitConfig& cfg)
{
    // 先更新供 BuildPipeline/交互读取的模式快照，再提交当前 View 独占的展示配置。
    m_pendingVizModeInt.store(static_cast<int>(cfg.vizMode));
    m_viewState->SetPreInitConfig(cfg);
}

PreInitConfig AppRuntime::GetVisualConfig() const
{
    // 读回当前完整值；自动窗宽窗位以 hasWindowLevel=false 保留其意图。
    PreInitConfig config;
    config.vizMode = GetVizMode();
    config.material = m_viewState->GetMaterial();
    config.volumeTransferFunction =
        m_viewState->GetVolumeTransferFunction();
    config.isoThreshold = m_viewState->GetIsoValue();
    config.bgColor = m_viewState->GetBackground();
    config.spacing = m_sharedState->GetSpacing();
    config.windowLevel = m_viewState->GetWindowLevel();
    config.hasVolumeTransferFunction =
        !m_viewState->GetTransferAuto()
        && GetVolumeTransferValid(config.volumeTransferFunction);
    config.hasIso = true;
    config.hasBgColor = true;
    config.hasSpacing = true;
    config.hasWindowLevel =
        m_viewState->GetWindowLevelMode()
        == WindowLevelMode::Manual;
    return config;
}

std::array<double, 2> AppRuntime::GetScalarRange() const
{
    return m_sharedState->GetScalarRange();
}

bool AppRuntime::SetVolumeQuality(const VolumeQuality quality)
{
    switch (quality) {
    case VolumeQuality::Auto:
    case VolumeQuality::Low:
    case VolumeQuality::High:
    case VolumeQuality::XHigh:
    case VolumeQuality::Ultra:
        break;
    default:
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_viewConfigMutex);
        if (m_requestedQuality == quality) return true;
        m_requestedQuality = quality;
    }
    SetPendingFlags(UpdateFlags::Quality);
    SetSyncNeeded();
    return true;
}

VolumeQuality AppRuntime::GetVolumeQuality() const
{
    std::lock_guard<std::mutex> lock(m_viewConfigMutex);
    return m_appliedQuality;
}

VolumeQuality AppRuntime::GetTargetQuality() const
{
    std::lock_guard<std::mutex> lock(m_viewConfigMutex);
    return m_requestedQuality;
}

bool AppRuntime::SetFeatureActive(
    const FeatureSource& source,
    bool isActive)
{
    if (source.id.empty()) return false;
    std::lock_guard<std::mutex> lock(m_viewConfigMutex);
    const auto featureIt = std::find_if(
        m_activeFeatures.begin(),
        m_activeFeatures.end(),
        [&source](const ActiveFeature& feature) {
            return feature.source == source;
        });
    if (isActive && featureIt == m_activeFeatures.end()) {
        m_activeFeatures.push_back({ source });
    }
    else if (!isActive && featureIt != m_activeFeatures.end()) {
        m_activeFeatures.erase(featureIt);
    }
    return true;
}

bool AppRuntime::GetIsFeatureActive() const
{
    std::lock_guard<std::mutex> lock(m_viewConfigMutex);
    return !m_activeFeatures.empty();
}

bool AppRuntime::SetGradientOpacity(
    const std::vector<GradientOpacityNode>& nodes)
{
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        if (!std::isfinite(node.gradient) || node.gradient < 0.0
            || !std::isfinite(node.opacity)
            || node.opacity < 0.0 || node.opacity > 1.0
            || (index > 0
                && node.gradient < nodes[index - 1].gradient)) {
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_viewConfigMutex);
        const bool isSame = m_gradientOpacity.size() == nodes.size()
            && std::equal(
                m_gradientOpacity.begin(),
                m_gradientOpacity.end(),
                nodes.begin(),
                [](const GradientOpacityNode& left,
                    const GradientOpacityNode& right) {
                    return left.gradient == right.gradient
                        && left.opacity == right.opacity;
                });
        if (isSame) return true;
        m_gradientOpacity = nodes;
    }
    SetPendingFlags(UpdateFlags::GradientOpacity);
    SetSyncNeeded();
    return true;
}

std::vector<GradientOpacityNode>
AppRuntime::GetGradientOpacity() const
{
    std::lock_guard<std::mutex> lock(m_viewConfigMutex);
    return m_gradientOpacity;
}

bool AppRuntime::SetDenoiseOn(bool isDenoiseOn)
{
    {
        std::lock_guard<std::mutex> lock(m_viewConfigMutex);
        if (m_isDenoiseOn == isDenoiseOn) return true;
        m_isDenoiseOn = isDenoiseOn;
    }
    SetPendingFlags(UpdateFlags::Denoise);
    SetSyncNeeded();
    return true;
}

bool AppRuntime::GetDenoiseOn() const
{
    std::lock_guard<std::mutex> lock(m_viewConfigMutex);
    return m_isDenoiseOn;
}

// ─────────────────────────────────────────────────────────────────────
// 数据加载 / 数据导出
// ─────────────────────────────────────────────────────────────────────
LoadState AppRuntime::GetFileLoadState() const
{
    return m_sharedState ? m_sharedState->GetFileLoadState() : LoadState::Idle;
}

LoadState AppRuntime::GetReloadLoadState() const
{
    return m_sharedState ? m_sharedState->GetReloadLoadState() : LoadState::Idle;
}

TaskAdmissionResult AppRuntime::StartTask(
    AppRuntime::TaskWork task,
    LoadEventKind loadKind,
    std::function<void(bool)> callback)
{
    // 未接纳请求不取得 callback；调用方可用同步结果区分错误，完成队列只承载已接纳任务。
    if (!m_isAccepting) return TaskAdmissionResult::Stopping;
    if (!task.valid()) return TaskAdmissionResult::InvalidRequest;
    if (!m_taskExecutor) return TaskAdmissionResult::Unavailable;

    // Load/Reload 严格单 admission；Export 最多双 admission。ready 结果在 owner tick
    // 消费前仍占槽，防止停更 View 累积完成 callback。
    std::unique_lock<std::mutex> lock(m_activeTaskMutex);
    const std::size_t activeCount = static_cast<std::size_t>(
        std::count_if(
            m_activeTasks.begin(), m_activeTasks.end(),
            [loadKind](const ActiveTask& active) {
                return loadKind == LoadEventKind::None
                    ? active.loadKind == LoadEventKind::None
                    : active.loadKind != LoadEventKind::None;
            }));
    const std::size_t taskLimit =
        loadKind == LoadEventKind::None ? 2U : 1U;
    if (activeCount >= taskLimit) {
        return TaskAdmissionResult::QueueFull;
    }

    // 先建立 future/callback 槽，再交给固定 worker lane；提交失败必须撤销完整槽。
    m_activeTasks.emplace_back();
    auto entry = std::prev(m_activeTasks.end());
    try {
        entry->loadKind = loadKind;
        entry->result = task.get_future();
        entry->callback = std::move(callback);
        const auto admission = m_taskExecutor->SendTask(
            std::move(task), loadKind);
        if (admission != TaskAdmissionResult::Accepted) {
            m_activeTasks.erase(entry);
            return admission;
        }
        return TaskAdmissionResult::Accepted;
    }
    catch (...) {
        m_activeTasks.erase(entry);
        return TaskAdmissionResult::Unavailable;
    }
}

TaskAdmissionResult AppRuntime::LoadFileAsync(
    std::string path,
    VolumeLayout layout,
    std::function<void(bool isSuccess)> onComplete)
{
    // File 链：构造任务 -> 领取全局 Load admission -> 清 pending -> 登记 owner -> 启动 worker。
    // 任一后置步骤失败都会发布 FileFailed、释放 admission/owner，并把 callback 排入完成队列。
    if (!m_isAccepting) return TaskAdmissionResult::Stopping;
    if (!m_dataLoadTaskService) return TaskAdmissionResult::Unavailable;
    auto task = m_dataLoadTaskService->BuildLoadFileTask(
        std::move(path), std::move(layout));
    if (!task) return TaskAdmissionResult::InvalidRequest;
    if (!m_sharedState) return TaskAdmissionResult::Unavailable;
    if (!m_sharedState->StartLoad(LoadEventKind::File)) {
        return TaskAdmissionResult::Busy;
    }
    if (!m_dataManager || !m_dataManager->ClearLoadStage()
        || !SetOwnedLoad(LoadEventKind::File)) {
        if (m_dataManager) m_dataManager->ClearLoadStage();
        m_sharedState->SetFileLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::File);
        ResetOwnedLoad(LoadEventKind::File);
        return TaskAdmissionResult::Unavailable;
    }
    const auto admission = StartTask(
        std::move(*task),
        LoadEventKind::File,
        std::move(onComplete));
    if (admission != TaskAdmissionResult::Accepted) {
        m_dataManager->ClearLoadStage();
        m_sharedState->SetFileLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::File);
        ResetOwnedLoad(LoadEventKind::File);
        return admission;
    }
    return TaskAdmissionResult::Accepted;
}

TaskAdmissionResult AppRuntime::ReloadFromBufferAsync(
    VolumeBuffer buffer,
    std::function<void(bool isSuccess)> onComplete)
{
    // Reload 使用与 File 相同的 admission/owner 协议，但失败状态允许 SharedState 保留旧可信数据。
    if (!m_isAccepting) return TaskAdmissionResult::Stopping;
    if (!m_dataLoadTaskService) return TaskAdmissionResult::Unavailable;
    auto task = m_dataLoadTaskService->BuildReloadTask(std::move(buffer));
    if (!task) return TaskAdmissionResult::InvalidRequest;
    if (!m_sharedState) return TaskAdmissionResult::Unavailable;
    if (!m_sharedState->StartLoad(LoadEventKind::Reload)) {
        return TaskAdmissionResult::Busy;
    }
    if (!m_dataManager || !m_dataManager->ClearLoadStage()
        || !SetOwnedLoad(LoadEventKind::Reload)) {
        if (m_dataManager) m_dataManager->ClearLoadStage();
        m_sharedState->SetReloadLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::Reload);
        ResetOwnedLoad(LoadEventKind::Reload);
        return TaskAdmissionResult::Unavailable;
    }
    const auto admission = StartTask(
        std::move(*task),
        LoadEventKind::Reload,
        std::move(onComplete));
    if (admission != TaskAdmissionResult::Accepted) {
        m_dataManager->ClearLoadStage();
        m_sharedState->SetReloadLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::Reload);
        ResetOwnedLoad(LoadEventKind::Reload);
        return admission;
    }
    return TaskAdmissionResult::Accepted;
}

TaskAdmissionResult AppRuntime::ExportDataAsync(
    std::string outputDir,
    std::string extension,
    std::function<void(bool isSuccess)> onComplete)
{
    auto task = m_dataExportTaskService
        ? m_dataExportTaskService->BuildDataTask(
            std::move(outputDir),
            std::move(extension)) : std::nullopt;
    if (!task) {
        return m_isAccepting
            ? TaskAdmissionResult::InvalidRequest
            : TaskAdmissionResult::Stopping;
    }
    // StartTask 从这里开始独占 callback，并负责启动失败通知。
    return StartTask(
        std::move(*task), LoadEventKind::None, std::move(onComplete));
}

TaskAdmissionResult AppRuntime::ExportSlicesAsync(
    const std::string& path,
    std::optional<double> rotationAngleDeg,
    std::function<void(bool isSuccess)> onComplete)
{
    const VizMode currentMode = static_cast<VizMode>(m_pendingVizModeInt.load());
    auto task = m_dataExportTaskService
        ? m_dataExportTaskService->BuildSlicesTask(
            path, rotationAngleDeg, currentMode) : std::nullopt;
    if (!task) {
        return m_isAccepting
            ? TaskAdmissionResult::InvalidRequest
            : TaskAdmissionResult::Stopping;
    }
    // StartTask 从这里开始独占 callback，并负责启动失败通知。
    return StartTask(
        std::move(*task), LoadEventKind::None, std::move(onComplete));
}

// ─────────────────────────────────────────────────────────────────────
// 交互能力内部实现
// ─────────────────────────────────────────────────────────────────────
void AppRuntime::SetSliceScroll(int delta)
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

void AppRuntime::SetCursorWorldPosition(double worldPos[3], int axis)
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

bool AppRuntime::SetCursorState(
    const std::array<double, 3>& worldPosition,
    const AppCursorAxis axis)
{
    const int axisValue = static_cast<int>(axis);
    if (!m_sharedState || axisValue < -1 || axisValue > 2
        || !std::all_of(
            worldPosition.begin(),
            worldPosition.end(),
            [](const double value) { return std::isfinite(value); })) {
        return false;
    }

    m_sharedState->SetCursorRawWorld(
        worldPosition[0], worldPosition[1], worldPosition[2]);
    m_sharedState->SetCursorAxis(axisValue);
    m_sharedState->SetCursorWorld(
        worldPosition[0], worldPosition[1], worldPosition[2]);
    SetSyncNeeded();
    return true;
}

std::array<double, 3> AppRuntime::GetCursorWorld()
{
    return m_sharedState->GetCursorWorld();
}

AppCursorAxis AppRuntime::GetCursorAxis() const
{
    if (!m_sharedState) return AppCursorAxis::Free;
    const int axis = m_sharedState->GetCursorAxis();
    return axis >= -1 && axis <= 2
        ? static_cast<AppCursorAxis>(axis)
        : AppCursorAxis::Free;
}

bool AppRuntime::SetInteracting(
    const InteractionSource& source,
    bool isInteracting)
{
    return m_sharedState->SetInteracting(source, isInteracting);
}

bool AppRuntime::GetIsInteracting() const
{
    return m_sharedState->GetIsInteracting();
}

int AppRuntime::GetPlaneAxis(vtkActor* actor)
{
    return m_currentStrategy ? m_currentStrategy->GetPlaneAxis(actor) : -1;
}

vtkProp3D* AppRuntime::GetMainProp()
{
    return m_currentStrategy ? m_currentStrategy->GetMainProp() : nullptr;
}

void AppRuntime::SetModelMatrix(vtkMatrix4x4* modelToWorldMatrix)
{
    if (!modelToWorldMatrix) return;

    std::array<double, 16> matData = { 0 }; // 当前模型矩阵快照，回写 SharedState 使用
    std::memcpy(matData.data(), modelToWorldMatrix->GetData(), 16 * sizeof(double));
    if (m_sharedState) {
        m_sharedState->SetModelMatrix(matData);
    }
}

std::array<double, 16> AppRuntime::GetModelMatrix()
{
    return m_sharedState ? m_sharedState->GetModelMatrix()
        : std::array<double, 16>{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
}

WindowLevelParams AppRuntime::GetWindowLevel() const
{
    if (m_viewState)
        return m_viewState->GetWindowLevel();

    WindowLevelParams p = {};
    return p;
}

int AppRuntime::GetNavigationAxis() const
{
    return m_currentStrategy ? m_currentStrategy->GetNavigationAxis() : -1;
}

void AppRuntime::SetElementVisible(uint32_t flagBit, bool isVisible)
{
    m_viewState->SetElementVisible(flagBit, isVisible);
}

uint32_t AppRuntime::GetVisibilityMask() const
{
    return m_viewState->GetVisibilityMask();
}

void AppRuntime::SetWindowLevelDrag(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC)
{
    if (!m_viewState) return;

    const WindowLevelParams windowLevel = InteractionComputeService::GetWindowLevel(
        totalDx,
        totalDy,
        viewWidth,
        viewHeight,
        startWW,
        startWC); // 当前拖拽结束后应写回状态的窗宽窗位

    m_viewState->SetWindowLevel(
        windowLevel.windowWidth, windowLevel.windowCenter);
    SetSyncNeeded();
}

void AppRuntime::SetModelTransform(
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

void AppRuntime::SetModelTransformReset()
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

void AppRuntime::GetModelPositionFromWorld(const double w[3], double m[3]) const
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

void AppRuntime::GetWorldPositionFromModel(const double m[3], double w[3]) const
{
    auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // SharedState 当前模型矩阵快照，保证 world/model 换算只依赖单一真源
    modelToWorldMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        modelToWorldMatrix->DeepCopy(matrixData.data());
    }

    InteractionComputeService::GetWorldPositionFromModel(modelToWorldMatrix, m, w);
}

bool AppRuntime::GetDataReadyState(
    const VtkImageGridSnapshot& snapshot,
    DataReadyState& state) const
{
    if (!snapshot || !snapshot->image || !snapshot->data
        || !snapshot->binding || snapshot->binding->revision == 0) {
        return false;
    }

    double imageRange[2] = {};
    double imageSpacing[3] = {};
    double imageCenter[3] = {};
    snapshot->image->GetScalarRange(imageRange);
    snapshot->image->GetSpacing(imageSpacing);
    snapshot->image->GetCenter(imageCenter);

    double centerWorld[3] = {};
    GetWorldPositionFromModel(imageCenter, centerWorld);
    const auto isFinite = [](const double value) {
        return std::isfinite(value);
    };
    if (!std::all_of(std::begin(imageRange), std::end(imageRange), isFinite)
        || !std::all_of(std::begin(imageSpacing), std::end(imageSpacing), isFinite)
        || !std::all_of(std::begin(centerWorld), std::end(centerWorld), isFinite)) {
        return false;
    }

    state.dataRevision = snapshot->data->self;
    state.bindingRevision = snapshot->binding->revision;
    std::copy_n(imageRange, state.scalarRange.size(), state.scalarRange.begin());
    std::copy_n(imageSpacing, state.spacing.size(), state.spacing.begin());
    std::copy_n(centerWorld, state.cursorWorld.size(), state.cursorWorld.begin());
    return true;
}

std::optional<WindowLevelParams> AppRuntime::GetAutoWindowLevel(
    const std::array<double, 2>& scalarRange) const
{
    const double rangeMin = scalarRange[0];
    const double rangeMax = scalarRange[1];
    const double rangeWidth = rangeMax - rangeMin;
    if (!std::isfinite(rangeMin)
        || !std::isfinite(rangeMax)
        || !std::isfinite(rangeWidth)
        || rangeWidth < 0.0) {
        return std::nullopt;
    }

    // 1. 窗位取 (min + max) / 2，拆成半值相加以避免大数求和溢出。
    // 2. 窗宽取 max(max - min, 安全下限)；常量体数据也必须保留正窗宽。
    // 3. 安全下限同时覆盖交互约束与当前数量级的浮点分辨率。
    const double rangeScale = std::max({
        1.0, std::abs(rangeMin), std::abs(rangeMax) });
    const double safeWidth = std::max(
        0.01,
        rangeScale * std::numeric_limits<double>::epsilon() * 16.0);
    const double windowCenter =
        rangeMin * 0.5 + rangeMax * 0.5;
    if (!std::isfinite(safeWidth)
        || !std::isfinite(windowCenter)) {
        return std::nullopt;
    }
    return WindowLevelParams{
        std::max(rangeWidth, safeWidth), windowCenter };
}

// ─────────────────────────────────────────────────────────────────────
// 主线程更新编排入口
// ─────────────────────────────────────────────────────────────────────
bool AppRuntime::SendUpdates()
{
    if (!GetIsOwnerThread()) {
        // 未绑定或非归属线程只可经状态入口累积 pending/dirty，不能直接触碰 VTK 或 strategy。
        return false;
    }
    // 更新入口按固定阶段收敛 pending/current、完成回调和渲染变更；常规由主线程 Timer 驱动，
    // 外部 reload handler 只允许发布 pending，最终提交仍由 owner Timer 消费。
    // 1. 先领取所有 ready 任务并 join worker，load 的 pending 只由 owner 提交。
    SendTasks();

    // 2. load 终态按完整 payload 顺序消费；队列锁只覆盖弹出，VTK 与 callback 始终在锁外。
    LoadNotice loadNotice;
    while (RemoveLoadNotice(loadNotice)) {
        if (!loadNotice.isStateSet) {
            if (loadNotice.isSucceeded) {
                const auto current = m_dataManager
                    ? m_dataManager->GetPrimaryImage()
                    : VtkImageGridSnapshot{};
                if (current != m_renderSnapshot
                    && !BuildPipeline()) {
                    loadNotice.isSucceeded = false;
                    ClearLoadFail(loadNotice.kind);
                }
                else {
                    SetPendingFlags(UpdateFlags::All);
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
    const bool isStrategySet = SetStrategyState();

    // 4. owner 已释放 admission 后再执行回调，允许业务方安全重入下一次 load。
    SendCompletions();
    return isStrategySet;
}

bool AppRuntime::SendReloadUpdate()
{
    if (!GetIsOwnerThread()) {
        return false;
    }
    SetPendingFlags(UpdateFlags::All);
    return BuildPipeline();
}

void AppRuntime::SendTasks()
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
    // 2. 固定 worker 已完成 packaged_task；锁外读取结果并区分普通任务与 Load/Reload。
    for (auto& task : readyTasks) {
        bool isSuccess = false;
        try { isSuccess = task.result.get(); }
        catch (...) { isSuccess = false; }
        SetTaskResult(std::move(task), isSuccess);
    }
}

void AppRuntime::SetTaskResult(ActiveTask task, bool isSuccess)
{
    if (task.loadKind == LoadEventKind::None) {
        SetCompletion(isSuccess, std::move(task.callback));
        return;
    }
    SetLoadResult(std::move(task), isSuccess);
}

void AppRuntime::SetLoadResult(ActiveTask task, bool isSuccess)
{
    // worker 成功只表示 pending 已准备；Host 注入时由跨视图候选事务统一提交，
    // 独立 AppRuntime 则保留原有单视图提交路径。
    if (isSuccess && m_dataManager) {
        if (m_setLoadCommit) {
            isSuccess = m_setLoadCommit(task.loadKind);
        }
        else {
            const auto stage = m_dataManager->GetLoadStage();
            VtkImageGridSnapshot published;
            isSuccess = stage
                && m_dataManager->SetLoadCommit(stage, published)
                && published;
        }
    }
    if (!isSuccess && m_dataManager) m_dataManager->ClearLoadStage();

    // callback 暂存到 owner 槽，待共享终态广播、各视图管线同步和 admission 释放后再执行。
    m_ownedCallback = std::move(task.callback);
    if (!m_sharedState || !m_dataManager) return;
    if (isSuccess) {
        const auto current = m_dataManager->GetPrimaryImage();
        DataReadyState readyState;
        const bool hasStagedState = m_readyState
            && current
            && current->data && current->binding
            && m_readyState->dataRevision == current->data->self
            && m_readyState->bindingRevision
                == current->binding->revision;
        if (hasStagedState) {
            readyState = *m_readyState;
        }
        else if (!GetDataReadyState(current, readyState)) {
            // current 已完成不可逆发布；此兜底只处理损坏实现，不能把成功 CAS 伪装成失败。
            readyState.dataRevision = current && current->data
                ? current->data->self : DataRevisionRef{};
            readyState.bindingRevision =
                m_dataManager->GetPrimaryBindingRevision();
            readyState.scalarRange = m_dataManager->GetScalarRange();
            readyState.spacing = m_dataManager->GetSpacing();
            readyState.cursorWorld = m_sharedState->GetCursorWorld();
        }
        m_readyState.reset();
        m_sharedState->SetDataReady(readyState);
    }
    else if (task.loadKind == LoadEventKind::File) {
        m_readyState.reset();
        m_sharedState->SetFileLoadFailed();
    }
    else {
        m_readyState.reset();
        m_sharedState->SetReloadLoadFailed();
    }
}

void AppRuntime::SetCompletion(
    bool isSuccess,
    std::function<void(bool)> callback)
{
    if (!callback) return;
    std::lock_guard<std::mutex> lock(m_completionMutex);
    m_completions.push_back({ isSuccess, std::move(callback) });
}

void AppRuntime::SendCompletions()
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
            std::cerr << "[AppRuntime] Completion failed: " << error.what() << '\n';
        }
        catch (...) {
            std::cerr << "[AppRuntime] Completion failed with an unknown exception.\n";
        }
    }
}

bool AppRuntime::CreateLoadNotice(
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

bool AppRuntime::RemoveLoadNotice(LoadNotice& loadNotice)
{
    std::lock_guard<std::mutex> lock(m_loadNoticeMutex);
    if (m_loadNotices.empty()) {
        return false;
    }
    loadNotice = m_loadNotices.front();
    m_loadNotices.pop_front();
    return true;
}

bool AppRuntime::SetOwnedLoad(LoadEventKind loadEventKind)
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

bool AppRuntime::GetOwnedLoad(LoadEventKind loadEventKind) const
{
    return m_ownedLoadKind.load() == static_cast<int>(loadEventKind);
}

bool AppRuntime::StartViewUpdate()
{
    return m_viewState && m_viewState->StartUpdate();
}

bool AppRuntime::SetViewUpdateCommit(
    const bool isCommitted,
    UpdateFlags& pendingFlags)
{
    return m_viewState
        && m_viewState->SetUpdateCommit(
            isCommitted, pendingFlags);
}

void AppRuntime::SendViewUpdateFlags(
    const UpdateFlags flags) noexcept
{
    if (m_viewState) {
        m_viewState->SendUpdateFlags(flags);
    }
}

bool AppRuntime::SendSessionUpdate(
    const AppSessionUpdate& update)
{
    const bool hasSpacing = update.spacing.has_value();
    const bool hasCursor = update.cursorWorld.has_value();
    if (!m_sharedState || (!hasSpacing && !hasCursor)) {
        return false;
    }
    if (hasSpacing
        && std::any_of(
            update.spacing->begin(), update.spacing->end(),
            [](const double value) {
                return !std::isfinite(value) || value <= 0.0;
            })) {
        return false;
    }
    const int cursorAxis = static_cast<int>(update.cursorAxis);
    if (hasCursor
        && (cursorAxis < -1 || cursorAxis > 2
            || std::any_of(
                update.cursorWorld->begin(), update.cursorWorld->end(),
                [](const double value) {
                    return !std::isfinite(value);
                }))) {
        return false;
    }

    const auto oldSpacing = m_sharedState->GetSpacing();
    const auto oldCursor = m_sharedState->GetCursorWorld();
    const int oldAxis = m_sharedState->GetCursorAxis();
    if (!m_sharedState->StartViewUpdate()) return false;

    bool isUpdated = true;
    if (hasSpacing) {
        isUpdated = SetSpacing(
            (*update.spacing)[0],
            (*update.spacing)[1],
            (*update.spacing)[2]);
    }
    if (isUpdated && hasCursor) {
        isUpdated = SetCursorState(
            *update.cursorWorld, update.cursorAxis);
    }

    UpdateFlags pendingFlags = UpdateFlags::None;
    if (isUpdated && m_sharedState->SetViewUpdateCommit(
            true, pendingFlags)) {
        m_sharedState->SendViewUpdateFlags(pendingFlags);
        return true;
    }

    bool isRestored = true;
    if (hasSpacing) {
        isRestored = SetSpacing(
            oldSpacing[0], oldSpacing[1], oldSpacing[2]);
    }
    if (hasCursor) {
        const AppCursorAxis axis = oldAxis >= -1 && oldAxis <= 2
            ? static_cast<AppCursorAxis>(oldAxis)
            : AppCursorAxis::Free;
        isRestored = SetCursorState(oldCursor, axis) && isRestored;
    }
    UpdateFlags droppedFlags = UpdateFlags::None;
    const bool isClosed = m_sharedState->SetViewUpdateCommit(
        false, droppedFlags);
    if (isClosed) {
        m_sharedState->SendViewUpdateFlags(droppedFlags);
    }
    if (!isRestored || !isClosed) {
        std::cerr << "[AppRuntime] Session update rollback failed.\n";
    }
    return false;
}

bool AppRuntime::ResetOwnedLoad(LoadEventKind loadEventKind)
{
    int expectedKind = static_cast<int>(loadEventKind);
    return m_ownedLoadKind.compare_exchange_strong(
        expectedKind,
        static_cast<int>(LoadEventKind::None));
}

void AppRuntime::SetDataRefresh()
{
    // 状态发布线程只置结构重建门铃，不在回调栈内修改渲染管线。
    m_hasDataRefreshNeed = true;
}

// ─────────────────────────────────────────────────────────────────────
// 私有辅助
// ─────────────────────────────────────────────────────────────────────
bool AppRuntime::BuildPipeline()
{
    if (!GetIsOwnerThread() || !m_dataManager) return false;
    const auto snapshot = m_dataManager->GetPrimaryImage();
    if (!BuildDataStage(snapshot)) return false;
    if (!SetViewStage(snapshot)) {
        (void)ClearDataStage();
        return false;
    }
    SetDataStageComplete();
    return true;
}

bool AppRuntime::BuildDataStage(const VtkImageGridSnapshot& snapshot)
{
    if (!GetIsOwnerThread() || m_dataStage
        || !snapshot || !snapshot->image
        || !m_sharedState || !m_viewState || !m_renderer) {
        return false;
    }
    int dimensions[3] = {};
    snapshot->image->GetDimensions(dimensions);
    if (dimensions[0] <= 0 || dimensions[1] <= 0
        || dimensions[2] <= 0) {
        return false;
    }

    DataStage stage;
    stage.oldSnapshot = m_renderSnapshot;
    stage.nextSnapshot = snapshot;
    stage.oldStrategy = m_currentStrategy;
    stage.oldMode = m_currentMode;
    stage.oldCamera = GetCameraState();
    stage.mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    if (!stage.oldCamera.isValid
        || !GetDataReadyState(snapshot, stage.readyState)) {
        return false;
    }
    stage.nextParams = GetRenderParams(UpdateFlags::All);
    stage.nextParams.scalarRange[0] =
        stage.readyState.scalarRange[0];
    stage.nextParams.scalarRange[1] =
        stage.readyState.scalarRange[1];
    stage.nextParams.cursor = stage.readyState.cursorWorld;
    stage.nextParams.cursorRaw = stage.readyState.cursorWorld;
    stage.nextParams.cursorAxis = -1;
    const bool hasColorTransfer =
        !stage.nextParams.volumeTransferFunction.colorNodes.empty();
    const bool hasOpacityTransfer =
        !stage.nextParams.volumeTransferFunction.opacityNodes.empty();
    if (m_viewState->GetTransferAuto()) {
        const auto function = GetDefaultVolumeTransfer(snapshot);
        if (!function) return false;
        stage.nextParams.volumeTransferFunction = *function;
        stage.hasDefaultTransfer = true;
    }
    else if (hasColorTransfer != hasOpacityTransfer
        || !hasColorTransfer
        || !GetVolumeTransferValid(
            stage.nextParams.volumeTransferFunction)
        || !GetTransferRangeValid(
            stage.nextParams.volumeTransferFunction,
            stage.readyState.scalarRange)) {
        return false;
    }
    const auto autoWindowLevel = GetAutoWindowLevel(
        stage.readyState.scalarRange);
    if (!autoWindowLevel) return false;
    stage.autoWindowLevel = *autoWindowLevel;
    if (m_viewState
        && m_viewState->GetWindowLevelMode()
            == WindowLevelMode::Auto) {
        stage.nextParams.windowLevel = stage.autoWindowLevel;
    }

    const auto effect = m_renderEffect.lock();
    if (stage.oldStrategy && effect
        && stage.oldSnapshot != snapshot) {
        const auto effectState =
            stage.oldStrategy->GetRenderEffectState();
        if (effectState.status == RenderEffectStatus::Staged
            || effectState.status == RenderEffectStatus::Ready) {
            return false;
        }
    }

    stage.nextStrategy = CreateStrategy(stage.mode);
    if (!stage.nextStrategy
        || stage.nextStrategy == stage.oldStrategy) {
        return false;
    }

    bool isRendererAttached = false;
    bool isSwapOverridden = false;
    vtkTypeBool oldSwapState = 1;
    bool hasRenderError = false;
    auto errorCallback = vtkSmartPointer<vtkCallbackCommand>::New();
    errorCallback->SetClientData(&hasRenderError);
    errorCallback->SetCallback(
        [](vtkObject*, unsigned long, void* clientData, void*) {
            if (clientData) *static_cast<bool*>(clientData) = true;
        });
    unsigned long windowErrorTag = 0;
    unsigned long rendererErrorTag = 0;
    const auto clearErrorWatch = [&]() {
        if (m_renderWindow && windowErrorTag != 0) {
            m_renderWindow->RemoveObserver(windowErrorTag);
            windowErrorTag = 0;
        }
        if (m_renderer && rendererErrorTag != 0) {
            m_renderer->RemoveObserver(rendererErrorTag);
            rendererErrorTag = 0;
        }
        errorCallback->SetClientData(nullptr);
    };
    try {
        // 先把 producer 配置写入候选，再设置输入。Volume 因此直接按目标
        // quality/denoise 构建一次，避免先按默认 Auto 物化后再整卷重建。
        const UpdateFlags producerFlags =
            UpdateFlags::Quality | UpdateFlags::Denoise;
        if (!stage.nextStrategy->SetVisualState(
                stage.nextParams, producerFlags)) {
            throw std::runtime_error(
                "Candidate rejected the producer configuration.");
        }
        if (!stage.nextStrategy->SetInputData(
                snapshot->image, snapshot->validityMask)) {
            throw std::runtime_error(
                "Candidate rejected the render input data.");
        }
        if (!stage.nextStrategy->SetRenderInputStamp({
                snapshot->data->self })) {
            throw std::runtime_error(
                "Candidate rejected the render input stamp.");
        }

        if (effect) {
            if (!stage.nextStrategy->AttachRenderEffect(
                    effect, RenderBindingUse::Candidate)) {
                throw std::runtime_error(
                    "Candidate render effect attach failed.");
            }
            stage.isEffectAttached = true;
        }

        stage.nextStrategy->AttachRenderer(m_renderer);
        isRendererAttached = true;
        if (!stage.nextStrategy->SetVisualState(
                stage.nextParams, UpdateFlags::All)) {
            throw std::runtime_error(
                "Candidate visual state was rejected.");
        }
        const auto candidateEffectState = effect
            ? stage.nextStrategy->GetRenderEffectState()
            : RenderEffectState{};
        const bool hasEffectStage = effect
            && candidateEffectState.status
                == RenderEffectStatus::Staged;
        if (hasEffectStage) {
            if (!m_renderWindow) {
                throw std::runtime_error(
                    "Candidate effect warm-up needs a render window.");
            }
            windowErrorTag = m_renderWindow->AddObserver(
                vtkCommand::ErrorEvent, errorCallback);
            rendererErrorTag = m_renderer->AddObserver(
                vtkCommand::ErrorEvent, errorCallback);
            if (windowErrorTag == 0 || rendererErrorTag == 0) {
                throw std::runtime_error(
                    "Candidate effect error observer attach failed.");
            }

            oldSwapState = m_renderWindow->GetSwapBuffers();
            m_renderWindow->SwapBuffersOff();
            isSwapOverridden = true;
            constexpr int renderLimit = 2;
            for (int renderCount = 0;
                renderCount < renderLimit; ++renderCount) {
                m_renderWindow->Render();
                if (hasRenderError) {
                    throw std::runtime_error(
                        "Candidate effect warm-up reported ErrorEvent.");
                }
                if (stage.nextStrategy->GetRenderEffectState().status
                    != RenderEffectStatus::Staged) {
                    break;
                }
            }
            m_renderWindow->SetSwapBuffers(oldSwapState);
            isSwapOverridden = false;
            clearErrorWatch();
        }

        if (effect) {
            const auto effectState =
                stage.nextStrategy->GetRenderEffectState();
            const bool isReadyCommitted =
                effectState.status == RenderEffectStatus::Ready
                && effectState.stagedRevision != 0
                && stage.nextStrategy->SetRenderEffectCommit(
                    effectState.stagedRevision);
            const bool hasNoReplay =
                effectState.status == RenderEffectStatus::Idle;
            const bool isCommitted =
                effectState.status == RenderEffectStatus::Committed;
            if ((!isReadyCommitted && !hasNoReplay && !isCommitted)
                || !stage.nextStrategy->SetRenderEffectUse(
                    RenderBindingUse::Current)) {
                throw std::runtime_error(
                    "Candidate render effect commit failed.");
            }
        }
        stage.nextStrategy->DetachRenderer(m_renderer);
        isRendererAttached = false;

        // 相机计算也在候选阶段验证，随即恢复；两次调用之间没有 Render。
        m_renderSnapshot = snapshot;
        if (!SetModeCamera(stage.mode, snapshot)
            || !SetCameraState(stage.oldCamera)) {
            throw std::runtime_error(
                "Candidate camera validation failed.");
        }
        m_renderSnapshot = stage.oldSnapshot;
        m_dataStage = std::move(stage);
        return true;
    }
    catch (const std::exception& error) {
        std::cerr << "[BuildDataStage] Failed: " << error.what() << '\n';
    }
    catch (...) {
        std::cerr << "[BuildDataStage] Failed with an unknown exception.\n";
    }

    m_renderSnapshot = stage.oldSnapshot;
    (void)SetCameraState(stage.oldCamera);
    try {
        if (isSwapOverridden && m_renderWindow) {
            m_renderWindow->SetSwapBuffers(oldSwapState);
            isSwapOverridden = false;
        }
        clearErrorWatch();
        if (isRendererAttached && stage.nextStrategy) {
            stage.nextStrategy->DetachRenderer(m_renderer);
        }
        if (stage.nextStrategy && effect && stage.isEffectAttached) {
            const auto effectState =
                stage.nextStrategy->GetRenderEffectState();
            if (effectState.stagedRevision != 0) {
                (void)stage.nextStrategy->ClearRenderEffectStage(
                    effectState.stagedRevision);
            }
            (void)stage.nextStrategy->DetachRenderEffect(effect.get());
        }
    }
    catch (...) {
    }
    return false;
}

bool AppRuntime::SetViewStage(const VtkImageGridSnapshot& snapshot)
{
    if (!GetIsOwnerThread() || !m_dataStage || !snapshot
        || !snapshot->image || m_dataStage->isCommitted
        || !m_dataStage->nextSnapshot->data || !snapshot->data
        || m_dataStage->nextSnapshot->data->self != snapshot->data->self) {
        return false;
    }

    try {
        if (!m_dataStage->nextStrategy->SetRenderInputStamp({
                snapshot->data->self })) {
            throw std::runtime_error(
                "Committed input stamp was rejected.");
        }
        m_renderSnapshot = snapshot;
        SetCurrentStrategy(
            m_dataStage->nextStrategy, m_dataStage->mode, false);
        {
            std::lock_guard<std::mutex> lock(m_viewConfigMutex);
            m_appliedQuality =
                m_dataStage->nextParams.volumeQuality;
        }
        SetRendererBg();
        SetPendingFlags(UpdateFlags::All);
        SetSyncNeeded();
        m_dataStage->isCommitted = true;
        return true;
    }
    catch (const std::exception& error) {
        std::cerr << "[SetViewStage] Failed: " << error.what() << '\n';
    }
    catch (...) {
        std::cerr << "[SetViewStage] Failed with an unknown exception.\n";
    }
    (void)ResetViewStage();
    return false;
}

bool AppRuntime::ResetViewStage()
{
    if (!GetIsOwnerThread() || !m_dataStage) return false;
    auto& stage = *m_dataStage;
    const auto effect = m_renderEffect.lock();
    const bool isCandidateCurrent =
        m_currentStrategy == stage.nextStrategy;
    bool isReset = true;
    try {
        if (isCandidateCurrent && stage.nextStrategy && m_renderer) {
            stage.nextStrategy->DetachRenderer(m_renderer);
        }
        if (stage.nextStrategy && effect && stage.isEffectAttached) {
            isReset = stage.nextStrategy->DetachRenderEffect(
                effect.get()) && isReset;
            stage.isEffectAttached = false;
        }

        if (isCandidateCurrent) {
            m_renderSnapshot = stage.oldSnapshot;
            m_currentStrategy = stage.oldStrategy;
            m_currentMode = stage.oldMode;
            if (stage.oldStrategy) {
                isReset = stage.oldStrategy->SetRenderInputStamp(
                    GetRenderInputStamp()) && isReset;
                if (effect) {
                    isReset = stage.oldStrategy->AttachRenderEffect(
                        effect, RenderBindingUse::Current) && isReset;
                }
                if (m_renderer) {
                    stage.oldStrategy->AttachRenderer(m_renderer);
                }
            }
        }
        isReset = SetCameraState(stage.oldCamera) && isReset;
    }
    catch (...) {
        isReset = false;
    }
    stage.isCommitted = false;
    m_isDirty = true;
    return isReset;
}

bool AppRuntime::ClearDataStage()
{
    if (!GetIsOwnerThread()) return false;
    if (!m_dataStage) return true;
    if (m_dataStage->isCommitted) {
        // 成功候选只能走 SetDataStageComplete；Clear 专用于失败补偿。
        return false;
    }
    if (m_currentStrategy == m_dataStage->nextStrategy) {
        return false;
    }

    const auto effect = m_renderEffect.lock();
    bool isCleared = true;
    try {
        if (m_dataStage->nextStrategy && m_renderer) {
            m_dataStage->nextStrategy->DetachRenderer(m_renderer);
        }
        if (m_dataStage->nextStrategy && effect
            && m_dataStage->isEffectAttached) {
            const auto effectState =
                m_dataStage->nextStrategy->GetRenderEffectState();
            if (effectState.stagedRevision != 0) {
                isCleared = m_dataStage->nextStrategy
                    ->ClearRenderEffectStage(effectState.stagedRevision)
                    && isCleared;
            }
            isCleared = m_dataStage->nextStrategy
                ->DetachRenderEffect(effect.get()) && isCleared;
        }
    }
    catch (...) {
        isCleared = false;
    }
    if (isCleared) m_dataStage.reset();
    return isCleared;
}

void AppRuntime::SetDataStageComplete() noexcept
{
    if (!m_dataStage || !m_dataStage->isCommitted) {
        // SetViewStage 成功后到 DataManager 发布之间禁止外部改变 stage；
        // 违反该协议时已无法安全回滚，不能伪装成普通 load 失败。
        std::terminate();
    }
    if (m_setLoadCommit) {
        m_readyState = m_dataStage->readyState;
    }
    if (m_viewState
        && m_viewState->GetWindowLevelMode()
            == WindowLevelMode::Auto) {
        // 候选阶段不改展示真源；只有数据已不可逆发布后才提交自动值。
        // 若期间出现显式设置/拖拽，SetAutoWindowLevel 会保留新的 Manual 意图。
        (void)m_viewState->SetAutoWindowLevel(
            m_dataStage->autoWindowLevel);
    }
    if (m_dataStage->hasDefaultTransfer && m_viewState) {
        // 自动值只在 Auto 意图仍有效时写回；加载期间到达的显式 TF
        // 已经把来源切为 Explicit，不能被旧候选覆盖。
        (void)m_viewState->SetAutoTransfer(
            m_dataStage->nextParams.volumeTransferFunction);
    }
    m_dataStage.reset();
}

std::optional<VolumeTransferFunction>
AppRuntime::GetDefaultVolumeTransfer(
    const VtkImageGridSnapshot& snapshot)
{
    if (!snapshot || !snapshot->image || !snapshot->data) {
        return std::nullopt;
    }

    double scalarRange[2] = {};
    snapshot->image->GetScalarRange(scalarRange);
    const double rangeMin = scalarRange[0];
    const double rangeMax = scalarRange[1];
    const double rangeWidth = rangeMax - rangeMin;
    if (!std::isfinite(rangeWidth) || rangeWidth < 0.0) {
        return std::nullopt;
    }
    VolumeTransferFunction function;
    if (rangeWidth == 0.0) {
        double upperScalar = std::nextafter(
            rangeMin, std::numeric_limits<double>::infinity());
        double lowerScalar = rangeMin;
        if (!std::isfinite(upperScalar)) {
            lowerScalar = std::nextafter(
                rangeMin,
                -std::numeric_limits<double>::infinity());
            upperScalar = rangeMin;
        }
        if (!std::isfinite(lowerScalar)
            || !std::isfinite(upperScalar)
            || upperScalar <= lowerScalar) {
            return std::nullopt;
        }
        function.colorNodes = {
            { lowerScalar, 1.0, 1.0, 1.0 },
            { upperScalar, 1.0, 1.0, 1.0 }
        };
        function.opacityNodes = {
            { lowerScalar, 1.0 },
            { upperScalar, 1.0 }
        };
        return function;
    }

    constexpr std::array<double, 4> positions{
        0.0, 0.5, 0.85, 1.0
    };
    constexpr std::array<double, 4> opacities{
        0.0, 0.0, 0.8, 1.0
    };
    constexpr double defaultColor = 0.75;
    std::array<double, 4> scalars{};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        scalars[index] = index == 0 ? rangeMin
            : index + 1 == positions.size() ? rangeMax
            : rangeMin + positions[index] * rangeWidth;
        if (!std::isfinite(scalars[index])) return std::nullopt;
    }
    const bool hasDistinctScalars = std::adjacent_find(
        scalars.begin(), scalars.end(),
        [](const double left, const double right) {
            return right <= left;
        }) == scalars.end();
    if (!hasDistinctScalars) {
        function.colorNodes = {
            { rangeMin, defaultColor, defaultColor, defaultColor },
            { rangeMax, defaultColor, defaultColor, defaultColor }
        };
        function.opacityNodes = {
            { rangeMin, 0.0 },
            { rangeMax, 1.0 }
        };
        return function;
    }

    function.colorNodes.reserve(positions.size());
    function.opacityNodes.reserve(positions.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        function.colorNodes.push_back({
            scalars[index], defaultColor, defaultColor, defaultColor });
        function.opacityNodes.push_back({
            scalars[index], opacities[index] });
    }
    return function;
}

double AppRuntime::GetRenderRate(
    const bool isInteracting) const noexcept
{
    constexpr double staticRate = 0.001;
    constexpr double fastRate = 15.0;
    return isInteracting ? fastRate : staticRate;
}

bool AppRuntime::SetStrategyState()
{
    bool isExpected = true;
    // CAS 同时领取并清掉同步闸门；没有外部请求时无需进入策略计算。
    if (!m_hasSyncNeed.compare_exchange_strong(isExpected, false)) {
        return true;
    }
    // 当前无 Strategy 时位图仍保留；只有后续再次置同步闸门才会消费。
    if (!m_currentStrategy) return true;

    // 用 exchange(0) 取走当前整包增量标志，相当于把这一帧前累计的状态改动做一次原子快照。
    // 后续新来的事件会写入新的 m_pendingFlags，留到下一帧继续消费，不会和本次同步互相覆盖。
    int flagsInt = m_pendingFlags.exchange(0);
    UpdateFlags flags = static_cast<UpdateFlags>(flagsInt);

    if (flags == UpdateFlags::None) {
        // CAS 已领取旧门铃；此处不能再次写 false，否则会覆盖 exchange
        // 之后由并发状态请求置起的新门铃，留下永不消费的 pending flags。
        return true;
    }

    // 刷新率只跟随通用交互生命周期，不参与质量档或 producer 选择。
    const double oldDesiredRate = m_renderWindow
        ? m_renderWindow->GetDesiredUpdateRate() : 0.0;
    if ((flags & (UpdateFlags::RenderRate | UpdateFlags::Quality))
            != UpdateFlags::None
        && m_renderWindow) {
        const bool isInteracting = m_sharedState->GetIsInteracting();
        m_renderWindow->SetDesiredUpdateRate(
            GetRenderRate(isInteracting));
    }
    // RenderRate 继续透传给 Strategy，使屏幕采样与慢速 LOD
    // 提交都能观察交互边界；它不直接更换 mapper input。

    const bool hasBackgroundChanged =
        (flags & UpdateFlags::Background) != UpdateFlags::None;
    const UpdateFlags strategyFlags = static_cast<UpdateFlags>(
        static_cast<int>(flags)
        & ~static_cast<int>(UpdateFlags::Background));

    RenderParams params;
    bool isVisualSet = true;
    if (strategyFlags != UpdateFlags::None) {
        params = GetRenderParams(strategyFlags);
        isVisualSet = m_currentStrategy->SetVisualState(
            params, strategyFlags);
        if (isVisualSet
            && (strategyFlags
                & (UpdateFlags::Cursor | UpdateFlags::Transform))
                != UpdateFlags::None) {
            FeatureOverlayState overlayState;
            overlayState.cursor = params.cursor;
            overlayState.modelToWorld = params.modelMatrix;
            for (auto& overlay : m_overlays) {
                overlay->SetOverlayState(overlayState);
            }
        }
    }
    if (!isVisualSet) {
        if (m_renderWindow) {
            m_renderWindow->SetDesiredUpdateRate(oldDesiredRate);
        }
        bool hasNewQuality = false;
        if ((strategyFlags & UpdateFlags::Quality)
            != UpdateFlags::None) {
            std::lock_guard<std::mutex> lock(m_viewConfigMutex);
            if (m_requestedQuality == params.volumeQuality) {
                m_requestedQuality = m_appliedQuality;
            }
            else {
                hasNewQuality = true;
            }
        }
        // 当前失败的 Quality 不重试；失败期间到达的更新必须保留。
        UpdateFlags retryFlags = static_cast<UpdateFlags>(
            static_cast<int>(flags)
            & ~static_cast<int>(UpdateFlags::Quality));
        if (hasNewQuality) {
            retryFlags = retryFlags | UpdateFlags::Quality;
        }
        if (retryFlags != UpdateFlags::None) {
            m_pendingFlags.fetch_or(static_cast<int>(retryFlags));
            m_hasSyncNeed = true;
        }
        return false;
    }
    if ((strategyFlags & UpdateFlags::Quality)
        != UpdateFlags::None) {
        bool hasNewQuality = false;
        std::lock_guard<std::mutex> lock(m_viewConfigMutex);
        // applied 必须记录本次交给 Strategy 的不可变快照；若执行期间又
        // 收到新挡位，则让新值留在 requested 并安排下一帧消费。
        m_appliedQuality = params.volumeQuality;
        hasNewQuality = m_requestedQuality != m_appliedQuality;
        if (hasNewQuality) {
            m_pendingFlags.fetch_or(
                static_cast<int>(UpdateFlags::Quality));
            m_hasSyncNeed = true;
        }
    }

    // 背景与主体视觉状态属于同一帧提交；策略拒绝时不得先暴露新背景。
    if (hasBackgroundChanged && m_renderer) {
        SetRendererBg();
    }

    // Strategy 只更新自身 prop；同一 Transform 快照随后由单一 view owner 平移相机中心。
    if ((strategyFlags & UpdateFlags::Transform)
        != UpdateFlags::None) {
        (void)SetCameraCenter(params.modelMatrix, m_renderSnapshot);
    }

    // Strategy 已消费本次快照，发布本帧 Render 请求；Timer 随后用 ResetDirty() 领取。
    m_isDirty = true;
    return true;
}

void AppRuntime::ClearLoadFail(LoadEventKind loadEventKind)
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
    ClearStrategies();
    m_renderSnapshot.reset();

    // 失败终态取消尚未消费的重建与增量，防止旧 DataReady 在后续帧恢复管线。
    m_hasDataRefreshNeed = false;
    m_pendingFlags = 0;

    // 标脏使渲染器刷新空场景
    m_isDirty = true;
}

RenderParams AppRuntime::GetRenderParams(UpdateFlags flags) const
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
    if ((flags & UpdateFlags::VolumeTransfer)
        != UpdateFlags::None) {
        p.volumeTransferFunction =
            m_viewState->GetVolumeTransferFunction();
        // material.opacity 是 scalar opacity 的独立倍率，
        // 传输函数快照必须携带当前材质值。
        p.material = m_viewState->GetMaterial();
    }

    if (((flags & UpdateFlags::WindowLevel) != UpdateFlags::None)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.windowLevel = m_viewState->GetWindowLevel();
    }

    if (((flags & UpdateFlags::Material) != UpdateFlags::None)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.material = m_viewState->GetMaterial();
        p.volumeTransferFunction =
            m_viewState->GetVolumeTransferFunction();
    }

    if ((flags & (UpdateFlags::Quality | UpdateFlags::RenderRate))
        != UpdateFlags::None) {
        p.isFeatureActive = GetIsFeatureActive();
        p.isInteracting = m_sharedState->GetIsInteracting();
        p.volumeQuality = GetTargetQuality();
    }

    if ((flags & UpdateFlags::GradientOpacity) != UpdateFlags::None) {
        p.gradientOpacity = GetGradientOpacity();
    }

    if ((flags & UpdateFlags::Denoise) != UpdateFlags::None) {
        p.isDenoiseOn = GetDenoiseOn();
    }

    if (((flags & UpdateFlags::IsoValue) != UpdateFlags::None))
        p.isoValue = m_viewState->GetIsoValue();

    if (((flags & UpdateFlags::Visibility) != UpdateFlags::None))
		p.visibilityMask = m_viewState->GetVisibilityMask();

    return p;
}

std::shared_ptr<AbstractVisualStrategy>
AppRuntime::CreateStrategy(const VizMode mode)
{
    return m_strategyCreate ? m_strategyCreate(mode) : nullptr;
}

class DataPortAdapter final : public AppDataPort {
public:
    explicit DataPortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    TaskAdmissionResult LoadFileAsync(
        std::string path,
        VolumeLayout layout,
        std::function<void(bool)> onComplete) override
    {
        return m_service
            ? m_service->LoadFileAsync(
                std::move(path), std::move(layout), std::move(onComplete))
            : TaskAdmissionResult::Unavailable;
    }

    TaskAdmissionResult ReloadFromBufferAsync(
        VolumeBuffer buffer,
        std::function<void(bool)> onComplete) override
    {
        return m_service
            ? m_service->ReloadFromBufferAsync(
                std::move(buffer), std::move(onComplete))
            : TaskAdmissionResult::Unavailable;
    }

    TaskAdmissionResult ExportDataAsync(
        std::string outputDir,
        std::string extension,
        std::function<void(bool)> onComplete) override
    {
        return m_service
            ? m_service->ExportDataAsync(
                std::move(outputDir),
                std::move(extension),
                std::move(onComplete))
            : TaskAdmissionResult::Unavailable;
    }

    TaskAdmissionResult ExportSlicesAsync(
        std::string path,
        std::optional<double> rotationAngleDeg,
        std::function<void(bool)> onComplete) override
    {
        return m_service
            ? m_service->ExportSlicesAsync(
                path, rotationAngleDeg, std::move(onComplete))
            : TaskAdmissionResult::Unavailable;
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class DataStageAdapter final : public AppDataStagePort {
public:
    explicit DataStageAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool BuildDataStage(const VtkImageGridSnapshot& snapshot) override
    {
        return m_service && m_service->BuildDataStage(snapshot);
    }

    bool SetViewStage(const VtkImageGridSnapshot& snapshot) override
    {
        return m_service && m_service->SetViewStage(snapshot);
    }

    bool ResetViewStage() override
    {
        return m_service && m_service->ResetViewStage();
    }

    bool ClearDataStage() override
    {
        return m_service && m_service->ClearDataStage();
    }

    void SetDataStageComplete() noexcept override
    {
        if (!m_service) std::terminate();
        m_service->SetDataStageComplete();
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class ViewPortAdapter final : public AppViewPort {
public:
    explicit ViewPortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    ~ViewPortAdapter() override = default;

    bool SetViewConfig(const PreInitConfig& config) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_service || !m_isAvailable) return false;
        const VizMode oldMode = m_service->GetVizMode();
        if (!m_service->StartViewUpdate()) return false;
        try { m_service->SetVisualConfig(config); }
        catch (...) {
            m_service->SetVizMode(oldMode);
            UpdateFlags droppedFlags = UpdateFlags::None;
            const bool isClosed = m_service->SetViewUpdateCommit(
                false, droppedFlags);
            if (!isClosed) m_isAvailable = false;
            const auto service = m_service;
            lock.unlock();
            if (isClosed) {
                service->SendViewUpdateFlags(droppedFlags);
            }
            return false;
        }

        UpdateFlags pendingFlags = UpdateFlags::None;
        if (m_service->SetViewUpdateCommit(
                true, pendingFlags)) {
            ++m_revision;
            const auto service = m_service;
            lock.unlock();
            service->SendViewUpdateFlags(pendingFlags);
            return true;
        }

        m_service->SetVizMode(oldMode);
        UpdateFlags droppedFlags = UpdateFlags::None;
        const bool isClosed = m_service->SetViewUpdateCommit(
            false, droppedFlags);
        if (!isClosed) m_isAvailable = false;
        const auto service = m_service;
        lock.unlock();
        if (isClosed) {
            service->SendViewUpdateFlags(droppedFlags);
        }
        return false;
    }

    bool SendViewUpdate(const AppViewUpdate& update) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_service || !m_isAvailable || !GetUpdateValid(update)) {
            return false;
        }

        const AppViewState oldState = GetState();
        if (!m_service->StartViewUpdate()) return false;
        bool isUpdated = false;
        try { isUpdated = SetUpdate(update); }
        catch (...) { isUpdated = false; }
        UpdateFlags pendingFlags = UpdateFlags::None;
        if (isUpdated && m_service->SetViewUpdateCommit(
                true, pendingFlags)) {
            ++m_revision;
            const auto service = m_service;
            lock.unlock();
            service->SendViewUpdateFlags(pendingFlags);
            return true;
        }

        // 这是 owner-thread 补偿事务而非跨线程值原子提交：事件屏障保证
        // 失败更新及其回滚不会触发同步 observer 或可渲染中间帧。
        bool isRestored = false;
        try { isRestored = SetState(oldState); }
        catch (...) { isRestored = false; }
        UpdateFlags droppedFlags = UpdateFlags::None;
        const bool isClosed = m_service->SetViewUpdateCommit(
            false, droppedFlags);
        if (!isRestored || !isClosed) m_isAvailable = false;
        const auto service = m_service;
        lock.unlock();
        if (isClosed) {
            service->SendViewUpdateFlags(droppedFlags);
        }
        return false;
    }

    bool SetViewState(
        const AppViewState& state,
        const std::uint64_t expectedRevision) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_service || !m_isAvailable
            || m_revision != expectedRevision) {
            return false;
        }

        const AppViewState oldState = GetState();
        if (!m_service->StartViewUpdate()) return false;
        bool isUpdated = false;
        try { isUpdated = SetState(state); }
        catch (...) { isUpdated = false; }
        UpdateFlags pendingFlags = UpdateFlags::None;
        if (isUpdated && m_service->SetViewUpdateCommit(
                true, pendingFlags)) {
            ++m_revision;
            const auto service = m_service;
            lock.unlock();
            service->SendViewUpdateFlags(pendingFlags);
            return true;
        }

        bool isRestored = false;
        try { isRestored = SetState(oldState); }
        catch (...) { isRestored = false; }
        UpdateFlags droppedFlags = UpdateFlags::None;
        const bool isClosed = m_service->SetViewUpdateCommit(
            false, droppedFlags);
        if (!isRestored || !isClosed) m_isAvailable = false;
        const auto service = m_service;
        lock.unlock();
        if (isClosed) {
            service->SendViewUpdateFlags(droppedFlags);
        }
        return false;
    }

    AppViewState GetViewState() const override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return GetState();
    }

private:
    bool GetUpdateValid(const AppViewUpdate& update) const
    {
        const auto isUnit = [](const double value) {
            return std::isfinite(value) && value >= 0.0 && value <= 1.0;
        };
        if (update.mode) {
            switch (*update.mode) {
            case VizMode::Volume:
            case VizMode::IsoSurface:
            case VizMode::SliceTop_down:
            case VizMode::SliceFront_back:
            case VizMode::SliceLeft_right:
            case VizMode::CompositeVolume:
            case VizMode::CompositeIsoSurface:
                break;
            default:
                return false;
            }
        }
        if (update.material) {
            const auto& material = *update.material;
            if (!isUnit(material.ambient) || !isUnit(material.diffuse)
                || !isUnit(material.specular) || !isUnit(material.opacity)
                || !std::isfinite(material.specularPower)
                || material.specularPower < 0.0) {
                return false;
            }
        }
        if (update.opacity && !isUnit(*update.opacity)) return false;
        if (update.volumeTransferFunction
            && !m_service->GetVolumeTransferValid(
                *update.volumeTransferFunction)) {
            return false;
        }
        if (update.isoThreshold && !std::isfinite(*update.isoThreshold)) {
            return false;
        }
        if (update.background
            && (!isUnit(update.background->r)
                || !isUnit(update.background->g)
                || !isUnit(update.background->b))) {
            return false;
        }
        if (update.windowLevel
            && (!std::isfinite(update.windowLevel->windowWidth)
                || update.windowLevel->windowWidth <= 0.0
                || !std::isfinite(update.windowLevel->windowCenter))) {
            return false;
        }
        if (update.windowLevelMode) {
            switch (*update.windowLevelMode) {
            case WindowLevelMode::Auto:
            case WindowLevelMode::Manual:
                break;
            default:
                return false;
            }
        }
        if (update.volumeQuality) {
            switch (*update.volumeQuality) {
            case VolumeQuality::Auto:
            case VolumeQuality::Low:
            case VolumeQuality::High:
            case VolumeQuality::XHigh:
            case VolumeQuality::Ultra:
                break;
            default:
                return false;
            }
        }
        return true;
    }

    bool SetUpdate(const AppViewUpdate& update)
    {
        // 先执行可能拒绝输入的写入；调用方持有完整快照并负责失败补偿。
        if (update.volumeQuality
            && !m_service->SetVolumeQuality(*update.volumeQuality)) {
            return false;
        }
        if (update.gradientOpacity
            && !m_service->SetGradientOpacity(*update.gradientOpacity)) {
            return false;
        }
        if (update.isDenoiseOn
            && !m_service->SetDenoiseOn(*update.isDenoiseOn)) {
            return false;
        }
        if (update.windowLevelMode) {
            if (*update.windowLevelMode == WindowLevelMode::Auto) {
                const bool isReset = update.windowLevel
                    ? m_service->ResetWindowLevel(*update.windowLevel)
                    : m_service->ResetWindowLevel();
                if (!isReset) return false;
            }
            else {
                const auto windowLevel = update.windowLevel
                    ? *update.windowLevel
                    : m_service->GetWindowLevel();
                m_service->SetWindowLevel(
                    windowLevel.windowWidth,
                    windowLevel.windowCenter);
            }
        }
        else if (update.windowLevel) {
            m_service->SetWindowLevel(
                update.windowLevel->windowWidth,
                update.windowLevel->windowCenter);
        }

        // 其余写入只在所有可失败字段通过后提交；optional 缺省保持当前状态。
        if (update.mode) m_service->SetVizMode(*update.mode);
        if (update.material) m_service->SetMaterial(*update.material);
        if (update.opacity) m_service->SetOpacity(*update.opacity);
        if (update.volumeTransferFunction
            && !m_service->SetVolumeTransferFunction(
                *update.volumeTransferFunction)) {
            return false;
        }
        if (update.isoThreshold) {
            m_service->SetIsoThreshold(*update.isoThreshold);
        }
        if (update.background) m_service->SetBackground(*update.background);
        if (update.visibility) {
            const auto& visibility = *update.visibility;
            if (visibility.isPlanes3DVisible) {
                m_service->SetElementVisible(
                    VisFlags::Planes3D,
                    *visibility.isPlanes3DVisible);
            }
            if (visibility.isCrosshairVisible) {
                m_service->SetElementVisible(
                    VisFlags::Crosshair,
                    *visibility.isCrosshairVisible);
            }
            if (visibility.isRulerVisible) {
                m_service->SetElementVisible(
                    VisFlags::Ruler,
                    *visibility.isRulerVisible);
            }
        }
        return true;
    }

    bool SetState(const AppViewState& state)
    {
        AppViewUpdate update;
        update.mode = state.mode;
        update.material = state.material;
        if (!state.isTransferAuto
            && (!state.volumeTransferFunction.colorNodes.empty()
            || !state.volumeTransferFunction.opacityNodes.empty())) {
            update.volumeTransferFunction =
                state.volumeTransferFunction;
        }
        update.isoThreshold = state.isoThreshold;
        update.background = state.background;
        update.windowLevel = state.windowLevel;
        update.windowLevelMode = state.windowLevelMode;
        update.volumeQuality = state.volumeQuality;
        update.gradientOpacity = state.gradientOpacity;
        update.isDenoiseOn = state.isDenoiseOn;
        AppVisibilityUpdate visibility;
        visibility.isPlanes3DVisible =
            (state.visibilityMask & VisFlags::Planes3D) != 0;
        visibility.isCrosshairVisible =
            (state.visibilityMask & VisFlags::Crosshair) != 0;
        visibility.isRulerVisible =
            (state.visibilityMask & VisFlags::Ruler) != 0;
        update.visibility = visibility;
        if (!GetUpdateValid(update) || !SetUpdate(update)) {
            return false;
        }
        return !state.isTransferAuto
            || m_service->ResetTransfer(
                state.volumeTransferFunction);
    }

    AppViewState GetState() const
    {
        AppViewState state;
        if (!m_service) return state;
        state.mode = m_service->GetVizMode();
        state.material = m_service->GetMaterial();
        state.volumeTransferFunction =
            m_service->GetVolumeTransferFunction();
        state.isTransferAuto = m_service->GetTransferAuto();
        state.isoThreshold = m_service->GetIsoThreshold();
        state.background = m_service->GetBackground();
        state.spacing = m_service->GetSpacing();
        state.windowLevel = m_service->GetWindowLevel();
        state.windowLevelMode = m_service->GetWindowLevelMode();
        state.scalarRange = m_service->GetScalarRange();
        state.volumeQuality = m_service->GetVolumeQuality();
        state.gradientOpacity = m_service->GetGradientOpacity();
        state.isFeatureActive = m_service->GetIsFeatureActive();
        state.isDenoiseOn = m_service->GetDenoiseOn();
        state.isInteracting = m_service->GetIsInteracting();
        state.cursorWorld = m_service->GetCursorWorld();
        state.cursorAxis = m_service->GetCursorAxis();
        state.visibilityMask = m_service->GetVisibilityMask();
        state.dataRevision = m_service->GetRenderInputStamp().dataRevision;
        state.bindingRevision = m_service->m_renderSnapshot
            && m_service->m_renderSnapshot->binding
            ? m_service->m_renderSnapshot->binding->revision : 0;
        state.revision = m_revision;
        return state;
    }

    std::shared_ptr<AppRuntime> m_service;
    mutable std::mutex m_mutex;
    std::uint64_t m_revision = 0;
    bool m_isAvailable = true;
};

class SessionPortAdapter final : public AppSessionPort {
public:
    explicit SessionPortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SendSessionUpdate(
        const AppSessionUpdate& update) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_service && m_service->SendSessionUpdate(update);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
    std::mutex m_mutex;
};

class FeaturePortAdapter final : public AppFeaturePort {
public:
    explicit FeaturePortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetFeatureActive(
        const FeatureSource& source,
        bool isActive) override
    {
        return m_service
            && m_service->SetFeatureActive(source, isActive);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class UpdatePortAdapter final : public RenderUpdatePort {
public:
    explicit UpdatePortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SendUpdates() override
    {
        return m_service && m_service->SendUpdates();
    }

    bool SetRenderNeeded() override
    {
        if (!m_service) return false;
        m_service->SetDirty();
        return true;
    }

    bool ResetRenderNeeded() override
    {
        return m_service && m_service->ResetDirty();
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class StatePortAdapter final : public InteractionStatePort {
public:
    explicit StatePortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetInteracting(
        const InteractionSource& source,
        bool isInteracting) override
    {
        return m_service
            && m_service->SetInteracting(source, isInteracting);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class SlicePortAdapter final : public SliceInputPort {
public:
    explicit SlicePortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetSliceScroll(int delta) override
    {
        if (!m_service) return false;
        m_service->SetSliceScroll(delta);
        return true;
    }

    int GetPlaneAxis(vtkActor* actor) const override
    {
        return m_service ? m_service->GetPlaneAxis(actor) : -1;
    }

    bool SetCursorWorld(
        const std::array<double, 3>& worldPosition,
        int axis) override
    {
        if (!m_service) return false;
        auto nextPosition = worldPosition;
        m_service->SetCursorWorldPosition(nextPosition.data(), axis);
        return true;
    }

    std::array<double, 3> GetCursorWorld() const override
    {
        return m_service
            ? m_service->GetCursorWorld()
            : std::array<double, 3>{};
    }

    WindowLevelParams GetWindowLevel() const override
    {
        return m_service
            ? m_service->GetWindowLevel()
            : WindowLevelParams{};
    }

    bool SetWindowLevelDrag(
        int totalDx,
        int totalDy,
        int viewWidth,
        int viewHeight,
        double startWidth,
        double startCenter) override
    {
        if (!m_service) return false;
        m_service->SetWindowLevelDrag(
            totalDx,
            totalDy,
            viewWidth,
            viewHeight,
            startWidth,
            startCenter);
        return true;
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class ModelPortAdapter final : public ModelInputPort {
public:
    explicit ModelPortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    vtkProp3D* GetMainProp() const override
    {
        return m_service ? m_service->GetMainProp() : nullptr;
    }

    std::array<double, 16> GetModelMatrix() const override
    {
        return m_service
            ? m_service->GetModelMatrix()
            : std::array<double, 16>{};
    }

    bool SetModelMatrix(
        const std::array<double, 16>& modelToWorld) override
    {
        if (!m_service) return false;
        auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
        matrix->DeepCopy(modelToWorld.data());
        m_service->SetModelMatrix(matrix);
        return true;
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class RenderBindAdapter final : public RenderBindPort {
public:
    explicit RenderBindAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetRenderTarget(
        vtkSmartPointer<vtkRenderWindow> window,
        vtkSmartPointer<vtkRenderer> renderer) override
    {
        return m_service
            && m_service->SetRenderContext(
                std::move(window), std::move(renderer));
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class FeatureViewAdapter final : public FeatureViewService {
public:
    explicit FeatureViewAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetInteracting(
        const InteractionSource& source,
        bool isInteracting) override
    {
        return m_service
            && m_service->SetInteracting(source, isInteracting);
    }

    std::optional<std::array<double, 16>>
        GetModelToWorld() const override
    {
        return m_service
            ? std::optional<std::array<double, 16>>(
                m_service->GetModelMatrix())
            : std::nullopt;
    }

    std::optional<std::array<double, 3>> GetWorldPosition(
        const std::array<double, 3>& modelPosition) const override
    {
        if (!m_service) return std::nullopt;
        std::array<double, 3> worldPosition{};
        m_service->GetWorldPositionFromModel(
            modelPosition.data(), worldPosition.data());
        return worldPosition;
    }

    std::optional<RenderInputStamp>
        GetRenderInputStamp() const override
    {
        return m_service
            ? std::optional<RenderInputStamp>(
                m_service->GetRenderInputStamp())
            : std::nullopt;
    }

    bool AttachRenderEffect(
        std::shared_ptr<RenderEffect> effect) override
    {
        return m_service
            && m_service->AttachRenderEffect(std::move(effect));
    }

    bool DetachRenderEffect(const RenderEffect* effect) override
    {
        return m_service && m_service->DetachRenderEffect(effect);
    }

    bool SetRenderNeeded() override
    {
        if (!m_service) return false;
        m_service->SetDirty();
        return true;
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class OverlayPortAdapter final : public OverlayService {
public:
    explicit OverlayPortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool AttachOverlay(
        std::shared_ptr<FeatureOverlay> overlay) override
    {
        return m_service
            && m_service->AttachOverlay(std::move(overlay));
    }

    void RemoveOverlay(
        std::shared_ptr<FeatureOverlay> overlay) noexcept override
    {
        if (m_service) {
            m_service->RemoveOverlay(std::move(overlay));
        }
    }

    void ClearOverlays() noexcept override
    {
        if (m_service) m_service->ClearOverlays();
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class TaskControlAdapter final : public AppTaskControlPort {
public:
    explicit TaskControlAdapter(
        std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetTaskStopping() override
    {
        return m_service && m_service->SetTaskStopping();
    }

    bool StopTasks(
        const std::chrono::steady_clock::time_point deadline) override
    {
        return m_service && m_service->StopTasks(deadline);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

AppFactoryResult CreateAppPorts(AppServiceArgs args)
{
    if (!args.dataManager
        || !args.interactionState
        || !args.eventSource) {
        return {};
    }

    std::shared_ptr<AppRuntime> service;
    try {
        service = std::make_shared<AppRuntime>(std::move(args));
    }
    catch (const std::exception& error) {
        std::cerr << "[AppRuntime] Worker startup failed: "
            << error.what() << '\n';
        return {};
    }
    catch (...) {
        std::cerr << "[AppRuntime] Worker startup failed.\n";
        return {};
    }

    AppFactoryResult result;
    result.app.data =
        std::make_shared<DataPortAdapter>(service);
    result.app.view =
        std::make_shared<ViewPortAdapter>(service);
    result.app.session =
        std::make_shared<SessionPortAdapter>(service);
    result.app.feature =
        std::make_shared<FeaturePortAdapter>(service);
    result.interaction.update =
        std::make_shared<UpdatePortAdapter>(service);
    result.interaction.state =
        std::make_shared<StatePortAdapter>(service);
    result.interaction.slice =
        std::make_shared<SlicePortAdapter>(service);
    result.interaction.model =
        std::make_shared<ModelPortAdapter>(service);
    result.dataStage =
        std::make_shared<DataStageAdapter>(service);
    result.renderBind =
        std::make_shared<RenderBindAdapter>(service);
    result.featureView =
        std::make_shared<FeatureViewAdapter>(service);
    result.overlay =
        std::make_shared<OverlayPortAdapter>(service);
    result.taskControl =
        std::make_shared<TaskControlAdapter>(service);
    return result;
}
