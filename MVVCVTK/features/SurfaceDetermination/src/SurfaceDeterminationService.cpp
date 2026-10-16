#include "FeatureSupport/WorkLimit.h"
#include "SurfaceDeterminationService.h"
#include "SurfaceContracts.h"
#include "SurfaceGeometryStorage.h"
#include <vtkImageData.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <limits>
#include <new>
#include <stdexcept>

SurfaceDeterminationService::SurfaceDeterminationService(std::function<void()> onWorkAvailable)
    : m_onWorkAvailable(std::move(onWorkAvailable))
{
    // 所有成员就绪后才启动线程；完成槽位由 Start 在接纳前预留。
    m_activeScope.reserve(128);
    m_worker = std::thread([this] { WorkerLoop(); });
}

SurfaceDeterminationService::~SurfaceDeterminationService() noexcept
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (!Stop(deadline)) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_isStopping = true;
            m_reads->StopLocked();
            if (m_activeCancel) {
                m_activeCancel->store(true, std::memory_order_release);
            }
            for (auto& job : m_pendingJobs) job.isCancelled->store(true);
            m_pendingJobs.clear();
        }
        m_workReady.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }
}

SurfaceAdmissionStatus SurfaceDeterminationService::Start(VtkImageGridSnapshot source,
                                                          SurfaceDeterminationStartParams params,
                                                          const WorkLimit maxWorkingBytes,
                                                          const std::uint64_t requestId,
                                                          SurfaceAlgorithmInputs inputs)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isStopping) return SurfaceAdmissionStatus::Stopping;
    if (!source || !source->image || requestId == 0) {
        return SurfaceAdmissionStatus::InvalidRequest;
    }
    const auto maxSlots = m_complete.max_size();
    auto outstandingCount = m_complete.size();
    if (m_activeRequestId != 0) {
        if (outstandingCount == maxSlots) return SurfaceAdmissionStatus::Unavailable;
        ++outstandingCount;
    }
    if (m_pendingJobs.size() >= maxSlots - outstandingCount)
        return SurfaceAdmissionStatus::Unavailable;
    const auto requiredSlots = outstandingCount + m_pendingJobs.size() + 1;
    if (m_complete.capacity() < requiredSlots) {
        // 几何增长避免逐请求扩容；后台 noexcept 完成发布始终无需重新分配。
        const auto grownSlots = m_complete.capacity() <= maxSlots / 2
            ? m_complete.capacity() * 2 : maxSlots;
        try { m_complete.reserve(std::max(requiredSlots, grownSlots)); }
        catch (const std::bad_alloc&) { return SurfaceAdmissionStatus::Unavailable; }
        catch (const std::length_error&) { return SurfaceAdmissionStatus::Unavailable; }
    }

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    Job nextJob{std::move(inputs), std::move(source), std::move(params),
                maxWorkingBytes,   requestId,         std::move(cancel)};

    // 同通道替代，其余工件/用途保持 FIFO；分配成功后才取消旧任务。
    const auto scope = nextJob.params.resultScope;
    const auto purpose = SurfaceContract::GetPurpose(nextJob.params);
    m_pendingJobs.push_back(std::move(nextJob));
    if (m_activeCancel && m_activeScope == scope && m_activePurpose == purpose)
        m_activeCancel->store(true, std::memory_order_release);
    for (auto item = m_pendingJobs.begin(); item != m_pendingJobs.end();) {
        if (item->requestId != requestId && item->params.resultScope == scope
            && SurfaceContract::GetPurpose(item->params) == purpose) {
            m_complete.push_back(BuildCancelled(*item));
            item = m_pendingJobs.erase(item);
        }
        else ++item;
    }
    for (auto& complete : m_complete) {
        if (complete.result.status == SurfaceResultStatus::Succeeded
            && complete.result.resolvedParams.resultScope == scope
            && SurfaceContract::GetPurpose(complete.result.resolvedParams) == purpose) {
            complete.result.status = SurfaceResultStatus::Cancelled;
            complete.result.failureReason = SurfaceFailureReason::Cancelled;
            complete.result.points = {};
            PagedArray<SurfacePointRecord>().swap(complete.result.points);
            PagedArray<std::uint32_t>().swap(complete.result.triangleIndices);
            PagedArray<std::uint8_t>().swap(complete.result.triangleValidity);
            PagedArray<SurfaceObjectRecord>().swap(complete.result.objects);
            PagedArray<SurfaceSelectionPiece>().swap(complete.result.selection);
            complete.meshes={};complete.result.prepared.reset();
            complete.result.requiredBytes = 0;
        }
    }
    m_latestRequestId = requestId;
    m_progressPermille.store(0, std::memory_order_relaxed);
    m_progressStage.store(
        static_cast<std::uint8_t>(SurfaceDeterminationStage::Preparing),
        std::memory_order_relaxed);
    m_progressRequestId.store(requestId, std::memory_order_release);
    m_workReady.notify_one();
    ++m_executionRevision;
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
    for (auto& job : m_pendingJobs) if (job.requestId == requestId) {
        job.isCancelled->store(true, std::memory_order_release);
        didStop = true;
    }
    // 计算已完成但 owner 尚未消费时，仍允许取消尚未发布的候选。
    for (auto& complete : m_complete) if (complete.requestId == requestId) {
        complete.result.status = SurfaceResultStatus::Cancelled;
        complete.result.failureReason = SurfaceFailureReason::Cancelled;
        PagedArray<SurfacePointRecord>().swap(complete.result.points);
        PagedArray<std::uint32_t>().swap(complete.result.triangleIndices);
        PagedArray<std::uint8_t>().swap(complete.result.triangleValidity);
        PagedArray<SurfaceObjectRecord>().swap(complete.result.objects);
        PagedArray<SurfaceSelectionPiece>().swap(complete.result.selection);
        complete.meshes={};complete.result.prepared.reset();
            complete.result.requiredBytes = 0;
        didStop = true;
    }
    if (didStop) ++m_executionRevision;
    return didStop;
}

