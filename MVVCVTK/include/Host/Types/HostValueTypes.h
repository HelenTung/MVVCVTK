#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

struct HostVolumeGeometry {
    std::array<int, 3> dimensions{ 0, 0, 0 }; // 体素数，顺序固定为 X/Y/Z。
    std::array<float, 3> spacing{};            // 相邻体素的物理间距，单位 mm。
    std::array<float, 3> origin{};             // 输入体数据的物理原点。
};

enum class HostRenderViewRole {
    Primary3D,
    Composite3D,
    TopDownSlice,
    FrontBackSlice,
    LeftRightSlice,
    Auxiliary
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

// Host 导出契约的唯一格式基元；内部链路只透明传递规范后缀，不再定义同义枚举。
enum class HostDataExportFormat {
    Raw,
    Ply,
    Stl,
    Obj
};

struct HostTransferNode {
    double position = 0.0; // 当前 scalar range 内的归一化位置，[0,1]。
    double opacity = 0.0;  // 该标量位置的不透明度控制点。
    double r = 0.0; // 颜色通道，按 VTK RGB [0,1] 约定解释。
    double g = 0.0;
    double b = 0.0;
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

enum class HostVolumeQuality {
    Quality,
    Custom
};

struct HostVolumeQualityParams {
    HostVolumeQuality quality = HostVolumeQuality::Quality;
    int maxDimension = 766;
    double sampleDistance = 1.0;
    bool isJitterOn = true;
};

struct HostGradientOpacityNode {
    double gradient = 0.0; // VTK gradient-opacity 原生域中的梯度幅值。
    double opacity = 0.0;  // 归一化不透明度 [0,1]。
};

enum class HostTransferPreset {
    Percentile = 0, // 保留既有 Host 请求的底层值。
    Manual = 1 // 当前 TF 由 transferNodes 直接控制；也用于状态读回。
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

// 单目标保持 id 优先且 id 未命中时不回退 role。
struct HostViewTarget {
    std::string viewId;
    bool isViewRoleUsed = false;
    HostRenderViewRole viewRole = HostRenderViewRole::Auxiliary;
};

// 多目标按 topology 顺序返回 ids/roles 的去重并集；空集合不表示全选。
struct HostViewTargets {
    std::vector<std::string> viewIds;
    std::vector<HostRenderViewRole> viewRoles;
};
