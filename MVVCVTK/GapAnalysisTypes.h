#pragma once
// =====================================================================
// GapAnalysisTypes.h — 间隙/空洞分析模块纯数据结构
// =====================================================================

#include <vector>
#include <array>
#include <cstddef>

// ── 分析任务状态（对齐 LoadState 风格）──────────────────────────────
enum class GapAnalysisState {
    Idle,       // 未发起
    Running,    // 后台计算中
    Succeeded,  // 完成，结果可读
    Failed      // 失败
};

// ── 表面参数（外部手动传入 isoValue，不在此自动估算）───────────────
struct SurfaceParams {
    float isoValue = 0.0f;  // 等值面阈值（ISO-50 由外部计算后传入）
    float background = 0.0f;  // 背景灰度上限
    float material = 0.0f;  // 材料灰度下限
};

// ── 高级法向精化参数 ──────────────────────────────────────────────────
struct AdvancedSurfaceParams {
    bool  enabled = false;
    float normalSearchDistance = 0.0f;
    bool  useMillimeter = false;
    float searchStep = 0.5f;
    float maxVertexShift = 2.0f;
    float gradientThreshold = 0.0f;
    int   normalSmoothIterations = 0;
};

// ── 空洞检测参数 ──────────────────────────────────────────────────────
struct VoidDetectionParams {
    float  grayMin = 0.0f;
    float  grayMax = 0.0f;
    double minVolumeMM3 = 0.01;
    float  angleThresholdDeg = 30.0f;
    int    tensorWindowSize = 1;
};

// ── 空洞区域统计结果 ──────────────────────────────────────────────────
struct VoidRegion {
    int                  id = 0;
    size_t               voxelCount = 0;
    double               volumeMM3 = 0.0;
    double               equivalentDiameterMM = 0.0;
    std::array<double, 3> centroidMM = { 0, 0, 0 };
    std::array<int, 6>    bbox = { 0, 0, 0, 0, 0, 0 };
};

// ── 完整分析结果（GapAnalysisService 填充，主线程消费）──────────────
struct GapAnalysisResult {
    std::vector<VoidRegion> voids;
    std::vector<int>        labelVolume;  // 与体素一一对应，0 = 非空洞
    bool                    succeeded = false;
};

class IGapAnalysisService {
public:
    virtual ~IGapAnalysisService() = default;

    // ── 前处理：设置计算参数（只写，零计算，线程安全）──────────────
    virtual void GapPreInit_SetSurfaceParams(const SurfaceParams& p) = 0;
    virtual void GapPreInit_SetAdvancedParams(const AdvancedSurfaceParams& p) = 0;
    virtual void GapPreInit_SetVoidParams(const VoidDetectionParams& p) = 0;

    // ── 触发：主动发起后台计算 ────────────────────────────────────────
    // onComplete 在后台线程回调，只允许写原子标记
    virtual void RunAsync(
        std::function<void(bool success)> onComplete = nullptr) = 0;

    // ── 查询：主线程轮询（对齐 GetLoadState 风格）────────────────────
    virtual GapAnalysisState GetAnalysisState() const = 0;

    // ── 取消（尽力，对齐 CancelLoad 语义）────────────────────────────
    virtual void CancelRun() {}

    // ── 后处理：主线程消费结果（对齐 PostData_RebuildPipeline 调用点）
    // 返回空洞区域列表（供 UI 展示）
    virtual std::vector<VoidRegion> GetVoidRegions() const = 0;

    // 生成空洞 Mesh，供 IsoSurfaceStrategy::SetInputData 消费
    virtual vtkSmartPointer<vtkPolyData> BuildVoidMesh() const = 0;
};