#include "Services/PartSegmentationService.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <new>
#include <utility>

namespace {

constexpr std::size_t stagingBatch = 4096;
// Phase 1 的每标签 LUT 与离散等值面均为 O(partCount)，必须保持显式上限。
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

bool GetSum(
    const std::size_t left,
    const std::size_t right,
    std::size_t& sum)
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    sum = left + right;
    return true;
}

PartFailureReason GetWorkingBudget(
    const std::size_t voxelCount,
    const bool hasMask,
    const std::uint64_t minPartVoxels,
    const std::size_t maxWorkingBytes)
{
    const std::size_t separatedCapacity =
        voxelCount / 2 + voxelCount % 2;
    const std::size_t sizeCapacity = minPartVoxels > voxelCount
        ? 0
        : voxelCount / static_cast<std::size_t>(minPartVoxels);
    const std::size_t partCapacity = std::min(
        separatedCapacity, sizeCapacity);
    std::size_t valueBytes = 0;
    std::size_t labelBytes = 0;
    std::size_t queueBytes = 0;
    std::size_t catalogBytes = 0;
    std::size_t totalBytes = 0;
    if (maxWorkingBytes == 0
        || !GetProduct(voxelCount, sizeof(double), valueBytes)
        || !GetProduct(voxelCount, sizeof(std::uint32_t), labelBytes)
        || !GetProduct(voxelCount, sizeof(std::size_t), queueBytes)
        || !GetProduct(partCapacity, sizeof(PartRecord), catalogBytes)
        || !GetSum(valueBytes, hasMask ? voxelCount : 0, totalBytes)
        || !GetSum(totalBytes, labelBytes, totalBytes)
        || !GetSum(totalBytes, queueBytes, totalBytes)
        || !GetSum(totalBytes, catalogBytes, totalBytes)
        || totalBytes > maxWorkingBytes) {
        return PartFailureReason::BudgetExceeded;
    }
    return PartFailureReason::None;
}

