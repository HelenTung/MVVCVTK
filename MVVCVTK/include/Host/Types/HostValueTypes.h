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

enum class HostViewMode {
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
    double position = 0.0;
    double opacity = 0.0;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

struct HostMaterialParams {
    double ambient = 0.1;
    double diffuse = 0.7;
    double specular = 0.2;
    double specularPower = 10.0;
    double opacity = 1.0;
    bool isShadeOn = false;
};

struct HostBackgroundColor {
    double r = 0.1;
    double g = 0.1;
    double b = 0.1;
};

struct HostWindowLevelParams {
    double windowWidth = 400.0;
    double windowCenter = 40.0;
};

struct HostViewInitConfig {
    HostViewMode viewMode = HostViewMode::IsoSurface;
    HostMaterialParams material;
    std::vector<HostTransferNode> transferNodes;
    double isoThreshold = 0.0;
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
    std::string title;
    int width = 600;
    int height = 600;
    int posX = 0;
    int posY = 0;
    bool isAxesVisible = false;
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
