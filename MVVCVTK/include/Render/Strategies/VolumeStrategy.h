#pragma once
#include "BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkCubeAxesActor.h>
#include <vtkImageResample.h>
#include <vtkRenderer.h>
#include <vtkGPUVolumeRayCastMapper.h>

// --- 策略 B: 体渲染 ---
class VolumeStrategy : public BaseVisualStrategy {
public:
    VolumeStrategy();

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    void SetCamera(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    vtkProp3D* GetMainProp() override; //
private:
    // 与最后下发 OTF 的 m_opacity 比较，决定纯材质更新是否需要重建透明度函数。
    bool GetOpacityChanged(double opacity) const;
    // modelMatrix 按 input model -> world 解释；相机保持原观察偏移，只把焦点移到变换后数据中心。
    void AlignCamera(const std::array<double, 16>& modelMatrix);
    // 坐标轴与体渲染主 prop 均由策略强持有，并登记到 m_managedProps 统一挂载。
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    vtkSmartPointer<vtkVolume> m_volume;
    // volume 使用的唯一 GPU mapper；质量/交互输入切换及裁切预览都写入该对象。
    vtkSmartPointer<vtkGPUVolumeRayCastMapper> m_mapper;
    // 双重采样 producer 的强引用：静止期最大轴上限 766，交互期最大轴上限 256。
    vtkSmartPointer<vtkImageResample> m_qualityResample;
    vtkSmartPointer<vtkImageResample> m_interactionResample;
    // 最近一次有效输入的强引用和身份缓存；只避免重复绑定，不冻结 vtkImageData 内部内容。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    // 非拥有 renderer 弱引用，仅用于相机与 clipping range；renderer 销毁后自动为空。
    vtkWeakPointer<vtkRenderer> m_renderer;
    // 最后一次已折算进 OTF 的全局透明度，通常取 [0,1]；TF 重建或 opacity 更新时同步。
    double m_opacity = 1.0;
    // 当前输入在 input model 坐标中的中心 [x,y,z]；Transform 时提升到 world 作为相机焦点。
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 };
    // ImageProcessor 的最大轴目标体素数；不表示三个轴都固定为该尺寸。
    int m_qualityTargetDim = 766;
    int m_interactionTargetDim = 256;
    // 双管线选择状态：true 使用交互输入，false 使用质量输入；绑定新图像时重置为 false。
    bool m_isInteracting = false;
};
