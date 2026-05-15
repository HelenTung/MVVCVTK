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
    void SetAllPositions(const double cursorWorld[3], const std::array<double, 16>& modelMatrix); // 把三张参考平面同步移动到当前 cursor 所在世界位置
    void SetWorldBounds(const std::array<double, 16>& modelMatrix, double worldBounds[6]) const; // 计算模型变换后的世界包围盒
    vtkSmartPointer<vtkActor> m_planeActors[3]; // 三个正交参考平面的可视 actor
    vtkSmartPointer<vtkPlaneSource> m_planeSources[3]; // 三个平面的几何源，更新位置时直接改这里
    vtkSmartPointer<vtkDataObject> m_lastInput;  // 缓存当前输入，避免重复初始化参考平面几何
    int m_maxIndices[3] = { 0, 0, 0 }; // 缓存最大索引，避免重复计算
    double m_bounds[6] = { 0.0 };      // 缓存局部空间包围盒，供世界坐标换算复用
    double m_spacing[3] = { 0 };       // 原始数据 spacing，供几何初始化和后续约束复用
    double m_origin[3] = { 0 };        // 原始数据 origin，决定三张初始参考平面的位置基准
};