#pragma once
// =====================================================================
// AppService.h
//
//   MedicalVizService 是"渲染业务调度层"，职责为：
//     【前处理】BindService 之后、数据到达之前：登记配置意图
//     【后处理-重建】DataReady 到达后：重建 VTK 渲染管线
//     【后处理-同步】普通交互事件：增量同步参数到 Strategy
//
//   坐标变换职责已拆出到 VolumeTransformService。
//   分析职责已有 VolumeAnalysisService。
//
// =====================================================================

#include "AppInterfaces.h"
#include "AppState.h"
#include "DataConverters.h"
#include "VolumeTransformService.h"
#include <vtkTable.h>
#include <map>
#include <vtkTransform.h>
#include <mutex>
class VolumeAnalysisService {
private:
    std::shared_ptr<AbstractDataManager> m_dataManager;

public:
    explicit VolumeAnalysisService(std::shared_ptr<AbstractDataManager> dataMgr)
        : m_dataManager(std::move(dataMgr)) {
    }

    vtkSmartPointer<vtkTable> GetHistogramData(int binCount = 2048) {
        if (!m_dataManager || !m_dataManager->GetVtkImage()) return nullptr;
        auto converter = std::make_shared<HistogramConverter>();
        converter->SetParameter("BinCount", (double)binCount);
        return converter->Process(m_dataManager->GetVtkImage());
    }

    void SaveHistogramImage(const std::string& filePath, int binCount = 2048) {
        if (!m_dataManager || !m_dataManager->GetVtkImage()) return;
        auto converter = std::make_shared<HistogramConverter>();
        converter->SetParameter("BinCount", (double)binCount);
        converter->SaveHistogramImage(m_dataManager->GetVtkImage(), filePath);
    }
};

// ─────────────────────────────────────────────────────────────────────
// MedicalVizService
// ─────────────────────────────────────────────────────────────────────
class MedicalVizService
    : public AbstractInteractiveService
    , public std::enable_shared_from_this<MedicalVizService>
{
    // ================================================================
    // 【私有成员】
    // ================================================================
private:
    // 策略缓存：key = VizMode，value = 已构建的策略实例
    //  策略构建（含 VTK pipeline 初始化）开销较大，
    //   同一模式切换回来时直接复用，避免重建。
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;
    std::shared_ptr<SharedInteractionState> m_sharedState;
    // 坐标变换子服务（依赖注入，单一职责）
    std::unique_ptr<VolumeTransformService> m_transformService;
    VizMode m_pendingVizMode = VizMode::IsoSurface;
    std::atomic<bool> m_needsDataRefresh{ false };

    // ================================================================
    // 【公共接口】
    // ================================================================
public:
    MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state);

    // ── 初始化（由 BindService 触发，必须在 shared_ptr 完全构建后调用）──
    void Initialize(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren) override;

    void PreInit_SetVizMode(VizMode mode);

    // 设置光照参数（写入 SharedState::Material，DataReady 后 Strategy 会读取）
    void PreInit_SetLuxParams(double ambient, double diffuse,
        double specular, double power, bool shadeOn = false);

    // 设置全局透明度
    void PreInit_SetOpacity(double opacity);

    // 设置传输函数节点（体渲染颜色映射）
    void PreInit_SetTransferFunction(const std::vector<TFNode>& nodes);

    // 设置等值面阈值
    void PreInit_SetIsoThreshold(double val);

    void LoadFileAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr);

    // ================================================================
    // 【交互接口实现】（AbstractInteractiveService 虚函数实现）
    //
    // 这些方法仍然保留，供 StdRenderContext 在事件处理中调用
    // ================================================================
    void UpdateInteraction(int delta) override;
    void SyncCursorToWorldPosition(double worldPos[3], int axis = -1) override;
    void ProcessPendingUpdates() override;
    std::array<int, 3> GetCursorPosition() override;
    void SetInteracting(bool val) override;
    int  GetPlaneAxis(vtkActor* actor) override;

    // ── 模型变换（委托给 VolumeTransformService）──────────────────
    vtkProp3D* GetMainProp() override;
    void SyncModelMatrix(vtkMatrix4x4* mat) override;
    void TransformModel(double translate[3], double rotate[3], double scale[3]);
    void ResetModelTransform();
    void WorldToModel(const double worldPos[3], double modelPos[3]);
    void ModelToWorld(const double modelPos[3], double worldPos[3]);

    // 获取共享状态（供 main.cpp 在回调中安全访问）
    std::shared_ptr<SharedInteractionState> GetSharedState() { return m_sharedState; }

    // ================================================================
    // 【后向兼容别名】
    //   保留原始名称作为 PreInit_* 的转发，避免大规模修改调用方。
    //   新代码应使用 PreInit_* 版本，旧代码仍然可用。
    // ================================================================
    void SetLuxParams(double a, double d, double s, double p, bool sh = false) {
        PreInit_SetLuxParams(a, d, s, p, sh);
    }
    void SetOpacity(double o) { PreInit_SetOpacity(o); }
    void SetIsoThreshold(double v) { PreInit_SetIsoThreshold(v); }
    void SetTransferFunction(const std::vector<TFNode>& n) { PreInit_SetTransferFunction(n); }
    // Show* 系列：仅登记模式，不立即重建管线
    void ShowIsoSurface() { PreInit_SetVizMode(VizMode::IsoSurface); }
    void ShowVolume() { PreInit_SetVizMode(VizMode::Volume); }
    void ShowSlice(VizMode m) { PreInit_SetVizMode(m); }
    void Show3DPlanes(VizMode m) { PreInit_SetVizMode(m); }

    // ================================================================
    // 【私有方法】
    // ================================================================
private:
    // ── 后处理路径 A：数据到达后重建渲染管线 ──────────────────────
    void PostData_RebuildPipeline();

    // ── 后处理路径 B：普通状态增量同步到 Strategy ─────────────────
    void PostData_SyncStateToStrategy();

    // ── 构建 RenderParams 纯数据对象 ──────────────────────────────
    RenderParams BuildRenderParams(UpdateFlags flags) const;

    // ── Strategy 管理 ──────────────────────────────────────────────
    std::shared_ptr<AbstractVisualStrategy> GetOrCreateStrategy(VizMode mode);
    void ClearStrategyCache();

    // ── 辅助 ───────────────────────────────────────────────────────
    void ResetCursorToCenter();
    // 标记"State 已变更，需要在下一帧同步到 Strategy"
    void MarkNeedsSync();
};