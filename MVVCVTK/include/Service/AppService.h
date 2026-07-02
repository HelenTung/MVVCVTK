#pragma once
// =====================================================================
// AppService.h — MedicalVizService 主线程编排层
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
//   3. 加载 / 导出：委托任务服务构建后台任务，主线程统一回调
//   4. 交互：读取状态、计算结果、回写 SharedState
//   5. 主线程编排：消费 pending 事件，按顺序触发重建 / 同步 / 回调
// =====================================================================

#include "AppInterfaces.h"
#include "AppState.h"
#include "AppStateSyncStrategy.h"
#include "InteractionComputeService.h"
#include <vtkMatrix4x4.h>
#include <atomic>
#include <map>
#include <mutex>
#include <future>

class AppDataExportTaskService;
class AppDataLoadTaskService;

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
        std::function<void(bool success)> onComplete = nullptr);

    // 状态查询：当前只对外暴露 File / Reload 两组状态。
    LoadState GetFileLoadState() const override;
    LoadState GetReloadLoadState() const override;

    // 请求尽力取消当前文件流加载；是否生效由后台加载流程自检决定。
    // 原生依赖对象：m_cancelFlag。
    void CancelFileLoad() override;

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
    void ScrollSlice(int delta) override;
    void SetCursorWorldPosition(double worldPos[3], int axis = -1) override;
    std::array<double, 3> GetCursorWorld() override;
    void SetInteracting(bool val) override;
    int GetPlaneAxis(vtkActor* actor) override;
    vtkProp3D* GetMainProp() override;
    void SyncModelMatrix(vtkMatrix4x4* modelToWorldMatrix) override;
    void SetElementVisible(uint32_t flagBit, bool show) override;
    void AdjustWindowLevel(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC) override;

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
    // AbstractAppService — 主线程编排入口
    // 功能：Timer 心跳驱动，只编排各类 pending 状态的消费顺序。
    // 作用：按下面优先级处理缓存清理、加载失败、数据重建和增量同步；具体任务构建已下沉到专门 service。
    // 原生依赖对象：m_strategyCache、m_currentStrategy、m_renderer、m_renderWindow。
    // 路由优先级：
    //   1. m_needsCacheClear  → ClearStrategyCache
    //   2. m_needsLoadFailed  → HandleLoadFailure
    //   3. m_needsDataRefresh → RebuildPipeline
    //   4. m_needsSync        → SyncStrategyState
    // ================================================================
    void ProcessPendingUpdates() override;

private:
    // ================================================================
    // 主线程编排辅助
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
    // 功能：只负责启动已构建好的后台任务和保存加载 future，不构建具体业务任务。
    // 原生依赖对象：m_sharedState、m_activeLoadFuture。
    // ================================================================
    void StartTask(std::packaged_task<void()> task,
        bool keepActiveLoadFuture);

    // ================================================================
    // 成员变量（按职责分组）
    // 1. 加载 / 导出任务服务
    // 2. 运行期原生成员对象 / 标志位
    // ================================================================
    std::shared_ptr<AppDataLoadTaskService> m_dataLoadTaskService; // 加载任务构建和加载回调状态
    std::shared_ptr<AppDataExportTaskService> m_dataExportTaskService; // 导出任务构建和保存回调状态
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache; // 已构建过的 Strategy 缓存，避免同模式反复创建渲染对象
    AppStateSyncStrategy m_stateSyncStrategy; // 把状态广播翻译成“重建/清理/增量同步”动作的策略对象
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
