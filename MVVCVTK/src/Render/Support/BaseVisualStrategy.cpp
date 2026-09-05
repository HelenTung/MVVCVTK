#include "Render/Support/BaseVisualStrategy.h"

#include "Data/ImageProcessor.h"

#include <vtkMatrix4x4.h>
#include <vtkProp3D.h>

#include <algorithm>
#include <utility>

void BaseVisualStrategy::AttachProp(
    vtkSmartPointer<vtkProp> prop)
{
    if (prop) m_managedProps.push_back(std::move(prop));
}

RenderEffectTarget
BaseVisualStrategy::GetRenderEffectTarget() const
{
    return {};
}

void BaseVisualStrategy::SetEffectBinding(
    RenderEffectBinding*)
{
}

bool BaseVisualStrategy::CreateRenderBinding()
{
    auto effect = m_renderEffect.lock();
    if (!effect || !m_effectRenderer) return false;
    auto target = GetRenderEffectTarget();
    target.inputStamp = m_renderInputStamp;
    if (target.targetKind == RenderTargetKind::Unknown
        || !target.mapper || !target.shaderProperty) {
        return false;
    }
    auto binding = effect->BuildEffectBinding(
        target, m_bindingUse);
    if (!binding
        || !binding->SetRenderInput(m_renderInputStamp)
        || !binding->SetLocalToInput(target.localToInput)) {
        return false;
    }
    SetEffectBinding(binding.get());
    m_renderBinding = std::move(binding);
    return true;
}

void BaseVisualStrategy::ClearRenderBinding()
{
    if (m_renderBinding) {
        (void)m_renderBinding->OnRenderStop();
    }
    SetEffectBinding(nullptr);
    m_renderBinding.reset();
}

bool BaseVisualStrategy::SetEffectLocalToInput(
    const std::array<double, 16>& localToInput)
{
    return !m_renderBinding
        || m_renderBinding->SetLocalToInput(localToInput);
}

void BaseVisualStrategy::Set3DPropsTransform(
    const std::array<double, 16>& modelToWorld)
{
    for (const auto& prop : m_managedProps) {
        auto* prop3D = vtkProp3D::SafeDownCast(prop);
        if (!prop3D) continue;
        auto* matrix = prop3D->GetUserMatrix();
        if (matrix) {
            matrix->DeepCopy(modelToWorld.data());
            continue;
        }
        auto nextMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
        nextMatrix->DeepCopy(modelToWorld.data());
        prop3D->SetUserMatrix(nextMatrix);
    }
}

vtkAlgorithmOutput* BaseVisualStrategy::GetScaledOutputPort(
    vtkImageData* input,
    const double dimensionRatio)
{
    if (!input) return nullptr;
    m_resampleFilter = ImageProcessor::CreateScaledImage(
        input, dimensionRatio);
    return m_resampleFilter
        ? m_resampleFilter->GetOutputPort() : nullptr;
}

void BaseVisualStrategy::ClampImageBounds(
    int& x,
    int& y,
    int& z,
    const int dims[3])
{
    if (!dims) return;
    x = std::max(0, std::min(x, dims[0] - 1));
    y = std::max(0, std::min(y, dims[1] - 1));
    z = std::max(0, std::min(z, dims[2] - 1));
}

BaseVisualStrategy::~BaseVisualStrategy()
{
    ClearRenderBinding();
}

bool BaseVisualStrategy::SetProductCommit()
{
    return true;
}

RenderTransitionState
BaseVisualStrategy::GetTransitionState() const
{
    return {};
}

bool BaseVisualStrategy::AttachRenderEffect(
    std::shared_ptr<RenderEffect> effect,
    const RenderBindingUse bindingUse)
{
    if (!effect || !m_renderEffect.expired()) return false;
    m_renderEffect = effect;
    m_bindingUse = bindingUse;
    if (!m_effectRenderer) return true;
    if (CreateRenderBinding()) return true;
    m_renderEffect.reset();
    return false;
}

bool BaseVisualStrategy::DetachRenderEffect(
    const RenderEffect* effect)
{
    const auto currentEffect = m_renderEffect.lock();
    if (!effect || currentEffect.get() != effect) return false;
    ClearRenderBinding();
    m_renderEffect.reset();
    return true;
}

bool BaseVisualStrategy::SetRenderInputStamp(
    const RenderInputStamp inputStamp)
{
    if (m_renderInputStamp == inputStamp) return true;
    if (m_renderBinding
        && !m_renderBinding->SetRenderInput(inputStamp)) {
        return false;
    }
    m_renderInputStamp = inputStamp;
    return true;
}

RenderInputStamp
BaseVisualStrategy::GetRenderInputStamp() const
{
    return m_renderInputStamp;
}

bool BaseVisualStrategy::SetRenderEffectUse(
    const RenderBindingUse bindingUse)
{
    if (m_bindingUse == bindingUse) return true;
    if (m_renderBinding
        && !m_renderBinding->SetBindingUse(bindingUse)) {
        return false;
    }
    m_bindingUse = bindingUse;
    return true;
}

RenderBindingUse BaseVisualStrategy::GetRenderEffectUse() const
{
    return m_bindingUse;
}

RenderEffectState
BaseVisualStrategy::GetRenderEffectState() const
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

bool BaseVisualStrategy::SetRenderEffectCommit(
    const std::uint64_t revision)
{
    return m_renderBinding
        && m_renderBinding->SetEffectCommit(revision);
}

bool BaseVisualStrategy::ClearRenderEffectStage(
    const std::uint64_t revision)
{
    return !m_renderBinding
        || m_renderBinding->ClearEffectStage(revision);
}

void BaseVisualStrategy::AttachRenderer(
    vtkSmartPointer<vtkRenderer> renderer)
{
    if (!renderer) return;
    if (m_effectRenderer
        && m_effectRenderer.GetPointer()
            != renderer.GetPointer()) {
        ClearRenderBinding();
        for (const auto& prop : m_managedProps) {
            m_effectRenderer->RemoveViewProp(prop);
        }
    }
    for (const auto& prop : m_managedProps) {
        renderer->AddViewProp(prop);
    }
    m_effectRenderer = renderer;
    if (!m_renderEffect.expired() && !m_renderBinding) {
        (void)CreateRenderBinding();
    }
}

void BaseVisualStrategy::DetachRenderer(
    vtkSmartPointer<vtkRenderer> renderer)
{
    if (!renderer) return;
    if (m_effectRenderer.GetPointer()
        == renderer.GetPointer()) {
        ClearRenderBinding();
        m_effectRenderer = nullptr;
    }
    for (const auto& prop : m_managedProps) {
        renderer->RemoveViewProp(prop);
    }
}
