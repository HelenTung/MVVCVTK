#include "Host/HostFrameCoordinator.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace {

bool GetPriorityValid(const FeatureScenePriority priority) noexcept
{
    switch (priority) {
    case FeatureScenePriority::Scene:
    case FeatureScenePriority::Overlay:
    case FeatureScenePriority::Refinement:
        return true;
    }
    return false;
}

bool GetScopeValid(const FeatureSceneScope scope) noexcept
{
    switch (scope) {
    case FeatureSceneScope::RequiredAllViews:
    case FeatureSceneScope::TargetViewOnly:
    case FeatureSceneScope::BestEffort:
        return true;
    }
    return false;
}

bool GetIntentSame(
    const HostFrameIntent& left,
    const HostFrameIntent& right) noexcept
{
    return left.featureId == right.featureId
        && left.delta.priority == right.delta.priority
        && left.delta.scope == right.delta.scope
        && left.delta.viewIds == right.delta.viewIds;
}

// inbox 已经被 swap 到 owner-thread vector 后，不能再通过临时容器合并；
// 否则分配失败会析构已移出的 intent。这里用稳定、原地的小规模排序与压缩，
// 保证 freeze 后的所有权始终留在调用方，直到 commit/unchanged 明确消费。
void NormalizeIntents(std::vector<HostFrameIntent>& intents) noexcept
{
    for (std::size_t index = 1; index < intents.size(); ++index) {
        auto value = std::move(intents[index]);
        std::size_t insert = index;
        while (insert > 0
            && static_cast<std::uint8_t>(
                intents[insert - 1].delta.priority)
                > static_cast<std::uint8_t>(value.delta.priority)) {
            intents[insert] = std::move(intents[insert - 1]);
            --insert;
        }
        intents[insert] = std::move(value);
    }

    std::size_t mergedSize = 0;
    for (std::size_t input = 0; input < intents.size(); ++input) {
        std::size_t current = 0;
        while (current < mergedSize
            && !GetIntentSame(intents[current], intents[input])) {
            ++current;
        }
        if (current == mergedSize) {
            if (mergedSize != input) {
                intents[mergedSize] = std::move(intents[input]);
            }
            ++mergedSize;
        }
        else if (intents[input].delta.requestId
            >= intents[current].delta.requestId) {
            intents[current] = std::move(intents[input]);
        }
    }
    intents.resize(mergedSize);
}

class FlushGuard final {
public:
    explicit FlushGuard(bool& isFlushing) noexcept
        : m_isFlushing(isFlushing)
    {
        m_isFlushing = true;
    }

    ~FlushGuard()
    {
        m_isFlushing = false;
    }

    FlushGuard(const FlushGuard&) = delete;
    FlushGuard& operator=(const FlushGuard&) = delete;

private:
    bool& m_isFlushing;
};

} // namespace

HostFrameCoordinator::HostFrameCoordinator(
    const std::uint64_t sessionGeneration,
    Callbacks callbacks)
    : m_sessionGeneration(sessionGeneration)
    , m_ownerThread(std::this_thread::get_id())
    , m_callbacks(std::move(callbacks))
{
}

