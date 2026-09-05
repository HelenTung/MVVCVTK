#include "Services/PartSegmentationService.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkType.h>

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

// 每标签 LUT 与离散等值面均为 O(partCount)，必须保持显式上限。
constexpr std::uint32_t maxOverlayPartCount = 4096;

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
    const std::size_t maxWorkingBytes)
{
    return "Part working-set budget is exceeded: requiredBytes="
        + std::to_string(requiredBytes)
        + ", maxWorkingBytes=" + std::to_string(maxWorkingBytes) + ".";
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

PartSegmentationService::PartSegmentationService()
    : m_worker([this] { WorkerLoop(); })
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
        }
        m_workReady.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }
}

PartAdmissionStatus PartSegmentationService::Start(
    TrustedImageSnapshot source,
    PartSegmentationStartParams params,
    const std::size_t maxWorkingBytes,
    const std::uint64_t requestId,
    PartHistorySnapshot previous,
    const std::uint64_t expectedResultRevision,
    const std::uint64_t expectedCatalogRevision)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isStopping) return PartAdmissionStatus::Stopping;
    if (m_isBusy || m_job || m_complete) return PartAdmissionStatus::Busy;
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
        expectedCatalogRevision
    };
    m_workReady.notify_one();
    return PartAdmissionStatus::Accepted;
}

void PartSegmentationService::StopRequest() noexcept
{
    m_cancelRequested.store(true, std::memory_order_release);
}

std::optional<PartLabelCandidate>
PartSegmentationService::GetComplete()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto complete = std::move(m_complete);
    m_complete.reset();
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

bool PartSegmentationService::GetIsBusy() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_isBusy || m_job.has_value() || m_complete.has_value();
}

bool PartSegmentationService::Stop(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_isStopping = true;
        m_cancelRequested.store(true, std::memory_order_release);
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
    if (m_progressRequestId.load(std::memory_order_acquire)
        != requestId) {
        return;
    }
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

void PartSegmentationService::WorkerLoop() noexcept
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workReady.wait(lock, [this] {
                return m_isStopping || m_job.has_value();
            });
            if (m_isStopping && !m_job) break;
            job = std::move(*m_job);
            m_job.reset();
            m_isBusy = true;
        }

        PartLabelCandidate candidate = BuildCandidate(job);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_isBusy = false;
            m_complete = std::move(candidate);
        }
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
    candidate.sourceVersion = job.source ? job.source->version : 0;
    candidate.expectedResultRevision = job.expectedResultRevision;
    candidate.expectedCatalogRevision = job.expectedCatalogRevision;
    candidate.failureReason = PartFailureReason::InvalidSource;
    candidate.message = "Part source is unavailable.";
    if (!job.source || !job.source->image) return candidate;

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
        if (volume.dimensions != job.source->dims) {
            candidate.failureReason = PartFailureReason::InvalidGeometry;
            candidate.message = "Part source dimensions are inconsistent.";
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
        if (!GetHistoryBytes(job.previous, historyBytes)
            || historyBytes >= job.maxWorkingBytes) {
            candidate.failureReason = PartFailureReason::BudgetExceeded;
            candidate.requiredBytes = historyBytes;
            candidate.message = BuildBudgetMessage(
                historyBytes, job.maxWorkingBytes);
            return candidate;
        }

        PartAlgorithmParams params;
        params.threshold = job.params.threshold;
        params.minPartVoxels = job.params.minPartVoxels;
        params.maxPartCount = maxOverlayPartCount;
        params.maxWorkingBytes = job.maxWorkingBytes - historyBytes;
        auto result = ClassicalPartSegmenter::BuildLabels(
            volume,
            params,
            [this] {
                return m_cancelRequested.load(std::memory_order_acquire);
            },
            [this, requestId = job.requestId](const double progress) {
                SetProgress(requestId, progress);
            });
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
                    "Part count exceeds the bounded overlay limit.";
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

        candidate.labels =
            std::make_shared<std::vector<PartLabelId>>(
                std::move(result.labels));
        PartLineageRequest lineageRequest;
        lineageRequest.previous = job.previous;
        lineageRequest.currentLabels = candidate.labels;
        lineageRequest.currentMetricsByLabel =
            std::move(result.metricsByLabel);
        lineageRequest.nextResultRevision = job.expectedResultRevision == 0
            ? 1 : job.expectedResultRevision + 1U;
        lineageRequest.maxWorkingBytes = job.maxWorkingBytes;
        auto lineage = PartLineageMatcher::BuildCatalog(
            std::move(lineageRequest),
            m_identities,
            [this] {
                return m_cancelRequested.load(std::memory_order_acquire);
            });
        candidate.requiredBytes = std::max(
            candidate.requiredBytes, lineage.requiredBytes);
        if (!lineage.catalog) {
            candidate.failureReason = lineage.failureReason;
            candidate.status = lineage.failureReason
                    == PartFailureReason::Cancelled
                ? PartResultStatus::Cancelled : PartResultStatus::Failed;
            candidate.message = lineage.failureReason
                    == PartFailureReason::BudgetExceeded
                ? BuildBudgetMessage(
                    lineage.requiredBytes, job.maxWorkingBytes)
                : lineage.failureReason == PartFailureReason::Cancelled
                    ? "Part request was cancelled."
                    : "Part catalog construction failed.";
            candidate.labels.reset();
            return candidate;
        }
        candidate.catalog = std::move(lineage.catalog);
        candidate.status = PartResultStatus::Succeeded;
        candidate.failureReason = PartFailureReason::None;
        candidate.message = BuildSuccessMessage(result.metrics);
        candidate.extent = volume.extent;
        candidate.dimensions = volume.dimensions;
        candidate.spacing = volume.spacing;
        candidate.origin = volume.origin;
        candidate.direction = volume.direction;
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
