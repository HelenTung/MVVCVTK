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

    // Handler 按值拥有外部适配 callback；仅命中 eventIds 后同步调用，捕获对象生命周期由 callback 决定。
    Callback m_callback;
    // VTK event id 白名单；空数组表示匹配所有送达本 Handler 的事件。
    std::vector<unsigned long> m_eventIds;
};
