#include "DefaultNavigationPolicy.h"

#include <cstdint>
#include <memory>

namespace {

void MergeResult(
    InteractionResult& aggregate,
    const InteractionResult& result) noexcept
{
    aggregate.isHandled = aggregate.isHandled || result.isHandled;
    aggregate.isPropagationStopped =
        aggregate.isPropagationStopped || result.isPropagationStopped;
    if (!result.isSucceeded) {
        aggregate.isSucceeded = false;
        if (aggregate.failureReason == InteractionFailureReason::None) {
            aggregate.failureReason = result.failureReason;
        }
    }
}

InteractionEventKind GetReleaseKind(
    InteractionEventKind pressKind) noexcept
{
    return pressKind == InteractionEventKind::SecondaryPress
        ? InteractionEventKind::SecondaryRelease
        : InteractionEventKind::PrimaryRelease;
}

class NavigationCapture final : public IInteractionCapture
{
public:
    NavigationCapture(
        IInteractionHandler& handler,
        InteractionEventKind releaseKind) noexcept
        : m_handler(handler)
        , m_releaseKind(releaseKind)
    {
    }

    InteractionCaptureKey GetKey() const noexcept override
    {
        return {
            0,
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(&m_handler)) };
    }

    InteractionEventKind GetReleaseKind() const noexcept override
    {
        return m_releaseKind;
    }

    InteractionResult Send(const InteractionEvent& event) override
    {
        return m_handler.Send(event);
    }

private:
    IInteractionHandler& m_handler;
    InteractionEventKind m_releaseKind;
};

InteractionResult GetAllocationFailure() noexcept
{
    return {
        true,
        true,
        false,
        InteractionFailureReason::StateRejected };
}

} // namespace

DefaultNavigationPolicy::DefaultNavigationPolicy(
    InteractionStatePort* statePort,
    SliceInputPort* slicePort,
    ModelInputPort* modelPort,
    RenderUpdatePort* updatePort,
    vtkPropPicker* picker,
    vtkRenderer* renderer)
    : m_viewer2D(
        statePort, slicePort, modelPort, updatePort, picker, renderer)
    , m_viewer3D(
        statePort, slicePort, modelPort, updatePort, picker, renderer)
{
}

InteractionResult DefaultNavigationPolicy::Send(
    const InteractionEvent& event)
{
    InteractionResult aggregate;
    const auto from2D = m_viewer2D.Send(event);
    MergeResult(aggregate, from2D);
    if (event.eventKind != InteractionEventKind::Cancel
        && from2D.isHandled) {
        return aggregate;
    }

    const auto from3D = m_viewer3D.Send(event);
    MergeResult(aggregate, from3D);
    return aggregate;
}

InteractionDispatch DefaultNavigationPolicy::Route(
    const InteractionEvent& event)
{
    const bool isPress =
        event.eventKind == InteractionEventKind::PrimaryPress
        || event.eventKind == InteractionEventKind::SecondaryPress;
    if (!isPress) {
        return { Send(event), nullptr };
    }

    std::unique_ptr<IInteractionCapture> capture;
    try {
        capture = std::make_unique<NavigationCapture>(
            m_viewer2D, GetReleaseKind(event.eventKind));
    }
    catch (...) {
        return { GetAllocationFailure(), nullptr };
    }

    InteractionResult aggregate;
    const auto from2D = m_viewer2D.Send(event);
    MergeResult(aggregate, from2D);
    if (from2D.isHandled) {
        return {
            aggregate,
            from2D.isSucceeded ? std::move(capture) : nullptr };
    }

    try {
        capture = std::make_unique<NavigationCapture>(
            m_viewer3D, GetReleaseKind(event.eventKind));
    }
    catch (...) {
        return { GetAllocationFailure(), nullptr };
    }
    const auto from3D = m_viewer3D.Send(event);
    MergeResult(aggregate, from3D);
    return {
        aggregate,
        from3D.isHandled && from3D.isSucceeded
            ? std::move(capture) : nullptr };
}
