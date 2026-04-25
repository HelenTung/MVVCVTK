#pragma once
// =====================================================================
// AppService.h — MedicalVizService 渲染业务调度层
//
// 继承关系：
//   AbstractInteractiveService  — 交互接口（StdRenderContext 通过此类型持有）
//   IVisualConfigService        — 前处理接口（main.cpp 配置阶段调用）
//   IDataLoaderService          — 数据加载接口（与渲染解耦）
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
#include "InteractionComputeService.h"
#include <vtkMatrix4x4.h>
#include <map>
#include <mutex>
#include <future>

class MedicalVizService
    : public AbstractInteractiveService
    , public IVisualConfigService
    , public IDataLoaderService
    , public IDataExportService
    , public std::enable_shared_from_this<MedicalVizService>
{
public:
    // ================================================================
    // 构造 / 析构
    // 功能：绑定 DataManager 与 SharedInteractionState 两个核心原生成员对象。
    // 作用：建立 MedicalVizService 作为“状态调度中枢”的运行基础。
    // ================================================================
    MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state);
    ~MedicalVizService();

    // ================================================================
    // RenderContext 绑定
    // 功能：接入 renderer / renderWindow，并注册 SharedState 观察回调。
    // 作用：把状态广播和渲染后处理主循环连起来。
    // ================================================================
    void SetRenderContext(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren) override;

    // ================================================================
    // IVisualConfigService — 前处理配置
    // 调用时机：SetServiceBound 之后，SetFileLoadedAsync 之前（或之后均可）
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
    // IDataLoaderService — 文件流加载 / 重载加载 / 状态查询
    // 功能：发起后台加载任务，并通过主线程回调把结果返回给 UI / 上层。
    // 作用：把文件流加载、重载加载与渲染同步解耦。
    // 原生依赖对象：m_dataManager、m_sharedState、m_cancelFlag、m_ActiveLoadFuture。
    // ================================================================
    void SetFileLoadedAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) override;

    // 重载入口：从上游重建缓冲区导入体数据，命名显式带 Reload。
    bool SetReloadFromBufferAsync(
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
    void SetLoadCanceled() override;

    // ================================================================
    // IDataExportService — 导出任务
    // 功能：发起后台保存任务，并把保存完成结果延迟回到主线程。
    // 作用：把导出 I/O 与当前渲染线程解耦。
    // 原生依赖对象：m_dataManager、m_sharedState。
    // ================================================================
    void SetTransformedDataSavedAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) override;
    void SetSliceImagesSavedAsync(const std::string& path, const double angle,
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
    void SetModelMatrixSynced(vtkMatrix4x4* mat) override;
    void SetElementVisible(uint32_t flagBit, bool show) override;
    void SetWindowLevelAdjusted(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC) override;

    std::array<double, 16> GetModelMatrix() override {
        return m_sharedState ? m_sharedState->GetModelMatrix() : AbstractInteractiveService::GetModelMatrix();
    }

    WindowLevelParams GetWindowLevel() const override {
        if (m_sharedState)
            return m_sharedState->GetWindowLevel();
        WindowLevelParams p = {};
        return p;
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
    //   1. m_needsCacheClear  → SetStrategyCacheCleared
    //   2. m_needsLoadFailed  → SetLoadFailedHandled
    //   3. m_needsDataRefresh → SetPipelineRebuilt
    //   4. m_needsSync        → SetStrategyStateSynced
    // ================================================================
    void SetPendingUpdatesProcessed() override;

private:
    // ================================================================
    // 渲染后处理 / 策略辅助
    // 功能：服务主线程渲染骨架，负责重建、同步、失败清理和参数快照组装。
    // 原生依赖对象：m_strategyCache、m_currentStrategy、m_renderer、m_renderWindow。
    // ================================================================
    void SetPipelineRebuilt();
    void SetStrategyStateSynced();
    void SetLoadFailedHandled();
    RenderParams GetRenderParams(UpdateFlags flags) const;
    std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode);
    void SetStrategyCacheClearRequested();
    void SetStrategyCacheCleared();
    void SetCursorCentered();
    void SetSyncRequested();

    // ================================================================
    // 异步任务启动辅助
    // 功能：统一文件流加载 / 重载加载任务的启动与线程托管。
    // 原生依赖对象：m_sharedState、m_cancelFlag、m_loadFuture。
    // ================================================================
    bool SetFileLoadStarted(std::function<void(bool)> callback);
    bool SetReloadLoadStarted(std::function<void(bool)> callback);
    void SetTaskStarted(std::packaged_task<void()> task,
        bool keepActiveLoadFuture);

    // ================================================================
    // 文件流加载回调队列
    // 功能：缓存文件流加载请求的完成回调，并把后台结果延迟到主线程执行。
    // 原生依赖对象：m_FileLoadCallbackMutex 及其配套状态字段。
    // ================================================================

    // 保存当前文件加载请求绑定的回调
    void SetFileLoadCallback(std::function<void(bool)> callback) {
        std::lock_guard<std::mutex> lk(m_FileLoadCallbackMutex);
        m_FileLoadCallback = std::move(callback);
    }

    // 将文件加载回调标记为完成，等待主线程统一执行
    void SetFileLoadCallbackReady(bool success, std::function<void(bool)> callback = nullptr) {
        {
            std::lock_guard<std::mutex> lk(m_FileLoadCallbackMutex);
            if (callback) {
                m_PendingFileLoadCallback = std::move(callback);
            }
            else if (m_FileLoadCallback) {
                callback = std::move(m_FileLoadCallback);
                m_FileLoadCallback = nullptr;
                m_PendingFileLoadCallback = std::move(callback);
            }
            else {
                return;
            }
            m_PendingFileLoadResult = success;
            m_HasPendingFileLoadCallback = true;
        }
    }

    // 在主线程执行待处理的文件加载回调
    void SetPendingFileLoadCallbackExecuted() {
        std::function<void(bool)> callback;
        bool success = false;
        {
            std::lock_guard<std::mutex> lk(m_FileLoadCallbackMutex);
            callback = std::move(m_PendingFileLoadCallback);
            m_PendingFileLoadCallback = nullptr;
            success = m_PendingFileLoadResult;
        }
        if (callback) callback(success);
    }

    // ================================================================
    // 重载回调队列
    // 功能：缓存重载请求的完成回调，并把后台结果延迟到主线程执行。
    // 原生依赖对象：m_ReloadLoadCallbackMutex 及其配套状态字段。
    // ================================================================

    // 保存当前重载请求绑定的回调
    void SetReloadLoadCallback(std::function<void(bool)> callback) {
        std::lock_guard<std::mutex> lk(m_ReloadLoadCallbackMutex);
        m_ReloadLoadCallback = std::move(callback);
    }

    // 将重载回调标记为完成，等待主线程统一执行
    void SetReloadLoadCallbackReady(bool success, std::function<void(bool)> callback = nullptr) {
        {
            std::lock_guard<std::mutex> lk(m_ReloadLoadCallbackMutex);
            if (callback) {
                m_PendingReloadLoadCallback = std::move(callback);
            }
            else if (m_ReloadLoadCallback) {
                callback = std::move(m_ReloadLoadCallback);
                m_ReloadLoadCallback = nullptr;
                m_PendingReloadLoadCallback = std::move(callback);
            }
            else {
                return;
            }
            m_PendingReloadLoadResult = success;
            m_HasPendingReloadLoadCallback = true;
        }
    }

    // 在主线程执行待处理的重载回调
    void SetPendingReloadLoadCallbackExecuted() {
        std::function<void(bool)> callback;
        bool success = false;
        {
            std::lock_guard<std::mutex> lk(m_ReloadLoadCallbackMutex);
            callback = std::move(m_PendingReloadLoadCallback);
            m_PendingReloadLoadCallback = nullptr;
            success = m_PendingReloadLoadResult;
        }
        if (callback) callback(success);
    }

    // ================================================================
    // 保存完成回调队列
    // 功能：缓存导出请求的完成回调，并把后台保存结果延迟到主线程执行。
    // 原生依赖对象：m_SaveCompletionCallbackMutex 及其配套状态字段。
    // ================================================================

    // 保存当前保存请求绑定的完成回调
    void SetSaveCompletionCallback(std::function<void(bool)> callback) {
        std::lock_guard<std::mutex> lk(m_SaveCompletionCallbackMutex);
        m_SaveCompletionCallback = std::move(callback);
    }

    // 将保存完成回调标记为就绪，等待主线程统一执行
    void SetSaveCompletionCallbackReady(bool success, std::function<void(bool)> callback = nullptr) {
        {
            std::lock_guard<std::mutex> lk(m_SaveCompletionCallbackMutex);
            if (callback) {
                m_PendingSaveCompletionCallback = std::move(callback);
            }
            else if (m_SaveCompletionCallback) {
                callback = std::move(m_SaveCompletionCallback);
                m_SaveCompletionCallback = nullptr;
                m_PendingSaveCompletionCallback = std::move(callback);
            }
            else {
                return;
            }
            m_PendingSaveCompletionResult = success;
            m_HasPendingSaveCompletionCallback = true;
        }
    }

    // 在主线程执行待处理的保存完成回调
    void SetPendingSaveCompletionCallbackExecuted() {
        std::function<void(bool)> callback;
        bool success = false;
        {
            std::lock_guard<std::mutex> lk(m_SaveCompletionCallbackMutex);
            callback = std::move(m_PendingSaveCompletionCallback);
            m_PendingSaveCompletionCallback = nullptr;
            success = m_PendingSaveCompletionResult;
        }
        if (callback) callback(success);
    }

    // ================================================================
    // 成员变量（按职责分组）
    // 1. 文件流加载回调状态
    // 2. 重载回调状态
    // 3. 保存完成回调状态
    // 4. 运行期原生成员对象 / 标志位
    // ================================================================
    mutable std::mutex m_FileLoadCallbackMutex;              // 保护文件加载回调状态
    std::function<void(bool)> m_FileLoadCallback;            // 当前文件加载请求绑定的回调
    std::function<void(bool)> m_PendingFileLoadCallback;     // 待主线程执行的文件加载回调
    bool m_PendingFileLoadResult{ false };                   // 待执行文件加载回调对应的结果
    std::atomic<bool> m_HasPendingFileLoadCallback{ false }; // 是否存在待执行文件加载回调

    mutable std::mutex m_ReloadLoadCallbackMutex;              // 保护重载回调状态
    std::function<void(bool)> m_ReloadLoadCallback;            // 当前重载请求绑定的回调
    std::function<void(bool)> m_PendingReloadLoadCallback;     // 待主线程执行的重载回调
    bool m_PendingReloadLoadResult{ false };                   // 待执行重载回调对应的结果
    std::atomic<bool> m_HasPendingReloadLoadCallback{ false }; // 是否存在待执行重载回调

    mutable std::mutex m_SaveCompletionCallbackMutex;              // 保护保存完成回调状态
    std::function<void(bool)> m_SaveCompletionCallback;            // 当前保存请求绑定的完成回调
    std::function<void(bool)> m_PendingSaveCompletionCallback;     // 待主线程执行的保存完成回调
    bool m_PendingSaveCompletionResult{ false };                   // 待执行保存完成回调对应的结果
    std::atomic<bool> m_HasPendingSaveCompletionCallback{ false }; // 是否存在待执行保存完成回调

    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;
    std::shared_ptr<SharedInteractionState> m_sharedState;

    std::atomic<int> m_pendingVizModeInt{ static_cast<int>(VizMode::IsoSurface) };
    std::atomic<bool> m_needsDataRefresh{ false };
    std::atomic<bool> m_needsCacheClear{ false };
    std::atomic<bool> m_needsLoadFailed{ false };
    std::shared_ptr<std::atomic<bool>> m_cancelFlag; // 尽力取消标记

    std::future<void> m_ActiveLoadFuture; // 当前活动加载任务的 future，用于析构时等待后台线程结束
    mutable std::mutex m_ActiveLoadMutex; // 保护 m_ActiveLoadFuture
};