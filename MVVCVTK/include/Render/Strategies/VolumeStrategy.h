#pragma once
#include "BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkCubeAxesActor.h>
#include <vtkImageResample.h>
#include <vtkRenderer.h>

class vtkImageAnisotropicDiffusion3D;

// --- 策略 B: 体渲染 ---
class VolumeStrategy : public BaseVisualStrategy {
public:
    VolumeStrategy();
    ~VolumeStrategy() override;

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetInputMask(
        vtkSmartPointer<vtkImageData> validityMask) override;
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    void SetCamera(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    vtkProp3D* GetMainProp() override; //
private:
    class Mapper;
    RenderEffectTarget GetRenderEffectTarget() const override;
    void SetEffectBinding(RenderEffectBinding* binding) override;
    // 与最后下发 OTF 的 m_opacity 比较，决定纯材质更新是否需要重建透明度函数。
    bool GetOpacityChanged(double opacity) const;
    // modelMatrix 按 input model -> world 解释；相机保持原观察偏移，只把焦点移到变换后数据中心。
    void AlignCamera(const std::array<double, 16>& modelMatrix);
    int GetCustomDim() const;
    bool GetProducersReady() const;
    bool GetMasksReady(int customTargetDim) const;
    double GetQualityStep(
        vtkImageResample* qualityResample) const;
    bool BuildProducers();
    bool BuildMasks(int customTargetDim);
    bool SetMapperInput();
    bool SetMapperQuality();
    // 坐标轴与体渲染主 prop 均由策略强持有，并登记到 m_managedProps 统一挂载。
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    vtkSmartPointer<vtkVolume> m_volume;
    // volume 使用的唯一 GPU mapper；Feature 只在 Quality 与 Custom 缓存间切换连接。
    vtkSmartPointer<Mapper> m_mapper;
    // 两档 producer 的强引用：Quality 最大轴 766，Custom 使用调用方目标尺寸。
    vtkSmartPointer<vtkImageResample> m_qualityResample;
    vtkSmartPointer<vtkImageResample> m_customResample;
    vtkSmartPointer<vtkImageResample> m_qualityMask;
    vtkSmartPointer<vtkImageResample> m_customMask;
    vtkSmartPointer<vtkImageAnisotropicDiffusion3D> m_denoiseFilter;
    // 最近一次有效输入的强引用和身份缓存；只避免重复绑定，不冻结 vtkImageData 内部内容。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    vtkSmartPointer<vtkImageData> m_lastMask;
    // 已构建管线的缓存键与期望输入分离，配置变化时先比较结构键，再决定是否重建。
    vtkSmartPointer<vtkDataObject> m_producerInput;
    vtkSmartPointer<vtkImageData> m_maskInput;
    // 非拥有 renderer 弱引用，仅用于相机与 clipping range；renderer 销毁后自动为空。
    vtkWeakPointer<vtkRenderer> m_renderer;
    // 最后一次已折算进 OTF 的全局透明度，通常取 [0,1]；TF 重建或 opacity 更新时同步。
    double m_opacity = 1.0;
    // 当前输入在 input model 坐标中的中心 [x,y,z]；Transform 时提升到 world 作为相机焦点。
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 };
    // ImageProcessor 的最大轴目标体素数；不表示三个轴都固定为该尺寸。
    int m_qualityTargetDim = 0;
    int m_customTargetDim = 0;
    int m_qualityMaskDim = 0;
    int m_customMaskDim = 0;
    VolumeQualityParams m_quality;
    bool m_isDenoiseOn = false;
    bool m_isProducerDenoiseOn = false;
    // Feature 活跃时锁定 Quality producer；m_quality 始终保留调用方配置，供退出时恢复。
    bool m_isFeatureActive = false;
};
