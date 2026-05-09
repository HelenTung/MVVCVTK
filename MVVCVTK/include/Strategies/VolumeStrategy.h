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
    double GetSampleDistance(const double spacing[3]) const;
    void SetSampleDistance(bool isInteracting);
    bool GetOpacityChanged(double opacity) const;
    void SetCameraAligned(const std::array<double, 16>& modelMatrix);
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes; // 坐标轴
    vtkSmartPointer<vtkVolume> m_volume;
    vtkSmartPointer<vtkSmartVolumeMapper> m_mapper; // 体渲染 mapper，保持单实例复用，避免频繁重建
    vtkSmartPointer<vtkDataObject> m_lastInput;
    vtkWeakPointer<vtkRenderer> m_renderer;
    double m_staticSampleDistance = 1.0;      // 静止状态采样步长，按加载后的 spacing 一次计算
    double m_interactionSampleDistance = 2.0; // 交互状态采样步长，放大步长以降低交互期开销
    double m_opacity = 1.0;                   // 当前已下发到 OTF 的全局透明度，避免纯光照更新时重复改 OTF
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 };
};