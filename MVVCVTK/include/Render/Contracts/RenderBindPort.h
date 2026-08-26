#pragma once

#include <vtkSmartPointer.h>

class vtkRenderer;
class vtkRenderWindow;

class RenderBindPort {
public:
    virtual ~RenderBindPort() = default;

    virtual bool SetRenderTarget(
        vtkSmartPointer<vtkRenderWindow> window,
        vtkSmartPointer<vtkRenderer> renderer) = 0;
};
