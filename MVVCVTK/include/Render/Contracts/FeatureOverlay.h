#pragma once

#include <vtkDataObject.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

#include <array>

// 主体只向 Feature overlay 发布定位所需的窄快照，不泄露材质、LOD 或传输函数。
struct FeatureOverlayState final {
    std::array<double, 3> cursor = { 0.0, 0.0, 0.0 };
    std::array<double, 16> modelToWorld = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
};

class FeatureOverlay {
public:
    virtual ~FeatureOverlay() = default;

    virtual void SetInputData(
        vtkSmartPointer<vtkDataObject> data) = 0;
    virtual void AttachRenderer(
        vtkSmartPointer<vtkRenderer> renderer) = 0;
    virtual void DetachRenderer(
        vtkSmartPointer<vtkRenderer> renderer) = 0;
    virtual void SetOverlayState(
        const FeatureOverlayState&) {}
};
