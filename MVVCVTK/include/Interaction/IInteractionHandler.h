#pragma once
#include "InteractionTypes.h"

// ─────────────────────────────────────────────────────────────────────
// IInteractionHandler — 交互处理器接口
//
// 所有具体 Handler 继承此接口并实现 Send()。
// Send() 的调用线程：主线程（VTK 事件回调线程），禁止在此加锁等待。
// ─────────────────────────────────────────────────────────────────────
class IInteractionHandler
{
public:
    virtual ~IInteractionHandler() = default;

    // 返回 isHandled=true 时，FirstMatch 模式的 Router 停止继续分发
    virtual InteractionResult Send(const InteractionEvent& eve) = 0;
};
