#pragma once

#include "Host/Types/HostValueTypes.h"

#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>

#include <string>
#include <vector>

class vtkRenderer;
class vtkRenderWindowInteractor;

struct HostRenderViewConfig {
    std::string id; // 会话内唯一稳定标识；HostViewTarget 优先按此值查找。
    HostRenderViewRole role = HostRenderViewRole::Auxiliary; // 允许同 role 多窗口，集合查询按拓扑顺序返回。
    HostWindowConfig window; // 窗口尺寸、位置与初始渲染状态。
    vtkSmartPointer<vtkRenderWindow> renderWindow; // 可选外部窗口；为空时 session 自建并拥有窗口。
    bool isCropPreviewIncluded = true; // 默认 preview 目标选择是否包含本窗口。
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