FeatureOperationState SurfaceDeterminationService::GetExecutionState(const std::uint64_t requestId) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    FeatureOperationState state;
    state.operation.requestId = requestId;
    state.stateRevision = m_executionRevision;
    if (m_progressRequestId.load() == requestId)
        state.progress = static_cast<double>(m_progressPermille.load()) / 1000.0;
    const auto complete = std::find_if(m_complete.begin(), m_complete.end(),
        [requestId](const auto& value) { return value.requestId == requestId; });
    if (complete != m_complete.end()) {
        state.status = complete->result.status == SurfaceResultStatus::Succeeded ? FeatureRunStatus::Ready
            : complete->result.status == SurfaceResultStatus::Cancelled ? FeatureRunStatus::Cancelled
            : FeatureRunStatus::Failed;
    }
    else if (m_activeRequestId == requestId) {
        state.status = m_activeCancel && m_activeCancel->load()
            ? FeatureRunStatus::Stopping : FeatureRunStatus::Running;
    }
    else {
        for (const auto& job : m_pendingJobs) if (job.requestId == requestId)
            state.status = job.isCancelled->load() ? FeatureRunStatus::Stopping : FeatureRunStatus::Preparing;
    }
    return state;
}

std::optional<SurfaceJobComplete> SurfaceDeterminationService::GetComplete()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_complete.empty()) return std::nullopt;
    SurfaceJobComplete complete = std::move(m_complete.front());
    m_complete.erase(m_complete.begin());
    ++m_executionRevision;
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
    return m_activeRequestId != 0 || !m_pendingJobs.empty();
}

bool SurfaceDeterminationService::Stop(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_isStopping = true;
        m_reads->StopLocked();
        ++m_executionRevision;
        if (m_activeCancel) {
            m_activeCancel->store(true, std::memory_order_release);
        }
        for (auto& job : m_pendingJobs) job.isCancelled->store(true);
        m_pendingJobs.clear();
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
    complete.result.sourceRevision = job.source && job.source->data ? job.source->data->self : DataRevisionRef{};
    complete.result.method = job.params.method;
    return complete;
}

void SurfaceDeterminationService::SetProgress(
    const std::uint64_t requestId,
    const SurfaceDeterminationStage stage,
    const double progress) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_progressRequestId.load(std::memory_order_acquire)
        != requestId) {
        return;
    }
    const auto previousStage = m_progressStage.load();
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
    if (current < target || previousStage != static_cast<std::uint8_t>(stage)) ++m_executionRevision;
}

