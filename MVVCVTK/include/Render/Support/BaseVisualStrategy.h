#pragma once

#include "Render/Contracts/VisualStrategy.h"

#include <vtkImageResample.h>
#include <vtkProp.h>
#include <vtkWeakPointer.h>

#include <array>
#include <memory>
#include <vector>

// Host 内部主体策略基类；Feature overlay 不继承或接触该完整渲染契约。
class BaseVisualStrategy : public AbstractVisualStrategy {
protected:
    std::vector<vtkSmartPointer<vtkProp>> m_managedProps;
    std::weak_ptr<RenderEffect> m_renderEffect;
    std::shared_ptr<RenderEffectBinding> m_renderBinding;
    vtkWeakPointer<vtkRenderer> m_effectRenderer;
    RenderInputStamp m_renderInputStamp;
    RenderBindingUse m_bindingUse = RenderBindingUse::Current;
    vtkSmartPointer<vtkImageResample> m_resampleFilter;

    void AttachProp(vtkSmartPointer<vtkProp> prop);
    virtual RenderEffectTarget GetRenderEffectTarget() const;
    virtual void SetEffectBinding(RenderEffectBinding* binding);
    bool CreateRenderBinding();
    void ClearRenderBinding();
    bool SetEffectLocalToInput(
        const std::array<double, 16>& localToInput);
    void Set3DPropsTransform(
        const std::array<double, 16>& modelToWorld);
    vtkAlgorithmOutput* GetScaledOutputPort(
        vtkImageData* input,
        double dimensionRatio);
    void ClampImageBounds(
        int& x,
        int& y,
        int& z,
        const int dims[3]);

public:
    ~BaseVisualStrategy() override;

    bool AttachRenderEffect(
        std::shared_ptr<RenderEffect> effect,
        RenderBindingUse bindingUse) override;
    bool DetachRenderEffect(const RenderEffect* effect) override;
    bool SetRenderInputStamp(RenderInputStamp inputStamp) override;
    RenderInputStamp GetRenderInputStamp() const override;
    bool SetRenderEffectUse(RenderBindingUse bindingUse) override;
    RenderBindingUse GetRenderEffectUse() const override;
    RenderEffectState GetRenderEffectState() const override;
    bool SetRenderEffectCommit(std::uint64_t revision) override;
    bool ClearRenderEffectStage(std::uint64_t revision) override;
    void AttachRenderer(
        vtkSmartPointer<vtkRenderer> renderer) override;
    void DetachRenderer(
        vtkSmartPointer<vtkRenderer> renderer) override;
};
