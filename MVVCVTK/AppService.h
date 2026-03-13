#pragma once
// =====================================================================
// AppService.h — MedicalVizService 渲染业务调度层
//
// 继承关系：
//   AbstractInteractiveService  — 交互接口（StdRenderContext 通过此类型持有）
//   IPreInitService             — 前处理接口（main.cpp 配置阶段调用）
//   IDataLoaderService          — 数据加载接口（与渲染解耦）
//   enable_shared_from_this     — Observer 注册需要 shared_from_this()
//
// 三阶段职责：
//   【前处理】  PreInit_* / PreInit_CommitConfig：只写 SharedState，零 VTK 操作
//   【后处理-重建】 ProcessPendingUpdates → PostData_RebuildPipeline
//   【后处理-同步】 ProcessPendingUpdates → PostData_SyncStateToStrategy
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
    , public IPreInitService
    , public IDataLoaderService
    , public std::enable_shared_from_this<MedicalVizService>
{
public:
    MedicalVizService(std::shared_ptr<AbstractDataManager>    dataMgr,
        std::shared_ptr<SharedInteractionState> state);
    ~MedicalVizService();
    void Initialize(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer>     ren) override;

    // ================================================================
    // IPreInitService — 前处理接口
    // 调用时机：BindService 之后，LoadFileAsync 之前（或之后均可）
    // 线程安全：写 SharedState（内部 mutex 保护）
    // ================================================================
    void PreInit_SetVizMode(VizMode mode)                               override;
    void PreInit_SetMaterial(const MaterialParams& mat)                 override;
    void PreInit_SetOpacity(double opacity)                             override;
    void PreInit_SetTransferFunction(const std::vector<TFNode>& nodes)  override;
    void PreInit_SetIsoThreshold(double val)                            override;
    void PreInit_SetBackground(const BackgroundColor& bg)               override;
    void PreInit_SetWindowLevel(double ww, double wc)                   override;
    void PreInit_CommitConfig(const PreInitConfig& cfg)                 override; // 批量提交

    // ================================================================
    // IDataLoaderService — 数据加载接口
    // onComplete 在后台线程执行，只允许操作 SharedState
    // ================================================================
    void LoadFileAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) override;
    
    // 重建注入异步接口：将 SetFromBuffer 的耗时操作投递到后台线程
    void SetFromBufferAsync(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool success)> onComplete = nullptr);

    LoadState GetLoadState() const override;

    // 尽力取消：设标记，加载函数内部自检后提前退出
    void CancelLoad() override;

    // ================================================================
    // AbstractAppService — 后处理入口（主线程 Timer 驱动）
    // 路由优先级：
    //   1. m_needsCacheClear  → ExecuteClearStrategyCache
    //   2. m_needsLoadFailed  → PostData_HandleLoadFailed  
    //   3. m_needsDataRefresh → PostData_RebuildPipeline
    //   4. m_needsSync        → PostData_SyncStateToStrategy
    // ================================================================
    void ProcessPendingUpdates() override;

    // ================================================================
    // AbstractInteractiveService — 交互接口
    // ================================================================
    void UpdateInteraction(int delta)                               override;
    void SyncCursorToWorldPosition(double worldPos[3], int axis = -1) override;
    std::array<int, 3> GetCursorPosition()                         override;
    void SetInteracting(bool val)                                   override;
    int  GetPlaneAxis(vtkActor* actor)                              override;
    vtkProp3D* GetMainProp()                                        override;
    void SyncModelMatrix(vtkMatrix4x4* mat)                         override;
    void SetElementVisible(uint32_t flagBit, bool show)             override;
    void AdjustWindowLevel(double deltaWW, double deltaWC)          override;

    // 模型变换扩展（委托 VolumeTransformService）
    void TransformModel(double translate[3], double rotate[3], double scale[3]);
    void ResetModelTransform();
    void WorldToModel(const double worldPos[3], double modelPos[3]);
    void ModelToWorld(const double modelPos[3], double worldPos[3]);

    // 供 main.cpp / 回调安全访问 SharedState
    std::shared_ptr<SharedInteractionState> GetSharedState() const { return m_sharedState; }

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

    // ── 仅设标记，主线程统一执行 Detach（修复：不在后台线程调 VTK）
    void RequestClearStrategyCache();   // 后台线程安全：只设标记
    void ExecuteClearStrategyCache();   // 主线程：真正执行 Detach + clear

    void ResetCursorToCenter();
    void MarkNeedsSync();

    // ── 成员 ────────────────────────────────────────────���────────
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