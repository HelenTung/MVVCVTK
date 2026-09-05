#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

enum class HostDataExportFormat {
    Raw,
    Ply,
    Stl,
    Obj
};

// Host 只提交真实 scalar 坐标的完整传输函数快照。
struct HostVolumeTransferFunction final {
    struct ColorNode final {
        double scalar = 0.0;
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
    };

    struct OpacityNode final {
        double scalar = 0.0;
        double opacity = 0.0;
    };

    std::vector<ColorNode> colorNodes;
    std::vector<OpacityNode> opacityNodes;
};

struct HostVolumeGeometry {
    std::array<int, 3> dimensions{ 0, 0, 0 }; // 体素数，顺序固定为 X/Y/Z。
    std::array<float, 3> spacing{};            // 相邻体素的物理间距，单位 mm。
    std::array<float, 3> origin{};             // 输入 ITK/LPS 体数据的物理原点。
    // 输入 ITK/LPS direction，行主序 3x3；Host 在加载边界一次转换为内部 RAS。
    std::array<double, 9> direction = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
};

enum class HostRenderMode {
    Volume,
    IsoSurface,
    SliceTopDown,
    SliceFrontBack,
    SliceLeftRight,
    CompositeVolume,
    CompositeIsoSurface
};

enum class HostToolMode {
    Navigation,
    ModelTransform
};

struct HostMaterialParams {
    double ambient = 0.1;        // 环境光系数。
    double diffuse = 0.7;        // 漫反射系数。
    double specular = 0.2;       // 镜面反射系数。
    double specularPower = 10.0; // 高光指数。
    double opacity = 1.0;        // 主 prop 整体不透明度。
    bool isShadeOn = false;      // 体渲染是否启用光照着色。
};

enum class HostMaterialPreset {
    Soft,
    Dense,
    Glossy
};

// Host 只表达主 3D 质量意图；尺寸、采样、jitter 与资源预算均由 Render 解析。
enum class HostVolumeQuality : std::uint8_t {
    Auto,
    Low,
    High,
    XHigh,
    Ultra
};

struct HostBackgroundColor {
    double r = 0.1; // renderer 背景 RGB，按 [0,1] 解释。
    double g = 0.1;
    double b = 0.1;
};

struct HostWindowLevelParams {
    double windowWidth = 400.0; // 映射到显示灰阶的标量窗口宽度。
    double windowCenter = 40.0; // 标量窗口中心。
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