bool HostFrameCoordinator::Enqueue(
    std::string featureId,
    FeatureSceneDelta delta)
{
    const bool hasStampIdentity = delta.inputStamp.identity != nullptr;
    const bool hasStampVersion = delta.inputStamp.version != 0;
    if (featureId.empty() || delta.requestId == 0
        || delta.viewIds.empty()
        || hasStampIdentity != hasStampVersion
        || !GetPriorityValid(delta.priority)
        || !GetScopeValid(delta.scope)
        || (delta.scope == FeatureSceneScope::TargetViewOnly
            && delta.viewIds.size() != 1)) {
        return false;
    }
    for (std::size_t index = 0; index < delta.viewIds.size(); ++index) {
        if (delta.viewIds[index].empty()
            || std::find(
                delta.viewIds.begin(),
                delta.viewIds.begin() + static_cast<std::ptrdiff_t>(index),
                delta.viewIds[index])
                != delta.viewIds.begin()
                    + static_cast<std::ptrdiff_t>(index)) {
            return false;
        }
    }
    // target 是集合语义；先规范化顺序，避免同一组 View 因排列不同绕过
    // latest-request 合并并在同一 epoch 重复置脏。
    std::sort(delta.viewIds.begin(), delta.viewIds.end());

    HostFrameIntent intent;
    intent.featureId = std::move(featureId);
    intent.delta = std::move(delta);
    intent.sessionGeneration = m_sessionGeneration;

    try {
        const std::lock_guard<std::mutex> lock(m_intentMutex);
        if (m_isStopped.load(std::memory_order_acquire)) return false;
        // 与 commit 后的 pending epoch 晋级使用同一把锁，消除“先读旧 epoch、
        // commit 晋级完成后才入队”的 TOCTOU 窗口。
        intent.baseSceneEpoch =
            m_sceneEpoch.load(std::memory_order_acquire);
        m_pendingIntents.push_back(std::move(intent));
    }
    catch (...) {
        return false;
    }
    return true;
}

void HostFrameCoordinator::FreezeIntents(
    std::vector<HostFrameIntent>& intents)
{
    {
        const std::lock_guard<std::mutex> lock(m_intentMutex);
        if (!m_retryIntents.empty()) {
            intents.swap(m_retryIntents);
        }
        else {
            intents.swap(m_pendingIntents);
        }
    }
    NormalizeIntents(intents);
}

HostFrameCoordinator::FlushStatus
HostFrameCoordinator::FlushOnOwnerTick(const bool isFeatureTick)
{
    if (m_isStopped.load(std::memory_order_acquire)) {
        return FlushStatus::Stopped;
    }
    if (m_ownerThread != std::this_thread::get_id()
        || !GetCallbacksValid()) {
        return FlushStatus::Failed;
    }
    if (m_isFlushing) return FlushStatus::Deferred;
    FlushGuard guard(m_isFlushing);
    std::vector<HostFrameIntent> intents;
    bool restoreIntents = false;

    try {
        // 已发布 epoch 的 Render 是当前唯一终态门；成功前不接纳下一普通 batch。
        if (m_hasPendingCompletion || m_callbacks.getRenderPending()) {
            const auto epoch = m_sceneEpoch.load(std::memory_order_acquire);
            if (!m_callbacks.sendRender(epoch)) {
                return FlushStatus::RenderPending;
            }
            if (m_hasPendingCompletion) {
                SendCompletions();
                m_hasPendingCompletion = false;
            }
            return FlushStatus::Completed;
        }

        if (!m_callbacks.collectUpdates()) {
            ClearStage();
            return FlushStatus::Failed;
        }
        if (isFeatureTick) m_callbacks.sendFeatureTicks();
        // 从这一点起，任一 commit 前失败都必须把同一批 intent 放回 inbox。
        // restoreIntents 在 freeze 前置位，因此 mutex 获取异常也走同一恢复出口。
        restoreIntents = true;
        FreezeIntents(intents);
        if (!m_callbacks.setIntents(intents)) {
            RestoreIntents(std::move(intents));
            restoreIntents = false;
            ClearStage();
            return FlushStatus::Failed;
        }
        if (isFeatureTick && !m_callbacks.applyFeatureUpdates()) {
            RestoreIntents(std::move(intents));
            restoreIntents = false;
            ClearStage();
            return FlushStatus::Failed;
        }

        const auto currentEpoch =
            m_sceneEpoch.load(std::memory_order_acquire);
        if (currentEpoch == std::numeric_limits<std::uint64_t>::max()) {
            RestoreIntents(std::move(intents));
            restoreIntents = false;
            ClearStage();
            return FlushStatus::Failed;
        }
        const auto nextEpoch = currentEpoch + 1;
        const auto stageStatus = m_callbacks.buildStage(nextEpoch);
        if (stageStatus == HostFrameStageStatus::Failed) {
            RestoreIntents(std::move(intents));
            restoreIntents = false;
            ClearStage();
            return FlushStatus::Failed;
        }
        if (stageStatus == HostFrameStageStatus::Unchanged) {
            restoreIntents = false;
            SendCompletions();
            return FlushStatus::Completed;
        }

        // setCommit 的目标实现必须 noexcept；全量 stage 已完成，之后不再回滚。
        m_callbacks.setCommit(nextEpoch);
        restoreIntents = false;
        m_sceneEpoch.store(nextEpoch, std::memory_order_release);
        AdvancePendingBaseEpoch(currentEpoch, nextEpoch);
        m_hasPendingCompletion = true;
        if (!m_callbacks.sendRender(nextEpoch)) {
            return FlushStatus::RenderPending;
        }
        SendCompletions();
        m_hasPendingCompletion = false;
        return FlushStatus::Completed;
    }
    catch (...) {
        // 若 epoch 已发布，异常属于 Render/notification 之后的失败，不能回滚。
        if (m_hasPendingCompletion) return FlushStatus::RenderPending;
        if (restoreIntents) RestoreIntents(std::move(intents));
        ClearStage();
        return FlushStatus::Failed;
    }
}

