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
#include <vtkImageResample.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>


// --- 策略 A: 等值面渲染 ---
class IsoSurfaceStrategy : public BaseVisualStrategy {
public:
    IsoSurfaceStrategy();

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    void ConfigureCamera(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    vtkProp3D* GetMainProp() override;
private:
    void AlignCamera(const std::array<double, 16>& modelMatrix); // 模型矩阵变化后重对齐相机焦点
    vtkSmartPointer<vtkActor> m_actor; // 等值面主 actor
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes; // 坐标轴
    vtkSmartPointer<vtkFlyingEdges3D> m_qualityIsoFilter; // 静止期 766 分辨率等值面过滤器
    vtkSmartPointer<vtkFlyingEdges3D> m_interactionIsoFilter; // 交互期粗等值面过滤器
    vtkSmartPointer<vtkImageResample> m_qualityResample; // 静止期降采样结果
    vtkSmartPointer<vtkImageResample> m_interactionResample; // 交互期降采样结果
    vtkSmartPointer<vtkPolyDataMapper> m_mapper; // 包装器数据
    vtkSmartPointer<vtkDataObject> m_lastInput; // 缓存当前输入，避免重复绑定相同数据触发额外管线更新
    vtkWeakPointer<vtkRenderer> m_renderer; // 弱引用 renderer，仅用于相机和裁剪范围更新
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 }; // 当前等值面数据中心，供 Transform 后重定焦使用
    double m_currentIsoValue = 0.0; // 当前共享状态中的等值面阈值快照
    bool m_isInteracting = false; // 当前是否处于交互状态
};
