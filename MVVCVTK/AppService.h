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

class MedicalVizService
    : public AbstractInteractiveService
    , public IPreInitService
    , public IDataLoaderService
    , public std::enable_shared_from_this<MedicalVizService>
{
public:
    MedicalVizService(std::shared_ptr<AbstractDataManager>    dataMgr,
        std::shared_ptr<SharedInteractionState> state);

    // 由 BindService 触发（shared_ptr 完全构建后，shared_from_this() 安全）
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
    void PreInit_CommitConfig(const PreInitConfig& cfg)                 override; // 批量提交

    // ================================================================
    // IDataLoaderService — 数据加载接口
    // onComplete 在后台线程执行，只允许操作 SharedState
    // ================================================================
    void LoadFileAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) override;

    // ================================================================
    // AbstractAppService — 后处理入口（主线程 Timer 驱动）
    // 路径 A：m_needsDataRefresh=true → PostData_RebuildPipeline
    // 路径 B：m_needsSync=true        → PostData_SyncStateToStrategy
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

    // ── ���处理路径 B：普通事件 → 增量同步参数到 Strategy（仅主线程）
    void PostData_SyncStateToStrategy();

    // ── 构建 RenderParams（只读 SharedState，无 VTK 操作）
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

    std::atomic<int>  m_pendingVizModeInt{ static_cast<int>(VizMode::IsoSurface) };
    std::atomic<bool> m_needsDataRefresh{ false };
    std::atomic<bool> m_needsCacheClear{ false }; // 新增：安全延迟清除缓存
};