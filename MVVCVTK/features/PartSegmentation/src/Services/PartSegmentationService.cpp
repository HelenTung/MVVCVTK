#include "FeatureSupport/WorkLimit.h"
#include "Services/PartSegmentationService.h"
#include "Model/LabelMapBuilder.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkType.h>
#include <vtkUnsignedIntArray.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace {

bool GetProduct(
    const std::size_t left,
    const std::size_t right,
    std::size_t& product)
{
    if (left != 0
        && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    product = left * right;
    return true;
}

bool GetImageGeometry(
    vtkImageData& image,
    PartVolumeView& volume)
{
    int dimensions[3]{};
    int extent[6]{};
    double spacing[3]{};
    double origin[3]{};
    image.GetDimensions(dimensions);
    image.GetExtent(extent);
    image.GetSpacing(spacing);
    image.GetOrigin(origin);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (dimensions[axis] <= 0
            || !std::isfinite(spacing[axis])
            || spacing[axis] <= 0.0
            || !std::isfinite(origin[axis])) {
            return false;
        }
        const auto extentSize = static_cast<std::int64_t>(
            extent[axis * 2 + 1])
            - static_cast<std::int64_t>(extent[axis * 2]) + 1;
        if (extentSize != dimensions[axis]) return false;
        volume.dimensions[axis] = dimensions[axis];
        volume.spacing[axis] = spacing[axis];
        volume.origin[axis] = origin[axis];
        volume.extent[axis * 2] = extent[axis * 2];
        volume.extent[axis * 2 + 1] = extent[axis * 2 + 1];
    }

    auto* direction = image.GetDirectionMatrix();
    if (!direction) return false;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const double value = direction->GetElement(
                static_cast<int>(row), static_cast<int>(column));
            if (!std::isfinite(value)) return false;
            volume.direction[row * 3 + column] = value;
        }
    }
    return true;
}

bool GetSameGeometry(
    const PartVolumeView& left,
    const PartVolumeView& right)
{
    return left.extent == right.extent
        && left.dimensions == right.dimensions
        && left.spacing == right.spacing
        && left.origin == right.origin
        && left.direction == right.direction;
}

std::optional<PartScalarType> GetScalarType(const int vtkType)
{
    switch (vtkType) {
    case VTK_CHAR:
        return std::is_signed_v<char>
            ? PartScalarType::Int8 : PartScalarType::UInt8;
    case VTK_SIGNED_CHAR:
        return PartScalarType::Int8;
    case VTK_UNSIGNED_CHAR:
        return PartScalarType::UInt8;
    case VTK_SHORT:
        return PartScalarType::Int16;
    case VTK_UNSIGNED_SHORT:
        return PartScalarType::UInt16;
    case VTK_INT:
        return PartScalarType::Int32;
    case VTK_UNSIGNED_INT:
        return PartScalarType::UInt32;
    case VTK_LONG:
        if constexpr (sizeof(long) == sizeof(std::int32_t)) {
            return PartScalarType::Int32;
        }
        else {
            return PartScalarType::Int64;
        }
    case VTK_UNSIGNED_LONG:
        if constexpr (sizeof(unsigned long) == sizeof(std::uint32_t)) {
            return PartScalarType::UInt32;
        }
        else {
            return PartScalarType::UInt64;
        }
    case VTK_LONG_LONG:
        return PartScalarType::Int64;
    case VTK_UNSIGNED_LONG_LONG:
        return PartScalarType::UInt64;
    case VTK_FLOAT:
        return PartScalarType::Float32;
    case VTK_DOUBLE:
        return PartScalarType::Float64;
    default:
        return std::nullopt;
    }
}

enum class ScalarViewStatus : std::uint8_t {
    Succeeded,
    InvalidCount,
    Unsupported
};

