#pragma once
// =====================================================================
// AppService.h — MedicalVizService 渲染业务调度层
//
// 继承关系：
//   AbstractInteractiveService  — 交互接口（StdRenderContext 通过此类型持有）
//   IVisualConfigService        — 前处理接口（main.cpp 配置阶段调用）
//   IFileLoadControlService     — 文件加载状态/控制接口（与渲染解耦）
//   IDataExportService          — 数据导出接口（与渲染解耦）
//   enable_shared_from_this     — Observer 注册需要 shared_from_this()
//
// 主线职责：
//   1. 构造 / 绑定渲染上下文
//   2. 前处理配置：只写 SharedState，零 VTK 操作
//   3. 加载 / 导出：后台处理，主线程统一回调
//   4. 交互：更新状态或委托变换服务
//   5. 主线程后处理：统一重建 / 同步 / 分发回调
// =====================================================================

#include "AppInterfaces.h"
#include "AppState.h"
#include "AppStateSyncStrategy.h"
#include "InteractionComputeService.h"
#include <vtkMatrix4x4.h>
#include <map>
#include <mutex>
#include <future>

class MedicalVizService
    : public AbstractInteractiveService
    , public IVisualConfigService
    , public IFileLoadControlService
    , public IDataExportService
    , private IAppStateSyncTarget
    , public std::enable_shared_from_this<MedicalVizService>
{
public:
    // ================================================================
    // 构造 / 析构
    // 功能：绑定 DataManager、SharedInteractionState 与状态事件源三个核心对象。
    // 作用：建立 MedicalVizService 作为“状态调度中枢”的运行基础。
    // ================================================================
    MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state,
        std::shared_ptr<IStateEventSource> stateEventSource);
    ~MedicalVizService();

    // ================================================================
    // RenderContext 绑定
    // 功能：接入 renderer / renderWindow，并注册状态事件源观察回调。
    // 作用：把状态广播和渲染后处理主循环连起来。
    // ================================================================
    void SetRenderContext(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren) override;

    // ================================================================
    // IVisualConfigService — 前处理配置
    // 调用时机：SetServiceBound 之后，LoadFileAsync 之前（或之后均可）
    // 线程安全：写 SharedState（内部 mutex 保护）
    // 这一组方法只做“配置意图登记”，不直接操作 VTK 原生对象。
    // 原生依赖对象：m_sharedState。
    // ================================================================
    void SetVizMode(VizMode mode) override;
    void SetMaterial(const MaterialParams& mat) override;
    void SetOpacity(double opacity) override;
    void SetTransferFunction(const std::vector<TFNode>& nodes) override;
    void SetIsoThreshold(double val) override;
    void SetBackground(const BackgroundColor& bg) override;
    void SetSpacing(double sx, double sy, double sz) override;
    void SetWindowLevel(double ww, double wc) override;
    void SetVisualConfig(const PreInitConfig& cfg) override;

    // ================================================================
    // 文件流加载 / 重载加载入口
    // 功能：发起后台加载任务，并通过主线程回调把结果返回给 UI / 上层。
    // 作用：把文件流加载、重载加载与渲染同步解耦。
    // 原生依赖对象：m_dataManager、m_sharedState、m_activeLoadFuture。
    // ================================================================
    void LoadFileAsync(const std::string& path,
        const std::array<float, 3>& spacing = {0.02125,0.02125,0.02125},
        const std::array<float, 3>& origin = {0,0,0},
        std::function<void(bool success)> onComplete = nullptr);

    // 重载入口：从上游重建缓冲区导入体数据，命名显式带 Reload。
    bool ReloadFromBufferAsync(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool success)> onComplete = nullptr,
        DataAlgorithmKind algorithmKind = DataAlgorithmKind::None);

    // 状态查询：当前只对外暴露 File / Reload 两组状态。
    LoadState GetFileLoadState() const override;
    LoadState GetReloadLoadState() const override;

    // 请求尽力取消当前文件流加载；是否生效由后台加载流程自检决定。
    // 原生依赖对象：m_cancelFlag。
    void SetFileLoadCanceled() override;

    // ================================================================
    // IDataExportService — 导出任务
    // 功能：发起后台保存任务，并把保存完成结果延迟回到主线程。
    // 作用：把导出 I/O 与当前渲染线程解耦。
    // 原生依赖对象：m_dataManager、m_sharedState。
    // ================================================================
    void SaveTransformedDataAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) override;
    void SaveSliceImagesAsync(const std::string& path, const double angle,
        std::function<void(bool success)> onComplete = nullptr) override;

    // ================================================================
    // AbstractInteractiveService — 交互接口
    // 功能：处理切片滚动、光标联动、模型矩阵同步、显隐切换等交互入口。
    // 作用：读取 SharedState / 当前策略 / 交互计算服务，并回写状态。
    // 原生依赖对象：m_sharedState、m_currentStrategy、m_dataManager。
    // 说明：这一组是交互主线，已尽量保持“读状态 → 算结果 → 回状态”的单一路径。
    // ================================================================
    void SetSliceScrolled(int delta) override;
    void SetCursorWorldPosition(double worldPos[3], int axis = -1) override;
    std::array<double, 3> GetCursorWorld() override;
    void SetInteracting(bool val) override;
    int GetPlaneAxis(vtkActor* actor) override;
    vtkProp3D* GetMainProp() override;
    void SetModelMatrixSynced(vtkMatrix4x4* modelToWorldMatrix) override;
    void SetElementVisible(uint32_t flagBit, bool show) override;
    void SetWindowLevelAdjusted(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC) override;
    void RegisterAlgorithmPost(const std::shared_ptr<IAlgorithmPost>& post);

    std::array<double, 16> GetModelMatrix() override {
        return m_sharedState ? m_sharedState->GetModelMatrix() : AbstractInteractiveService::GetModelMatrix();
    }

    WindowLevelParams GetWindowLevel() const override {
        if (m_sharedState)
            return m_sharedState->GetWindowLevel();
        WindowLevelParams p = {};
        return p;
    }

    int GetNavigationAxis() const override {
        return m_currentStrategy ? m_currentStrategy->GetNavigationAxis() : -1;
    }

    // 模型变换扩展：统一走 InteractionComputeService 的静态计算逻辑。
    // 原生依赖对象：m_sharedState。
    void SetModelTransform(double translate[3], double rotate[3], double scale[3]);
    void SetModelTransformReset();
    void GetModelPositionFromWorld(const double worldPos[3], double modelPos[3]) const override;
    void GetWorldPositionFromModel(const double modelPos[3], double worldPos[3]) const override;

    // ================================================================
    // AbstractAppService — 主线程后处理入口
    // 功能：Timer 心跳驱动的统一后处理入口。
    // 作用：按下面优先级处理缓存清理、加载失败、数据重建和增量同步。
    // 原生依赖对象：m_strategyCache、m_currentStrategy、m_renderer、m_renderWindow。
    // 路由优先级：
    //   1. m_needsCacheClear  → ClearStrategyCache
    //   2. m_needsLoadFailed  → HandleLoadFailure
    //   3. m_needsDataRefresh → RebuildPipeline
    //   4. m_needsSync        → SyncStrategyState
    // ================================================================
    void ProcessPendingUpdates() override;

