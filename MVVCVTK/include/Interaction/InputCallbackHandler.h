#pragma once

#include "IInteractionHandler.h"

#include <functional>
#include <vector>

// callback 型 handler 只负责把外部输入适配器接入 InteractionRouter。
// 它不保存 host、feature 或业务状态，避免 standalone hotkey 语义扩散到渲染层。
class InputCallbackHandler final : public IInteractionHandler {
public:
    using Callback = std::function<InteractionResult(const InteractionEvent&)>;

    InputCallbackHandler(
        Callback callback,
        std::vector<unsigned long> eventIds);

    InteractionResult Send(const InteractionEvent& eve) override;

private:
    bool GetEventMatched(unsigned long eventId) const;

    Callback m_callback;
    std::vector<unsigned long> m_eventIds;
};
