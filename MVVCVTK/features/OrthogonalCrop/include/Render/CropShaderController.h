#pragma once

#include "OrthogonalCropTypes.h"
#include "Render/RenderEffect.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class vtkObject;
class vtkRenderer;
class vtkShaderProperty;

// 每个 Strategy/context 独占一个 controller；公共接口只交换值对象，GL 资源只在 Render hook 内访问。
class CropShaderController final {
public:
    explicit CropShaderController(RenderTargetKind targetKind);
    ~CropShaderController();

    CropShaderController(const CropShaderController&) = delete;
    CropShaderController& operator=(const CropShaderController&) = delete;

    bool SetShaderTarget(vtkObject* mapper, vtkShaderProperty* shaderProperty);
    bool SetCropParams(CropShaderPayload payload);
    RenderEffectState GetState() const;
    bool SetCropCommit(std::uint64_t revision);
    bool GetCropCommitReady(std::uint64_t revision) const;
    bool SetCropComplete(std::uint64_t revision);
    bool ClearCropCommit(std::uint64_t revision);
    bool ClearCropStage(std::uint64_t revision);
    bool ClearCropParams();
    bool SetLocalToInput(
        const std::array<double, 16>& localToInput);
    bool StartRender(vtkRenderer* renderer);
    bool StopRender();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// Feature 级效果根保存业务事务，只弱观察各 Strategy binding。
// GPU observer、texture 与 context 均由单目标 binding/controller 独占。
class CropShaderEffect final
    : public RenderEffect
    , public std::enable_shared_from_this<CropShaderEffect> {
public:
    CropShaderEffect();
    ~CropShaderEffect() override;

    CropShaderEffect(const CropShaderEffect&) = delete;
    CropShaderEffect& operator=(const CropShaderEffect&) = delete;

    bool SetCropParams(CropShaderPayload payload);
    RenderEffectState GetState() const;
    bool StartCropCommit(std::uint64_t revision);
    bool GetCropCommitReady(std::uint64_t revision) const;
    bool SetCropCommit(std::uint64_t revision);
    bool SetCropComplete(std::uint64_t revision);
    bool ClearCropCommit(std::uint64_t revision);
    bool ClearCropStage(std::uint64_t revision);
    bool ClearCropParams();

    std::shared_ptr<RenderEffectBinding> BuildEffectBinding(
        const RenderEffectTarget& target,
        RenderBindingUse bindingUse) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