private:
    struct CallbackState {
        mutable std::mutex m_mutex;
        std::function<void(bool)> m_callback; // 当前业务方注册但尚未触发的回调
        std::function<void(bool)> m_pendingCallback; // 已由后台任务准备好、等待主线程执行的回调
        bool m_pendingResult{ false }; // 与 PendingCallback 对应的任务结果快照
        std::atomic<bool> m_hasPendingCallback{ false }; // 回调就绪位：后台线程只投递结果，主线程在心跳阶段统一执行 UI/业务回调

        void SetCallback(std::function<void(bool)> callback) {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_callback = std::move(callback);
        }

        void SetCallbackReady(bool success, std::function<void(bool)> callback = nullptr) {
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                if (callback) {
                    m_pendingCallback = std::move(callback);
                }
                else if (m_callback) {
                    callback = std::move(m_callback);
                    m_callback = nullptr;
                    m_pendingCallback = std::move(callback);
                }
                else {
                    return;
                }
                m_pendingResult = success;
                m_hasPendingCallback = true;
            }
        }

        void SetPendingCallbackExecuted() {
            std::function<void(bool)> callback;
            bool success = false;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                callback = std::move(m_pendingCallback);
                m_pendingCallback = nullptr;
                success = m_pendingResult;
            }
            if (callback) {
                callback(success);
            }
        }

        bool GetPendingCallbackConsumed() {
            return m_hasPendingCallback.exchange(false);
        }
    };

    // ================================================================
    // 渲染后处理 / 策略辅助
    // 功能：服务主线程渲染骨架，负责重建、同步、失败清理和参数快照组装。
    // 原生依赖对象：m_strategyCache、m_currentStrategy、m_renderer、m_renderWindow。
    // ================================================================
    void RebuildPipeline();
    void SyncStrategyState();
    void HandleLoadFailure();
    RenderParams GetRenderParams(UpdateFlags flags) const;
    std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode);
    void ApplyRendererBackground();
    void RequestStrategyCacheClear();
    void ClearStrategyCache();
    void CenterCursor();
    void RequestSync();
    void MergePendingFlags(UpdateFlags flags) override;
    void RequestDataRefresh() override;
    void RequestLoadFailed() override;

    // ================================================================
    // 异步任务启动辅助
    // 功能：统一文件流加载 / 重载加载任务的启动与线程托管。
    // 原生依赖对象：m_sharedState、m_activeLoadFuture。
    // ================================================================
    void StartTask(std::packaged_task<void()> task,
        bool keepActiveLoadFuture);

    // ================================================================
    // 保存完成回调队列
    // 功能：缓存导出请求的完成回调，并把后台保存结果延迟到主线程执行。
    // 原生依赖对象：m_saveCompletionCallbackState。
    // ================================================================

    // 保存当前保存请求绑定的完成回调
    void SetSaveCallback(std::function<void(bool)> callback) {
        m_saveCompletionCallbackState.SetCallback(std::move(callback));
    }

    // 将保存完成回调标记为就绪，等待主线程统一执行
    void SetSaveCallbackReady(bool success, std::function<void(bool)> callback = nullptr) {
        m_saveCompletionCallbackState.SetCallbackReady(success, std::move(callback));
    }

    // 在主线程执行待处理的保存完成回调
    void SetPendingSaveCompletionCallbackExecuted() {
        m_saveCompletionCallbackState.SetPendingCallbackExecuted();
    }

    bool ConsumeSaveCallback() {
        return m_saveCompletionCallbackState.GetPendingCallbackConsumed();
    }

    // ================================================================
    // 成员变量（按职责分组）
    // 1. 文件流加载回调状态
    // 2. 重载回调状态
    // 3. 保存完成回调状态
    // 4. 运行期原生成员对象 / 标志位
    // ================================================================
    CallbackState m_fileLoadCallbackState; // 文件流加载回调状态
    CallbackState m_reloadLoadCallbackState; // 重载回调状态
    CallbackState m_saveCompletionCallbackState; // 保存完成回调状态

    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache; // 已构建过的 Strategy 缓存，避免同模式反复创建渲染对象
    AppStateSyncStrategy m_stateSyncStrategy; // 把状态广播翻译成“重建/清理/增量同步”动作的策略对象
    AlgorithmPostRegistry m_algorithmPostRegistry; // 数据算法后处理注册表；Service 只负责调度
    DataAlgorithmKind m_pendingReloadAlgorithmKind = DataAlgorithmKind::None; // 当前 reload 完成后需要同步的算法 runtime
    LoadEventKind m_pendingLoadEventKind = LoadEventKind::None; // 当前 service 本地待消费的 load 事件来源，避免多个窗口抢同一个 Share pending 标记
    std::shared_ptr<SharedInteractionState> m_sharedState; // 运行期单一状态真源，前处理与交互都回写到这里
    std::shared_ptr<IStateEventSource> m_stateEventSource; // 状态广播源，Service 通过它订阅 SharedState 的增量事件

    std::atomic<int> m_pendingVizModeInt{ static_cast<int>(VizMode::IsoSurface) }; // 模式切换快照：前处理阶段可先写入，主线程重建管线时再一次性读取
    std::atomic<bool> m_needsDataRefresh{ false }; // 管线重建请求：DataReady/Spacing 这类结构性变化只置位，不直接重建
    std::atomic<bool> m_needsCacheClear{ false }; // 缓存清理请求：Strategy Detach 涉及渲染对象，必须推迟到主线程处理
    std::atomic<bool> m_needsLoadFailed{ false }; // 失败收敛请求：把后台失败信号汇总到主线程做统一清场

    std::future<void> m_activeLoadFuture; // 当前活动加载任务的 future，用于析构时等待后台线程结束
    mutable std::mutex m_activeLoadMutex; // 保护 m_activeLoadFuture
};
