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
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    int GetPlaneAxis(vtkActor* actor) override;

private:
    void SetAllPositions(const double cursorWorld[3], const std::array<double, 16>& modelMatrix);
    void SetWorldBounds(const std::array<double, 16>& modelMatrix, double worldBounds[6]) const;
    vtkSmartPointer<vtkActor> m_planeActors[3];
    vtkSmartPointer<vtkPlaneSource> m_planeSources[3];
    vtkSmartPointer<vtkDataObject> m_lastInput;  // 缓存当前输入，避免重复初始化参考平面几何
    int m_maxIndices[3] = { 0, 0, 0 }; // 缓存最大索引，避免重复计算
    double m_bounds[6] = { 0.0 };      // 缓存局部空间包围盒，供世界坐标换算复用
    double m_spacing[3] = { 0 };
    double m_origin[3] = { 0 };
};