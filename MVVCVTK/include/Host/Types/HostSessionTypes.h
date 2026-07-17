#pragma once

#include "Host/Types/HostValueTypes.h"

#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>

#include <string>
#include <vector>

class vtkRenderer;
class vtkRenderWindowInteractor;

struct HostRenderViewConfig {
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    HostWindowConfig window;
    vtkSmartPointer<vtkRenderWindow> renderWindow;
    bool isCropPreviewIncluded = true;
    bool isEventLoopEnabled = false;
};

struct HostRenderViewEndpoint {
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    vtkRenderer* renderer = nullptr;
    vtkRenderWindow* renderWindow = nullptr;
    vtkRenderWindowInteractor* interactor = nullptr;
};

struct HostSessionConfig {
    std::vector<HostRenderViewConfig> renderViews;
};
