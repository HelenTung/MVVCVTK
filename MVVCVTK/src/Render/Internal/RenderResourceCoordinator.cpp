#include "Render/Internal/RenderResourceCoordinator.h"
#include "Render/Internal/IsoSurfaceProductBuilder.h"
#include "Render/Internal/VolumeLodProductBuilder.h"

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

struct RenderChannelState;

std::uint64_t GetDefaultCpuBudgetBytes() noexcept
{
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        constexpr long double sessionMemoryFraction = 0.25L;
        const long double bytes =
            static_cast<long double>(status.ullAvailPhys)
            * sessionMemoryFraction;
        if (bytes >= 1.0L
            && bytes < static_cast<long double>(
                (std::numeric_limits<std::uint64_t>::max)())) {
            return static_cast<std::uint64_t>(bytes);
        }
    }
#endif
    return (std::numeric_limits<std::uint64_t>::max)();
}

struct VolumeCacheEntry final {
    VolumeLodKey key;
    std::shared_ptr<const VolumeLodProduct> product;
    std::uint64_t bytes = 0;
    std::uint64_t lastUse = 0;
};

struct IsoCacheEntry final {
    IsoSurfaceKey key;
    std::shared_ptr<const IsoSurfaceProduct> product;
    std::uint64_t bytes = 0;
    std::uint64_t lastUse = 0;
};

struct GpuReservation final {
    const void* ownerIdentity = nullptr;
    std::uint64_t bytes = 0;
};

struct GpuContextEntry final {
    const void* contextIdentity = nullptr;
    std::uint64_t budgetBytes = 0;
    std::vector<GpuReservation> reservations;
};

struct RenderRunningTask final {
    std::shared_ptr<RenderChannelState> channel;
    std::shared_ptr<std::atomic<bool>> isCancelled;
    std::uint64_t requestRevision = 0;
    std::uint64_t accountedBytes = 0;
};

struct RenderCoordinatorState final {
    explicit RenderCoordinatorState(RenderLaneStart start)
        : onTaskStart(std::move(start))
    {
    }

    mutable std::mutex mutex;
    std::condition_variable stopped;
    RenderLaneStart onTaskStart;
    std::vector<std::weak_ptr<RenderChannelState>> channels;
    std::deque<std::weak_ptr<RenderChannelState>> readyChannels;
    std::optional<RenderRunningTask> running;
    std::vector<VolumeCacheEntry> volumeCache;
    std::vector<IsoCacheEntry> isoCache;
    std::vector<GpuContextEntry> gpuContexts;
    std::uint64_t cacheBytes = 0;
    std::uint64_t cacheUseStamp = 0;
    std::uint64_t topologyRevision = 0;
    std::uint64_t cpuBudgetBytes = GetDefaultCpuBudgetBytes();
    bool isAccepting = true;
    bool isStopping = false;
};

