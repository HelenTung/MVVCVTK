#pragma once
#include "InteractionTypes.h"

#include <cstdint>
#include <memory>

struct InteractionCaptureKey final
{
    std::uint64_t domain = 0;
    std::uint64_t generation = 0;
};

inline bool operator==(
    const InteractionCaptureKey& lhs,
    const InteractionCaptureKey& rhs) noexcept
{
    return lhs.domain == rhs.domain
        && lhs.generation == rhs.generation;
}

inline bool operator!=(
    const InteractionCaptureKey& lhs,
    const InteractionCaptureKey& rhs) noexcept
{
    return !(lhs == rhs);
}

class IInteractionCapture
{
public:
    virtual ~IInteractionCapture() noexcept = default;

    virtual InteractionCaptureKey GetKey() const noexcept = 0;
    virtual InteractionEventKind GetReleaseKind() const noexcept = 0;
    virtual bool GetIsActive() const noexcept { return true; }
    // 原生控件在 SDK release 返回后才结束，须由其存活谓词确认清理完成。
    virtual bool GetCompletesOnRelease() const noexcept { return true; }
    virtual InteractionResult Send(const InteractionEvent& event) = 0;
};

struct InteractionDispatch final
{
    InteractionResult result;
    std::unique_ptr<IInteractionCapture> capture;
    bool needsCaptureCleanup=false;
};

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
    virtual bool GetIsCaptureActive() const noexcept { return true; }
    virtual InteractionResult Send(const InteractionEvent& eve) = 0;

    // 只有需要建立指针捕获的组合 Handler 覆盖；默认仅返回处理结果。
    virtual InteractionDispatch Route(const InteractionEvent& eve)
    {
        return { Send(eve), nullptr };
    }
};