ScalarViewStatus BuildScalarView(
    vtkDataArray* scalars,
    const std::size_t expectedCount,
    PartScalarView& view)
{
    if (!scalars
        || scalars->GetNumberOfComponents() != 1
        || !scalars->HasStandardMemoryLayout()) {
        return ScalarViewStatus::Unsupported;
    }
    const auto scalarType = GetScalarType(scalars->GetDataType());
    if (!scalarType) return ScalarViewStatus::Unsupported;

    const vtkIdType tupleCount = scalars->GetNumberOfTuples();
    if (tupleCount <= 0
        || static_cast<unsigned long long>(tupleCount)
            > std::numeric_limits<std::size_t>::max()
        || static_cast<std::size_t>(tupleCount) != expectedCount) {
        return ScalarViewStatus::InvalidCount;
    }
    const void* data = scalars->GetVoidPointer(0);
    if (!data) return ScalarViewStatus::Unsupported;
    view = { data, expectedCount, *scalarType };
    return ScalarViewStatus::Succeeded;
}

std::string BuildBudgetMessage(
    const std::size_t requiredBytes,
    const WorkLimit maxWorkingBytes)
{
    return "Part working-set budget is exceeded: requiredBytes="
        + std::to_string(requiredBytes)
        + ", maxWorkingBytes=" + maxWorkingBytes.GetText() + ".";
}

std::string BuildSuccessMessage(const PartAlgorithmMetrics& metrics)
{
    return "Part segmentation succeeded: peakWorkingBytes="
        + std::to_string(metrics.peakWorkingBytes)
        + ", labelBytes=" + std::to_string(metrics.labelBytes)
        + ", frontierPeakBytes="
        + std::to_string(metrics.frontierPeakBytes)
        + ", filteredPeakBytes="
        + std::to_string(metrics.filteredPeakBytes)
        + ", catalogBytes=" + std::to_string(metrics.catalogBytes) + ".";
}

bool GetHistoryBytes(
    const PartHistorySnapshot& previous,
    std::size_t& historyBytes)
{
    historyBytes = 0;
    if (!previous.labels && !previous.catalog) return true;
    if (!previous.labels || !previous.catalog) return false;

    std::size_t labelBytes = 0;
    std::size_t catalogBytes = 0;
    if (!GetProduct(
            previous.labels->capacity(), sizeof(PartLabelId), labelBytes)
        || !GetPartCatalogStorageBytes(
            *previous.catalog, catalogBytes)
        || catalogBytes > std::numeric_limits<std::size_t>::max()
            - labelBytes) {
        historyBytes = std::numeric_limits<std::size_t>::max();
        return false;
    }
    historyBytes = labelBytes + catalogBytes;
    return true;
}

} // namespace

PartSegmentationService::PartSegmentationService(std::function<void()> onWorkAvailable)
    : m_onWorkAvailable(std::move(onWorkAvailable))
    , m_worker([this] { WorkerLoop(); })
{
}

PartSegmentationService::~PartSegmentationService() noexcept
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (!Stop(deadline)) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_isStopping = true;
            m_cancelRequested.store(true, std::memory_order_release);
    if (m_surfaceCancel) m_surfaceCancel->store(true);
    m_surfaceJob.reset();
        }
        m_workReady.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }
}

PartAdmissionStatus PartSegmentationService::Start(
    VtkImageGridSnapshot source,
    PartSegmentationStartParams params,
    const WorkLimit maxWorkingBytes,
    const std::uint64_t requestId,
    PartHistorySnapshot previous,
    const std::uint64_t expectedResultRevision,
    const std::uint64_t expectedCatalogRevision,
    const std::size_t retainedSurfaceBytes)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isStopping) return PartAdmissionStatus::Stopping;
    if ((m_isBusy && !m_isSurfaceBusy) || m_job || m_complete) return PartAdmissionStatus::Busy;
    if (!source || !source->image || requestId == 0
        || !std::isfinite(params.threshold)
        || params.minPartVoxels == 0
        || maxWorkingBytes == 0
        || (static_cast<bool>(previous.labels)
            != static_cast<bool>(previous.catalog))
        || (previous.catalog
            && (previous.catalog->resultRevision
                    != expectedResultRevision
                || previous.catalog->catalogRevision
                    != expectedCatalogRevision))
        || (!previous.catalog
            && (expectedResultRevision != 0
                || expectedCatalogRevision != 0))) {
        return PartAdmissionStatus::InvalidRequest;
    }
    if (m_surfaceCancel) m_surfaceCancel->store(true);
    m_surfaceJob.reset();
    m_cancelRequested.store(false, std::memory_order_release);
    m_progressPermille.store(0, std::memory_order_relaxed);
    m_progressRequestId.store(requestId, std::memory_order_release);
    m_job = Job{
        std::move(source),
        std::move(params),
        maxWorkingBytes,
        requestId,
        std::move(previous),
        expectedResultRevision,
        expectedCatalogRevision,
        retainedSurfaceBytes
    };
    m_workReady.notify_one();
    ++m_executionRevision;
    return PartAdmissionStatus::Accepted;
}

