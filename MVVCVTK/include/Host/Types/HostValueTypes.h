#pragma once

#include <array>
#include <string>
#include <vector>

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

struct HostTransferNode {
    double position = 0.0; // 数据标量坐标，不是归一化纹理坐标。
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

struct HostBackgroundColor {
    double r = 0.1; // renderer 背景 RGB，按 [0,1] 解释。
    double g = 0.1;
    double b = 0.1;
};

struct HostWindowLevelParams {
    double windowWidth = 400.0; // 映射到显示灰阶的标量窗口宽度。
    double windowCenter = 40.0; // 标量窗口中心。
};

struct HostViewInitConfig {
    // has* 是显式写入位：字段本身保留可用默认值，但只有对应 has* 为 true 时才覆盖策略状态。
    HostRenderMode viewMode = HostRenderMode::IsoSurface; // 首次构建的主策略模式。
    HostMaterialParams material; // 始终作为预初始化材质写入。
    std::vector<HostTransferNode> transferNodes; // hasTransferNodes=true 时替换默认 TF。
    double isoThreshold = 0.0; // hasIso=true 时使用的数据标量阈值。
    HostBackgroundColor background;
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
    HostWindowLevelParams windowLevel;
    bool hasTransferNodes = false;
    bool hasIso = false;
    bool hasBackground = false;
    bool hasSpacing = false;
    bool hasWindowLevel = false;
};

struct HostWindowConfig {
    std::string title; // context 会尝试写入窗口；外部注入窗口仍可由宿主继续管理标题。
    int width = 600;
    int height = 600;
    int posX = 0;
    int posY = 0;
    bool isAxesVisible = false; // 控制方向轴 overlay 的初始可见性。
    HostViewInitConfig viewInit;
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
