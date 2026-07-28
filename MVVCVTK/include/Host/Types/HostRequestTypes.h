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

enum class HostDataAction { None, LoadFile, ReloadBuffer, ExportData, ExportSlices };

struct HostLoadRequest {
    std::string filePath; // UTF-8 文件路径。
    HostVolumeGeometry geometry;
};

struct HostReloadRequest {
    std::vector<float> voxels; // X 最快、随后 Y/Z 的连续 float32 标量；请求对象拥有其存储。
    HostVolumeGeometry geometry; // dimensions 的乘积必须与 voxels.size() 一致。
};

struct HostDataExportRequest {
    std::string outputPath; // UTF-8 输出目录；文件名由 Data 层基于冻结数据生成。
    // 缺省时由 sourceView 模式收敛：体渲染导出 RAW，等值面导出 PLY。
    std::optional<HostDataExportFormat> format;
    // 未指定 selector 时使用 Primary3D。
    HostViewTarget sourceView;
};

struct HostSliceExportRequest {
    std::string outputDir; // UTF-8 输出目录；每层写入 PNG。
    // 热键缺省时由触发窗口补齐；显式请求仅在需要指定切片方向时设置。
    HostViewTarget sourceView;
    std::optional<double> angleDeg; // 可选的平面内旋转角；未提供时保持目标视图当前方向。
};

using HostDataPayload = std::variant<std::monostate, HostLoadRequest,
    HostReloadRequest, HostDataExportRequest, HostSliceExportRequest>;

struct HostDataRequest {
    HostDataAction action = HostDataAction::None;
    HostDataPayload payload;
};

struct HostCursorParams {
    std::array<double, 3> world{}; // VTK world 坐标；axis=-1 时三轴全部写入。
    int axis = -1; // -1 为自由点；0/1/2 保持对应轴的当前联动位置。
};

struct HostVisibilityParams {
    // 每个 optional 只控制一个共享可见性位；缺省字段保留当前位。
    std::optional<bool> isPlanes3DVisible;
    std::optional<bool> isCrosshairVisible;
    std::optional<bool> isRulerVisible;
};

struct HostViewSetRequest {
    HostViewTarget targetView; // 单目标解析遵循 id 优先且失败不回退 role。
    // optional 表示“本次是否写入该维度”；缺省字段必须保留视图当前状态。
    std::optional<HostRenderMode> mode;
    std::optional<HostMaterialParams> material;
    std::optional<HostMaterialPreset> materialPreset;
    std::optional<double> opacity;
    std::optional<std::vector<HostTransferNode>> transferNodes;
    std::optional<HostTransferPreset> transferPreset;
    std::optional<double> iso;
    std::optional<HostBackgroundColor> background;
    std::optional<std::array<double, 3>> spacing;
    std::optional<HostWindowLevelParams> windowLevel;
    std::optional<HostVolumeQualityParams> volumeQuality;
    std::optional<std::vector<HostGradientOpacityNode>> gradientOpacity;
    std::optional<bool> isDenoiseOn;
    std::optional<HostCursorParams> cursor; // 会话共享 world cursor；数据未就绪时 service 保持现状。
    std::optional<HostVisibilityParams> visibility; // 会话共享业务元素显隐。
    std::optional<bool> isAxesVisible; // 目标 context 的世界方向轴 marker。
};

struct HostViewResetRequest {
    HostViewTarget targetView;
};

enum class HostViewAction { None, Set, ResetCamera };
using HostViewPayload = std::variant<std::monostate,
    HostViewSetRequest, HostViewResetRequest>;
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