PartAdmissionStatus PartSegmentationService::StartEdit(PartEditJob edit)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isStopping) return PartAdmissionStatus::Stopping;
    if ((m_isBusy && !m_isSurfaceBusy) || m_job || m_complete) return PartAdmissionStatus::Busy;
    if (!GetPartEditBytes(edit.request)) return PartAdmissionStatus::BudgetExceeded;
    if (!edit.source || !edit.source->image || !edit.previous.labels
        || !edit.previous.catalog || edit.requestId == 0 || edit.maxWorkingBytes == 0
        || edit.timeoutMs == 0 || (edit.timeoutMs.GetValue() && edit.timeoutMs > 86400000)
        || edit.previous.catalog->catalogRevision != edit.request.expectedCatalogRevision) {
        return PartAdmissionStatus::InvalidRequest;
    }
    Job job;
    job.source = std::move(edit.source);
    job.previous = std::move(edit.previous);
    job.requestId = edit.requestId;
    job.maxWorkingBytes = edit.maxWorkingBytes;
    job.expectedResultRevision = job.previous.catalog->resultRevision;
    job.expectedCatalogRevision = job.previous.catalog->catalogRevision;
    job.retainedSurfaceBytes = edit.retainedBytes;
    job.edit = std::move(edit);
    if (m_surfaceCancel) m_surfaceCancel->store(true);
    m_surfaceJob.reset();
    m_cancelRequested.store(false, std::memory_order_release);
    m_progressPermille.store(0, std::memory_order_relaxed);
    m_progressRequestId.store(job.requestId, std::memory_order_release);
    m_job = std::move(job);
    m_workReady.notify_one();
    return PartAdmissionStatus::Accepted;
}

bool PartSegmentationService::StartSurface(PartSurfaceBuildRequest request,
    DataRevisionRef labels, std::uint64_t requestId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isStopping || m_job || m_complete || (m_isBusy && !m_isSurfaceBusy)
        || !GetDataRevisionRefValid(labels) || requestId == 0 || !request.labels) return false;
    if (m_surfaceCancel) m_surfaceCancel->store(true);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_surfaceCancel = cancelled;
    m_surfaceComplete.reset();
    m_surfaceJob = SurfaceJob{std::move(request), labels, requestId, std::move(cancelled)};
    m_workReady.notify_one();
    return true;
}

std::optional<PartSurfaceCompletion> PartSegmentationService::RemoveSurfaceComplete()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto result = std::move(m_surfaceComplete);
    m_surfaceComplete.reset();
    return result;
}

void PartSegmentationService::StopSurface() noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_surfaceCancel) m_surfaceCancel->store(true);
    m_surfaceJob.reset();
    m_surfaceComplete.reset();
}

void PartSegmentationService::StopRequest() noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_cancelRequested.store(true, std::memory_order_release);
    if (m_surfaceCancel) m_surfaceCancel->store(true);
    m_surfaceJob.reset();
    ++m_executionRevision;
}

