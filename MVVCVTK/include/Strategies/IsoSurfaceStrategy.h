#pragma once
#include "BaseVisualStrategy.h"
#include <vtkLODActor.h>
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
#include <vtkRenderer.h>


// --- 策略 A: 等值面渲染 ---
class IsoSurfaceStrategy : public BaseVisualStrategy {
public:
    IsoSurfaceStrategy();

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetRendererAttached(vtkSmartPointer<vtkRenderer> renderer);
    void SetCameraConfigured(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    vtkProp3D* GetMainProp() override;
private:
    void SetCameraAligned(const std::array<double, 16>& modelMatrix); // 模型矩阵变化后重对齐相机焦点
    vtkSmartPointer<vtkLODActor> m_actor; // 等值面主 actor
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes; // 坐标轴
    vtkSmartPointer<vtkFlyingEdges3D> m_isoFilter; // 等值面过滤器
    vtkSmartPointer<vtkPolyDataMapper> m_mapper; // 包装器数据
    vtkSmartPointer<vtkDataObject> m_lastInput; // 缓存当前输入，避免重复绑定相同数据触发额外管线更新
    vtkWeakPointer<vtkRenderer> m_renderer; // 弱引用 renderer，仅用于相机和裁剪范围更新
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 }; // 当前等值面数据中心，供 Transform 后重定焦使用
};