void HostFrameCoordinator::AdvancePendingBaseEpoch(
    const std::uint64_t currentEpoch,
    const std::uint64_t nextEpoch) noexcept
{
    try {
        const std::lock_guard<std::mutex> lock(m_intentMutex);
        for (auto& intent : m_pendingIntents) {
            if (intent.sessionGeneration == m_sessionGeneration
                && intent.baseSceneEpoch == currentEpoch) {
                intent.baseSceneEpoch = nextEpoch;
            }
        }
    }
    catch (...) {
    }
}

void HostFrameCoordinator::RestoreIntents(
    std::vector<HostFrameIntent> intents) noexcept
{
    if (intents.empty()
        || m_isStopped.load(std::memory_order_acquire)) {
        return;
    }
    try {
        const std::lock_guard<std::mutex> lock(m_intentMutex);
        if (m_isStopped.load(std::memory_order_acquire)) return;
        if (m_retryIntents.empty()) {
            m_retryIntents.swap(intents);
        }
        else {
            // 只有同一 flush 重复恢复才可能到达这里。保留已经登记的
            // retry batch；调用方本轮不会触发该分支。
            m_pendingIntents.insert(
                m_pendingIntents.begin(),
                std::make_move_iterator(intents.begin()),
                std::make_move_iterator(intents.end()));
        }
    }
    catch (...) {
    }
}

void HostFrameCoordinator::Stop() noexcept
{
    m_isStopped.store(true, std::memory_order_release);
    try {
        const std::lock_guard<std::mutex> lock(m_intentMutex);
        m_retryIntents.clear();
        m_pendingIntents.clear();
    }
    catch (...) {
    }
    if (m_ownerThread == std::this_thread::get_id()) ClearStage();
}

std::uint64_t HostFrameCoordinator::GetCommittedEpoch() const noexcept
{
    return m_sceneEpoch.load(std::memory_order_acquire);
}

std::uint64_t HostFrameCoordinator::GetSessionGeneration() const noexcept
{
    return m_sessionGeneration;
}

void HostFrameCoordinator::SendCompletions() noexcept
{
    try {
        m_callbacks.sendCompletions();
    }
    catch (...) {
    }
}

void HostFrameCoordinator::ClearStage() noexcept
{
    try {
        if (m_callbacks.clearStage) m_callbacks.clearStage();
    }
    catch (...) {
    }
}

bool HostFrameCoordinator::GetCallbacksValid() const noexcept
{
    return m_sessionGeneration != 0
        && static_cast<bool>(m_callbacks.collectUpdates)
        && static_cast<bool>(m_callbacks.sendFeatureTicks)
        && static_cast<bool>(m_callbacks.setIntents)
        && static_cast<bool>(m_callbacks.applyFeatureUpdates)
        && static_cast<bool>(m_callbacks.buildStage)
        && static_cast<bool>(m_callbacks.setCommit)
        && static_cast<bool>(m_callbacks.sendRender)
        && static_cast<bool>(m_callbacks.getRenderPending)
        && static_cast<bool>(m_callbacks.sendCompletions);
}
