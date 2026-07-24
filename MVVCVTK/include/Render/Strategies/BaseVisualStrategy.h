#pragma once
#include "AppInterfaces.h"
#include "ImageProcessor.h"
#include <vtkProp.h>
#include <vtkProp3D.h>
#include <vtkMatrix4x4.h>
#include <vtkLookupTable.h>
#include <vtkImageResample.h>
#include <vtkWeakPointer.h>
#include <vector>
#include <algorithm>

class BaseVisualStrategy : public AbstractVisualStrategy {
protected:
    // 策略对全部可视 prop 的强引用 owner；renderer 在 Attach 后另持 VTK 引用，Detach 只解除挂载，不销毁本集合。
    std::vector<vtkSmartPointer<vtkProp>> m_managedProps;
    // GetDownsampledOutputPort 最近一次创建的 producer；返回端口在下次替换该成员或策略析构前有效。
    vtkSmartPointer<vtkImageResample> m_resampleFilter;
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

public:
    ~BaseVisualStrategy() override
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

    bool SetRenderInputStamp(const RenderInputStamp inputStamp) override
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

    bool SetRenderEffectUse(const RenderBindingUse bindingUse) override
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

    RenderEffectState GetRenderEffectState() const override
    {
        if (m_renderBinding) {
            return m_renderBinding->GetEffectState();
        }
        RenderEffectState state;
        state.status = RenderEffectStatus::Failed;
        state.failureReason = RenderEffectFailure::Unsupported;
        state.message = "The current visual strategy has no render-effect binding.";
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

    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) override
    {
        if (!renderer) {
            return;
        }
        if (m_effectRenderer
            && m_effectRenderer.GetPointer() != renderer.GetPointer()) {
            // renderer/window 换代必须先释放旧 context 的 binding；
            // 否则 Feature GPU 资源会被错误地迁移到新 OpenGL context。
            ClearRenderBinding();
            for (auto& prop : m_managedProps) {
                m_effectRenderer->RemoveViewProp(prop);
            }
        }
        // 基类只处理“把当前策略拥有的 prop 全部挂到 renderer 上”，
        // 具体背景色、相机或附加 VTK 关系由派生类继续补充。
        for (auto& prop : m_managedProps) {
            renderer->AddViewProp(prop);
        }
        m_effectRenderer = renderer;
        if (!m_renderEffect.expired() && !m_renderBinding) {
            (void)CreateRenderBinding();
        }
    }

    void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) override
    {
        if (!renderer) {
            return;
        }
        if (m_effectRenderer.GetPointer() == renderer.GetPointer()) {
            ClearRenderBinding();
            m_effectRenderer = nullptr;
        }
        for (auto& prop : m_managedProps) {
            renderer->RemoveViewProp(prop);
        }
    }

protected:
    void Set3DPropsTransform(const std::array<double, 16>& modelToWorldMatrixData)
    {
        // 对策略拥有的 3D prop 批量下发同一份模型矩阵，
        // 保证 actor、axes 等附属对象始终跟随同一个世界变换。
        for (auto prop : m_managedProps) {
            auto prop3D = vtkProp3D::SafeDownCast(prop);
            if (!prop3D) {
                continue; // 仅对 3D prop 应用模型矩阵，跳过文本等 2D prop
            }
            auto userMatrix = prop3D->GetUserMatrix();
            if (!userMatrix) {
                vtkSmartPointer<vtkMatrix4x4> modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
                modelToWorldMatrix->DeepCopy(modelToWorldMatrixData.data());
                prop3D->SetUserMatrix(modelToWorldMatrix);
            }
            else {
                userMatrix->DeepCopy(modelToWorldMatrixData.data());
            }
        }
    }

    vtkAlgorithmOutput* GetDownsampledOutputPort(vtkImageData* input, int targetDim = 766)
    {
        if (!input) {
            return nullptr;
        }
        // targetDim 是输出最大轴的目标体素数；降采样集中放在基类，避免 3D 策略复制同一逻辑。
        m_resampleFilter = ImageProcessor::GetDownsampledImage(input, targetDim);
        return m_resampleFilter ? m_resampleFilter->GetOutputPort() : nullptr;
    }

    void ClampImageBounds(int& x, int& y, int& z, const int dims[3])
    {
        if (!dims) {
            return;
        }
        x = std::max(0, std::min(x, dims[0] - 1));
        y = std::max(0, std::min(y, dims[1] - 1));
        z = std::max(0, std::min(z, dims[2] - 1));
    }
};
