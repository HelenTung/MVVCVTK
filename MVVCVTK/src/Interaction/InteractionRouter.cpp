#include "InteractionRouter.h"

void InteractionRouter::AddHandler(std::unique_ptr<IInteractionHandler> handler)
{
    if (!handler) {
        return;
    }
    m_handlers.push_back(std::move(handler));
}

void InteractionRouter::ClearHandlers()
{
    m_handlers.clear();
}

InteractionResult InteractionRouter::Dispatch(const InteractionEvent& eve,
    RouterDispatchMode mode)
{
    InteractionResult aggregated;

    for (const auto& handler : m_handlers) {
        if (!handler) {
            continue;
        }

        const InteractionResult result = handler->Send(eve);

        // hasVtkAbort 做 OR 聚合：任一 Handler 要求中止即中止
        if (result.hasVtkAbort) {
            aggregated.hasVtkAbort = true;
        }

        if (mode == RouterDispatchMode::FirstMatch && result.isHandled) {
            aggregated.isHandled = true;
            return aggregated;  // 找到第一个消费者，提前退出
        }

        if (result.isHandled) {
            aggregated.isHandled = true;
        }
    }

    return aggregated;
}