#pragma once

#include "Host/Types/HostValueTypes.h"

#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct HostVolumeGeometry {
    std::array<int, 3> dimensions{ 0, 0, 0 }; // 体素数，顺序固定为 X/Y/Z。
    std::array<float, 3> spacing{};            // 相邻体素的物理间距，单位 mm。
    std::array<float, 3> origin{};             // 输入体数据的物理原点。
};

enum class HostCropPreviewMode { KeepInside, RemoveInside };
enum class HostGapAnalysisIsoMode { DataRangeRatio, AbsoluteValue };

enum class HostDataAction { None, LoadFile, ReloadBuffer, ExportVolume, ExportSlices };

struct HostLoadRequest {
    std::string filePath;
    HostVolumeGeometry geometry;
};

struct HostReloadRequest {
    std::vector<float> voxels; // X 最快、随后 Y/Z 的连续 float32 标量；请求对象拥有其存储。
    HostVolumeGeometry geometry; // dimensions 的乘积必须与 voxels.size() 一致。
};

struct HostVolumeExportRequest { std::string outputPath; };

struct HostSliceExportRequest {
    std::string outputDir; // 每层 PNG 的输出目录。
    HostViewTarget sourceView{ "", false, HostRenderViewRole::TopDownSlice }; // 决定切片法向与相机朝向。
    std::optional<double> angleDeg; // 可选的平面内旋转角；未提供时保持目标视图当前方向。
};

using HostDataPayload = std::variant<std::monostate, HostLoadRequest,
    HostReloadRequest, HostVolumeExportRequest, HostSliceExportRequest>;

struct HostDataRequest {
    HostDataAction action = HostDataAction::None;
    HostDataPayload payload;
};

struct HostViewSetRequest {
    HostViewTarget targetView; // 单目标解析遵循 id 优先且失败不回退 role。
    // optional 表示“本次是否写入该维度”；缺省字段必须保留视图当前状态。
    std::optional<HostRenderMode> mode;
    std::optional<HostMaterialParams> material;
    std::optional<double> opacity;
    std::optional<std::vector<HostTransferNode>> transferNodes;
    std::optional<double> iso;
    std::optional<HostBackgroundColor> background;
    std::optional<std::array<double, 3>> spacing;
    std::optional<HostWindowLevelParams> windowLevel;
};

enum class HostViewAction { None, Set };
using HostViewPayload = std::variant<std::monostate, HostViewSetRequest>;
struct HostViewRequest {
    HostViewAction action = HostViewAction::None;
    HostViewPayload payload;
};

struct HostToolSetRequest {
    HostViewTarget targetView;
    HostToolMode toolMode = HostToolMode::Navigation;
};

struct HostToolSwitchRequest { HostViewTarget targetView; };
enum class HostToolAction { None, Set, Switch };
using HostToolPayload = std::variant<std::monostate, HostToolSetRequest, HostToolSwitchRequest>;
struct HostToolRequest {
    HostToolAction action = HostToolAction::None;
    HostToolPayload payload;
};

struct HostCropTargetRequest {
    HostViewTarget referenceView{ "", false, HostRenderViewRole::Primary3D }; // 交互 widget 所在的权威 3D 视图。
    HostViewTargets previewViews; // 接收裁切预览的显式目标集合。
    bool isPreviewViewsUsed = false; // false 时由 host 使用拓扑中允许预览的默认窗口。
};

struct HostCropPreviewRequest {
    HostCropTargetRequest target;
    HostCropPreviewMode previewMode = HostCropPreviewMode::KeepInside;
};

enum class HostCropAction { None, Start, Box, Plane, Preview, Submit, Exit };
using HostCropPayload = std::variant<std::monostate, HostCropTargetRequest, HostCropPreviewRequest>;
struct HostCropRequest {
    HostCropAction action = HostCropAction::None;
    HostCropPayload payload;
};

struct HostGapSurfaceConfig {
    HostGapAnalysisIsoMode isoMode = HostGapAnalysisIsoMode::DataRangeRatio; // 选择下面哪个阈值字段生效。
    double dataRangeRatio = 0.0; // 标量范围内的相对位置，仅在 DataRangeRatio 模式使用。
    double absoluteIsoValue = 0.0; // 数据标量单位下的绝对等值面阈值。
};

struct HostGapVoidConfig {
    float grayMin = 0.0f;            // 候选孔隙灰度闭区间下界。
    float grayMax = 0.0f;            // 候选孔隙灰度闭区间上界。
    float minVolumeMM3 = 0.0f;       // 连通域最小物理体积，单位 mm^3。
    float angleThresholdDeg = 0.0f;  // 结构方向判别阈值，单位度。
    int tensorWindowSize = 0;        // 局部结构张量窗口边长（体素）。
    int erosionIterations = 0;       // 表面约束掩膜的腐蚀迭代次数。
};

struct HostGapConfig {
    HostGapSurfaceConfig surface;
    HostGapVoidConfig voidDetection;
};

struct HostGapStartRequest {
    HostViewTargets targetViews; // overlay 发布目标；按 host topology 去重并保持拓扑顺序。
    bool isDefaultOverlayUsed = false; // true 时忽略显式集合，使用 feature 约定的默认视图。
    HostGapConfig algorithm; // 本次后台分析的完整参数快照。
};

enum class HostGapAction { None, Start, Overlay, Exit };
using HostGapPayload = std::variant<std::monostate, HostGapStartRequest>;
struct HostGapRequest {
    HostGapAction action = HostGapAction::None;
    HostGapPayload payload;
};
