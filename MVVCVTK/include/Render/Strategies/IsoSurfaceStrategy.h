#pragma once
#include "Render/Internal/IsoLodController.h"
#include "Render/Support/BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkCubeAxesActor.h>
#include <vtkRenderer.h>
#include <vtkType.h>

#include <array>
#include <cstdint>
#include <memory>

// --- 策略 A: 等值面渲染 ---
class IsoSurfaceStrategy : public BaseVisualStrategy {
public:
    IsoSurfaceStrategy();
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
    vtkProp3D* GetMainProp() override;
    VolumeQuality GetQuality() const noexcept;
    std::array<int, 3> GetLodDimensions(
        VolumeQuality quality) const noexcept;
    std::array<int, 3> GetInputDimensions() const noexcept;
    std::array<int, 3> GetMaskDimensions() const noexcept;
private:
    class Mapper;
    class MaskImplicit;
    struct Pipeline;
    RenderEffectTarget GetRenderEffectTarget() const override;
    void SetEffectBinding(RenderEffectBinding* binding) override;
    bool SetIsoInput(
        vtkSmartPointer<vtkDataObject> data,
        vtkSmartPointer<vtkImageData> validityMask);
    bool SetPolyInput(vtkSmartPointer<vtkDataObject> data);
    std::unique_ptr<Pipeline> BuildPipeline(
        vtkImageData* image,
        vtkImageData* validityMask,
        const std::array<int, 3>& outputDimensions,
        double isoValue) const;
    std::unique_ptr<Pipeline> BuildCandidate(
        vtkImageData* image,
        vtkImageData* validityMask,
        const std::array<int, 3>& outputDimensions,
        double isoValue) const;
    bool MaterializePipeline(Pipeline& pipeline) const;
    bool SetPipeline(std::unique_ptr<Pipeline> pipeline);
    bool GetInputCurrent(
        vtkDataObject* data,
        vtkImageData* validityMask) const;
    bool GetGeometryMatch(
        vtkImageData* image,
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
    std::unique_ptr<Pipeline> m_pipeline;
    // 最近一次已经完整物化并提交的输入；失败候选不得覆盖这些强引用。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    vtkSmartPointer<vtkImageData> m_lastMask;
    // 最后一次共享状态等值面阈值，单位与输入标量一致。
    double m_currentIsoValue = 0.0;
    vtkMTimeType m_inputMTime = 0;
    vtkMTimeType m_maskMTime = 0;
    vtkMTimeType m_inputScalarMTime = 0;
    vtkMTimeType m_maskScalarMTime = 0;
    std::array<int, 3> m_inputDimensions{};
    std::array<int, 3> m_maskDimensions{};
};
