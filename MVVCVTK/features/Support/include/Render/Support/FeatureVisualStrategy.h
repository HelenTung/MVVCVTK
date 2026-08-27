#pragma once

#include "Render/Contracts/VisualStrategy.h"

#include <vtkMatrix4x4.h>
#include <vtkProp.h>
#include <vtkProp3D.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// Feature 侧可复用的渲染策略骨架：只实现契约层的 prop 挂载、变换与 effect 生命周期，
// 不携带 Host 的数据处理、采样或具体渲染策略实现。
class FeatureVisualStrategy : public AbstractVisualStrategy {
protected:
    // 策略拥有全部可视 prop；renderer 在 Attach 后另持 VTK 引用。
    std::vector<vtkSmartPointer<vtkProp>> m_managedProps;
    std::weak_ptr<RenderEffect> m_renderEffect;
    std::shared_ptr<RenderEffectBinding> m_renderBinding;
    vtkWeakPointer<vtkRenderer> m_effectRenderer;
    RenderInputStamp m_renderInputStamp;
    RenderBindingUse m_bindingUse = RenderBindingUse::Current;

    void AttachProp(vtkSmartPointer<vtkProp> prop)
    {
        if (prop) {
            m_managedProps.push_back(prop);
        }
    }

    virtual RenderEffectTarget GetRenderEffectTarget() const
    {
        return {};
    }

    virtual void SetEffectBinding(RenderEffectBinding*)
    {
    }

    bool CreateRenderBinding()
    {
        auto effect = m_renderEffect.lock();
        if (!effect || !m_effectRenderer) {
            return false;
        }
        auto target = GetRenderEffectTarget();
        target.inputStamp = m_renderInputStamp;
        if (target.targetKind == RenderTargetKind::Unknown
            || !target.mapper
            || !target.shaderProperty) {
            return false;
        }
        auto binding = effect->BuildEffectBinding(target, m_bindingUse);
        if (!binding) {
            return false;
        }
        if (!binding->SetRenderInput(m_renderInputStamp)) {
            return false;
        }
        if (!binding->SetLocalToInput(target.localToInput)) {
            return false;
        }
        SetEffectBinding(binding.get());
        m_renderBinding = std::move(binding);
        return true;
    }

    void ClearRenderBinding()
    {
        if (m_renderBinding) {
            (void)m_renderBinding->OnRenderStop();
        }
        SetEffectBinding(nullptr);
        m_renderBinding.reset();
    }

    bool SetEffectLocalToInput(
        const std::array<double, 16>& localToInput)
    {
        return !m_renderBinding
            || m_renderBinding->SetLocalToInput(localToInput);
    }

    void Set3DPropsTransform(
        const std::array<double, 16>& modelToWorldMatrixData)
    {
        for (auto prop : m_managedProps) {
            auto prop3D = vtkProp3D::SafeDownCast(prop);
            if (!prop3D) {
                continue;
            }
            auto userMatrix = prop3D->GetUserMatrix();
            if (!userMatrix) {
                auto modelToWorldMatrix =
                    vtkSmartPointer<vtkMatrix4x4>::New();
                modelToWorldMatrix->DeepCopy(
                    modelToWorldMatrixData.data());
                prop3D->SetUserMatrix(modelToWorldMatrix);
            }
            else {
                userMatrix->DeepCopy(modelToWorldMatrixData.data());
            }
        }
    }

public:
    ~FeatureVisualStrategy() override
    {
        ClearRenderBinding();
    }

    bool AttachRenderEffect(
        std::shared_ptr<RenderEffect> effect,
        const RenderBindingUse bindingUse) override
    {
        if (!effect || !m_renderEffect.expired()) {
            return false;
        }
        m_renderEffect = effect;
        m_bindingUse = bindingUse;
        if (!m_effectRenderer) {
            return true;
        }
        if (CreateRenderBinding()) {
            return true;
        }
        m_renderEffect.reset();
        return false;
    }

    bool DetachRenderEffect(const RenderEffect* effect) override
    {
        auto currentEffect = m_renderEffect.lock();
        if (!effect || currentEffect.get() != effect) {
            return false;
        }
        ClearRenderBinding();
        m_renderEffect.reset();
        return true;
    }

    bool SetRenderInputStamp(RenderInputStamp inputStamp) override
    {
        if (m_renderInputStamp == inputStamp) {
            return true;
        }
        if (m_renderBinding
            && !m_renderBinding->SetRenderInput(inputStamp)) {
            return false;
        }
        m_renderInputStamp = inputStamp;
        return true;
    }

    RenderInputStamp GetRenderInputStamp() const override
    {
        return m_renderInputStamp;
    }

    bool SetRenderEffectUse(RenderBindingUse bindingUse) override
    {
        if (m_bindingUse == bindingUse) {
            return true;
        }
        if (m_renderBinding
            && !m_renderBinding->SetBindingUse(bindingUse)) {
            return false;
        }
        m_bindingUse = bindingUse;
        return true;
    }

    RenderBindingUse GetRenderEffectUse() const override
    {
        return m_bindingUse;
    }

    RenderEffectState GetRenderEffectState() const override
    {
        if (m_renderBinding) {
            return m_renderBinding->GetEffectState();
        }
        RenderEffectState state;
        state.status = RenderEffectStatus::Failed;
        state.failureReason = RenderEffectFailure::Unsupported;
        state.message =
            "The current visual strategy has no render-effect binding.";
        return state;
    }

    bool SetRenderEffectCommit(const std::uint64_t revision) override
    {
        return m_renderBinding
            && m_renderBinding->SetEffectCommit(revision);
    }

    bool ClearRenderEffectStage(const std::uint64_t revision) override
    {
        return !m_renderBinding
            || m_renderBinding->ClearEffectStage(revision);
    }

    void AttachRenderer(
        vtkSmartPointer<vtkRenderer> renderer) override
    {
        if (!renderer) {
            return;
        }
        if (m_effectRenderer
            && m_effectRenderer.GetPointer()
                != renderer.GetPointer()) {
            ClearRenderBinding();
            for (auto& prop : m_managedProps) {
                m_effectRenderer->RemoveViewProp(prop);
            }
        }
        for (auto& prop : m_managedProps) {
            renderer->AddViewProp(prop);
        }
        m_effectRenderer = renderer;
        if (!m_renderEffect.expired() && !m_renderBinding) {
            (void)CreateRenderBinding();
        }
    }

    void DetachRenderer(
        vtkSmartPointer<vtkRenderer> renderer) override
    {
        if (!renderer) {
            return;
        }
        if (m_effectRenderer.GetPointer()
            == renderer.GetPointer()) {
            ClearRenderBinding();
            m_effectRenderer = nullptr;
        }
        for (auto& prop : m_managedProps) {
            renderer->RemoveViewProp(prop);
        }
    }
};
