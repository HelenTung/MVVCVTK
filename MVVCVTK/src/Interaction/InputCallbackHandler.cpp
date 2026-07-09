#include "InputCallbackHandler.h"

#include <algorithm>
#include <utility>

InputCallbackHandler::InputCallbackHandler(
    Callback callback,
    std::vector<unsigned long> eventIds)
    : m_callback(std::move(callback))
    , m_eventIds(std::move(eventIds))
{
}

InteractionResult InputCallbackHandler::Send(const InteractionEvent& eve)
{
    if (!m_callback || !GetEventMatched(eve.vtkEventId)) {
        return {};
    }

    return m_callback(eve);
}

bool InputCallbackHandler::GetEventMatched(unsigned long eventId) const
{
    if (m_eventIds.empty()) {
        return true;
    }

    return std::find(m_eventIds.begin(), m_eventIds.end(), eventId) != m_eventIds.end();
}
