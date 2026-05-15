#pragma once
#include "BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkCubeAxesActor.h>
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
    double GetSampleDistance(const double spacing[3]) const; // 从 spacing 推导体渲染基础采样步长
    void SetSampleDistance(bool isInteracting); // 按交互态切换采样步长，在质量与帧率之间折中
    bool GetOpacityChanged(double opacity) const; // 判断是否只需更新 OTF 透明度而无需重建整套 TF
    void SetCameraAligned(const std::array<double, 16>& modelMatrix); // 模型变换后保持相机相对观察关系不突变
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes; // 坐标轴
    vtkSmartPointer<vtkVolume> m_volume; // 体渲染主 prop
    vtkSmartPointer<vtkSmartVolumeMapper> m_mapper; // 体渲染 mapper，保持单实例复用，避免频繁重建
    vtkSmartPointer<vtkDataObject> m_lastInput; // 最近一次绑定的数据快照，避免重复喂同一输入
    vtkWeakPointer<vtkRenderer> m_renderer; // 仅弱引用 renderer，用于更新相机与裁剪面
    double m_staticSampleDistance = 1.0;      // 静止状态采样步长，按加载后的 spacing 一次计算
    double m_interactionSampleDistance = 2.0; // 交互状态采样步长，放大步长以降低交互期开销
    double m_opacity = 1.0;                   // 当前已下发到 OTF 的全局透明度，避免纯光照更新时重复改 OTF
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 }; // 数据局部中心，模型变换后用于重新对齐相机焦点
};