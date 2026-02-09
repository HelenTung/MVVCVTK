#pragma once
#include "AppInterfaces.h"
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

// --- 策略 A: 等值面渲染 ---
class IsoSurfaceStrategy : public AbstractVisualStrategy {
public:
    IsoSurfaceStrategy();

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params, UpdateFlags flags) override;
    vtkProp3D* GetMainProp() override;
private:
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes; // 坐标轴
    vtkSmartPointer<vtkFlyingEdges3D> m_isoFilter; // 等值面过滤器
    vtkSmartPointer<vtkPolyDataMapper> m_mapper; // 包装器数据
};