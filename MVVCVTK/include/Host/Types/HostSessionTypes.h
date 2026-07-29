#pragma once

#include "Host/Types/HostValueTypes.h"

#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>

#include <string>
#include <vector>

class vtkRenderer;
class vtkRenderWindowInteractor;

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

struct HostRenderViewConfig {
    std::string id; // 会话内唯一稳定标识；HostViewTarget 优先按此值查找。
    HostRenderViewRole role = HostRenderViewRole::Auxiliary; // 允许同 role 多窗口，集合查询按拓扑顺序返回。
    HostWindowConfig window; // 窗口尺寸、位置与初始渲染状态。
    vtkSmartPointer<vtkRenderWindow> renderWindow; // 可选外部窗口；为空时 session 自建并拥有窗口。
    bool isEventLoopEnabled = false; // standalone Start 候选；一个会话必须能解析出唯一启动窗口。
};

struct HostRenderViewEndpoint {
    // endpoint 是对 session 内 VTK 对象的非拥有观察视图，不得跨 session 重建或析构缓存。
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    vtkRenderer* renderer = nullptr;
    vtkRenderWindow* renderWindow = nullptr;
    vtkRenderWindowInteractor* interactor = nullptr;
};

struct HostSessionConfig {
    std::vector<HostRenderViewConfig> renderViews; // 声明顺序即 topology 顺序，也决定多目标返回与首选窗口顺序。
};
