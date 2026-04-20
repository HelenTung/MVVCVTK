#pragma once
#include "BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkImageSlice.h>
#include <vtkImageResliceMapper.h>
#include <vtkLineSource.h>
#include <vtkPlane.h>
#include <vtkPlaneSource.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCubeAxesActor.h>
#include <vtkFlyingEdges3D.h>
#include <vtkPolyDataMapper.h>

class ColoredPlanesStrategy : public BaseVisualStrategy {
public:
    ColoredPlanesStrategy();

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void UpdateVisuals(const RenderParams& params, UpdateFlags flags) override;
    int GetPlaneAxis(vtkActor* actor) override;

private:
    void UpdateAllPositions(const double cursorWorld[3], const std::array<double, 16>& modelMatrix);
    vtkSmartPointer<vtkActor> m_planeActors[3];
    vtkSmartPointer<vtkPlaneSource> m_planeSources[3];
    vtkSmartPointer<vtkImageData> m_imageData;
    int m_maxIndices[3] = { 0, 0, 0 }; // 缓存最大索引，避免重复计算
    double m_spacing[3] = { 0 };
    double m_origin[3] = { 0 };
};