std::uint64_t GetDoubleBits(const double value) noexcept
{
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool GetVolumeKeyEqual(
    const VolumeLodKey& left,
    const VolumeLodKey& right) noexcept
{
    return left.inputStamp == right.inputStamp
        && left.inputIdentity == right.inputIdentity
        && left.maskIdentity == right.maskIdentity
        && left.inputMTime == right.inputMTime
        && left.inputScalarMTime == right.inputScalarMTime
        && left.maskMTime == right.maskMTime
        && left.maskScalarMTime == right.maskScalarMTime
        && left.outputDimensions == right.outputDimensions
        && GetDoubleBits(left.denoiseThreshold)
            == GetDoubleBits(right.denoiseThreshold)
        && left.isDenoiseOn == right.isDenoiseOn;
}

bool GetIsoKeyEqual(
    const IsoSurfaceKey& left,
    const IsoSurfaceKey& right) noexcept
{
    return left.inputStamp == right.inputStamp
        && left.inputIdentity == right.inputIdentity
        && left.maskIdentity == right.maskIdentity
        && left.inputMTime == right.inputMTime
        && left.inputScalarMTime == right.inputScalarMTime
        && left.maskMTime == right.maskMTime
        && left.maskScalarMTime == right.maskScalarMTime
        && left.outputDimensions == right.outputDimensions
        && GetDoubleBits(left.isoValue) == GetDoubleBits(right.isoValue);
}

struct RenderChannelState final {
    RenderProductKind productKind = RenderProductKind::VolumeLod;
    RenderTransitionState transition;
    std::optional<RenderTaskRequest> pending;
    std::uint64_t activeBytes = 0;
    const void* activeProductIdentity = nullptr;
    std::uint64_t retiringRevision = 0;
    std::uint64_t retiringBytes = 0;
    const void* retiringProductIdentity = nullptr;
    std::uint64_t runningRevision = 0;
    bool isQueued = false;
    bool isStopped = false;
};

bool GetSumValid(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept
{
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return false;
    }
    result = left + right;
    return true;
}

RenderResourceState GetResourceStateLocked(
    const RenderCoordinatorState& state)
{
    RenderResourceState result;
    result.cpuBudgetBytes = state.cpuBudgetBytes;
    struct ActiveAllocation final {
        const void* identity = nullptr;
        std::uint64_t bytes = 0;
    };
    std::vector<ActiveAllocation> activeAllocations;
    const auto addBytes = [](std::uint64_t& total, const std::uint64_t bytes) {
        if (bytes
            <= (std::numeric_limits<std::uint64_t>::max)() - total) {
            total += bytes;
        }
        else {
            total = (std::numeric_limits<std::uint64_t>::max)();
        }
    };
    const auto addActive = [&result, &activeAllocations, &addBytes](
        const void* identity,
        const std::uint64_t bytes) {
        if (bytes == 0) return;
        if (!identity) {
            addBytes(result.activeBytes, bytes);
            return;
        }
        const auto existing = std::find_if(
            activeAllocations.begin(), activeAllocations.end(),
            [identity](const ActiveAllocation& allocation) {
                return allocation.identity == identity;
            });
        if (existing == activeAllocations.end()) {
            activeAllocations.push_back({ identity, bytes });
            addBytes(result.activeBytes, bytes);
            return;
        }
        if (bytes > existing->bytes) {
            addBytes(result.activeBytes, bytes - existing->bytes);
            existing->bytes = bytes;
        }
    };
    for (const auto& weakChannel : state.channels) {
        const auto channel = weakChannel.lock();
        if (!channel) continue;
        addActive(channel->activeProductIdentity, channel->activeBytes);
        addActive(
            channel->retiringProductIdentity, channel->retiringBytes);
    }
    const auto addCacheBytes = [&result](const std::uint64_t bytes) {
        if (bytes
            <= (std::numeric_limits<std::uint64_t>::max)()
                - result.cacheBytes) {
            result.cacheBytes += bytes;
        }
        else {
            result.cacheBytes =
                (std::numeric_limits<std::uint64_t>::max)();
        }
    };
    for (const auto& entry : state.volumeCache) {
        const bool isActive = entry.product
            && std::any_of(
                activeAllocations.begin(), activeAllocations.end(),
                [&entry](const ActiveAllocation& allocation) {
                    return allocation.identity == entry.product.get();
                });
        if (entry.product && !isActive) {
            addCacheBytes(entry.bytes);
        }
    }
    for (const auto& entry : state.isoCache) {
        const bool isActive = entry.product
            && std::any_of(
                activeAllocations.begin(), activeAllocations.end(),
                [&entry](const ActiveAllocation& allocation) {
                    return allocation.identity == entry.product.get();
                });
        if (entry.product && !isActive) {
            addCacheBytes(entry.bytes);
        }
    }
    if (state.running) {
        result.runningBytes = state.running->accountedBytes;
    }
    for (const auto& weakChannel : state.channels) {
        const auto channel = weakChannel.lock();
        if (!channel) continue;
        if (channel->pending
            && channel->pending->estimatedBytes
                <= (std::numeric_limits<std::uint64_t>::max)()
                    - result.pendingBytes) {
            result.pendingBytes += channel->pending->estimatedBytes;
        }
        else if (channel->pending) {
            result.pendingBytes =
                (std::numeric_limits<std::uint64_t>::max)();
        }
        const std::uint64_t readyBytes =
            channel->transition.status == RenderProductStatus::Ready
            ? channel->transition.stats.candidateBytes : 0;
        if (readyBytes
            <= (std::numeric_limits<std::uint64_t>::max)()
                - result.pendingBytes) {
            result.pendingBytes += readyBytes;
        }
        else {
            result.pendingBytes =
                (std::numeric_limits<std::uint64_t>::max)();
        }
    }
    return result;
}

bool GetProductActiveLocked(
    const RenderCoordinatorState& state,
    const void* productIdentity)
{
    if (!productIdentity) return false;
    return std::any_of(
        state.channels.begin(), state.channels.end(),
        [productIdentity](const auto& weakChannel) {
            const auto channel = weakChannel.lock();
            return channel
                && ((channel->activeBytes != 0
                        && channel->activeProductIdentity == productIdentity)
                    || (channel->retiringBytes != 0
                        && channel->retiringProductIdentity
                            == productIdentity));
        });
}

bool GetAdmissionValidLocked(
    const RenderCoordinatorState& state,
    const RenderChannelState* replacedChannel,
    const std::uint64_t candidateBytes) noexcept
{
    const auto resources = GetResourceStateLocked(state);
    std::uint64_t total = 0;
    if (!GetSumValid(resources.activeBytes, resources.runningBytes, total)
        || !GetSumValid(total, resources.pendingBytes, total)
        || !GetSumValid(total, resources.cacheBytes, total)) {
        return false;
    }
    if (replacedChannel && replacedChannel->pending) {
        const auto replacedBytes =
            replacedChannel->pending->estimatedBytes;
        if (replacedBytes > total) return false;
        total -= replacedBytes;
    }
    if (replacedChannel
        && replacedChannel->transition.status
            == RenderProductStatus::Ready) {
        const auto replacedBytes =
            replacedChannel->transition.stats.candidateBytes;
        if (replacedBytes > total) return false;
        total -= replacedBytes;
    }
    return candidateBytes <= state.cpuBudgetBytes
        && total <= state.cpuBudgetBytes - candidateBytes;
}

bool GetCpuTotalLocked(
    const RenderCoordinatorState& state,
    std::uint64_t& total) noexcept
{
    const auto resources = GetResourceStateLocked(state);
    total = 0;
    return GetSumValid(resources.activeBytes, resources.runningBytes, total)
        && GetSumValid(total, resources.pendingBytes, total)
        && GetSumValid(total, resources.cacheBytes, total);
}

bool TryMakeCacheSpaceLocked(
    RenderCoordinatorState& state,
    const std::uint64_t addedBytes)
{
    std::uint64_t total = 0;
    if (!GetCpuTotalLocked(state, total)
        || addedBytes > state.cpuBudgetBytes) {
        return false;
    }
    const auto getFits = [&]() {
        return total <= state.cpuBudgetBytes - addedBytes;
    };
    while (!getFits()) {
        bool isVolume = false;
        std::size_t victimIndex = 0;
        std::uint64_t victimStamp =
            (std::numeric_limits<std::uint64_t>::max)();
        bool hasVictim = false;
        // 只有 cache 是唯一 strong owner 时，erase 才会真实释放这笔
        // 已核算内存；active/retiring 另由 product identity 去重，临时
        // caller handle 仍必须阻止“账面驱逐、物理内存未释放”。
        for (std::size_t index = 0;
            index < state.volumeCache.size(); ++index) {
            const auto& entry = state.volumeCache[index];
            if (!entry.product || entry.product.use_count() != 1) continue;
            if (!hasVictim || entry.lastUse < victimStamp) {
                hasVictim = true;
                isVolume = true;
                victimIndex = index;
                victimStamp = entry.lastUse;
            }
        }
        for (std::size_t index = 0;
            index < state.isoCache.size(); ++index) {
            const auto& entry = state.isoCache[index];
            if (!entry.product || entry.product.use_count() != 1) continue;
            if (!hasVictim || entry.lastUse < victimStamp) {
                hasVictim = true;
                isVolume = false;
                victimIndex = index;
                victimStamp = entry.lastUse;
            }
        }
        if (!hasVictim) return false;
        const std::uint64_t removedBytes = isVolume
            ? state.volumeCache[victimIndex].bytes
            : state.isoCache[victimIndex].bytes;
        if (removedBytes > state.cacheBytes || removedBytes > total) {
            return false;
        }
        state.cacheBytes -= removedBytes;
        total -= removedBytes;
        if (isVolume) {
            state.volumeCache.erase(
                state.volumeCache.begin()
                    + static_cast<std::ptrdiff_t>(victimIndex));
        }
        else {
            state.isoCache.erase(
                state.isoCache.begin()
                    + static_cast<std::ptrdiff_t>(victimIndex));
        }
    }
    return true;
}

bool TryEvictOldestVolumeLocked(RenderCoordinatorState& state)
{
    auto victim = state.volumeCache.end();
    for (auto iterator = state.volumeCache.begin();
        iterator != state.volumeCache.end(); ++iterator) {
        // 外部 strong owner 存在时，移除 cache 不会释放产品内存。
        if (!iterator->product || iterator->product.use_count() != 1) {
            continue;
        }
        if (victim == state.volumeCache.end()
            || iterator->lastUse < victim->lastUse) {
            victim = iterator;
        }
    }
    if (victim == state.volumeCache.end()
        || victim->bytes > state.cacheBytes) {
        return false;
    }
    state.cacheBytes -= victim->bytes;
    state.volumeCache.erase(victim);
    return true;
}

void SetRequestFailureLocked(
    RenderChannelState& channel,
    const std::uint64_t requestRevision,
    const RenderProductFailure failure,
    const char* message)
{
    if (channel.transition.stats.requestRevision
        != requestRevision) {
        return;
    }
    channel.transition.status = failure == RenderProductFailure::Cancelled
        || failure == RenderProductFailure::Stopping
        ? RenderProductStatus::Cancelled
        : RenderProductStatus::Failed;
    channel.transition.failureReason = failure;
    channel.transition.message = message ? message : "";
    channel.transition.stats.candidateBytes = 0;
}

void SetTaskComplete(
    const std::shared_ptr<RenderCoordinatorState>& state,
    const std::shared_ptr<RenderChannelState>& channel,
    const std::shared_ptr<std::atomic<bool>>& isCancelled,
    const std::uint64_t requestRevision,
    const bool isSucceeded)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    const bool isCurrentRunning = state->running
        && state->running->channel == channel
        && state->running->requestRevision == requestRevision
        && state->running->isCancelled == isCancelled;
    if (!isCurrentRunning) return;

    const bool isStopped = state->isStopping
        || channel->isStopped
        || isCancelled->load(std::memory_order_acquire);
    if (channel->transition.stats.requestRevision
        == requestRevision) {
        if (isStopped) {
            if (channel->transition.failureReason
                != RenderProductFailure::ResourceRejected) {
                SetRequestFailureLocked(
                    *channel,
                    requestRevision,
                    state->isStopping
                        ? RenderProductFailure::Stopping
                        : RenderProductFailure::Cancelled,
                    state->isStopping
                        ? "The render coordinator is stopping."
                        : "The render product request was replaced.");
            }
        }
        else if (!isSucceeded) {
            SetRequestFailureLocked(
                *channel,
                requestRevision,
                RenderProductFailure::BuildFailed,
                "The render product task failed.");
        }
        else {
            channel->transition.status = RenderProductStatus::Ready;
            channel->transition.failureReason = RenderProductFailure::None;
            channel->transition.message.clear();
            channel->transition.stats.candidateBytes =
                state->running->accountedBytes;
        }
    }
    channel->runningRevision = 0;
    state->running.reset();
    state->stopped.notify_all();
}

} // namespace

