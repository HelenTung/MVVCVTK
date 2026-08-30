#pragma once
#include "Render/Support/BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkCubeAxesActor.h>
#include <vtkRenderer.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class vtkImageAnisotropicDiffusion3D;
class vtkImageResample;
class vtkColorTransferFunction;
class vtkPiecewiseFunction;
class VolumeLodController;

// --- 策略 B: 体渲染 ---
class VolumeStrategy : public BaseVisualStrategy {
public:
    VolumeStrategy();
    ~VolumeStrategy() override;

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    bool SetInputData(
        vtkSmartPointer<vtkDataObject> data,
        vtkSmartPointer<vtkImageData> validityMask) override;
    void SetInputMask(
        vtkSmartPointer<vtkImageData> validityMask) override;
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    bool SetVisualState(
        const RenderParams& params,
        UpdateFlags flags) override;
    vtkProp3D* GetMainProp() override; //
    std::uint64_t GetMapperInputCount() const noexcept
    {
        return m_mapperInputCount;
    }
    std::uint64_t GetResampleUpdateCount() const noexcept
    {
        return m_resampleUpdateCount;
    }
    std::uint64_t GetResampleBuildCount() const noexcept
    {
        return m_resampleBuildCount;
    }
    std::uint64_t GetLodPlanCount() const noexcept
    {
        return m_lodPlanCount;
    }
    std::array<int, 3> GetLodDimensions(
        VolumeQuality quality) const noexcept;
private:
    class Mapper;
    struct LodEntry;
    RenderEffectTarget GetRenderEffectTarget() const override;
    void SetEffectBinding(RenderEffectBinding* binding) override;
    // 与最后下发 OTF 的 m_opacity 比较，决定纯材质更新是否需要重建透明度函数。
    bool GetOpacityChanged(double opacity) const;
    std::array<int, 3> GetSourceDims() const;
    std::uint64_t GetImageBytes(vtkImageData* image) const;
    std::uint64_t GetSourceBytes() const;
    std::uint64_t GetLodBytes(const LodEntry& lod) const;
    std::uint64_t GetCacheBudget() const;
    std::uint64_t GetSystemMemoryBytes() const;
    std::uint64_t GetGpuMemoryBytes() const;
    unsigned int GetCpuThreadCount() const noexcept;
    bool GetQualityValid(VolumeQuality quality) const;
    bool GetInputKey(vtkImageData* image) const;
    bool GetMaskKey(vtkImageData* image) const;
    bool GetProducersReady() const;
    double GetQualityStep(
        const LodEntry& lod) const;
    bool BuildLodPlan();
    bool BuildDenoise();
    bool BuildPendingLod(
        const std::array<int, 3>& outputDimensions,
        double dimensionRatio);
    bool ClearPendingLod();
    bool SetTargetLod(
        const std::array<int, 3>& outputDimensions,
        double dimensionRatio);
    LodEntry* GetCachedLod(
        const std::array<int, 3>& outputDimensions,
        double dimensionRatio) const;
    bool SwitchLod(LodEntry& lod);
    bool SwitchPendingLod();
    bool RemoveUnusedLods();
    bool SetVolumeInput(
        vtkSmartPointer<vtkDataObject> data,
        vtkSmartPointer<vtkImageData> validityMask);
    bool SetMapperInput(const LodEntry& lod);
    bool SetMapperQuality(const LodEntry& lod);
    vtkSmartPointer<vtkColorTransferFunction> BuildColorTransfer(
        const RenderParams& params) const;
    vtkSmartPointer<vtkPiecewiseFunction> BuildOpacityTransfer(
        const RenderParams& params) const;
    bool SetInputKey(vtkImageData* image);
    bool SetMaskKey(vtkImageData* image);
    // 坐标轴与体渲染主 prop 均由策略强持有，并登记到 m_managedProps 统一挂载。
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    vtkSmartPointer<vtkVolume> m_volume;
    // volume 使用的唯一 GPU mapper；质量档位只影响内部 LOD 与采样策略。
    vtkSmartPointer<Mapper> m_mapper;
    std::unique_ptr<VolumeLodController> m_lodController;
    // cache 独占已触发档位；active 仅观察 cache 中稳定的 LodEntry 地址。
    // pending 完整构建后才进入 cache，GPU 纹理由正常 Render 惰性加载。
    std::vector<std::unique_ptr<LodEntry>> m_lodCache;
    LodEntry* m_activeLod = nullptr;
    std::unique_ptr<LodEntry> m_pendingLod;
    vtkSmartPointer<vtkImageAnisotropicDiffusion3D> m_denoiseFilter;
    // 最近一次有效输入的强引用和身份缓存；只避免重复绑定，不冻结 vtkImageData 内部内容。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    vtkSmartPointer<vtkImageData> m_lastMask;
    // 非拥有 renderer 弱引用，仅用于相机与 clipping range；renderer 销毁后自动为空。
    vtkWeakPointer<vtkRenderer> m_renderer;
    // 最后一次已折算进 OTF 的全局透明度，通常取 [0,1]；TF 重建或 opacity 更新时同步。
    double m_opacity = 1.0;
    // 当前输入在 input model 坐标中的中心 [x,y,z]；Transform 时提升到 world 作为相机焦点。
    double m_dataCenter[3] = { 0.0, 0.0, 0.0 };
    vtkMTimeType m_inputMTime = 0;
    vtkMTimeType m_maskMTime = 0;
    std::array<int, 6> m_inputExtent{};
    std::array<int, 6> m_maskExtent{};
    std::array<double, 3> m_inputSpacing{};
    std::array<double, 3> m_maskSpacing{};
    std::uint64_t m_dataVersion = 0;
    std::uint64_t m_maskVersion = 0;
    VolumeQuality m_quality = VolumeQuality::Auto;
    bool m_isDenoiseOn = false;
    bool m_isProducerDenoiseOn = false;
    bool m_isInteracting = false;
    // 保存用户期望的静止材质；交互期可临时 ShadeOff，退出后精确恢复。
    bool m_isShadeOn = false;
    std::uint64_t m_lodPlanCount = 0;
    std::uint64_t m_lodUseStamp = 0;
    std::uint64_t m_mapperInputCount = 0;
    std::uint64_t m_resampleBuildCount = 0;
    std::uint64_t m_resampleUpdateCount = 0;
};
