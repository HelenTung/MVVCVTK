#include "InputCallbackHandler.h"

#include <algorithm>
#include <utility>

InputCallbackHandler::InputCallbackHandler(
    Callback callback,
    std::vector<InteractionEventKind> eventKinds)
    : m_callback(
        [callback = std::move(callback)](const InteractionEvent& event) {
            return InteractionDispatch{
                callback ? callback(event) : InteractionResult{},
                nullptr };
        })
    , m_eventKinds(std::move(eventKinds))
{
}

InputCallbackHandler::InputCallbackHandler(
    RoutedCallback callback,
    std::vector<InteractionEventKind> eventKinds)
    : m_callback(std::move(callback))
    , m_eventKinds(std::move(eventKinds))
{
}

InteractionResult InputCallbackHandler::Send(const InteractionEvent& eve)
{
    return Route(eve).result;
}

InteractionDispatch InputCallbackHandler::Route(
    const InteractionEvent& eve)
{
    if (!m_callback || !GetEventMatched(eve.eventKind)) {
        return {};
    }

    return m_callback(eve);
}

bool InputCallbackHandler::GetEventMatched(InteractionEventKind eventKind) const
{
    if (m_eventKinds.empty()) {
        return true;
    }

    return std::find(m_eventKinds.begin(), m_eventKinds.end(), eventKind)
        != m_eventKinds.end();
}
