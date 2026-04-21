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
#include "DataConverters.h"
#include "VolumeTransformService.h"
#include <vtkTable.h>
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
    // ================================================================
    MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state);
    ~MedicalVizService();

    // ================================================================
    // RenderContext 绑定
    // ================================================================
    void SetRenderContext(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren) override;

    // ================================================================
    // IVisualConfigService — 前处理配置
    // 调用时机：SetServiceBound 之后，SetFileLoadedAsync 之前（或之后均可）
    // 线程安全：写 SharedState（内部 mutex 保护）
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
    // 数据加载 / 数据导出
    // onComplete 由主线程延迟执行，只允许操作状态或 UI
    // ================================================================
    void SetFileLoadedAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) override;

    bool SetFromBufferAsync(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool success)> onComplete = nullptr);

    LoadState GetLoadState() const override;
    void SetTransformedDataSavedAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) override;
    void SetLoadCanceled() override;

    // ================================================================
    // AbstractInteractiveService — 交互接口
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

    // 模型变换扩展（委托 VolumeTransformService，统一走 Set/Get 语义）
    void SetModelTransform(double translate[3], double rotate[3], double scale[3]);
    void SetModelTransformReset();
    void GetModelPositionFromWorld(const double worldPos[3], double modelPos[3]) const override;
    void GetWorldPositionFromModel(const double modelPos[3], double worldPos[3]) const override;

    // ================================================================
    // AbstractAppService — 主线程后处理入口
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
    // 异步回调辅助
    // ================================================================
    // 保存当前加载请求绑定的回调
    void SetLoadCallback(std::function<void(bool)> callback) {
        std::lock_guard<std::mutex> lk(m_LoadCallbackMutex);
        m_LoadCallback = std::move(callback);
    }

    // 将加载回调标记为完成，等待主线程统一执行
    void SetLoadCallbackReady(bool success, std::function<void(bool)> callback = nullptr) {
        {
            std::lock_guard<std::mutex> lk(m_LoadCallbackMutex);
            if (callback) {
                m_PendingLoadCallback = std::move(callback);
            }
            else if (m_LoadCallback) {
                callback = std::move(m_LoadCallback);
                m_LoadCallback = nullptr;
                m_PendingLoadCallback = std::move(callback);
            }
            else {
                return;
            }
            m_PendingLoadResult = success;
            m_HasPendingLoadCallback = true;
        }
    }

    // 在主线程执行待处理的加载回调
    void SetPendingLoadCallbackExecuted() {
        std::function<void(bool)> callback;
        bool success = false;
        {
            std::lock_guard<std::mutex> lk(m_LoadCallbackMutex);
            callback = std::move(m_PendingLoadCallback);
            m_PendingLoadCallback = nullptr;
            success = m_PendingLoadResult;
        }
        if (callback) callback(success);
    }

    // 保存当前保存请求绑定的回调
    void SetSaveCallback(std::function<void(bool)> callback) {
        std::lock_guard<std::mutex> lk(m_SaveCallbackMutex);
        m_SaveCallback = std::move(callback);
    }

    // 将保存回调标记为完成，等待主线程统一执行
    void SetSaveCallbackReady(bool success, std::function<void(bool)> callback = nullptr) {
        {
            std::lock_guard<std::mutex> lk(m_SaveCallbackMutex);
            if (callback) {
                m_PendingSaveCallback = std::move(callback);
            }
            else if (m_SaveCallback) {
                callback = std::move(m_SaveCallback);
                m_SaveCallback = nullptr;
                m_PendingSaveCallback = std::move(callback);
            }
            else {
                return;
            }
            m_PendingSaveResult = success;
            m_HasPendingSaveCallback = true;
        }
    }

    // 在主线程执行待处理的保存回调
    void SetPendingSaveCallbackExecuted() {
        std::function<void(bool)> callback;
        bool success = false;
        {
            std::lock_guard<std::mutex> lk(m_SaveCallbackMutex);
            callback = std::move(m_PendingSaveCallback);
            m_PendingSaveCallback = nullptr;
            success = m_PendingSaveResult;
        }
        if (callback) callback(success);
    }

    // ================================================================
    // 成员变量
    // ================================================================
    mutable std::mutex m_LoadCallbackMutex;              // 保护加载回调状态
    std::function<void(bool)> m_LoadCallback;            // 当前加载请求绑定的回调
    std::function<void(bool)> m_PendingLoadCallback;     // 待主线程执行的加载回调
    bool m_PendingLoadResult{ false };                   // 待执行加载回调对应的结果
    std::atomic<bool> m_HasPendingLoadCallback{ false }; // 是否存在待执行加载回调

    mutable std::mutex m_SaveCallbackMutex;              // 保护保存回调状态
    std::function<void(bool)> m_SaveCallback;            // 当前保存请求绑定的回调
    std::function<void(bool)> m_PendingSaveCallback;     // 待主线程执行的保存回调
    bool m_PendingSaveResult{ false };                   // 待执行保存回调对应的结果
    std::atomic<bool> m_HasPendingSaveCallback{ false }; // 是否存在待执行保存回调

    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;
    std::shared_ptr<SharedInteractionState> m_sharedState;
    std::unique_ptr<VolumeTransformService> m_transformService;

    std::atomic<int> m_pendingVizModeInt{ static_cast<int>(VizMode::IsoSurface) };
    std::atomic<bool> m_needsDataRefresh{ false };
    std::atomic<bool> m_needsCacheClear{ false };
    std::atomic<bool> m_needsLoadFailed{ false };
    std::shared_ptr<std::atomic<bool>> m_cancelFlag; // 尽力取消标记

    std::future<void> m_loadFuture; // 加载线程 future（用于析构时等待）
    mutable std::mutex m_loadMutex; // 保护 m_loadFuture
};