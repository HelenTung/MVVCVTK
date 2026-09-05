#include "SurfaceDeterminationService.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr std::size_t completionLimit = 64;

std::vector<SurfaceJobComplete> BuildCompletionQueue()
{
    std::vector<SurfaceJobComplete> complete;
    complete.reserve(completionLimit);
    return complete;
}

} // namespace

SurfaceDeterminationService::SurfaceDeterminationService()
    : m_complete(BuildCompletionQueue())
    , m_worker([this] { WorkerLoop(); })
{
    // completion 槽在 worker 启动前一次性分配；运行期间不再扩容。
}

SurfaceDeterminationService::~SurfaceDeterminationService() noexcept
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (!Stop(deadline)) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_isStopping = true;
            if (m_activeCancel) {
                m_activeCancel->store(true, std::memory_order_release);
            }
            if (m_pendingJob && m_pendingJob->isCancelled) {
                m_pendingJob->isCancelled->store(
                    true, std::memory_order_release);
            }
            m_pendingJob.reset();
        }
        m_workReady.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }
}

SurfaceAdmissionStatus SurfaceDeterminationService::Start(
    TrustedImageSnapshot source,
    SurfaceDeterminationStartParams params,
    const std::size_t maxWorkingBytes,
    const std::uint64_t requestId)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isStopping) return SurfaceAdmissionStatus::Stopping;
    if (!source || !source->image || requestId == 0
        || maxWorkingBytes == 0) {
        return SurfaceAdmissionStatus::InvalidRequest;
    }
    const std::size_t outstandingCount = m_complete.size()
        + (m_activeRequestId != 0 ? 1U : 0U)
        + (m_pendingJob ? 1U : 0U);
    if (outstandingCount >= completionLimit) {
        return SurfaceAdmissionStatus::Unavailable;
    }

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    Job nextJob{
        std::move(source),
        std::move(params),
        maxWorkingBytes,
        requestId,
        std::move(cancel)
    };

    if (m_activeCancel) {
        m_activeCancel->store(true, std::memory_order_release);
    }
    if (m_pendingJob) {
        m_pendingJob->isCancelled->store(
            true, std::memory_order_release);
        m_complete.push_back(BuildCancelled(*m_pendingJob));
        m_pendingJob.reset();
    }
    m_pendingJob = std::move(nextJob);
    m_latestRequestId = requestId;
    m_progressPermille.store(0, std::memory_order_relaxed);
    m_progressStage.store(
        static_cast<std::uint8_t>(SurfaceDeterminationStage::Preparing),
        std::memory_order_relaxed);
    m_progressRequestId.store(requestId, std::memory_order_release);
    m_workReady.notify_one();
    return SurfaceAdmissionStatus::Accepted;
}

bool SurfaceDeterminationService::StopRequest(
    std::uint64_t requestId) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (requestId == 0) requestId = m_latestRequestId;
    bool didStop = false;
    if (m_activeRequestId == requestId && m_activeCancel) {
        m_activeCancel->store(true, std::memory_order_release);
        didStop = true;
    }
    if (m_pendingJob && m_pendingJob->requestId == requestId) {
        m_pendingJob->isCancelled->store(
            true, std::memory_order_release);
        didStop = true;
    }
    return didStop;
}

std::optional<SurfaceJobComplete>
SurfaceDeterminationService::GetComplete()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_complete.empty()) return std::nullopt;
    SurfaceJobComplete complete = std::move(m_complete.front());
    m_complete.erase(m_complete.begin());
    return complete;
}

std::optional<SurfaceRequestProgress>
SurfaceDeterminationService::GetProgress(
    const std::uint64_t requestId) const noexcept
{
    if (requestId == 0
        || m_progressRequestId.load(std::memory_order_acquire)
            != requestId) {
        return std::nullopt;
    }
    SurfaceRequestProgress progress;
    progress.stage = static_cast<SurfaceDeterminationStage>(
        m_progressStage.load(std::memory_order_acquire));
    progress.progress01 = static_cast<double>(
        m_progressPermille.load(std::memory_order_acquire)) / 1000.0;
    if (m_progressRequestId.load(std::memory_order_acquire)
        != requestId) {
        return std::nullopt;
    }
    return progress;
}

bool SurfaceDeterminationService::GetIsBusy() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeRequestId != 0 || m_pendingJob.has_value();
}

bool SurfaceDeterminationService::Stop(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_isStopping = true;
        if (m_activeCancel) {
            m_activeCancel->store(true, std::memory_order_release);
        }
        if (m_pendingJob && m_pendingJob->isCancelled) {
            m_pendingJob->isCancelled->store(
                true, std::memory_order_release);
        }
        m_pendingJob.reset();
    }
    m_workReady.notify_all();

    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_workerExited.wait_until(
            lock, deadline, [this] { return m_hasExited; })) {
        return false;
    }
    lock.unlock();
    if (m_worker.joinable()) m_worker.join();
    return true;
}

SurfaceJobComplete SurfaceDeterminationService::BuildCancelled(
    const Job& job)
{
    SurfaceJobComplete complete;
    complete.requestId = job.requestId;
    complete.result.status = SurfaceResultStatus::Cancelled;
    complete.result.failureReason = SurfaceFailureReason::Cancelled;
    complete.result.sourceVersion = job.source ? job.source->version : 0;
    complete.result.method = job.params.method;
    return complete;
}

void SurfaceDeterminationService::SetProgress(
    const std::uint64_t requestId,
    const SurfaceDeterminationStage stage,
    const double progress) noexcept
{
    if (m_progressRequestId.load(std::memory_order_acquire)
        != requestId) {
        return;
    }
    m_progressStage.store(
        static_cast<std::uint8_t>(stage),
        std::memory_order_release);
    const double bounded = std::clamp(progress, 0.0, 1.0);
    const auto target = static_cast<std::uint32_t>(
        std::lround(bounded * 1000.0));
    std::uint32_t current =
        m_progressPermille.load(std::memory_order_relaxed);
    while (current < target
        && !m_progressPermille.compare_exchange_weak(
            current,
            target,
            std::memory_order_release,
            std::memory_order_relaxed)) {
    }
}

void SurfaceDeterminationService::WorkerLoop() noexcept
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workReady.wait(lock, [this] {
                return m_isStopping || m_pendingJob.has_value();
            });
            if (m_isStopping && !m_pendingJob) break;
            job = std::move(*m_pendingJob);
            m_pendingJob.reset();
            m_activeRequestId = job.requestId;
            m_activeCancel = job.isCancelled;
        }

        SurfaceJobComplete complete;
        complete.requestId = job.requestId;
        if (job.isCancelled->load(std::memory_order_acquire)) {
            complete = BuildCancelled(job);
        }
        else {
            complete.result = SurfaceDeterminationAlgorithm::BuildSurface(
                job.source,
                job.params,
                job.maxWorkingBytes,
                [cancel = job.isCancelled] {
                    return cancel->load(std::memory_order_acquire);
                },
                [this, requestId = job.requestId](
                    const SurfaceDeterminationStage stage,
                    const double progress) {
                    SetProgress(requestId, stage, progress);
                });
        }

        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (job.requestId != m_latestRequestId
                && complete.result.status
                    == SurfaceResultStatus::Succeeded) {
                complete = BuildCancelled(job);
            }
            // Start 为每个已接纳请求预留一个 completion 槽位，不能在此
            // 静默丢弃，否则 owner-thread callback 将永远无法完成。
            m_complete.push_back(std::move(complete));
            m_activeRequestId = 0;
            m_activeCancel.reset();
        }
    }

    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_hasExited = true;
    }
    m_workerExited.notify_all();
}