std::optional<FeatureOperationState> PartSegmentationService::GetExecutionState(
    const std::uint64_t requestId) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (requestId == 0 || m_progressRequestId.load() != requestId) return std::nullopt;
    FeatureOperationState state;
    state.operation.requestId = requestId;
    state.stateRevision = m_executionRevision;
    state.progress = static_cast<double>(m_progressPermille.load()) / 1000.0;
    if (m_complete) {
        state.status = m_complete->status == PartResultStatus::Succeeded ? FeatureRunStatus::Ready
            : m_complete->status == PartResultStatus::Cancelled ? FeatureRunStatus::Cancelled
            : FeatureRunStatus::Failed;
    }
    else if (m_cancelRequested.load() && (m_job || m_isBusy)) state.status = FeatureRunStatus::Stopping;
    else if (m_isBusy) state.status = FeatureRunStatus::Running;
    else if (m_job) state.status = FeatureRunStatus::Preparing;
    return state;
}

std::optional<PartLabelCandidate>
PartSegmentationService::GetComplete()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto complete = std::move(m_complete);
    m_complete.reset();
    if (complete) ++m_executionRevision;
    return complete;
}

std::optional<double> PartSegmentationService::GetProgress(
    const std::uint64_t requestId) const noexcept
{
    if (requestId == 0
        || m_progressRequestId.load(std::memory_order_acquire)
            != requestId) {
        return std::nullopt;
    }
    const std::uint32_t permille =
        m_progressPermille.load(std::memory_order_acquire);
    if (m_progressRequestId.load(std::memory_order_acquire)
        != requestId) {
        return std::nullopt;
    }
    return static_cast<double>(permille) / 1000.0;
}

bool PartSegmentationService::GetIsSurfaceBusy() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_isSurfaceBusy || m_surfaceJob.has_value() || m_surfaceComplete.has_value();
}

bool PartSegmentationService::GetIsBusy() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return (m_isBusy && !m_isSurfaceBusy) || m_job.has_value() || m_complete.has_value();
}

