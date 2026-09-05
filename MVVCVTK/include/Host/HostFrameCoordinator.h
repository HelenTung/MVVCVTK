#pragma once

#include "Host/HostFeature.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class HostFrameStageStatus : std::uint8_t {
    Failed,
    Unchanged,
    Ready
};

// Host 私有 intent 会在入队时补齐 Session 与 Scene 身份；Feature 永远看不到
// Coordinator、Registry 或 App 的具体实现。
struct HostFrameIntent final {
    std::string featureId;
    FeatureSceneDelta delta;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t baseSceneEpoch = 0;
};

class HostFrameCoordinator final {
public:
    enum class FlushStatus : std::uint8_t {
        Completed,
        Deferred,
        RenderPending,
        Failed,
        Stopped
    };

    struct Callbacks final {
        std::function<bool()> collectUpdates;
        std::function<void()> sendFeatureTicks;
        std::function<bool(const std::vector<HostFrameIntent>&)>
            setIntents;
        std::function<bool()> applyFeatureUpdates;
        std::function<HostFrameStageStatus(std::uint64_t)> buildStage;
        std::function<void(std::uint64_t)> setCommit;
        std::function<bool(std::uint64_t)> sendRender;
        std::function<bool()> getRenderPending;
        std::function<void()> sendCompletions;
        std::function<void()> clearStage;
        // 普通 owner 终态独立于显示成功；有效 tick 退出前有界派发。
        std::function<void()> sendReadyCompletions;
    };

    HostFrameCoordinator(
        std::uint64_t sessionGeneration,
        Callbacks callbacks);

    HostFrameCoordinator(const HostFrameCoordinator&) = delete;
    HostFrameCoordinator& operator=(const HostFrameCoordinator&) = delete;
    HostFrameCoordinator(HostFrameCoordinator&&) = delete;
    HostFrameCoordinator& operator=(HostFrameCoordinator&&) = delete;

    // 任意线程只可提交纯值意图；此调用不读取 View，也不触碰 VTK。
    bool Enqueue(std::string featureId, FeatureSceneDelta delta);

    // owner thread 专用；nullopt 表示目标尚在准备，false 表示失败或已被替代。
    bool SendDisplayComplete(
        std::function<std::optional<bool>()> getApplied,
        std::function<void(bool)> onComplete);

    // 只允许构造 Coordinator 的 Session owner thread 调用。
    FlushStatus FlushOnOwnerTick(bool isFeatureTick);
    void Stop() noexcept;

    std::uint64_t GetCommittedEpoch() const noexcept;
    std::uint64_t GetSessionGeneration() const noexcept;

private:
    void FreezeIntents(std::vector<HostFrameIntent>& intents);
    void RestoreIntents(std::vector<HostFrameIntent> intents) noexcept;
    void AdvancePendingBaseEpoch(
        std::uint64_t currentEpoch,
        std::uint64_t nextEpoch) noexcept;
    void SendCompletions() noexcept;
    void SendDisplayCompletions(bool isRendered = true) noexcept;
    void ClearStage() noexcept;
    bool GetCallbacksValid() const noexcept;

    const std::uint64_t m_sessionGeneration = 0;
    const std::thread::id m_ownerThread;
    Callbacks m_callbacks;
    mutable std::mutex m_intentMutex;
    // commit 前失败的 frozen batch 独立重试；freeze 后新到达的 intent
    // 继续留在 pending，不能被并回已经冻结的逻辑帧。
    std::vector<HostFrameIntent> m_retryIntents;
    std::vector<HostFrameIntent> m_pendingIntents;
    std::atomic<std::uint64_t> m_sceneEpoch{ 0 };
    std::atomic<bool> m_isStopped{ false };
    bool m_isFlushing = false;
    bool m_hasPendingCompletion = false;
    struct CompleteEntry final {
        std::function<std::optional<bool>()> getApplied;
        std::function<void(bool)> onComplete;
        std::optional<bool> result;
    };
    std::list<CompleteEntry> m_pendingCompletes;
    std::list<CompleteEntry> m_frameCompletes;
    std::size_t m_completeCount = 0;
};
