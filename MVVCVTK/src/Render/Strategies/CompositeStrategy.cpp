#include "CompositeStrategy.h"
#include "ColoredPlanesStrategy.h"

CompositeStrategy::CompositeStrategy(
    std::shared_ptr<AbstractVisualStrategy> mainStrategy)
    : m_mainStrategy(std::move(mainStrategy))
    , m_referencePlanes(std::make_shared<ColoredPlanesStrategy>())
{
}

void CompositeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    if (m_lastInput == data) {
        return;
    }
    m_lastInput = data;

    // 组合策略本身不产生新 VTK 对象，它只是把同一份输入同时分发给主内容和参考平面两部分。

    if (m_mainStrategy) {
        m_mainStrategy->SetInputData(data);
    }

    if (m_referencePlanes) {
        m_referencePlanes->SetInputData(data);
    }
}

void CompositeStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) {
    if (m_mainStrategy) m_mainStrategy->AttachRenderer(renderer);
    if (m_referencePlanes) m_referencePlanes->AttachRenderer(renderer);
}

void CompositeStrategy::DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) {
    if (m_mainStrategy) m_mainStrategy->DetachRenderer(renderer);
    if (m_referencePlanes) m_referencePlanes->DetachRenderer(renderer);
}

int CompositeStrategy::GetPlaneAxis(vtkActor* actor) {
    return m_referencePlanes->GetPlaneAxis(actor);
}

vtkProp3D* CompositeStrategy::GetMainProp()
{
    if (m_mainStrategy) return m_mainStrategy->GetMainProp();
    else return nullptr;
}

void CompositeStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    // 参考平面先同步，这样 3D 主内容与切片参照在同一帧里看到的是一致状态。
    if (m_referencePlanes) {
        m_referencePlanes->SetVisualState(params, flags);
    }

    // 更新主视图
    if (m_mainStrategy) {
        m_mainStrategy->SetVisualState(params, flags);
    }
}

void CompositeStrategy::SetInputMask(
    vtkSmartPointer<vtkImageData> validityMask)
{
    if (m_mainStrategy) {
        m_mainStrategy->SetInputMask(validityMask);
    }
    if (m_referencePlanes) {
        m_referencePlanes->SetInputMask(
            std::move(validityMask));
    }
}

bool CompositeStrategy::AttachRenderEffect(
    std::shared_ptr<RenderEffect> effect,
    const RenderBindingUse bindingUse)
{
    return m_mainStrategy
        && m_mainStrategy->AttachRenderEffect(
            std::move(effect), bindingUse);
}

bool CompositeStrategy::DetachRenderEffect(const RenderEffect* effect)
{
    return m_mainStrategy
        && m_mainStrategy->DetachRenderEffect(effect);
}

bool CompositeStrategy::SetRenderInputStamp(
    const RenderInputStamp inputStamp)
{
    return m_mainStrategy
        && m_mainStrategy->SetRenderInputStamp(inputStamp);
}

RenderInputStamp CompositeStrategy::GetRenderInputStamp() const
{
    return m_mainStrategy
        ? m_mainStrategy->GetRenderInputStamp()
        : BaseVisualStrategy::GetRenderInputStamp();
}

bool CompositeStrategy::SetRenderEffectUse(
    const RenderBindingUse bindingUse)
{
    return m_mainStrategy
        && m_mainStrategy->SetRenderEffectUse(bindingUse);
}

RenderBindingUse CompositeStrategy::GetRenderEffectUse() const
{
    return m_mainStrategy
        ? m_mainStrategy->GetRenderEffectUse()
        : BaseVisualStrategy::GetRenderEffectUse();
}

RenderEffectState CompositeStrategy::GetRenderEffectState() const
{
    return m_mainStrategy
        ? m_mainStrategy->GetRenderEffectState()
        : BaseVisualStrategy::GetRenderEffectState();
}

bool CompositeStrategy::SetRenderEffectCommit(
    const std::uint64_t revision)
{
    return m_mainStrategy
        && m_mainStrategy->SetRenderEffectCommit(revision);
}

bool CompositeStrategy::ClearRenderEffectStage(
    const std::uint64_t revision)
{
    return !m_mainStrategy
        || m_mainStrategy->ClearRenderEffectStage(revision);
}
