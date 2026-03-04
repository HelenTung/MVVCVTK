#pragma once
// =====================================================================
// AppService.h  — MedicalVizService 渲染业务调度层
//
// MedicalVizService 同时实现：
//   AbstractInteractiveService — 原有交互接口（供 StdRenderContext 调用）
//   IPreInitService            — 【新增】前处理接口（供 main.cpp 调用）
//
// 三阶段职责：
//   【前处理】  PreInit_* 系列：只写 SharedState，零 VTK 操作
//   【后处理-重建】 ProcessPendingUpdates 路径 A：重建 VTK 管线
//   【后处理-同步】 ProcessPendingUpdates 路径 B：增量同步参数
//
// =====================================================================

#include "AppInterfaces.h"
#include "AppState.h"
#include "DataConverters.h"
#include "VolumeTransformService.h"
#include <vtkTable.h>
#include <map>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────
// MedicalVizService
//
// 继承关系：
//   AbstractInteractiveService  — 交互接口（StdRenderContext 通过此类型持有）
//   IPreInitService             — 前处理接口（main.cpp 通过此类型配置）
//   enable_shared_from_this     — Observer 注册需要 shared_from_this()
// ─────────────────────────────────────────────────────────────────────
class MedicalVizService
    : public AbstractInteractiveService
    , public IPreInitService
    , public std::enable_shared_from_this<MedicalVizService>
{
private:
    // 策略缓存（主线程独占读写，Timer 保证单线程，无需加锁）
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;

    std::shared_ptr<SharedInteractionState> m_sharedState;

    // 坐标变换子服务（依赖注入，单一职责）
    std::unique_ptr<VolumeTransformService> m_transformService;

    // 前处理意图（atomic 保证后台/主线程可见性）
    std::atomic<int>  m_pendingVizModeInt{ static_cast<int>(VizMode::IsoSurface) };

    // DataReady 触发标记（后台线程写 true，主线程读并 exchange(false)）
    std::atomic<bool> m_needsDataRefresh{ false };

public:
    MedicalVizService(std::shared_ptr<AbstractDataManager>    dataMgr,
        std::shared_ptr<SharedInteractionState> state);

    // 由 BindService 触发，shared_ptr 完全构建后才能调用（shared_from_this 前提）
    void Initialize(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer>    ren) override;

    // ================================================================
    // 【IPreInitService 实现】前处理接口
    // 调用时机：BindService 之后，LoadFileAsync 之前（或之后均可）
    // 线程安全：写 SharedState（内部 mutex 保护）
    // ================================================================
    void PreInit_SetVizMode(VizMode mode)                              override;
    void PreInit_SetLuxParams(double ambient, double diffuse,
        double specular, double power, bool shadeOn = false)           override;
    void PreInit_SetOpacity(double opacity)                            override;
    void PreInit_SetTransferFunction(const std::vector<TFNode>& nodes) override;
    void PreInit_SetIsoThreshold(double val)                           override;

    // ================================================================
    // 【异步数据加载】
    // 只需调用一次，加载完成后 NotifyDataReady 广播给所有已注册 Service。
    // onComplete 在后台线程执行，只允许操作 SharedState。
    // ================================================================
    void LoadFileAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr);

    // ================================================================
    // 【AbstractAppService 实现】后处理入口（主线程 Timer 驱动）
    // 路径 A：m_needsDataRefresh=true → PostData_RebuildPipeline
    // 路径 B：m_needsSync=true        → PostData_SyncStateToStrategy
    // ================================================================
    void ProcessPendingUpdates() override;

    // ================================================================
    // 【AbstractInteractiveService 实现】交互接口
    // ================================================================
    void UpdateInteraction(int delta)                              override;
    void SyncCursorToWorldPosition(double worldPos[3], int axis = -1) override;
    std::array<int, 3> GetCursorPosition()                         override;
    void SetInteracting(bool val)                                  override;
    int  GetPlaneAxis(vtkActor* actor)                             override;

    // 模型变换（委托给 VolumeTransformService）
    vtkProp3D* GetMainProp()                                       override;
    void SyncModelMatrix(vtkMatrix4x4* mat)                        override;
    void TransformModel(double translate[3], double rotate[3], double scale[3]);
    void ResetModelTransform();
    void WorldToModel(const double worldPos[3], double modelPos[3]);
    void ModelToWorld(const double modelPos[3], double worldPos[3]);

    // 获取共享状态（供 main.cpp 的 LoadFileAsync 回调安全访问）
    std::shared_ptr<SharedInteractionState> GetSharedState() { return m_sharedState; }

private:
    // 后处理路径 A：DataReady → 重建 VTK 渲染管线（仅主线程）
    void PostData_RebuildPipeline();

    // 后处理路径 B：普通事件 → 增量同步参数到 Strategy（仅主线程）
    void PostData_SyncStateToStrategy();

    // 构建 RenderParams 纯数据对象（只读 SharedState，无 VTK 操作）
    RenderParams BuildRenderParams(UpdateFlags flags) const;

    // Strategy 缓存管理
    std::shared_ptr<AbstractVisualStrategy> GetOrCreateStrategy(VizMode mode);
    void ClearStrategyCache();

    // 辅助
    void ResetCursorToCenter();
    void MarkNeedsSync();
};