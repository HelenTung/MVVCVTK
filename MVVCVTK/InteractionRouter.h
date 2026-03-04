#pragma once
#include <memory>
#include <vector>
#include "IInteractionHandler.h"

// ─────────────────────────────────────────────────────────────────────
// RouterDispatchMode — Dispatch 策略
//
//   FirstMatch : 找到第一个 handled=true 的 Handler 后立即停止（默认）
//                适用于：鼠标点击、移动、滚轮等互斥性事件
//
//   Broadcast  : 所有 Handler 均执行，忽略 handled 标志
//                适用于：TimerEvent（心跳更新 + 渲染 + 其他逻辑均需触发）
// ─────────────────────────────────────────────────────────────────────
enum class RouterDispatchMode
{
    FirstMatch,
    Broadcast
};

// ─────────────────────────────────────────────────────────────────────
// InteractionRouter — 将 InteractionEvent 分发给已注册的 Handler 列表
//
// 用法：
//   router.Add(std::make_unique<TimeUpdateHandler>(...));
//   router.Add(std::make_unique<Viewer2DHandler>(...));
//   ...
//   InteractionResult r = router.Dispatch(eve, RouterDispatchMode::FirstMatch);
//   if (r.abortVtk) callback->SetAbortFlag(1);
// ─────────────────────────────────────────────────────────────────────
class InteractionRouter
{
public:
    // 追加一个 Handler（按添加顺序分发，先添加的优先级高）
    void Add(std::unique_ptr<IInteractionHandler> handler);

    // 清空所有 Handler（通常在重新 BuildRouter 时调用）
    void Clear();

    // 分发事件
    // - FirstMatch：第一个 handled=true 后停止，返回其结果（abortVtk 聚合）
    // - Broadcast ：所有 Handler 均执行，abortVtk 取 OR 聚合
    InteractionResult Dispatch(const InteractionEvent& eve,
        RouterDispatchMode mode = RouterDispatchMode::FirstMatch);

private:
    std::vector<std::unique_ptr<IInteractionHandler>> m_handlers;
};