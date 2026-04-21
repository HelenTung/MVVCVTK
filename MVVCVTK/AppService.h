#pragma once
// =====================================================================
// AppService.h — MedicalVizService 渲染业务调度层
//
// 继承关系：
//   AbstractInteractiveService  — 交互接口（StdRenderContext 通过此类型持有）
//   IVisualConfigService             — 前处理接口（main.cpp 配置阶段调用）
//   IDataLoaderService          — 数据加载接口（与渲染解耦）
//   enable_shared_from_this     — Observer 注册需要 shared_from_this()
//
// 三阶段职责：
//   【前处理】  SetXxx / SetVisualConfig：只写 SharedState，零 VTK 操作
//   【后处理-重建】 SetPendingUpdatesProcessed → PostData_RebuildPipeline
//   【后处理-同步】 SetPendingUpdatesProcessed → PostData_SyncStateToStrategy
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
    MedicalVizService(std::shared_ptr<AbstractDataManager>    dataMgr,
        std::shared_ptr<SharedInteractionState> state);
    ~MedicalVizService();
    void SetRenderContext(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer>     ren) override;

    // ================================================================
    // IVisualConfigService — 前处理接口
    // 调用时机：SetServiceBound 之后，SetFileLoadedAsync 之前（或之后均可）
    // 线程安全：写 SharedState（内部 mutex 保护）
    // ================================================================
    void SetVizMode(VizMode mode)                               override;
    void SetMaterial(const MaterialParams& mat)                 override;
    void SetOpacity(double opacity)                             override;
    void SetTransferFunction(const std::vector<TFNode>& nodes)  override;
    void SetIsoThreshold(double val)                            override;
    void SetBackground(const BackgroundColor& bg)               override;
    void SetWindowLevel(double ww, double wc)                   override;
    void SetVisualConfig(const PreInitConfig& cfg)              override; // 批量提交

    // ================================================================
    // IDataLoaderService — 数据加载接口
    // onComplete 在后台线程执行，只允许操作 SharedState
    // ================================================================
    void SetFileLoadedAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) override;
    
    // 重建注入异步接口：将 SetFromBuffer 的耗时操作投递到后台线程
    bool SetFromBufferAsync(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool success)> onComplete = nullptr);

    LoadState GetLoadState() const override;

	// ================================================================
	// IDataExportService — 数据导出接口
	// 线程安全：读取 SharedState（内部 mutex 保护），不操作 VTK
	// ================================================================
    void SetTransformedDataSavedAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) override;
    
    // 尽力取消：设标记，加载函数内部自检后提前退出
    void SetLoadCanceled() override;

    // ================================================================
    // AbstractAppService — 后处理入口（主线程 Timer 驱动）
    // 路由优先级：
    //   1. m_needsCacheClear  → ExecuteClearStrategyCache
    //   2. m_needsLoadFailed  → PostData_HandleLoadFailed  
    //   3. m_needsDataRefresh → PostData_RebuildPipeline
    //   4. m_needsSync        → PostData_SyncStateToStrategy
    // ================================================================
    void SetPendingUpdatesProcessed() override;

    // ================================================================
    // AbstractInteractiveService — 交互接口
    // ================================================================
    void SetSliceScrolled(int delta)                                     override;
    void SetCursorWorldPosition(double worldPos[3], int axis = -1) override;
    std::array<double, 3> GetCursorWorld()                          override;
    void SetInteracting(bool val)                                   override;
    int  GetPlaneAxis(vtkActor* actor)                              override;
    vtkProp3D* GetMainProp()                                        override;
    void SetModelMatrixSynced(vtkMatrix4x4* mat)                    override;
    void SetElementVisible(uint32_t flagBit, bool show)             override;

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

    // 模型变换扩展（委托 VolumeTransformService）
    void TransformModel(double translate[3], double rotate[3], double scale[3]);
    void ResetModelTransform();
    void GetModelPositionFromWorld(const double worldPos[3], double modelPos[3]) const override;
    void GetWorldPositionFromModel(const double modelPos[3], double worldPos[3]) const override;

private:
    // ── 后处理路径 A：DataReady → 重建 VTK 渲染管线（仅主线程）
    void PostData_RebuildPipeline();

    // ── 后处理路径 B：普通事件 → 增量同步参数到 Strategy（仅主线程）
    void PostData_SyncStateToStrategy();

    // ── 后处理路径 C：LoadFailed → 清理状态 + 显示占位符（仅主线程）
    void PostData_HandleLoadFailed();

    // ── 构建 RenderParams（只读 SharedState，按 flags 精确读取）
    RenderParams BuildRenderParams(UpdateFlags flags) const;

    // ── Strategy 缓存管理
    std::shared_ptr<AbstractVisualStrategy> GetOrCreateStrategy(VizMode mode);

    // ── 仅设标记，主线程统一执行 Detach   （不在后台线程调 VTK）
    void RequestClearStrategyCache();   // 后台线程安全：只设标记
    void ExecuteClearStrategyCache();   // 主线程：真正执行 Detach + clear

    void ResetCursorToCenter();
    void MarkNeedsSync();

    // ————— 异步加载回调管理（SetFileLoadedAsync / SetFromBuffer）——————————
    void ExecutePendingLoadCallback(bool success) {
        std::function<void(bool)> cb;
		{
			std::lock_guard<std::mutex> lk(m_LoadCallbackMutex);
			cb = std::move(m_LoadCallbackFunc);
            m_LoadCallbackFunc = nullptr;
		}
        if (cb) cb(success);
    }

    mutable std::mutex m_LoadCallbackMutex;
    std::function<void(bool)> m_LoadCallbackFunc;


    // ————— 异步保存回调管理（SetTransformedDataSaved）——————————
    void ExecutePendingSaveCallback(bool success) {
        std::function<void(bool)> cb;
        {
            std::lock_guard<std::mutex> lk(m_SaveCallbackMutex);
            cb = std::move(m_SaveCallbackFunc);
            m_SaveCallbackFunc = nullptr;
        }
        if (cb) cb(success);
    }

    mutable std::mutex        m_SaveCallbackMutex;
    std::function<void(bool)> m_SaveCallbackFunc;
    std::atomic<bool>         m_needsSaveTrigger{ false }; // 是一个触发信号，放进去数据就准备就绪后
    std::atomic<bool>         m_lastSaveResult{ false };// 存储后台任务执行的最终结果

    // ── 成员 ────────────────────────────────────────────────────
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;
    std::shared_ptr<SharedInteractionState>    m_sharedState;
    std::unique_ptr<VolumeTransformService>    m_transformService;

    // 原子标记
    std::atomic<int>  m_pendingVizModeInt{ static_cast<int>(VizMode::IsoSurface) };
    std::atomic<bool> m_needsDataRefresh{ false };
    std::atomic<bool> m_needsCacheClear{ false };
    std::atomic<bool> m_needsLoadFailed{ false };   // 
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;   // 尽力取消标记

    // 加载线程 future（用于析构时 join，避免 detach UB）
    std::future<void>  m_loadFuture;                //
    mutable std::mutex m_loadMutex;                 //保护 m_loadFuture
};