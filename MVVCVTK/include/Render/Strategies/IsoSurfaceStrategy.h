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
    void SetCamera(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    vtkProp3D* GetMainProp() override;
private:
    // modelMatrix 按 input model -> world 解释；相机只跟随变换后的 m_dataCenter 平移焦点。
    void AlignCamera(const std::array<double, 16>& modelMatrix);
    // 等值面主 prop 与坐标轴 prop 均由策略强持有，并登记到基类 m_managedProps 统一挂载。
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    // ImageData 路径的双等值面 producer：静止期最大轴上限 766，交互期最大轴上限 256。
    vtkSmartPointer<vtkFlyingEdges3D> m_qualityIsoFilter;
    vtkSmartPointer<vtkFlyingEdges3D> m_interactionIsoFilter;
    // 双重采样 producer 的强引用；分别维持质量/交互过滤器输入端口的生命周期。
    vtkSmartPointer<vtkImageResample> m_qualityResample;
    vtkSmartPointer<vtkImageResample> m_interactionResample;
    // actor 使用的唯一 mapper；输入可在直接 PolyData、质量 filter 输出和交互 filter 输出之间切换。
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;
    // 最近一次有效输入的强引用和身份缓存；同一 VTK 对象原地修改不等价于不可变快照。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    // 非拥有 renderer 弱引用，仅用于相机与 clipping range；renderer 销毁后自动为空。
    vtkWeakPointer<vtkRenderer> m_renderer;
    // 当前输入在 input model 坐标中的中心 [x,y,z]；Transform 时提升到 world 作为相机焦点。
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 };
    // 最后一次共享状态等值面阈值，单位与输入标量一致；只向当前活动 filter 下发。
    double m_currentIsoValue = 0.0;
    // 双管线选择状态：true 使用 256 交互管线，false 使用 766 质量管线；新 ImageData 重置为 false。
    bool m_isInteracting = false;
};