bool PartSegmentationService::Stop(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_isStopping = true;
        m_cancelRequested.store(true, std::memory_order_release);
    if (m_surfaceCancel) m_surfaceCancel->store(true);
    m_surfaceJob.reset();
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

void PartSegmentationService::SetProgress(
    const std::uint64_t requestId,
    const double progress) noexcept
{
    if (!std::isfinite(progress)
        || m_progressRequestId.load(std::memory_order_acquire) != requestId) return;
    const auto target = static_cast<std::uint32_t>(
        std::lround(std::clamp(progress, 0.0, 1.0) * 1000.0));
    if (m_progressPermille.load(std::memory_order_relaxed) >= target) return;
    const std::lock_guard<std::mutex> lock(m_mutex);
    // 原子读只作无变化过滤；请求替换与正式发布仍在同一把锁下复核。
    if (m_progressRequestId.load(std::memory_order_acquire) != requestId) return;
    if (m_progressPermille.load(std::memory_order_relaxed) < target) {
        m_progressPermille.store(target, std::memory_order_release);
        ++m_executionRevision;
    }
}

void PartSegmentationService::WorkerLoop() noexcept
{
    for (;;) {
        Job job;
        std::optional<SurfaceJob> surface;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workReady.wait(lock, [this] {
                return m_isStopping || m_job.has_value() || m_surfaceJob.has_value();
            });
            if (m_isStopping && !m_job && !m_surfaceJob) break;
            if (!m_job) {
                surface = std::move(m_surfaceJob);
                m_surfaceJob.reset();
                m_isSurfaceBusy = m_isBusy = true;
            } else {
            job = std::move(*m_job);
            m_job.reset();
            m_isBusy = true;
            ++m_executionRevision;
            }
        }

        if (surface) {
            auto result = PartSurfaceProductBuilder::BuildProduct(surface->request,
                [cancel = surface->cancelled] { return cancel->load(); }, {});
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_isSurfaceBusy = m_isBusy = false;
                if (!surface->cancelled->load() && !m_isStopping)
                    m_surfaceComplete = PartSurfaceCompletion{surface->labels, surface->requestId, std::move(result)};
            }
            try { if (m_onWorkAvailable) m_onWorkAvailable(); } catch (...) {}
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        PartLabelCandidate candidate = BuildCandidate(job);
        if (job.edit && candidate.failureReason == PartFailureReason::Cancelled
            && !m_cancelRequested.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() >= job.edit->timeoutMs.GetDeadline(started)) {
            candidate.status = PartResultStatus::Failed;
            candidate.failureReason = PartFailureReason::TimedOut;
            candidate.message = "Part edit deadline was exceeded.";
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_isBusy = false;
            m_complete = std::move(candidate);
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

PartLabelCandidate PartSegmentationService::BuildCandidate(
    const Job& job) noexcept
{
    PartLabelCandidate candidate;
    candidate.requestId = job.requestId;
    candidate.expectedResultRevision = job.expectedResultRevision;
    candidate.expectedCatalogRevision = job.expectedCatalogRevision;
    candidate.sourceRevision = job.source && job.source->data
        ? job.source->data->self : DataRevisionRef{};
    candidate.sourceBindingRevision = job.source && job.source->binding
        ? job.source->binding->revision : 0;
    candidate.failureReason = PartFailureReason::InvalidSource;
    candidate.message = "Part source is unavailable.";
    if (!job.source || !job.source->image) return candidate;

    const auto deadline = job.edit
        ? job.edit->timeoutMs.GetDeadline()
        : std::chrono::steady_clock::time_point::max();
    const auto getStopped = [this, deadline] {
        return m_cancelRequested.load(std::memory_order_acquire)
            || std::chrono::steady_clock::now() >= deadline;
    };

    try {
        // snapshot 是 worker 读取期间唯一的 source owner。这里只建立只读
        // typed view，不复制整卷 scalar，也不在 worker 修改 VTK 对象。
        PartVolumeView volume;
        auto* image = job.source->image.GetPointer();
        if (!image || !GetImageGeometry(*image, volume)) {
            candidate.failureReason = PartFailureReason::InvalidGeometry;
            candidate.message = "Part source geometry is invalid.";
            return candidate;
        }

        std::size_t voxelCount = 1;
        for (const int dimension : volume.dimensions) {
            if (!GetProduct(
                    voxelCount,
                    static_cast<std::size_t>(dimension),
                    voxelCount)) {
                candidate.failureReason = PartFailureReason::InvalidGeometry;
                candidate.message = "Part source voxel count overflows.";
                return candidate;
            }
        }
        auto* scalars = image->GetPointData()
            ? image->GetPointData()->GetScalars() : nullptr;
        const ScalarViewStatus sourceStatus =
            BuildScalarView(scalars, voxelCount, volume.values);
        if (sourceStatus != ScalarViewStatus::Succeeded) {
            candidate.failureReason =
                sourceStatus == ScalarViewStatus::InvalidCount
                ? PartFailureReason::InvalidGeometry
                : PartFailureReason::UnsupportedScalar;
            candidate.message =
                sourceStatus == ScalarViewStatus::InvalidCount
                ? "Part source tuple count is inconsistent."
                : "Part source requires one supported contiguous scalar.";
            return candidate;
        }

        if (job.source->validityMask) {
            auto* mask = job.source->validityMask.GetPointer();
            PartVolumeView maskVolume;
            if (!mask
                || !GetImageGeometry(*mask, maskVolume)
                || !GetSameGeometry(volume, maskVolume)) {
                candidate.failureReason = PartFailureReason::InvalidGeometry;
                candidate.message = "Part validity geometry is inconsistent.";
                return candidate;
            }
            auto* maskScalars = mask->GetPointData()
                ? mask->GetPointData()->GetScalars() : nullptr;
            PartScalarView maskView;
            const ScalarViewStatus maskStatus =
                BuildScalarView(maskScalars, voxelCount, maskView);
            if (maskStatus != ScalarViewStatus::Succeeded) {
                candidate.failureReason =
                    maskStatus == ScalarViewStatus::InvalidCount
                    ? PartFailureReason::InvalidGeometry
                    : PartFailureReason::UnsupportedScalar;
                candidate.message =
                    maskStatus == ScalarViewStatus::InvalidCount
                    ? "Part validity tuple count is inconsistent."
                    : "Part validity requires one supported contiguous scalar.";
                return candidate;
            }
            volume.validity = maskView;
        }

        std::size_t historyBytes = 0;
        // 编辑的 retainedBytes 已包含 Host 统计的全部历史（含 previous）；
        // 普通分割只传旧 surface，因此仅普通分割需要在此追加 previous。
        if (!GetHistoryBytes(job.previous, historyBytes)
            || (!job.edit && job.retainedSurfaceBytes
                > std::numeric_limits<std::size_t>::max() - historyBytes)) {
            candidate.failureReason = PartFailureReason::BudgetExceeded;
            candidate.requiredBytes = std::numeric_limits<std::size_t>::max();
            candidate.message = BuildBudgetMessage(
                candidate.requiredBytes, job.maxWorkingBytes);
            return candidate;
        }
        historyBytes = job.edit ? std::max(historyBytes, job.retainedSurfaceBytes)
            : historyBytes + job.retainedSurfaceBytes;
        if (historyBytes >= job.maxWorkingBytes) {
            candidate.failureReason = PartFailureReason::BudgetExceeded;
            candidate.requiredBytes = historyBytes;
            candidate.message = BuildBudgetMessage(
                historyBytes, job.maxWorkingBytes);
            return candidate;
        }

        PartAlgorithmParams params;
        params.threshold = job.params.threshold;
        params.minPartVoxels = job.params.minPartVoxels;
        params.maxWorkingBytes = job.maxWorkingBytes - historyBytes;
        const auto started = std::chrono::steady_clock::now();
        const auto elapsedMs = [](const auto& since) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - since).count();
        };
        std::int64_t labelMs = 0;
        std::int64_t lineageMs = 0;
        std::int64_t freezeMs = 0;
        GridGeometry3D geometry{ volume.extent, volume.dimensions, volume.spacing,
            volume.origin, volume.direction, "RAS" };
        const auto sourcePayload = job.source->data
            ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(job.source->data->payload) : nullptr;
        if (sourcePayload) geometry.coordinateFrame = sourcePayload->GetGeometry().coordinateFrame;
        if (job.edit) {
            PartEditBuildResult edited;
            if (std::holds_alternative<PartHistoryEdit>(job.edit->request.operation)) {
                edited = PartLabelEditor::BuildRestore(job.previous, job.edit->restored,
                    job.maxWorkingBytes - historyBytes, getStopped);
            }
            else {
                PartEditInput editInput;
                editInput.volume = volume;
                editInput.coordinateFrame = geometry.coordinateFrame;
                editInput.sourceRevision = job.source->data->self;
                editInput.previous = job.previous;
                editInput.request = job.edit->request;
                editInput.editRoi = job.edit->editRoi;
                editInput.protectionRoi = job.edit->protectionRoi;
                editInput.maxWorkingBytes = job.maxWorkingBytes - historyBytes;
                edited = PartLabelEditor::BuildLabels(editInput, m_identities, getStopped);
            }
            candidate.requiredBytes = edited.requiredBytes > std::numeric_limits<std::size_t>::max() - historyBytes
                ? std::numeric_limits<std::size_t>::max() : edited.requiredBytes + historyBytes;
            if (edited.failureReason != PartFailureReason::None || !edited.labels || !edited.catalog) {
                candidate.failureReason = edited.failureReason;
                candidate.status = edited.failureReason == PartFailureReason::Cancelled
                    ? PartResultStatus::Cancelled : PartResultStatus::Failed;
                candidate.message = edited.message;
                return candidate;
            }
            candidate.labelPayload = std::move(edited.labelPayload);
            candidate.labels = std::move(edited.labels);
            candidate.catalog = std::move(edited.catalog);
            labelMs = elapsedMs(started);
            SetProgress(job.requestId, 0.9);
        }
        else {
        auto result = ClassicalPartSegmenter::BuildLabels(
            volume,
            params,
            getStopped,
            [this, requestId = job.requestId](const double progress) {
                SetProgress(requestId, progress * 0.9);
            });
        labelMs = elapsedMs(started);
        candidate.requiredBytes = result.requiredBytes
            > std::numeric_limits<std::size_t>::max() - historyBytes
            ? std::numeric_limits<std::size_t>::max()
            : result.requiredBytes + historyBytes;
        candidate.metrics = result.metrics;
        if (result.error != PartAlgorithmError::None) {
            if (result.error == PartAlgorithmError::Cancelled) {
                candidate.status = PartResultStatus::Cancelled;
                candidate.failureReason = PartFailureReason::Cancelled;
                candidate.message = "Part request was cancelled.";
            }
            else if (result.error == PartAlgorithmError::BudgetExceeded) {
                candidate.failureReason = PartFailureReason::BudgetExceeded;
                candidate.message = BuildBudgetMessage(
                    candidate.requiredBytes, job.maxWorkingBytes);
            }
            else if (result.error == PartAlgorithmError::LabelOverflow) {
                candidate.failureReason = PartFailureReason::BudgetExceeded;
                candidate.message =
                    "Part count exceeds the label representation.";
            }
            else if (result.error == PartAlgorithmError::InvalidInput) {
                candidate.failureReason = PartFailureReason::InvalidGeometry;
                candidate.message = "Part algorithm input is invalid.";
            }
            else {
                candidate.failureReason = PartFailureReason::InternalError;
                candidate.message = "Part segmentation failed.";
            }
            return candidate;
        }

        const auto freezeStarted = std::chrono::steady_clock::now();
        candidate.labelPayload = LabelMapBuilder::Build(geometry,
            std::make_unique<std::vector<PartLabelId>>(std::move(result.labels)), getStopped);
        freezeMs = elapsedMs(freezeStarted);
        if (!candidate.labelPayload) {
            candidate.status = getStopped() ? PartResultStatus::Cancelled : PartResultStatus::Failed;
            candidate.failureReason = candidate.status == PartResultStatus::Cancelled
                ? PartFailureReason::Cancelled : PartFailureReason::InvalidGeometry;
            candidate.message = "Part label freezing did not complete.";
            return candidate;
        }
        candidate.labels = candidate.labelPayload->GetLabels();
        PartLineageRequest lineageRequest;
        lineageRequest.previous = job.previous;
        lineageRequest.currentLabels = candidate.labels;
        lineageRequest.currentMetricsByLabel =
            std::move(result.metricsByLabel);
        lineageRequest.nextResultRevision = job.expectedResultRevision == 0
            ? 1 : job.expectedResultRevision + 1U;
        lineageRequest.maxWorkingBytes =
            job.maxWorkingBytes - job.retainedSurfaceBytes;
        const auto lineageStarted = std::chrono::steady_clock::now();
        auto lineage = PartLineageMatcher::BuildCatalog(
            std::move(lineageRequest),
            m_identities,
            getStopped);
        lineageMs = elapsedMs(lineageStarted);
        const auto lineageRequiredBytes = lineage.requiredBytes
            > std::numeric_limits<std::size_t>::max() - job.retainedSurfaceBytes
            ? std::numeric_limits<std::size_t>::max()
            : lineage.requiredBytes + job.retainedSurfaceBytes;
        candidate.requiredBytes = std::max(
            candidate.requiredBytes, lineageRequiredBytes);
        if (!lineage.catalog) {
            candidate.failureReason = lineage.failureReason;
            candidate.status = lineage.failureReason
                    == PartFailureReason::Cancelled
                ? PartResultStatus::Cancelled : PartResultStatus::Failed;
            candidate.message = lineage.failureReason
                    == PartFailureReason::BudgetExceeded
                ? BuildBudgetMessage(
                    lineageRequiredBytes, job.maxWorkingBytes)
                : lineage.failureReason == PartFailureReason::Cancelled
                    ? "Part request was cancelled."
                    : "Part catalog construction failed.";
            candidate.labels.reset();
            return candidate;
        }
        candidate.catalog = std::move(lineage.catalog);
        }
        if (!candidate.labelPayload) {
            const auto freezeStarted = std::chrono::steady_clock::now();
            if (job.edit && job.edit->restoredPayload
                && candidate.labels == job.edit->restoredPayload->GetLabels()) {
                candidate.labelPayload = job.edit->restoredPayload;
            }
            else {
                std::size_t catalogBytes = 0, copyBytes = 0;
                if (!GetPartCatalogStorageBytes(*candidate.catalog, catalogBytes)
                    || catalogBytes > std::numeric_limits<std::size_t>::max() - historyBytes
                    || !GetProduct(candidate.labels->size(), 2U * sizeof(PartLabelId), copyBytes)
                    || copyBytes > std::numeric_limits<std::size_t>::max() - historyBytes - catalogBytes) {
                    candidate.requiredBytes = std::numeric_limits<std::size_t>::max();
                }
                else candidate.requiredBytes = std::max(candidate.requiredBytes, historyBytes + catalogBytes + copyBytes);
                if (candidate.requiredBytes > job.maxWorkingBytes) {
                    candidate.failureReason = PartFailureReason::BudgetExceeded;
                    candidate.message = BuildBudgetMessage(candidate.requiredBytes, job.maxWorkingBytes);
                    return candidate;
                }
                candidate.labelPayload = std::make_shared<const LabelMap3DPayload>(
                    geometry, LabelMapValues{candidate.labels}, std::vector<LabelDefinition>{},
                    "PartSegmentation.labels", "Part segmentation");
            }
            candidate.labels = candidate.labelPayload->GetLabels();
            freezeMs += elapsedMs(freezeStarted);
        }
        candidate.extent = volume.extent;
        candidate.dimensions = volume.dimensions;
        candidate.spacing = volume.spacing;
        candidate.origin = volume.origin;
        candidate.direction = volume.direction;
        // 标签和目录构成业务结果；发布后才在既有受控 worker 上请求可选表面。
        const std::int64_t surfaceMs = 0;
        if (getStopped()) {
            candidate.status = PartResultStatus::Cancelled;
            candidate.failureReason = PartFailureReason::Cancelled;
            return candidate;
        }
        const auto freezeStarted = std::chrono::steady_clock::now();
        candidate.labelImage = vtkSmartPointer<vtkImageData>::New();
        candidate.labelImage->SetExtent(candidate.extent.data());
        candidate.labelImage->SetSpacing(candidate.spacing.data());
        candidate.labelImage->SetOrigin(candidate.origin.data());
        auto direction = vtkSmartPointer<vtkMatrix3x3>::New();
        direction->DeepCopy(candidate.direction.data());
        candidate.labelImage->SetDirectionMatrix(direction);
        auto labelScalars = vtkSmartPointer<vtkUnsignedIntArray>::New();
        // 数组由 DataGraph payload 管理；此 VTK 壳只供 Feature 私有 mapper 只读使用。
        labelScalars->SetArray(const_cast<unsigned int*>(candidate.labels->data()),
            static_cast<vtkIdType>(candidate.labels->size()), 1);
        candidate.labelImage->GetPointData()->SetScalars(labelScalars);
        if (getStopped()) {
            candidate.status = PartResultStatus::Cancelled;
            candidate.failureReason = PartFailureReason::Cancelled;
            candidate.message = "Part request was cancelled before publication.";
            return candidate;
        }
        candidate.metrics.peakWorkingBytes = std::max(
            candidate.metrics.peakWorkingBytes, candidate.requiredBytes);
        candidate.status = PartResultStatus::Succeeded;
        candidate.failureReason = PartFailureReason::None;
        candidate.message = BuildSuccessMessage(candidate.metrics)
            + " surfaceBytes="
            + std::to_string(candidate.surfaceBytes) + "."
            + " labelsMs=" + std::to_string(labelMs)
            + " lineageMs=" + std::to_string(lineageMs)
            + " surfaceMs=" + std::to_string(surfaceMs)
            + " freezeMs=" + std::to_string(freezeMs + elapsedMs(freezeStarted)) + ".";
        SetProgress(job.requestId, 1.0);
        return candidate;
    }
    catch (const std::bad_alloc&) {
        candidate.failureReason = PartFailureReason::BudgetExceeded;
        candidate.message =
            "Part allocation failed within the configured budget.";
    }
    catch (...) {
        candidate.failureReason = PartFailureReason::InternalError;
        candidate.message = "Part segmentation raised an internal error.";
    }
    return candidate;
}
