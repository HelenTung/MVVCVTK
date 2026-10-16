#pragma once

#include "App/Services/AppServiceFactory.h"
#include "App/Services/PrimaryDataActivation.h"
#include "App/Services/Internal/AppTaskExecutor.h"
#include "App/Services/AppPorts.h"
#include "App/Services/FeatureViewService.h"
#include "AppDataExportTaskService.h"
#include "AppDataLoadTaskService.h"
#include "AppState.h"
#include "DataConverters.h"
#include "DataManager.h"
#include "Data/DataPayloads.h"
#include "InteractionComputeService.h"
#include "Interaction/InteractionPorts.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Contracts/RenderBindPort.h"
#include "Render/Contracts/RenderEffect.h"
#include "Render/Support/RenderFrameLifetime.h"
#include "Render/Contracts/RenderStrategyFactory.h"
#include "Render/Contracts/VisualStrategy.h"
#include "Render/Internal/RulerOverlay.h"
#include <vtkActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkMatrix4x4.h>
#include <vtkMatrix3x3.h>
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
    TaskAdmissionResult StartDataActivation(const DataRevisionRef& ref, DataBindingRevision expected,
        std::function<void(bool)> onComplete);
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
    bool SetModelMatrix(vtkMatrix4x4* modelToWorldMatrix);
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
    bool SendPendingUpdates();
    void SendCompletions();
    bool SendReloadUpdate();
    bool GetDirty() const;
    bool GetPointVisible(const std::array<double,3>& world) const;
    void SetDirty();
    bool ResetDirty();
    bool SetInteractionPhase();
    void SetRenderComplete(std::uint64_t durationUs) noexcept;
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
    DataStageStatus StartDataStage(
        const VtkRenderInputSnapshot& snapshot,
        std::uint64_t transactionRevision,const std::optional<RenderEffectChange>& effect={});
    DataStageStatus SetDataStageReady(
        const VtkRenderInputSnapshot& snapshot,
        std::uint64_t transactionRevision);
    DataStageStatus GetDataStageStatus(
        std::uint64_t transactionRevision) const;
    RenderEffectFailure GetDataStageFailure(std::uint64_t revision) const {
        return m_dataStage&&m_dataStage->transactionRevision==revision&&m_dataStage->nextStrategy
            ?m_dataStage->nextStrategy->GetRenderEffectState().failureReason:RenderEffectFailure::None;
    }
    bool SetViewStage(
        const VtkRenderInputSnapshot& snapshot,
        std::uint64_t transactionRevision);
    bool ResetViewStage(std::uint64_t transactionRevision);
    bool ClearDataStage(std::uint64_t transactionRevision);
    void SetDataStageComplete(
        std::uint64_t transactionRevision) noexcept;
    bool StartViewUpdate();
    bool SetViewUpdateCommit(
        bool isCommitted,
        UpdateFlags& pendingFlags);
    void SendViewUpdateFlags(UpdateFlags flags) noexcept;
    bool SendSessionUpdate(const AppSessionUpdate& update);
    bool GetCanClearRenderInput() const;
    bool ClearRenderInput(const DataEntityId&) noexcept;
    bool SetPrimaryData(
        const DataRevisionRef& dataRevision,
        DataBindingRevision expectedBindingRevision);
    bool SetDataTaskStopping();
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
        std::optional<RenderEffectChange> effectChange;
        std::shared_ptr<RenderEffect> oldEffect,nextEffect;
        std::array<double,16> oldModelMatrix{};
        VtkRenderInputSnapshot oldSnapshot;
        VtkRenderInputSnapshot nextSnapshot;
        RulerInput oldRulerInput;
        std::shared_ptr<AbstractVisualStrategy> oldStrategy;
        std::shared_ptr<AbstractVisualStrategy> nextStrategy;
        std::optional<VizMode> oldMode;
        CameraState oldCamera;
        DataReadyState readyState;
        RenderParams nextParams;
        WindowLevelParams autoWindowLevel;
        VizMode mode = VizMode::Volume;
        bool isEffectAttached = false;
        bool isRendererAttached = false;
        bool isPrepared = false;
        bool hasDefaultTransfer = false;
        bool hasDefaultIso = false;
        double previousIso = 0.0;
        bool isCommitted = false;
        std::uint64_t transactionRevision = 0;
        std::uint64_t transitionRevision = 0;
        bool isRefresh = false;
        DataStageStatus status = DataStageStatus::Idle;
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
        std::uint64_t transactionRevision = 0;
    };

    struct PendingLoadCommit final {
        LoadEventKind loadKind = LoadEventKind::None;
        std::uint64_t transactionRevision = 0;
        VtkImageGridSnapshot pending;
        std::function<void(bool)> callback;
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
    void SetTaskResult(ActiveTask task, bool isSuccess);
    void SetLoadResult(ActiveTask task, bool isSuccess);
    void SendLoadCommit();
    void SetCompletion(bool isSuccess, std::function<void(bool)> callback);
    bool CreateLoadNotice(
        LoadEventKind loadEventKind,
        bool isSucceeded,
        bool isStateSet = false);
    bool RemoveLoadNotice(LoadNotice& loadNotice);
    bool GetOwnedLoad(LoadEventKind loadEventKind) const;
    bool SetOwnedLoad(LoadEventKind loadEventKind);
    bool ResetOwnedLoad(LoadEventKind loadEventKind);
    void ClearStoppedOwnedLoad();
    bool SetPreparingLoadReplaced(
        LoadEventKind loadEventKind,
        bool& isReplaced);
    VtkRenderInputSnapshot GetPrimaryRenderInput() const;
    DataStageStatus BuildPipeline();
    bool GetStageCurrent(const DataStage& stage) const;
    static bool GetSameInput(
        const VtkRenderInputSnapshot& left,
        const VtkRenderInputSnapshot& right)
    {
        if (!left || !right || !left->data || !right->data
            || left->data->self != right->data->self
            || left->binding.has_value() != right->binding.has_value()) return false;
        return !left->binding
            || (left->binding->name == right->binding->name
                && left->binding->revision == right->binding->revision
                && left->binding->target == right->binding->target);
    }
    std::optional<VolumeTransferFunction> GetDefaultVolumeTransfer(
        const VtkRenderInputSnapshot& snapshot);
    bool GetVolumeTransferValid(
        const VolumeTransferFunction& function) const;
    bool GetTransferRangeValid(
        const VolumeTransferFunction& function,
        const std::array<double, 2>& scalarRange) const;
    double GetRenderRate(bool isInteracting) const noexcept;
    VolumeQuality GetTargetQuality() const;
    bool SetProductState();
    void SetRulerInput(const RenderParams& params, UpdateFlags flags);
    bool SetStrategyState();
    void ClearLoadFail(LoadEventKind loadEventKind);
    RenderParams GetRenderParams(UpdateFlags flags) const;
    std::shared_ptr<AbstractVisualStrategy> CreateStrategy(VizMode mode);
    CameraState GetCameraState() const;
    bool SetCameraState(const CameraState& state);
    bool SetModeCamera(VizMode mode, const VtkRenderInputSnapshot& snapshot);
    bool SetCameraCenter(
        const std::array<double, 16>& modelToWorld,
        const VtkRenderInputSnapshot& snapshot);
    void SetRendererBg();
    void ClearStrategies();
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
    std::uint64_t m_renderNeededProductRevision = 0;
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
    const std::function<void()> m_onWorkAvailable;
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
    VtkRenderInputSnapshot m_renderSnapshot;
    std::array<double,16> m_renderModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    RulerOverlay m_ruler;
    RulerInput m_rulerInput;
    RulerParams m_rulerParams;
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
    std::shared_ptr<RenderStrategyServices> m_renderServices;
    bool m_ownsRenderResources = false;
    std::function<LoadCommitResult(
        LoadEventKind,
        std::uint64_t,
        const VtkImageGridSnapshot&)> m_setLoadCommit;
    std::function<LoadCommitResult(
        std::uint64_t,
        LoadCommitFailure)> m_setLoadCancelled;
    std::optional<DataStage> m_dataStage;
    // 显示请求代数独立于 LoadCommitCoordinator 的事务号；模式 ABA 也会失效。
    mutable std::mutex m_transitionMutex;
    std::uint64_t m_transitionRevision = 0;
    bool m_hasVisualConfig = false;
    bool m_hasExplicitIso = false; // m_transitionMutex 保护阈值来源和默认值版本。
    DataRevisionRef m_isoSource;
    std::atomic<bool> m_hasTransitionFailure{false};
    std::optional<PendingLoadCommit> m_pendingLoadCommit;
    std::uint64_t m_nextLoadTransactionRevision = 0;
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
