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


class MultiSliceStrategy : public BaseVisualStrategy {
public:
    MultiSliceStrategy();

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetRendererAttached(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);

private:
    void SetAllPositions(const double cursorWorld[3], const std::array<double, 16>& modelMatrix);

    vtkSmartPointer<vtkImageSlice> m_slices[3];
    vtkSmartPointer<vtkImageResliceMapper> m_mappers[3];
    vtkSmartPointer<vtkPlane> m_slicePlanes[3];      // 持久切片平面，避免重复创建切片节点导致管线抖动
    vtkSmartPointer<vtkDataObject> m_lastInput;      // 缓存当前输入，重复数据不再重新绑定 mapper
    int m_indices[3] = { 0, 0, 0 };
};