bool GetImageGeometry(
    vtkImageData& image,
    PartVolumeData& volume)
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
    vtkImageData& left,
    vtkImageData& right)
{
    int leftExtent[6]{};
    int rightExtent[6]{};
    left.GetExtent(leftExtent);
    right.GetExtent(rightExtent);
    return std::equal(
        std::begin(leftExtent), std::end(leftExtent),
        std::begin(rightExtent));
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
    const std::uint64_t requestId)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isStopping) return PartAdmissionStatus::Stopping;
    if (m_isBusy || m_job || m_complete) return PartAdmissionStatus::Busy;
    if (!source || !source->image || requestId == 0
        || !std::isfinite(params.threshold)
        || params.minPartVoxels == 0
        || maxWorkingBytes == 0) {
        return PartAdmissionStatus::InvalidRequest;
    }
    m_cancelRequested.store(false, std::memory_order_release);
    m_job = Job{
        std::move(source), std::move(params), maxWorkingBytes, requestId
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
    candidate.failureReason = PartFailureReason::InvalidSource;
    candidate.message = "Part source is unavailable.";
    if (!job.source || !job.source->image) return candidate;

    try {
        // DataManager 以新 TrustedImageState 批次替换 current，已发布 snapshot 的
        // VTK 标量只读且生命周期由 Feature 保持到 owner thread 消费 candidate。
        // worker 只做读取与私有值 staging，不修改或销毁 source payload。
        PartVolumeData volume;
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

        auto* scalars = image->GetPointData()
            ? image->GetPointData()->GetScalars() : nullptr;
        if (!scalars || scalars->GetNumberOfComponents() != 1) {
            candidate.failureReason = PartFailureReason::UnsupportedScalar;
            candidate.message = "Part source must contain one numeric component.";
            return candidate;
        }
        const vtkIdType tupleCount = scalars->GetNumberOfTuples();
        if (tupleCount <= 0
            || static_cast<unsigned long long>(tupleCount)
                > std::numeric_limits<std::size_t>::max()) {
            candidate.failureReason = PartFailureReason::InvalidGeometry;
            candidate.message = "Part source tuple count is invalid.";
            return candidate;
        }
        const std::size_t voxelCount = static_cast<std::size_t>(tupleCount);

        vtkDataArray* maskScalars = nullptr;
        if (job.source->validityMask) {
            auto* mask = job.source->validityMask.GetPointer();
            if (!mask || !GetSameGeometry(*image, *mask)) {
                candidate.failureReason = PartFailureReason::InvalidGeometry;
                candidate.message = "Part validity geometry is inconsistent.";
                return candidate;
            }
            maskScalars = mask->GetPointData()
                ? mask->GetPointData()->GetScalars() : nullptr;
            if (!maskScalars
                || maskScalars->GetNumberOfComponents() != 1
                || maskScalars->GetNumberOfTuples() != tupleCount) {
                candidate.failureReason = PartFailureReason::InvalidGeometry;
                candidate.message = "Part validity data is invalid.";
                return candidate;
            }
        }

        const auto budgetError = GetWorkingBudget(
            voxelCount,
            maskScalars != nullptr,
            job.params.minPartVoxels,
            job.maxWorkingBytes);
        if (budgetError != PartFailureReason::None) {
            candidate.failureReason = budgetError;
            candidate.message = "Part working-set budget is exceeded.";
            return candidate;
        }

        volume.values.resize(voxelCount);
        if (maskScalars) volume.validity.resize(voxelCount);
        for (std::size_t index = 0; index < voxelCount; ++index) {
            if (index % stagingBatch == 0
                && m_cancelRequested.load(std::memory_order_acquire)) {
                candidate.status = PartResultStatus::Cancelled;
                candidate.failureReason = PartFailureReason::Cancelled;
                candidate.message = "Part request was cancelled.";
                return candidate;
            }
            volume.values[index] = scalars->GetComponent(
                static_cast<vtkIdType>(index), 0);
            if (maskScalars) {
                const double maskValue = maskScalars->GetComponent(
                    static_cast<vtkIdType>(index), 0);
                volume.validity[index] =
                    std::isfinite(maskValue) && maskValue != 0.0
                    ? std::uint8_t{ 1 }
                    : std::uint8_t{ 0 };
            }
        }

        PartAlgorithmParams params;
        params.threshold = job.params.threshold;
        params.minPartVoxels = job.params.minPartVoxels;
        params.maxPartCount = maxOverlayPartCount;
        params.maxWorkingBytes = job.maxWorkingBytes;
        auto result = ClassicalPartSegmenter::BuildLabels(
            volume, params,
            [this] {
                return m_cancelRequested.load(std::memory_order_acquire);
            });
        if (result.error != PartAlgorithmError::None) {
            if (result.error == PartAlgorithmError::Cancelled) {
                candidate.status = PartResultStatus::Cancelled;
                candidate.failureReason = PartFailureReason::Cancelled;
                candidate.message = "Part request was cancelled.";
            }
            else if (result.error == PartAlgorithmError::BudgetExceeded) {
                candidate.failureReason = PartFailureReason::BudgetExceeded;
                candidate.message = "Part working-set budget is exceeded.";
            }
            else if (result.error == PartAlgorithmError::LabelOverflow) {
                candidate.failureReason = PartFailureReason::BudgetExceeded;
                candidate.message =
                    "Part count exceeds the bounded overlay limit.";
            }
            else {
                candidate.failureReason = PartFailureReason::InternalError;
                candidate.message = "Part segmentation failed.";
            }
            return candidate;
        }

        candidate.status = PartResultStatus::Succeeded;
        candidate.failureReason = PartFailureReason::None;
        candidate.message = "Part segmentation succeeded.";
        candidate.extent = volume.extent;
        candidate.dimensions = volume.dimensions;
        candidate.spacing = volume.spacing;
        candidate.origin = volume.origin;
        candidate.direction = volume.direction;
        candidate.labels = std::move(result.labels);
        candidate.parts = std::move(result.parts);
        return candidate;
    }
    catch (const std::bad_alloc&) {
        candidate.failureReason = PartFailureReason::BudgetExceeded;
        candidate.message = "Part allocation failed within the configured budget.";
    }
    catch (...) {
        candidate.failureReason = PartFailureReason::InternalError;
        candidate.message = "Part segmentation raised an internal error.";
    }
    return candidate;
}
