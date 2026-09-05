#pragma once
#include "Render/Internal/VolumeLodProductBuilder.h"
#include "Render/Support/BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkCubeAxesActor.h>
#include <vtkRenderer.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <mutex>

class vtkColorTransferFunction;
class vtkPiecewiseFunction;
class VolumeLodController;

// --- 策略 B: 体渲染 ---
class VolumeStrategy : public BaseVisualStrategy {
public:
    VolumeStrategy();
    explicit VolumeStrategy(
        std::shared_ptr<RenderStrategyServices> services);
    ~VolumeStrategy() override;

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    bool SetInputData(
        vtkSmartPointer<vtkDataObject> data,
        vtkSmartPointer<vtkImageData> validityMask) override;
    void SetInputMask(
        vtkSmartPointer<vtkImageData> validityMask) override;
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) override;
    bool SetVisualState(
        const RenderParams& params,
        UpdateFlags flags) override;
    bool SetProductCommit() override;
    RenderTransitionState GetTransitionState() const override;
    void SetFirstRenderDuration(std::uint64_t durationUs) noexcept override;
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
    std::array<unsigned short, 3> GetGpuPartitions() const noexcept;
    std::uint64_t GetGpuReleaseCount() const noexcept
    {
        return m_gpuReleaseCount;
    }
    std::uint64_t GetGpuPreloadCount() const noexcept
    {
        return m_gpuPreloadCount;
    }
    std::uint64_t GetGpuQueryCount() const noexcept
    {
        return m_gpuQueryCount;
    }
private:
    class Mapper;
    struct LodEntry;
    struct AsyncState;
    RenderEffectTarget GetRenderEffectTarget() const override;
    void SetEffectBinding(RenderEffectBinding* binding) override;
    // 与最后下发 OTF 的 m_opacity 比较，决定纯材质更新是否需要重建透明度函数。
    bool GetOpacityChanged(double opacity) const;
    std::array<int, 3> GetSourceDims() const;
    std::uint64_t GetImageBytes(vtkImageData* image) const;
    std::uint64_t GetSourceBytes() const;
    std::uint64_t GetLodTextureBytes(const LodEntry& lod) const;
    std::uint64_t GetLodBlockBytes(
        const LodEntry& lod,
        const std::array<unsigned short, 3>& partitions) const;
    std::optional<std::array<unsigned short, 3>> GetLodPartitions(
        const LodEntry& lod,
        std::uint64_t blockBudget) const;
    std::uint64_t GetSystemMemoryBytes() const;
    std::uint64_t GetGpuMemoryBytes() const;
    std::uint64_t GetGpuBlockBudget(
        std::optional<std::uint64_t> freeBytes) const;
    // 独立成虚函数只为隔离厂商显存查询，使释放后取样顺序可重复测试。
    virtual std::optional<std::uint64_t> GetGpuFreeBytes() const;
    unsigned int GetCpuThreadCount() const noexcept;
    bool GetQualityValid(VolumeQuality quality) const;
    bool GetInputCurrent(
        vtkDataObject* data,
        vtkImageData* validityMask) const;
    bool GetKeyCurrent(const VolumeLodKey& key) const;
    void SetInputTimes(
        vtkDataObject* data,
        vtkImageData* validityMask);
    vtkMTimeType GetScalarTime(vtkImageData* image) const;
    double GetDenoiseThreshold(vtkImageData* image) const;
    double GetQualityStep(
        const LodEntry& lod) const;
    bool BuildLodPlan();
    std::optional<VolumeLodBuildRequest> BuildRequest(
        std::uint64_t requestRevision,
        VolumeQuality requestedQuality,
        const std::array<int, 3>& outputDimensions,
        bool isDenoiseOn) const;
    bool StartProduct(
        VolumeLodBuildRequest request,
        double dimensionRatio);
    bool SetProduct(
        const VolumeLodKey& key,
        const VolumeLodBuildResult& result,
        double dimensionRatio,
        std::uint64_t cpuPrepareUs,
        bool isChannelReady);
    std::unique_ptr<LodEntry> BuildLodEntry(
        std::shared_ptr<const VolumeLodProduct> product,
        double dimensionRatio,
        VolumeQuality requestedQuality) const;
    bool SwitchLod(
        std::unique_ptr<LodEntry> lod,
        std::uint64_t& gpuReleaseUs,
        std::uint64_t& gpuUploadUs);
    bool SetVolumeInput(
        vtkSmartPointer<vtkDataObject> data,
        vtkSmartPointer<vtkImageData> validityMask);
    bool SetGpuPartitions(
        const std::array<unsigned short, 3>& partitions);
    bool SetMapperInput(const LodEntry& lod);
    bool SetMapperQuality(const LodEntry& lod);
    bool ClearGpuInput();
    bool BuildGpuInput(
        const std::array<unsigned short, 3>& partitions);
    vtkSmartPointer<vtkColorTransferFunction> BuildColorTransfer(
        const RenderParams& params) const;
    vtkSmartPointer<vtkPiecewiseFunction> BuildOpacityTransfer(
        const RenderParams& params) const;
    // 坐标轴与体渲染主 prop 均由策略强持有，并登记到 m_managedProps 统一挂载。
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    vtkSmartPointer<vtkVolume> m_volume;
    // volume 使用的唯一 GPU mapper；质量档位只影响内部 LOD 与采样策略。
    vtkSmartPointer<Mapper> m_mapper;
    std::unique_ptr<VolumeLodController> m_lodController;
    std::shared_ptr<RenderResourceCoordinator> m_resources;
    std::shared_ptr<RenderTaskChannel> m_taskChannel;
    std::shared_ptr<AsyncState> m_asyncState;
    // 只保留已提交的不可变 CPU 产品；候选由 worker mailbox 暂存。
    std::unique_ptr<LodEntry> m_activeLod;
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
    vtkMTimeType m_inputScalarMTime = 0;
    vtkMTimeType m_maskScalarMTime = 0;
    std::array<int, 6> m_inputExtent{};
    std::array<int, 6> m_maskExtent{};
    std::array<double, 3> m_inputSpacing{};
    std::array<double, 3> m_maskSpacing{};
    VolumeQuality m_quality = VolumeQuality::Low;
    VolumeQuality m_appliedQuality = VolumeQuality::Auto;
    bool m_isDenoiseOn = false;
    bool m_isInteracting = false;
    // 保存用户期望的静止材质；交互期可临时 ShadeOff，退出后精确恢复。
    bool m_isShadeOn = false;
    std::uint64_t m_lodPlanCount = 0;
    std::uint64_t m_autoTopologyRevision = 0;
    RenderTransitionState m_transition;
    std::uint64_t m_requestRevision = 0;
    RenderInteractionPhase m_interactionPhase =
        RenderInteractionPhase::Still;
    std::uint64_t m_mapperInputCount = 0;
    std::uint64_t m_resampleBuildCount = 0;
    std::uint64_t m_resampleUpdateCount = 0;
    std::uint64_t m_gpuReleaseCount = 0;
    std::uint64_t m_gpuPreloadCount = 0;
    std::uint64_t m_gpuQueryCount = 0;
};
