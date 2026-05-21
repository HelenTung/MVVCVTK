#pragma once
#include "BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkCubeAxesActor.h>
#include <vtkImageResample.h>
#include <vtkRenderer.h>
#include <vtkSmartVolumeMapper.h>

// --- 策略 B: 体渲染 ---
class VolumeStrategy : public BaseVisualStrategy {
public:
    VolumeStrategy();

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetRendererAttached(vtkSmartPointer<vtkRenderer> renderer);
    void SetCameraConfigured(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    vtkProp3D* GetMainProp() override; //
private:
    bool GetOpacityChanged(double opacity) const; // 判断是否只需更新 OTF 透明度而无需重建整套 TF
    void SetCameraAligned(const std::array<double, 16>& modelMatrix); // 模型变换后保持相机相对观察关系不突变
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes; // 坐标轴
    vtkSmartPointer<vtkVolume> m_volume; // 体渲染主 prop
    vtkSmartPointer<vtkSmartVolumeMapper> m_mapper; // 体渲染 mapper，保持单实例复用，避免频繁重建
    vtkSmartPointer<vtkImageResample> m_qualityResample; // 静止期 766 分辨率输入
    vtkSmartPointer<vtkImageResample> m_interactionResample; // 交互期 256 分辨率输入
    vtkSmartPointer<vtkDataObject> m_lastInput; // 最近一次绑定的数据快照，避免重复喂同一输入
    vtkWeakPointer<vtkRenderer> m_renderer; // 仅弱引用 renderer，用于更新相机与裁剪面
    double m_opacity = 1.0;                   // 当前已下发到 OTF 的全局透明度，避免纯光照更新时重复改 OTF
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 }; // 数据局部中心，模型变换后用于重新对齐相机焦点
    int m_qualityTargetDim = 766; // 静止期目标分辨率，保留为成员方便后续按策略或配置调整
    int m_interactionTargetDim = 256; // 交互期目标分辨率，保留为成员方便后续按策略或配置调整
    bool m_isInteracting = false; // 当前是否处于交互状态
};
