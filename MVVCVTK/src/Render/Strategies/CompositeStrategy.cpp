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

bool CompositeStrategy::SetVisualState(
    const RenderParams& params,
    const UpdateFlags flags)
{
    if (!m_mainStrategy || !m_referencePlanes) return false;
    const auto referenceStage =
        m_referencePlanes->BuildVisualStage(params, flags);
    if (!referenceStage
        || !m_mainStrategy->SetVisualState(params, flags)) {
        return false;
    }

    const auto state = m_mainStrategy->GetTransitionState();
    if (state.status == RenderProductStatus::Failed
        || state.status == RenderProductStatus::Cancelled) {
        m_pendingReference.reset();
        return false;
    }
    if (state.status == RenderProductStatus::Preparing
        || state.status == RenderProductStatus::Ready) {
        if (state.stats.requestRevision == 0) return false;
        m_pendingReference = PendingReference{
            *referenceStage, state.stats.requestRevision
        };
        return true;
    }

    m_pendingReference.reset();
    return m_referencePlanes->SetVisualCommit(*referenceStage);
}

bool CompositeStrategy::SetProductCommit()
{
    if (!m_mainStrategy || !m_mainStrategy->SetProductCommit()) {
        m_pendingReference.reset();
        return false;
    }
    const auto state = m_mainStrategy->GetTransitionState();
    if (state.status == RenderProductStatus::Failed
        || state.status == RenderProductStatus::Cancelled) {
        m_pendingReference.reset();
        return state.status == RenderProductStatus::Cancelled;
    }
    if (!m_pendingReference) return true;
    if (state.status != RenderProductStatus::Active) return true;
    if (state.stats.activeRevision
        != m_pendingReference->requestRevision) {
        if (state.stats.activeRevision
            > m_pendingReference->requestRevision) {
            m_pendingReference.reset();
        }
        return true;
    }
    const auto stage = std::move(m_pendingReference->stage);
    m_pendingReference.reset();
    return m_referencePlanes
        && m_referencePlanes->SetVisualCommit(stage);
}

RenderTransitionState CompositeStrategy::GetTransitionState() const
{
    return m_mainStrategy
        ? m_mainStrategy->GetTransitionState()
        : RenderTransitionState{};
}

void CompositeStrategy::SetFirstRenderDuration(
    const std::uint64_t durationUs) noexcept
{
    if (m_mainStrategy) {
        m_mainStrategy->SetFirstRenderDuration(durationUs);
    }
}

bool CompositeStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data,
    vtkSmartPointer<vtkImageData> validityMask)
{
    if (!m_mainStrategy
        || !m_mainStrategy->SetInputData(data, validityMask)) {
        return false;
    }
    // 参考平面沿用无失败返回的窄 setter；不能在主体提交后再制造一个可失败分支。
    if (m_referencePlanes) {
        m_referencePlanes->SetInputData(data);
        m_referencePlanes->SetInputMask(validityMask);
    }
    m_lastInput = std::move(data);
    return true;
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