class RenderTaskToken::Impl final {
public:
    std::shared_ptr<RenderCoordinatorState> state;
    std::shared_ptr<RenderChannelState> channel;
    std::shared_ptr<std::atomic<bool>> isCancelled;
    TaskStopToken laneToken;
    std::uint64_t requestRevision = 0;
};

class RenderTaskChannel::Impl final {
public:
    Impl(
        std::shared_ptr<RenderCoordinatorState> coordinator,
        std::shared_ptr<RenderChannelState> taskChannel)
        : state(std::move(coordinator))
        , channel(std::move(taskChannel))
    {
    }

    std::shared_ptr<RenderCoordinatorState> state;
    std::shared_ptr<RenderChannelState> channel;
};

class RenderResourceCoordinator::Impl final {
public:
    explicit Impl(RenderLaneStart onTaskStart)
        : state(std::make_shared<RenderCoordinatorState>(
            std::move(onTaskStart)))
    {
    }

    static bool SendTasks(
        const std::shared_ptr<RenderCoordinatorState>& state)
    {
        std::shared_ptr<RenderChannelState> channel;
        RenderTaskRequest request;
        auto isCancelled = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->isStopping || state->running) return true;
            while (!state->readyChannels.empty()) {
                channel = state->readyChannels.front().lock();
                state->readyChannels.pop_front();
                if (!channel) continue;
                channel->isQueued = false;
                if (channel->isStopped || !channel->pending) {
                    channel.reset();
                    continue;
                }
                request = std::move(*channel->pending);
                channel->pending.reset();
                break;
            }
            if (!channel) return true;

