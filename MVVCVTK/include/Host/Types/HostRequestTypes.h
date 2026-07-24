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

enum class HostDataAction { None, LoadFile, ReloadBuffer, ExportVolume, ExportSlices };

struct HostLoadRequest {
    std::string filePath; // UTF-8 文件路径。
    HostVolumeGeometry geometry;
};

struct HostReloadRequest {
    std::vector<float> voxels; // X 最快、随后 Y/Z 的连续 float32 标量；请求对象拥有其存储。
    HostVolumeGeometry geometry; // dimensions 的乘积必须与 voxels.size() 一致。
};

struct HostVolumeExportRequest { std::string outputPath; /* UTF-8 文件路径。 */ };

struct HostSliceExportRequest {
    std::string outputDir; // UTF-8 输出目录；每层写入 PNG。
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
