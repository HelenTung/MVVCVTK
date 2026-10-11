#pragma once
#include "Data/DataPayloads.h"

enum class VoxelVolumeMethod : std::uint8_t { StoredValueRange, Labels };
enum class VoxelVolumeValidity : std::uint8_t { Complete, IncompleteSamples, TouchesBoundary, EmptySelection };
struct VoxelVolumeRequest final {
    std::string id;
    DataRevisionRef source;
    VoxelVolumeMethod method=VoxelVolumeMethod::StoredValueRange;
    // 源修订的含端点索引区域；空值使用整个源网格，不是世界坐标包围盒。
    std::optional<std::array<int,6>> extent;
    // 闭区间，直接比较存储值，不应用 metadata.scalar.slope/intercept。
    std::optional<std::array<double,2>> storedValueRange;
    // 明确选中的非负标签（包括显式指定的 0）；接纳时排序去重。整数比较不经 double，负标签记为不可观测。
    std::vector<std::uint64_t> labels;
    // Labels 可显式使用同几何源灰度体的有效性 mask/NoData/非有限值，关联记录在结果中。
    // NoData 按源标量类型解释（包括浮点 NaN）；不施加标量校准的 slope/intercept。
    std::optional<DataRevisionRef> validityImage;
    // 默认量是选区内的单元体积和；true 时还要求非空且不接触索引区域边界。
    bool requireEnclosedSelection=false;
};
struct VoxelVolumeStatistics final {
    std::array<int,6> evaluatedExtent{};
    std::uint64_t total=0,selected=0,unselected=0,masked=0,noData=0,nonFinite=0,invalidLabels=0;
    double cellVolumeMm3=0,observedVolumeMm3=0;
    // 存在不可观测单元或未满足完整性要求时为空；observedVolumeMm3 仅代表有效选中部分。
    std::optional<double> volumeMm3;
    bool touchesRegionBoundary=false;
    VoxelVolumeValidity validity=VoxelVolumeValidity::IncompleteSamples;
};
struct VoxelVolumeSnapshot final {
    DataRevisionRef result,source;
    std::shared_ptr<const VoxelVolumeRequest> parameters;
    GridGeometry3D geometry;
    VoxelVolumeStatistics statistics;
    std::string algorithmVersion="voxel-cell-volume-1";
    bool isCurrent=false;
};