            channel->runningRevision = request.requestRevision;
            state->running = RenderRunningTask{
                channel,
                isCancelled,
                request.requestRevision,
                request.estimatedBytes
            };
        }

        const std::uint64_t requestRevision = request.requestRevision;
        RenderLaneWork laneWork(
            [coordinator = state,
                taskChannel = channel,
                cancel = isCancelled,
                request = std::move(request)](
                    const TaskStopToken laneToken) mutable {
                auto tokenImpl = std::make_shared<RenderTaskToken::Impl>();
                tokenImpl->state = coordinator;
                tokenImpl->channel = taskChannel;
                tokenImpl->isCancelled = cancel;
                tokenImpl->laneToken = laneToken;
                tokenImpl->requestRevision = request.requestRevision;
                RenderTaskToken token(std::move(tokenImpl));

                bool isSucceeded = false;
                if (!token.GetIsStopped()) {
                    try {
                        request.work(token);
                        isSucceeded = !token.GetIsStopped();
                    }
                    catch (...) {
                        isSucceeded = false;
                    }
                }
                SetTaskComplete(
                    coordinator,
                    taskChannel,
                    cancel,
                    request.requestRevision,
                    isSucceeded);
                return isSucceeded;
            });

        bool isStarted = false;
        try {
            isStarted = state->onTaskStart
                && state->onTaskStart(std::move(laneWork));
        }
        catch (...) {
            isStarted = false;
        }
        if (isStarted) return true;

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const bool isCurrentRunning = state->running
                && state->running->channel == channel
                && state->running->requestRevision == requestRevision;
            if (isCurrentRunning) {
                channel->runningRevision = 0;
                SetRequestFailureLocked(
                    *channel,
                    requestRevision,
                    RenderProductFailure::TaskRejected,
                    "The render task lane rejected the request.");
                state->running.reset();
                state->stopped.notify_all();
            }
        }
        return false;
    }

    bool SendTasks()
    {
        return SendTasks(state);
    }

    std::shared_ptr<RenderCoordinatorState> state;
};

