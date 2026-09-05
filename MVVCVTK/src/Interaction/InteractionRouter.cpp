#include "InteractionRouter.h"

namespace {

class DispatchGuard final
{
public:
    explicit DispatchGuard(std::size_t& depth) noexcept
        : m_depth(depth)
    {
        ++m_depth;
    }

    ~DispatchGuard()
    {
        --m_depth;
    }

private:
    std::size_t& m_depth;
};

bool GetIsPointerEvent(InteractionEventKind eventKind) noexcept
{
    switch (eventKind) {
    case InteractionEventKind::PrimaryPress:
    case InteractionEventKind::PrimaryRelease:
    case InteractionEventKind::SecondaryPress:
    case InteractionEventKind::SecondaryRelease:
    case InteractionEventKind::PointerMove:
        return true;
    default:
        return false;
    }
}

bool GetIsPointerPress(InteractionEventKind eventKind) noexcept
{
    return eventKind == InteractionEventKind::PrimaryPress
        || eventKind == InteractionEventKind::SecondaryPress;
}

InteractionResult GetReentrantFailure() noexcept
{
    return {
        true,
        true,
        false,
        InteractionFailureReason::StateRejected };
}

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

} // namespace

bool InteractionRouter::AttachHandler(
    std::unique_ptr<IInteractionHandler> handler)
{
    if (!handler || m_capture || m_dispatchDepth != 0) {
        return false;
    }
    try {
        m_handlers.push_back(std::move(handler));
        return true;
    }
    catch (...) {
        return false;
    }
}

bool InteractionRouter::ClearHandlers()
{
    if (m_capture || m_dispatchDepth != 0) {
        return false;
    }
    m_handlers.clear();
    return true;
}

InteractionResult InteractionRouter::CancelCapture(
    const InteractionCaptureKey& key,
    const InteractionEvent& eve)
{
    if (m_dispatchDepth != 0) {
        return GetReentrantFailure();
    }
    if (!m_capture || m_capture->GetKey() != key) {
        return {};
    }

    DispatchGuard guard(m_dispatchDepth);
    auto cancel = eve;
    cancel.eventKind = InteractionEventKind::Cancel;
    const auto result = m_capture->Send(cancel);
    if (result.isSucceeded) {
        m_capture.reset();
        m_captureHandler = nullptr;
    }
    return result;
}

InteractionResult InteractionRouter::SendCancel(const InteractionEvent& eve)
{
    if (m_dispatchDepth != 0) {
        return GetReentrantFailure();
    }

    DispatchGuard guard(m_dispatchDepth);
    auto cancel = eve;
    cancel.eventKind = InteractionEventKind::Cancel;
    InteractionResult aggregate;

    IInteractionHandler* capturedHandler = nullptr;
    if (m_capture) {
        capturedHandler = m_captureHandler;
        const auto captured = m_capture->Send(cancel);
        MergeResult(aggregate, captured);
        if (!captured.isSucceeded) {
            return aggregate;
        }
        m_capture.reset();
        m_captureHandler = nullptr;
    }

    for (const auto& handler : m_handlers) {
        if (!handler || handler.get() == capturedHandler) continue;
        const auto routed = handler->Route(cancel);
        MergeResult(aggregate, routed.result);
    }
    return aggregate;
}

InteractionResult InteractionRouter::Dispatch(const InteractionEvent& eve,
    RouterDispatchMode mode)
{
    if (m_dispatchDepth != 0) {
        return GetReentrantFailure();
    }
    DispatchGuard guard(m_dispatchDepth);

    if (m_capture && GetIsPointerEvent(eve.eventKind)) {
        const auto releaseKind = m_capture->GetReleaseKind();
        const auto result = m_capture->Send(eve);
        if (eve.eventKind == releaseKind && result.isSucceeded) {
            m_capture.reset();
            m_captureHandler = nullptr;
        }
        return result;
    }

    // FirstMatch 的“命中”由 isHandled 决定并立即停止；Broadcast 始终遍历完整列表。
    // 两种模式都 OR 聚合传播停止状态，因此任一路径提出的停止请求都会保留到调用方。
    InteractionResult aggregated;

    for (const auto& handler : m_handlers) {
        if (!handler) {
            continue;
        }

        auto routed = handler->Route(eve);
        const InteractionResult& result = routed.result;

        // 传播停止状态做 OR 聚合，与 FirstMatch 的 isHandled 截断条件相互独立。
        if (result.isPropagationStopped) {
            aggregated.isPropagationStopped = true;
        }
        if (!result.isSucceeded) {
            aggregated.isSucceeded = false;
            if (aggregated.failureReason
                    == InteractionFailureReason::None) {
                aggregated.failureReason = result.failureReason;
            }
        }

        if (mode == RouterDispatchMode::FirstMatch && result.isHandled) {
            aggregated.isHandled = true;
            if (GetIsPointerPress(eve.eventKind)
                && result.isSucceeded && routed.capture) {
                m_capture = std::move(routed.capture);
                m_captureHandler = handler.get();
            }
            return aggregated;  // 找到第一个消费者，提前退出
        }

        if (result.isHandled) {
            aggregated.isHandled = true;
        }
    }

    return aggregated;
}
