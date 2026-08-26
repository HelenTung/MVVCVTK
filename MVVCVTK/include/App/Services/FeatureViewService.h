#pragma once

#include "App/AppTypes.h"
#include "Render/Contracts/RenderEffect.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>

// Feature 只获得该窄端口，不能恢复 App 的视图或交互实现 identity。
class FeatureViewService {
public:
    virtual ~FeatureViewService() = default;

    virtual bool SetInteracting(
        const InteractionSource& source,
        bool isInteracting) = 0;
    virtual std::optional<std::array<double, 16>>
        GetModelToWorld() const = 0;
    virtual std::optional<std::array<double, 3>> GetWorldPosition(
        const std::array<double, 3>& modelPosition) const = 0;
    virtual std::optional<RenderInputStamp>
        GetRenderInputStamp() const = 0;
    virtual bool AttachRenderEffect(
        std::shared_ptr<RenderEffect> effect) = 0;
    virtual bool DetachRenderEffect(
        const RenderEffect* effect) = 0;
    // Widget/effect 只投递一次重绘门铃，不接触 App 的完整 dirty 状态。
    virtual bool SetRenderNeeded() = 0;
};

// Lease 仅证明 owner-thread session 仍有效；它不是互斥锁。
class FeatureViewLease final {
public:
    explicit FeatureViewLease(
        std::thread::id ownerThread) noexcept
        : m_ownerThread(ownerThread)
    {
    }

    bool GetIsActive() const noexcept
    {
        return m_isActive.load();
    }

    bool GetIsOwnerThread() const noexcept
    {
        return m_ownerThread == std::this_thread::get_id();
    }

    bool StopLease() noexcept
    {
        if (!GetIsOwnerThread()) {
            return false;
        }
        m_isActive = false;
        return true;
    }

private:
    std::thread::id m_ownerThread;
    std::atomic<bool> m_isActive{ true };
};