RenderTaskToken::RenderTaskToken(std::shared_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

bool RenderTaskToken::GetIsStopped() const noexcept
{
    if (!m_impl) return false;
    if (!m_impl->state || !m_impl->isCancelled) return true;
    if (m_impl->laneToken.GetIsStopped()
        || m_impl->isCancelled->load(std::memory_order_acquire)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    return m_impl->state->isStopping
        || !m_impl->channel
        || m_impl->channel->isStopped;
}

bool RenderTaskToken::SetActualBytes(
    const std::uint64_t actualBytes) const
{
    if (!m_impl) return true;
    if (!m_impl->state || !m_impl->channel
        || !m_impl->isCancelled) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& state = *m_impl->state;
    const bool isCurrentRunning = state.running
        && state.running->channel == m_impl->channel
        && state.running->requestRevision == m_impl->requestRevision
        && state.running->isCancelled == m_impl->isCancelled;
    if (!isCurrentRunning || state.isStopping
        || m_impl->channel->isStopped
        || m_impl->isCancelled->load(std::memory_order_acquire)) {
        return false;
    }

    const std::uint64_t previousBytes =
        state.running->accountedBytes;
    const std::uint64_t addedBytes = actualBytes > previousBytes
        ? actualBytes - previousBytes : 0;
    if (!TryMakeCacheSpaceLocked(state, addedBytes)) {
        m_impl->isCancelled->store(true, std::memory_order_release);
        SetRequestFailureLocked(
            *m_impl->channel,
            m_impl->requestRevision,
            RenderProductFailure::ResourceRejected,
            "The actual render product size exceeds the CPU budget.");
        return false;
    }

    const auto resources = GetResourceStateLocked(state);
    std::uint64_t committedBytes = 0;
    const bool hasCommittedSum = GetSumValid(
        resources.activeBytes,
        resources.pendingBytes,
        committedBytes)
        && GetSumValid(
            committedBytes,
            resources.cacheBytes,
            committedBytes);
    if (!hasCommittedSum
        || actualBytes > state.cpuBudgetBytes
        || committedBytes > state.cpuBudgetBytes - actualBytes) {
        m_impl->isCancelled->store(true, std::memory_order_release);
        SetRequestFailureLocked(
            *m_impl->channel,
            m_impl->requestRevision,
            RenderProductFailure::ResourceRejected,
            "The actual render product size exceeds the CPU budget.");
        return false;
    }
    state.running->accountedBytes = actualBytes;
    m_impl->channel->transition.stats.candidateBytes = actualBytes;
    return true;
}

RenderTaskChannel::RenderTaskChannel(std::shared_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

RenderTaskChannel::~RenderTaskChannel() noexcept
{
    (void)Stop();
}

RenderTaskAdmission RenderTaskChannel::StartTask(
    RenderTaskRequest request)
{
    if (!m_impl || !m_impl->state || !m_impl->channel) {
        return RenderTaskAdmission::Unavailable;
    }
    auto& state = *m_impl->state;
    auto& channel = *m_impl->channel;
    RenderTaskAdmission admission = RenderTaskAdmission::Accepted;
    std::optional<RenderTaskRequest> replacedRequest;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.isStopping || channel.isStopped) {
            return RenderTaskAdmission::Stopping;
        }
        if (request.requestRevision == 0 || !request.work) {
            channel.transition.status = RenderProductStatus::Failed;
            channel.transition.failureReason =
                RenderProductFailure::InvalidInput;
            channel.transition.message =
                "The render task request is invalid.";
            return RenderTaskAdmission::Unavailable;
        }
        if (!GetAdmissionValidLocked(
                state, &channel, request.estimatedBytes)
            && (!TryMakeCacheSpaceLocked(
                    state, request.estimatedBytes)
                || !GetAdmissionValidLocked(
                    state, &channel, request.estimatedBytes))) {
            channel.transition.status = RenderProductStatus::Failed;
            channel.transition.failureReason =
                RenderProductFailure::ResourceRejected;
            channel.transition.message =
                "The render task estimate exceeds the CPU budget.";
            return RenderTaskAdmission::ResourceRejected;
        }

        if (channel.pending) {
            admission = RenderTaskAdmission::Replaced;
            replacedRequest = std::move(channel.pending);
        }
        if (state.running
            && state.running->channel == m_impl->channel) {
            state.running->isCancelled->store(
                true, std::memory_order_release);
        }
        channel.pending = std::move(request);
        channel.transition.status = RenderProductStatus::Preparing;
        channel.transition.failureReason = RenderProductFailure::None;
        channel.transition.message.clear();
        channel.transition.stats.requestRevision =
            channel.pending->requestRevision;
        channel.transition.stats.candidateBytes =
            channel.pending->estimatedBytes;
        if (!channel.isQueued) {
            state.readyChannels.push_back(m_impl->channel);
            channel.isQueued = true;
        }
    }

    if (!RenderResourceCoordinator::Impl::SendTasks(m_impl->state)) {
        return RenderTaskAdmission::Unavailable;
    }
    return admission;
}

RenderTransitionState RenderTaskChannel::GetState() const
{
    if (!m_impl || !m_impl->state || !m_impl->channel) {
        RenderTransitionState state;
        state.status = RenderProductStatus::Failed;
        state.failureReason = RenderProductFailure::Stopping;
        state.message = "The render task channel is unavailable.";
        return state;
    }
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    return m_impl->channel->transition;
}

bool RenderTaskChannel::SetActiveBytes(
    const std::uint64_t requestRevision,
    const std::uint64_t activeBytes,
    const void* productIdentity)
{
    if (!m_impl || !m_impl->state || !m_impl->channel) return false;
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& state = *m_impl->state;
    auto& channel = *m_impl->channel;
    if (channel.isStopped
        || channel.transition.status != RenderProductStatus::Ready
        || channel.transition.stats.requestRevision != requestRevision
        || channel.retiringBytes != 0
        || channel.retiringProductIdentity) {
        return false;
    }

    const auto previousTransition = channel.transition;
    const std::uint64_t previousActiveBytes = channel.activeBytes;
    const void* previousActiveIdentity = channel.activeProductIdentity;
    channel.retiringRevision = channel.transition.stats.activeRevision;
    channel.retiringBytes = channel.activeBytes;
    channel.retiringProductIdentity = channel.activeProductIdentity;
    channel.activeBytes = activeBytes;
    channel.activeProductIdentity = productIdentity;
    channel.transition.status = RenderProductStatus::Active;
    channel.transition.failureReason = RenderProductFailure::None;
    channel.transition.stats.activeRevision = requestRevision;
    channel.transition.stats.activeBytes = activeBytes;
    channel.transition.stats.candidateBytes = 0;
    channel.transition.message.clear();
    std::uint64_t total = 0;
    if (!GetCpuTotalLocked(state, total)
        || total > state.cpuBudgetBytes) {
        channel.transition = previousTransition;
        channel.activeBytes = previousActiveBytes;
        channel.activeProductIdentity = previousActiveIdentity;
        channel.retiringRevision = 0;
        channel.retiringBytes = 0;
        channel.retiringProductIdentity = nullptr;
        return false;
    }
    return true;
}

bool RenderTaskChannel::SetCachedActive(
    const std::uint64_t requestRevision,
    const std::uint64_t activeBytes,
    const void* productIdentity)
{
    if (!m_impl || !m_impl->state || !m_impl->channel
        || requestRevision == 0 || activeBytes == 0) {
        return false;
    }
    std::optional<RenderTaskRequest> discardedRequest;
    {
        std::lock_guard<std::mutex> lock(m_impl->state->mutex);
        auto& state = *m_impl->state;
        auto& channel = *m_impl->channel;
        if (state.isStopping || channel.isStopped
            || channel.retiringBytes != 0
            || channel.retiringProductIdentity) {
            return false;
        }

        const auto previousTransition = channel.transition;
        const std::uint64_t previousActiveBytes = channel.activeBytes;
        const void* previousActiveIdentity = channel.activeProductIdentity;
        const bool wasQueued = channel.isQueued;
        discardedRequest = std::move(channel.pending);
        channel.pending.reset();
        channel.retiringRevision = channel.transition.stats.activeRevision;
        channel.retiringBytes = channel.activeBytes;
        channel.retiringProductIdentity = channel.activeProductIdentity;
        channel.activeBytes = activeBytes;
        channel.activeProductIdentity = productIdentity;
        channel.transition.status = RenderProductStatus::Active;
        channel.transition.failureReason = RenderProductFailure::None;
        channel.transition.stats.requestRevision = requestRevision;
        channel.transition.stats.activeRevision = requestRevision;
        channel.transition.stats.activeBytes = activeBytes;
        channel.transition.stats.candidateBytes = 0;
        channel.transition.message.clear();
        std::uint64_t total = 0;
        if (!GetCpuTotalLocked(state, total)
            || total > state.cpuBudgetBytes) {
            channel.pending = std::move(discardedRequest);
            channel.transition = previousTransition;
            channel.activeBytes = previousActiveBytes;
            channel.activeProductIdentity = previousActiveIdentity;
            channel.retiringRevision = 0;
            channel.retiringBytes = 0;
            channel.retiringProductIdentity = nullptr;
            channel.isQueued = wasQueued;
            return false;
        }
        channel.isQueued = false;
        if (state.running
            && state.running->channel == m_impl->channel) {
            state.running->isCancelled->store(
                true, std::memory_order_release);
        }
    }
    return true;
}

bool RenderTaskChannel::CompleteActiveBytes(
    const std::uint64_t requestRevision)
{
    if (!m_impl || !m_impl->state || !m_impl->channel) return false;
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& channel = *m_impl->channel;
    if (channel.isStopped
        || channel.transition.status != RenderProductStatus::Active
        || channel.transition.stats.activeRevision != requestRevision) {
        return false;
    }
    channel.retiringRevision = 0;
    channel.retiringBytes = 0;
    channel.retiringProductIdentity = nullptr;
    return true;
}

bool RenderTaskChannel::RestoreActiveBytes(
    const std::uint64_t requestRevision)
{
    if (!m_impl || !m_impl->state || !m_impl->channel) return false;
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& channel = *m_impl->channel;
    if (channel.isStopped
        || channel.transition.status != RenderProductStatus::Active
        || channel.transition.stats.activeRevision != requestRevision) {
        return false;
    }
    channel.activeBytes = channel.retiringBytes;
    channel.activeProductIdentity = channel.retiringProductIdentity;
    channel.transition.status = channel.retiringRevision != 0
        ? RenderProductStatus::Active : RenderProductStatus::Failed;
    channel.transition.failureReason = channel.retiringRevision != 0
        ? RenderProductFailure::None
        : RenderProductFailure::CommitFailed;
    channel.transition.stats.activeRevision = channel.retiringRevision;
    channel.transition.stats.activeBytes = channel.retiringBytes;
    channel.transition.stats.candidateBytes = 0;
    channel.transition.message = channel.retiringRevision != 0
        ? "" : "The render product commit failed.";
    channel.retiringRevision = 0;
    channel.retiringBytes = 0;
    channel.retiringProductIdentity = nullptr;
    return true;
}

bool RenderTaskChannel::SetReadyFailed(
    const std::uint64_t requestRevision,
    const RenderProductFailure failureReason,
    std::string message)
{
    if (!m_impl || !m_impl->state || !m_impl->channel
        || failureReason == RenderProductFailure::None) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& channel = *m_impl->channel;
    if (channel.isStopped
        || channel.transition.status != RenderProductStatus::Ready
        || channel.transition.stats.requestRevision != requestRevision) {
        return false;
    }
    SetRequestFailureLocked(
        channel, requestRevision, failureReason, message.c_str());
    return true;
}

bool RenderTaskChannel::Stop()
{
    if (!m_impl || !m_impl->state || !m_impl->channel) return true;
    std::optional<RenderTaskRequest> discardedRequest;
    {
        std::lock_guard<std::mutex> lock(m_impl->state->mutex);
        auto& state = *m_impl->state;
        auto& channel = *m_impl->channel;
        if (channel.isStopped) return true;
        channel.isStopped = true;
        discardedRequest = std::move(channel.pending);
        channel.pending.reset();
        channel.isQueued = false;
        if (state.running && state.running->channel == m_impl->channel) {
            state.running->isCancelled->store(
                true, std::memory_order_release);
        }
        if (channel.transition.status == RenderProductStatus::Preparing
            || channel.transition.status == RenderProductStatus::Ready) {
            SetRequestFailureLocked(
                channel,
                channel.transition.stats.requestRevision,
                RenderProductFailure::Stopping,
                "The render task channel is stopping.");
        }
    }
    return true;
}

RenderResourceCoordinator::RenderResourceCoordinator(
    RenderLaneStart onTaskStart)
    : m_impl(std::make_unique<Impl>(std::move(onTaskStart)))
{
}

RenderResourceCoordinator::~RenderResourceCoordinator() noexcept
{
    if (m_impl) {
        (void)m_impl->state;
        (void)Stop(
            std::chrono::steady_clock::time_point::max());
    }
}

std::shared_ptr<RenderTaskChannel>
RenderResourceCoordinator::CreateTaskChannel(
    const RenderProductKind productKind)
{
    if (!m_impl || !m_impl->state) return nullptr;
    auto channel = std::make_shared<RenderChannelState>();
    channel->productKind = productKind;
    {
        std::lock_guard<std::mutex> lock(m_impl->state->mutex);
        if (m_impl->state->isStopping) return nullptr;
        m_impl->state->channels.push_back(channel);
    }
    auto channelImpl = std::make_shared<RenderTaskChannel::Impl>(
        m_impl->state, channel);
    return std::shared_ptr<RenderTaskChannel>(
        new RenderTaskChannel(std::move(channelImpl)));
}

bool RenderResourceCoordinator::SendTasks()
{
    return m_impl && m_impl->SendTasks();
}

bool RenderResourceCoordinator::StartStop()
{
    if (!m_impl || !m_impl->state) return true;
    std::vector<RenderTaskRequest> discardedRequests;
    std::vector<VolumeCacheEntry> discardedVolumeCache;
    std::vector<IsoCacheEntry> discardedIsoCache;
    std::vector<GpuContextEntry> discardedGpuContexts;
    {
        std::lock_guard<std::mutex> lock(m_impl->state->mutex);
        auto& state = *m_impl->state;
        if (state.isStopping) return true;
        state.isAccepting = false;
        state.isStopping = true;
        if (state.running) {
            state.running->isCancelled->store(
                true, std::memory_order_release);
        }
        discardedRequests.reserve(state.channels.size());
        for (const auto& weakChannel : state.channels) {
            const auto channel = weakChannel.lock();
            if (!channel) continue;
            if (channel->pending) {
                discardedRequests.push_back(
                    std::move(*channel->pending));
                channel->pending.reset();
            }
            channel->isQueued = false;
            if (channel->transition.status == RenderProductStatus::Preparing
                || channel->transition.status == RenderProductStatus::Ready) {
                SetRequestFailureLocked(
                    *channel,
                    channel->transition.stats.requestRevision,
                    RenderProductFailure::Stopping,
                    "The render coordinator is stopping.");
            }
        }
        state.readyChannels.clear();
        discardedVolumeCache = std::move(state.volumeCache);
        discardedIsoCache = std::move(state.isoCache);
        discardedGpuContexts = std::move(state.gpuContexts);
        state.cacheBytes = 0;
        state.stopped.notify_all();
    }
    return true;
}

bool RenderResourceCoordinator::Stop(
    const std::chrono::steady_clock::time_point deadline)
{
    if (!StartStop() || !m_impl || !m_impl->state) return false;
    std::unique_lock<std::mutex> lock(m_impl->state->mutex);
    const auto isStopped = [this] {
        return !m_impl->state->running.has_value();
    };
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        m_impl->state->stopped.wait(lock, isStopped);
        return true;
    }
    return m_impl->state->stopped.wait_until(
        lock, deadline, isStopped);
}

