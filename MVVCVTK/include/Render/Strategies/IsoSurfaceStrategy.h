#pragma once
#include "Render/Internal/IsoLodController.h"
#include "Render/Internal/IsoSurfaceProductBuilder.h"
#include "Render/Support/BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkCubeAxesActor.h>
#include <vtkRenderer.h>
#include <vtkType.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

// --- 策略 A: 等值面渲染 ---
class IsoSurfaceStrategy : public BaseVisualStrategy {
public:
    IsoSurfaceStrategy();
    explicit IsoSurfaceStrategy(
        std::shared_ptr<RenderStrategyServices> services);
    ~IsoSurfaceStrategy() override;

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
    bool SetProductCommit() override;
    RenderTransitionState GetTransitionState() const override;
    void SetFirstRenderDuration(std::uint64_t durationUs) noexcept override;
    vtkProp3D* GetMainProp() override;
    VolumeQuality GetQuality() const noexcept;
    std::array<int, 3> GetLodDimensions(
        VolumeQuality quality) const noexcept;
    std::array<int, 3> GetInputDimensions() const noexcept;
    std::array<int, 3> GetMaskDimensions() const noexcept;
private:
    class Mapper;
    struct AsyncState;
    RenderEffectTarget GetRenderEffectTarget() const override;
    void SetEffectBinding(RenderEffectBinding* binding) override;
    bool SetIsoInput(
        vtkSmartPointer<vtkDataObject> data,
        vtkSmartPointer<vtkImageData> validityMask);
    bool SetPolyInput(vtkSmartPointer<vtkDataObject> data);
    std::optional<IsoSurfaceBuildRequest> BuildRequest(
        std::uint64_t requestRevision,
        VolumeQuality requestedQuality,
        const std::array<int, 3>& outputDimensions,
        double isoValue,
        bool isPreview) const;
    bool StartProduct(IsoSurfaceBuildRequest request);
    bool SetProduct(
        const IsoSurfaceKey& key,
        const IsoSurfaceBuildResult& result,
        std::uint64_t cpuPrepareUs,
        bool isChannelReady);
    bool GetKeyCurrent(const IsoSurfaceKey& key) const;
    bool GetInputCurrent(
        vtkDataObject* data,
        vtkImageData* validityMask) const;
    void SetInputTimes(
        vtkDataObject* data,
        vtkImageData* validityMask);
    vtkMTimeType GetScalarTime(vtkImageData* image) const;
    std::uint64_t GetImageBytes(vtkImageData* image) const;
    std::uint64_t GetSystemMemoryBytes() const;
    unsigned int GetCpuThreadCount() const noexcept;
    // 等值面主 prop 与坐标轴 prop 均由策略强持有，并登记到基类 m_managedProps 统一挂载。
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    // actor 使用唯一 mapper；质量切换只事务替换 mapper 背后的完整候选管线。
    vtkSmartPointer<Mapper> m_mapper;
    std::unique_ptr<IsoLodController> m_lodController;
    std::shared_ptr<RenderResourceCoordinator> m_resources;
    std::shared_ptr<RenderTaskChannel> m_taskChannel;
    std::shared_ptr<AsyncState> m_asyncState;
    std::shared_ptr<const IsoSurfaceProduct> m_activeProduct;
    // 最近一次已经完整物化并提交的输入；失败候选不得覆盖这些强引用。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    vtkSmartPointer<vtkImageData> m_lastMask;
    // 最后一次共享状态等值面阈值，单位与输入标量一致。
    double m_currentIsoValue = 0.0;
    double m_appliedIsoValue = 0.0;
    vtkMTimeType m_inputMTime = 0;
    vtkMTimeType m_maskMTime = 0;
    vtkMTimeType m_inputScalarMTime = 0;
    vtkMTimeType m_maskScalarMTime = 0;
    std::array<int, 3> m_inputDimensions{};
    std::array<int, 3> m_maskDimensions{};
    RenderTransitionState m_transition;
    std::uint64_t m_requestRevision = 0;
    std::uint64_t m_autoTopologyRevision = 0;
    VolumeQuality m_appliedQuality = VolumeQuality::Auto;
    RenderInteractionPhase m_interactionPhase =
        RenderInteractionPhase::Still;
};
