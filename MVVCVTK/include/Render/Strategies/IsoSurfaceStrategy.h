#pragma once
#include "Render/Support/BaseVisualStrategy.h"
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
#include <vtkRenderer.h>

class vtkClipPolyData;

// --- 策略 A: 等值面渲染 ---
class IsoSurfaceStrategy : public BaseVisualStrategy {
public:
    IsoSurfaceStrategy();
    ~IsoSurfaceStrategy() override;

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetInputMask(
        vtkSmartPointer<vtkImageData> validityMask) override;
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    vtkProp3D* GetMainProp() override;
private:
    class Mapper;
    class MaskImplicit;
    RenderEffectTarget GetRenderEffectTarget() const override;
    void SetEffectBinding(RenderEffectBinding* binding) override;
    bool SetMapperInput();
    // 等值面主 prop 与坐标轴 prop 均由策略强持有，并登记到基类 m_managedProps 统一挂载。
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    // ImageData 路径固定使用最大轴 766 的单一等值面 producer；
    // 通用交互来源只控制刷新调度，不改变几何分辨率或 mapper 输入。
    vtkSmartPointer<vtkFlyingEdges3D> m_isoFilter;
    vtkSmartPointer<vtkImageResample> m_resample;
    vtkSmartPointer<vtkImageResample> m_mask;
    vtkSmartPointer<MaskImplicit> m_maskFunc;
    vtkSmartPointer<vtkClipPolyData> m_clip;
    // actor 使用的唯一 mapper；mask 只在同一质量 producer 与 clip 输出之间接线。
    vtkSmartPointer<Mapper> m_mapper;
    // 最近一次有效输入的强引用和身份缓存；同一 VTK 对象原地修改不等价于不可变快照。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    // 非拥有 renderer 弱引用，仅用于相机与 clipping range；renderer 销毁后自动为空。
    vtkWeakPointer<vtkRenderer> m_renderer;
    // 当前输入在 input model 坐标中的中心 [x,y,z]；Transform 时提升到 world 作为相机焦点。
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 };
    // 最后一次共享状态等值面阈值，单位与输入标量一致。
    double m_currentIsoValue = 0.0;
    bool m_hasMask = false;
};
