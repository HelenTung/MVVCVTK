#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/GapAnalysisTypes.h
// GapAnalysisTypes.h - 间隙/空洞分析模块纯数据结构与插件接口
// =====================================================================

#include "Host/GapHostTypes.h"
#include "Data/DataPayloads.h"

#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// ── 表面参数（外部手动传入 isoValue，不在此自动估算）───────────────
struct GapSurfaceParams {
    float isoValue = 0.0f;  // 输入标量域阈值；当前 worker 将它映射为私有内核表面阈值。
    float background = 0.0f;  // DefX 材料定义的背景均值。
    float material = 0.0f;  // DefX 材料定义的材料均值。
};

// ── 空洞区域结果 ──────────────────────────────────────────────────────
struct VoidRegion {
    // 无掩膜时以下字段逐项保留供应商区域输出；掩膜路径由 bridge 有效域派生边界状态，可能重标标签。
    std::int32_t id = 0;
    std::int64_t voxelCount = 0;
    float volumeMM3 = 0.0f;
    float equivalentDiameterMM = 0.0f;
    float radiusMM = 0.0f;
    float diameterMM = 0.0f;
    std::array<double, 3> centerMM = { 0.0, 0.0, 0.0 };
    std::array<std::int32_t, 3> centroidMM = { 0, 0, 0 };
    std::array<std::int32_t, 6> bbox = { 0, 0, 0, 0, 0, 0 };
    std::array<std::int32_t, 3> seedVoxel = { 0, 0, 0 };
    float minGray = 0.0f;
    float maxGray = 0.0f;
    float meanGray = 0.0f;
    float stdDevGray = 0.0f;
    float grayDeviation = 0.0f;
    // 相邻缺陷外接球表面的最小距离，单位 mm。
    float gapMM = 0.0f;
    float compactness = 0.0f;
    float surfaceAreaMM2 = 0.0f;
    float sphericity = 0.0f;
    // 三个主轴标准差，而非旧实现的协方差特征值。
    std::array<float, 3> pcaDeviation = { 0.0f, 0.0f, 0.0f };
    float pcaMaxDeviationRatio = 0.0f;
    float pcaMinDeviationRatio = 0.0f;
    float projectedAreaXMM2 = 0.0f;
    float projectedAreaYMM2 = 0.0f;
    float projectedAreaZMM2 = 0.0f;
    // 供应商定义为三个方向的 voxel 索引差，不擅自换算为物理长度。
    std::array<float, 3> projectedSize = { 0.0f, 0.0f, 0.0f };
    float defectProbability = 0.0f;
    std::int64_t parentVoxelCount = 0, intersectionVoxelCount = 0;
    double parentVolumeMM3 = 0, intersectionVolumeMM3 = 0;
    bool isRoiPartial = false;
    // 其余形状量属于完整父缺陷；边界不确定区域必须为 false，非完整对象也用 false 标记。
    bool hasWholeObjectGeometry = true;
    GapBoundaryStatus boundaryStatus = GapBoundaryStatus::Interior;
};

class LabelMap3DPayload;
class SurfaceMeshPayload;
class RecordTablePayload;
struct GapDisplayData;

class GapParentRecord;
struct GapResultPayloads final {
    std::shared_ptr<const LabelMap3DPayload> labels;
    std::shared_ptr<const SurfaceMeshPayload> mesh;
    std::shared_ptr<const RecordTablePayload> voids;
    std::shared_ptr<const RecordTablePayload> statistics;
    std::shared_ptr<const LabelMap3DPayload> parentLabels;
    std::shared_ptr<const RecordTablePayload> parentVoids;
    std::shared_ptr<const GapParentRecord> parentRecord;
};

// ── 完整分析结果（GapAnalysisService 填充，主线程消费）──────────────
struct GapAnalysisResult {
    DataRevisionRef sourceRevision;
    GapSurfaceParams detectionParameters;
    std::shared_ptr<const GapAnalysisResult> parent;
    // worker 准备的私有显示候选，不进入正式 payload 或导出。
    std::shared_ptr<const GapDisplayData> display;
    std::string displayError;
    // worker 完成所有首次转换；Host 仍遵守 DataGraph 的防御性 snapshot/校验。
    std::shared_ptr<const GapResultPayloads> payloads;
    // 私有 bridge 的可见区域集合，包含边界不确定对象。
    DataArray<VoidRegion> voids;
    // 此 VTK_INT 图持有私有 bridge 标签 owner；掩膜路径清理无效域并按六邻接重标。
    // 标签体完整继承输入快照的 extent、spacing、origin 与 direction。
    vtkSmartPointer<vtkImageData> labelImage;
    // worker 以各标签的二值边界构建；逐面 gap.region.id 保留正式孔隙归属。
    vtkSmartPointer<vtkPolyData> voidMesh;
    // 由私有 bridge 结果投影；正式孔统计排除边界不确定对象。
    GapStatistics statistics;
    bool                    isSucceeded = false; // 只表示分析 payload 有效，不代表 display/overlay 已显示。
};