RenderResourceState
RenderResourceCoordinator::GetResourceState() const
{
    if (!m_impl || !m_impl->state) return {};
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    return GetResourceStateLocked(*m_impl->state);
}

bool RenderResourceCoordinator::SetCpuBudgetBytes(
    const std::uint64_t budgetBytes)
{
    if (!m_impl || !m_impl->state || budgetBytes == 0) return false;
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    std::uint64_t total = 0;
    if (!GetCpuTotalLocked(*m_impl->state, total)
        || total > budgetBytes) {
        return false;
    }
    m_impl->state->cpuBudgetBytes = budgetBytes;
    return true;
}

std::shared_ptr<const VolumeLodProduct>
RenderResourceCoordinator::GetVolumeProduct(
    const VolumeLodKey& key)
{
    if (!m_impl || !m_impl->state) return nullptr;
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& state = *m_impl->state;
    const auto entry = std::find_if(
        state.volumeCache.begin(), state.volumeCache.end(),
        [&key](const VolumeCacheEntry& candidate) {
            return candidate.product
                && GetVolumeKeyEqual(candidate.key, key);
        });
    if (entry == state.volumeCache.end()) return nullptr;
    entry->lastUse = ++state.cacheUseStamp;
    return entry->product;
}

bool RenderResourceCoordinator::SetVolumeProduct(
    const VolumeLodKey& key,
    std::shared_ptr<const VolumeLodProduct> product)
{
    if (!m_impl || !m_impl->state || !product || !product->volume
        || product->actualBytes == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& state = *m_impl->state;
    const auto existing = std::find_if(
        state.volumeCache.begin(), state.volumeCache.end(),
        [&key](const VolumeCacheEntry& candidate) {
            return candidate.product
                && GetVolumeKeyEqual(candidate.key, key);
        });
    if (existing != state.volumeCache.end()) {
        existing->lastUse = ++state.cacheUseStamp;
        return true;
    }
    constexpr std::size_t maxVolumeCacheEntries = 3;
    while (state.volumeCache.size() >= maxVolumeCacheEntries) {
        if (!TryEvictOldestVolumeLocked(state)) return false;
    }
    const std::uint64_t addedBytes = GetProductActiveLocked(
        state, product.get()) ? 0 : product->actualBytes;
    if (!TryMakeCacheSpaceLocked(state, addedBytes)
        || product->actualBytes
            > (std::numeric_limits<std::uint64_t>::max)()
                - state.cacheBytes) {
        return false;
    }
    const std::uint64_t bytes = product->actualBytes;
    try {
        state.volumeCache.push_back(VolumeCacheEntry{
            key,
            std::move(product),
            bytes,
            ++state.cacheUseStamp
        });
    }
    catch (...) {
        return false;
    }
    state.cacheBytes += bytes;
    return true;
}

std::shared_ptr<const IsoSurfaceProduct>
RenderResourceCoordinator::GetIsoSurfaceProduct(
    const IsoSurfaceKey& key)
{
    if (!m_impl || !m_impl->state) return nullptr;
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& state = *m_impl->state;
    const auto entry = std::find_if(
        state.isoCache.begin(), state.isoCache.end(),
        [&key](const IsoCacheEntry& candidate) {
            return candidate.product
                && GetIsoKeyEqual(candidate.key, key);
        });
    if (entry == state.isoCache.end()) return nullptr;
    entry->lastUse = ++state.cacheUseStamp;
    return entry->product;
}

bool RenderResourceCoordinator::SetIsoSurfaceProduct(
    const IsoSurfaceKey& key,
    std::shared_ptr<const IsoSurfaceProduct> product)
{
    if (!m_impl || !m_impl->state || !product || !product->surface
        || product->actualBytes == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& state = *m_impl->state;
    const auto existing = std::find_if(
        state.isoCache.begin(), state.isoCache.end(),
        [&key](const IsoCacheEntry& candidate) {
            return candidate.product
                && GetIsoKeyEqual(candidate.key, key);
        });
    if (existing != state.isoCache.end()) {
        existing->lastUse = ++state.cacheUseStamp;
        return true;
    }
    const std::uint64_t addedBytes = GetProductActiveLocked(
        state, product.get()) ? 0 : product->actualBytes;
    if (!TryMakeCacheSpaceLocked(state, addedBytes)
        || product->actualBytes
            > (std::numeric_limits<std::uint64_t>::max)()
                - state.cacheBytes) {
        return false;
    }
    const std::uint64_t bytes = product->actualBytes;
    try {
        state.isoCache.push_back(IsoCacheEntry{
            key,
            std::move(product),
            bytes,
            ++state.cacheUseStamp
        });
    }
    catch (...) {
        return false;
    }
    state.cacheBytes += bytes;
    return true;
}

bool RenderResourceCoordinator::SetGpuContextBudget(
    const void* contextIdentity,
    const std::uint64_t budgetBytes)
{
    if (!m_impl || !m_impl->state || !contextIdentity
        || budgetBytes == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& contexts = m_impl->state->gpuContexts;
    auto context = std::find_if(
        contexts.begin(), contexts.end(),
        [contextIdentity](const GpuContextEntry& entry) {
            return entry.contextIdentity == contextIdentity;
        });
    if (context == contexts.end()) {
        contexts.push_back(GpuContextEntry{
            contextIdentity, budgetBytes, {}
        });
        return true;
    }
    std::uint64_t reservedBytes = 0;
    for (const auto& reservation : context->reservations) {
        if (!GetSumValid(
                reservedBytes, reservation.bytes, reservedBytes)) {
            return false;
        }
    }
    if (reservedBytes > budgetBytes) return false;
    context->budgetBytes = budgetBytes;
    return true;
}

bool RenderResourceCoordinator::SetGpuReservation(
    const void* contextIdentity,
    const void* ownerIdentity,
    const std::uint64_t reservedBytes)
{
    if (!m_impl || !m_impl->state || !contextIdentity
        || !ownerIdentity || reservedBytes == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& contexts = m_impl->state->gpuContexts;
    auto context = std::find_if(
        contexts.begin(), contexts.end(),
        [contextIdentity](const GpuContextEntry& entry) {
            return entry.contextIdentity == contextIdentity;
        });
    if (context == contexts.end()) return false;
    auto reservation = std::find_if(
        context->reservations.begin(), context->reservations.end(),
        [ownerIdentity](const GpuReservation& entry) {
            return entry.ownerIdentity == ownerIdentity;
        });
    std::uint64_t otherBytes = 0;
    for (const auto& entry : context->reservations) {
        if (&entry == (reservation != context->reservations.end()
                ? &*reservation : nullptr)) {
            continue;
        }
        if (!GetSumValid(otherBytes, entry.bytes, otherBytes)) {
            return false;
        }
    }
    if (reservedBytes > context->budgetBytes
        || otherBytes > context->budgetBytes - reservedBytes) {
        return false;
    }
    if (reservation == context->reservations.end()) {
        context->reservations.push_back(
            GpuReservation{ ownerIdentity, reservedBytes });
    }
    else {
        reservation->bytes = reservedBytes;
    }
    return true;
}

bool RenderResourceCoordinator::ClearGpuReservation(
    const void* contextIdentity,
    const void* ownerIdentity)
{
    if (!m_impl || !m_impl->state || !contextIdentity
        || !ownerIdentity) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& contexts = m_impl->state->gpuContexts;
    const auto context = std::find_if(
        contexts.begin(), contexts.end(),
        [contextIdentity](const GpuContextEntry& entry) {
            return entry.contextIdentity == contextIdentity;
        });
    if (context == contexts.end()) return true;
    context->reservations.erase(
        std::remove_if(
            context->reservations.begin(), context->reservations.end(),
            [ownerIdentity](const GpuReservation& entry) {
                return entry.ownerIdentity == ownerIdentity;
            }),
        context->reservations.end());
    return true;
}

bool RenderResourceCoordinator::ClearGpuContext(
    const void* contextIdentity)
{
    if (!m_impl || !m_impl->state || !contextIdentity) return false;
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    auto& contexts = m_impl->state->gpuContexts;
    contexts.erase(
        std::remove_if(
            contexts.begin(), contexts.end(),
            [contextIdentity](const GpuContextEntry& entry) {
                return entry.contextIdentity == contextIdentity;
            }),
        contexts.end());
    return true;
}

RenderGpuResourceState
RenderResourceCoordinator::GetGpuResourceState(
    const void* contextIdentity) const
{
    if (!m_impl || !m_impl->state || !contextIdentity) return {};
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    const auto& contexts = m_impl->state->gpuContexts;
    const auto context = std::find_if(
        contexts.begin(), contexts.end(),
        [contextIdentity](const GpuContextEntry& entry) {
            return entry.contextIdentity == contextIdentity;
        });
    if (context == contexts.end()) return {};
    RenderGpuResourceState result;
    result.budgetBytes = context->budgetBytes;
    result.reservationCount = context->reservations.size();
    for (const auto& reservation : context->reservations) {
        if (!GetSumValid(
                result.reservedBytes,
                reservation.bytes,
                result.reservedBytes)) {
            result.reservedBytes =
                (std::numeric_limits<std::uint64_t>::max)();
            break;
        }
    }
    return result;
}

std::uint64_t RenderResourceCoordinator::AdvanceTopologyRevision()
{
    if (!m_impl || !m_impl->state) return 0;
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    if (m_impl->state->topologyRevision
        != (std::numeric_limits<std::uint64_t>::max)()) {
        ++m_impl->state->topologyRevision;
    }
    return m_impl->state->topologyRevision;
}

std::uint64_t RenderResourceCoordinator::GetTopologyRevision() const
{
    if (!m_impl || !m_impl->state) return 0;
    std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    return m_impl->state->topologyRevision;
}