void SurfaceDeterminationService::WorkerLoop() noexcept
{
    for (;;) {
        Job job;
        std::optional<FeatureInternal::ReadTaskQueue::Job> read;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workReady.wait(lock, [this] {
                return m_isStopping || !m_pendingJobs.empty() || m_reads->GetHasPendingLocked();
            });
            if (m_isStopping && m_pendingJobs.empty()) break;
            if(m_pendingJobs.empty())read=m_reads->TakeLocked();
            else {
                job = std::move(m_pendingJobs.front());
                m_pendingJobs.pop_front();
                m_activeScope = job.params.resultScope;
                m_activePurpose = SurfaceContract::GetPurpose(job.params);
                m_activeRequestId = job.requestId;
                m_activeCancel = job.isCancelled;
                ++m_executionRevision;
            }
        }
        if(read) {
            try {read->task([token=read->cancelled]{return token->load(std::memory_order_acquire);});}catch(...){}
            {const std::lock_guard<std::mutex> lock(m_mutex);m_reads->FinishLocked();}
            try {if(m_onWorkAvailable)m_onWorkAvailable();}catch(...){}
            continue;
        }

        // 已冻结结果由各自后备存储保活；其逻辑长度不能扣减本次活动工作区。
        const WorkLimit available = job.maxWorkingBytes;
        SurfaceJobComplete complete;
        complete.requestId = job.requestId;
        if (job.isCancelled->load(std::memory_order_acquire)) {
            complete = BuildCancelled(job);
        }
        else {
            complete.result = SurfaceDeterminationAlgorithm::BuildSurface(
                job.source, job.params, available,
                [cancel = job.isCancelled] { return cancel->load(std::memory_order_acquire); },
                [this, requestId = job.requestId](const SurfaceDeterminationStage stage,
                                                  const double progress) {
                    SetProgress(requestId, stage, progress);
                },
                job.inputs);
            if(complete.result.status==SurfaceResultStatus::Succeeded){
                try {
                    const auto* image=dynamic_cast<const ImageGrid3DPayload*>(job.source->data->payload.get());
                    complete.result.prepared=image?PrepareSurfaceGeometry(complete.result,image->GetGeometry().coordinateFrame,
                        job.params.componentSelection==SurfaceComponentSelection::All && !job.params.analysisRoi && job.params.minimumObjectVoxels<=1,
                        [cancel=job.isCancelled]{return cancel->load();},job.inputs.parentStorage):nullptr;
                    if(!complete.result.prepared){
                        if(job.isCancelled->load())complete=BuildCancelled(job);
                        else {complete.result.status=SurfaceResultStatus::Failed;complete.result.failureReason=SurfaceFailureReason::InvalidGeometry;complete.result.message="Surface storage preparation failed.";}
                    }
                } catch(const std::exception& error){complete.result.status=SurfaceResultStatus::Failed;complete.result.failureReason=SurfaceFailureReason::InternalError;complete.result.message=error.what();}
            }
            if(complete.result.status==SurfaceResultStatus::Succeeded) {
                complete.meshes.measurement=complete.result.prepared->mesh;
                complete.meshes.selection=complete.result.prepared->selectionMesh;
                try {
                    const auto* image=dynamic_cast<const ImageGrid3DPayload*>(job.source->data->payload.get());
                    complete.meshes.display=PrepareSurfaceDisplay(complete.result,complete.meshes,job.inputs,
                        image->GetGeometry().coordinateFrame,job.maxWorkingBytes,[cancel=job.isCancelled]{return cancel->load();});
                }catch(const std::exception& error){complete.meshes.displayError=error.what();}
                catch(...){complete.meshes.displayError="Surface spatial display preparation failed.";}
            }
        }

        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (job.isCancelled->load(std::memory_order_acquire)) {
                complete = BuildCancelled(job);
            }
            // Start 为每个已接纳请求预留一个 completion 槽位，不能在此
            // 静默丢弃，否则 owner-thread callback 将永远无法完成。
            m_complete.push_back(std::move(complete));
            m_activeRequestId = 0;
            m_activeCancel.reset();
            ++m_executionRevision;
        }
        // completion 已发布；通知只排队，业务仍由 owner tick 消费。
        try { if (m_onWorkAvailable) m_onWorkAvailable(); }
        catch (...) {}
    }

    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_hasExited = true;
    }
    m_workerExited.notify_all();
}
