#pragma once

#include "Host/Types/HostRequest.h"
#include "Host/Types/HostValueTypes.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

struct HostLoadRequest final : HostRequest {
    std::string filePath; // UTF-8 文件路径。
    HostVolumeGeometry geometry;
};

struct HostReloadRequest final : HostRequest {
    std::vector<float> voxels; // X 最快、随后 Y/Z 的连续 float32 标量；请求对象拥有其存储。
    HostVolumeGeometry geometry; // dimensions 的乘积必须与 voxels.size() 一致。
};

struct HostDataExportRequest final : HostRequest {
    std::string outputPath; // UTF-8 输出目录；文件名由 Data 层基于冻结数据生成。
    // 缺省时由 sourceView 模式收敛：体渲染导出 RAW，等值面导出 PLY。
    std::optional<HostDataExportFormat> format;
    // 未指定 selector 时使用 Primary3D。
    HostViewTarget sourceView;
};

struct HostSliceExportRequest final : HostRequest {
    std::string outputDir; // UTF-8 输出目录；每层写入 PNG。
    // 热键缺省时由触发窗口补齐；显式请求仅在需要指定切片方向时设置。
    HostViewTarget sourceView;
    std::optional<double> angleDeg; // 可选的平面内旋转角；未提供时保持目标视图当前方向。
};

struct HostViewSetRequest final : HostRequest {
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

struct HostViewResetRequest final : HostRequest {
    HostViewTarget targetView;
};

struct HostToolSetRequest final : HostRequest {
    HostViewTarget targetView;
    HostToolMode toolMode = HostToolMode::Navigation;
};

struct HostToolSwitchRequest final : HostRequest {
    HostViewTarget targetView;
};
