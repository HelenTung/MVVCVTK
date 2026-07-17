#include "InteractionRouter.h"

void InteractionRouter::AttachHandler(std::unique_ptr<IInteractionHandler> handler)
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
    // FirstMatch 的“命中”由 isHandled 决定并立即停止；Broadcast 始终遍历完整列表。
    // 两种模式都 OR 聚合传播停止状态，因此任一路径提出的停止请求都会保留到调用方。
    InteractionResult aggregated;

    for (const auto& handler : m_handlers) {
        if (!handler) {
            continue;
        }

        const InteractionResult result = handler->Send(eve);

        // 传播停止状态做 OR 聚合，与 FirstMatch 的 isHandled 截断条件相互独立。
        if (result.isPropagationStopped) {
            aggregated.isPropagationStopped = true;
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
