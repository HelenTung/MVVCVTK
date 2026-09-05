#include "Algorithms/ClassicalPartSegmenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace {

constexpr std::uint32_t unvisitedLabel =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t pendingLabel = unvisitedLabel - 1U;
constexpr std::size_t cancelBatch = 4096;
constexpr std::size_t progressBatch = 1024U * 1024U;
constexpr std::size_t indexChunkSize = 64U * 1024U;

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

bool GetStopped(const std::function<bool()>& getStopRequested)
{
    return getStopRequested && getStopRequested();
}

void SendProgress(
    const PartProgressCallback& sendProgress,
    const double progress)
{
    if (sendProgress) sendProgress(std::clamp(progress, 0.0, 1.0));
}

bool GetScalarTypeValid(const PartScalarType scalarType)
{
    switch (scalarType) {
    case PartScalarType::Int8:
    case PartScalarType::UInt8:
    case PartScalarType::Int16:
    case PartScalarType::UInt16:
    case PartScalarType::Int32:
    case PartScalarType::UInt32:
    case PartScalarType::Int64:
    case PartScalarType::UInt64:
    case PartScalarType::Float32:
    case PartScalarType::Float64:
        return true;
    }
    return false;
}

class WorkingBudget final {
public:
    explicit WorkingBudget(const std::size_t limit) noexcept
        : m_limit(limit)
    {
    }

    bool Add(const std::size_t bytes) noexcept
    {
        std::size_t required = 0;
        if (!GetSum(m_current, bytes, required)) {
            m_required = std::numeric_limits<std::size_t>::max();
            return false;
        }
        m_required = std::max(m_required, required);
        if (required > m_limit) return false;
        m_current = required;
        m_peak = std::max(m_peak, m_current);
        return true;
    }

    void Remove(const std::size_t bytes) noexcept
    {
        m_current = bytes <= m_current ? m_current - bytes : 0;
    }

    void SetOverflow() noexcept
    {
        m_required = std::numeric_limits<std::size_t>::max();
    }

    std::size_t GetPeak() const noexcept
    {
        return m_peak;
    }

    std::size_t GetRequired() const noexcept
    {
        return std::max(m_required, m_peak);
    }

private:
    std::size_t m_limit = 0;
    std::size_t m_current = 0;
    std::size_t m_peak = 0;
    std::size_t m_required = 0;
};

class IndexBuffer final {
private:
    struct IndexChunk final {
        std::array<std::size_t, indexChunkSize> values;
        IndexChunk* next = nullptr;
    };

public:
    IndexBuffer() = default;

    ~IndexBuffer() noexcept
    {
        auto* chunk = m_first;
        while (chunk) {
            auto* next = chunk->next;
            delete chunk;
            chunk = next;
        }
    }

    IndexBuffer(const IndexBuffer&) = delete;
    IndexBuffer& operator=(const IndexBuffer&) = delete;

    bool Push(
        const std::size_t value,
        WorkingBudget& budget,
        bool& didGrow)
    {
        didGrow = false;
        if (m_size == m_capacity) {
            if (m_capacity
                > std::numeric_limits<std::size_t>::max()
                    - indexChunkSize) {
                budget.SetOverflow();
                return false;
            }
            if (!budget.Add(sizeof(IndexChunk))) return false;

            IndexChunk* next = nullptr;
            try {
                next = new IndexChunk;
            }
            catch (...) {
                budget.Remove(sizeof(IndexChunk));
                throw;
            }
            if (m_last) {
                m_last->next = next;
            }
            else {
                m_first = next;
            }
            m_last = next;
            m_capacity += indexChunkSize;
            ++m_chunkCount;
            didGrow = true;
        }

        if (m_size == 0) {
            m_write = m_first;
        }
        else if (m_size % indexChunkSize == 0) {
            m_write = m_write ? m_write->next : nullptr;
        }
        if (!m_write) {
            budget.SetOverflow();
            return false;
        }
        m_write->values[m_size % indexChunkSize] = value;
        ++m_size;
        return true;
    }

    template<typename SendIndex>
    bool SendItems(SendIndex&& sendIndex) const
    {
        std::size_t remaining = m_size;
        const auto* chunk = m_first;
        while (chunk && remaining > 0) {
            const std::size_t count = std::min(remaining, indexChunkSize);
            for (std::size_t index = 0; index < count; ++index) {
                if (!sendIndex(chunk->values[index])) return false;
            }
            remaining -= count;
            chunk = chunk->next;
        }
        return remaining == 0;
    }

    void Clear() noexcept
    {
        m_size = 0;
        m_write = m_first;
    }

    void Swap(IndexBuffer& other) noexcept
    {
        std::swap(m_first, other.m_first);
        std::swap(m_last, other.m_last);
        std::swap(m_write, other.m_write);
        std::swap(m_size, other.m_size);
        std::swap(m_capacity, other.m_capacity);
        std::swap(m_chunkCount, other.m_chunkCount);
    }

    std::size_t GetSize() const noexcept
    {
        return m_size;
    }

    std::size_t GetBytes() const noexcept
    {
        return m_chunkCount * sizeof(IndexChunk);
    }

private:
    IndexChunk* m_first = nullptr;
    IndexChunk* m_last = nullptr;
    IndexChunk* m_write = nullptr;
    std::size_t m_size = 0;
    std::size_t m_capacity = 0;
    std::size_t m_chunkCount = 0;
};

void SetFailure(
    PartAlgorithmResult& result,
    const PartAlgorithmError error,
    const WorkingBudget& budget)
{
    std::vector<PartLabelId>().swap(result.labels);
    std::vector<PartMetrics>().swap(result.metricsByLabel);
    result.error = error;
    result.requiredBytes = budget.GetRequired();
    result.metrics.peakWorkingBytes = budget.GetPeak();
}

void SetFrontierPeak(
    PartAlgorithmMetrics& metrics,
    const IndexBuffer& current,
    const IndexBuffer& next)
{
    metrics.frontierPeakBytes = std::max(
        metrics.frontierPeakBytes,
        current.GetBytes() + next.GetBytes());
}

PartAlgorithmError GetVolumeError(
    const PartVolumeView& volume,
    const PartAlgorithmParams& params,
    std::size_t& voxelCount,
    std::size_t& partCapacity)
{
    if (!std::isfinite(params.threshold)
        || params.minPartVoxels == 0
        || params.maxPartCount == 0
        || params.maxPartCount >= pendingLabel
        || params.maxWorkingBytes == 0
        || !volume.values.data
        || !GetScalarTypeValid(volume.values.scalarType)) {
        return PartAlgorithmError::InvalidInput;
    }

    std::size_t product = 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const int dimension = volume.dimensions[axis];
        const auto extentSize = static_cast<std::int64_t>(
            volume.extent[axis * 2 + 1])
            - static_cast<std::int64_t>(
                volume.extent[axis * 2]) + 1;
        if (dimension <= 0
            || extentSize != static_cast<std::int64_t>(dimension)
            || !std::isfinite(volume.spacing[axis])
            || volume.spacing[axis] <= 0.0
            || !std::isfinite(volume.origin[axis])) {
            return PartAlgorithmError::InvalidInput;
        }
        if (!GetProduct(
                product,
                static_cast<std::size_t>(dimension),
                product)) {
            return PartAlgorithmError::BudgetExceeded;
        }
    }
    for (const double value : volume.direction) {
        if (!std::isfinite(value)) return PartAlgorithmError::InvalidInput;
    }
    const double voxelVolume = volume.spacing[0]
        * volume.spacing[1] * volume.spacing[2];
    if (!std::isfinite(voxelVolume) || voxelVolume <= 0.0) {
        return PartAlgorithmError::InvalidInput;
    }
    if (volume.values.valueCount != product) {
        return PartAlgorithmError::InvalidInput;
    }
    if (volume.validity
        && (!volume.validity->data
            || volume.validity->valueCount != product
            || !GetScalarTypeValid(volume.validity->scalarType))) {
        return PartAlgorithmError::InvalidInput;
    }

    const std::size_t separatedCapacity = product / 2 + product % 2;
    const std::size_t sizeCapacity = params.minPartVoxels
            > static_cast<std::uint64_t>(product)
        ? 0
        : product / static_cast<std::size_t>(params.minPartVoxels);
    partCapacity = std::min({
        separatedCapacity,
        sizeCapacity,
        static_cast<std::size_t>(params.maxPartCount)
    });
    voxelCount = product;
    return PartAlgorithmError::None;
}

template<typename Scalar>
bool GetForeground(const Scalar value, const double threshold)
{
    if constexpr (std::is_floating_point_v<Scalar>) {
        return std::isfinite(value)
            && static_cast<long double>(value)
                >= static_cast<long double>(threshold);
    }
    else if constexpr (std::is_unsigned_v<Scalar>) {
        if (threshold <= 0.0) return true;
        const double upperExclusive = std::ldexp(
            1.0, std::numeric_limits<Scalar>::digits);
        if (threshold >= upperExclusive) return false;
        const double minimum = std::ceil(threshold);
        if (minimum >= upperExclusive) return false;
        return value >= static_cast<Scalar>(minimum);
    }
    else {
        const double lowerInclusive = -std::ldexp(
            1.0, std::numeric_limits<Scalar>::digits);
        const double upperExclusive = std::ldexp(
            1.0, std::numeric_limits<Scalar>::digits);
        if (threshold <= lowerInclusive) return true;
        if (threshold >= upperExclusive) return false;
        const double minimum = std::ceil(threshold);
        if (minimum >= upperExclusive) return false;
        return value >= static_cast<Scalar>(minimum);
    }
}

template<typename Scalar>
bool GetMaskValid(const Scalar value)
{
    if constexpr (std::is_floating_point_v<Scalar>) {
        return std::isfinite(value) && value != Scalar{};
    }
    return value != Scalar{};
}

template<typename Scalar>
Scalar GetScalarValue(
    const PartScalarView& view,
    const std::size_t index) noexcept
{
    Scalar value{};
    const auto* bytes = static_cast<const unsigned char*>(view.data);
    std::memcpy(
        &value,
        bytes + index * sizeof(Scalar),
        sizeof(Scalar));
    return value;
}

template<typename Scalar>
bool SetValueLabels(
    const PartScalarView& values,
    const double threshold,
    std::vector<PartLabelId>& labels,
    const std::function<bool()>& getStopRequested,
    const PartProgressCallback& sendProgress,
    const double endProgress)
{
    for (std::size_t index = 0; index < values.valueCount; ++index) {
        if (index % cancelBatch == 0
            && GetStopped(getStopRequested)) {
            return false;
        }
        labels[index] = GetForeground(
            GetScalarValue<Scalar>(values, index), threshold)
            ? unvisitedLabel : 0U;
        if (index % progressBatch == 0) {
            SendProgress(
                sendProgress,
                endProgress * static_cast<double>(index)
                    / static_cast<double>(values.valueCount));
        }
    }
    SendProgress(sendProgress, endProgress);
    return true;
}

template<typename Scalar>
bool SetMaskLabels(
    const PartScalarView& validity,
    std::vector<PartLabelId>& labels,
    const std::function<bool()>& getStopRequested,
    const PartProgressCallback& sendProgress)
{
    for (std::size_t index = 0; index < validity.valueCount; ++index) {
        if (index % cancelBatch == 0
            && GetStopped(getStopRequested)) {
            return false;
        }
        if (!GetMaskValid(
                GetScalarValue<Scalar>(validity, index))) {
            labels[index] = 0U;
        }
        if (index % progressBatch == 0) {
            SendProgress(
                sendProgress,
                0.15 + 0.15 * static_cast<double>(index)
                    / static_cast<double>(validity.valueCount));
        }
    }
    SendProgress(sendProgress, 0.30);
    return true;
}

template<typename Scalar>
bool SetTypedValues(
    const PartVolumeView& volume,
    const PartAlgorithmParams& params,
    std::vector<PartLabelId>& labels,
    const std::function<bool()>& getStopRequested,
    const PartProgressCallback& sendProgress)
{
    return SetValueLabels<Scalar>(
        volume.values,
        params.threshold,
        labels,
        getStopRequested,
        sendProgress,
        volume.validity ? 0.15 : 0.30);
}

bool SetTypedValues(
    const PartVolumeView& volume,
    const PartAlgorithmParams& params,
    std::vector<PartLabelId>& labels,
    const std::function<bool()>& getStopRequested,
    const PartProgressCallback& sendProgress)
{
    switch (volume.values.scalarType) {
    case PartScalarType::Int8:
        return SetTypedValues<std::int8_t>(
            volume, params, labels, getStopRequested, sendProgress);
    case PartScalarType::UInt8:
        return SetTypedValues<std::uint8_t>(
            volume, params, labels, getStopRequested, sendProgress);
    case PartScalarType::Int16:
        return SetTypedValues<std::int16_t>(
            volume, params, labels, getStopRequested, sendProgress);
    case PartScalarType::UInt16:
        return SetTypedValues<std::uint16_t>(
            volume, params, labels, getStopRequested, sendProgress);
    case PartScalarType::Int32:
        return SetTypedValues<std::int32_t>(
            volume, params, labels, getStopRequested, sendProgress);
    case PartScalarType::UInt32:
        return SetTypedValues<std::uint32_t>(
            volume, params, labels, getStopRequested, sendProgress);
    case PartScalarType::Int64:
        return SetTypedValues<std::int64_t>(
            volume, params, labels, getStopRequested, sendProgress);
    case PartScalarType::UInt64:
        return SetTypedValues<std::uint64_t>(
            volume, params, labels, getStopRequested, sendProgress);
    case PartScalarType::Float32:
        return SetTypedValues<float>(
            volume, params, labels, getStopRequested, sendProgress);
    case PartScalarType::Float64:
        return SetTypedValues<double>(
            volume, params, labels, getStopRequested, sendProgress);
    }
    return false;
}

template<typename Scalar>
bool SetTypedMask(
    const PartScalarView& validity,
    std::vector<PartLabelId>& labels,
    const std::function<bool()>& getStopRequested,
    const PartProgressCallback& sendProgress)
{
    return SetMaskLabels<Scalar>(
        validity,
        labels,
        getStopRequested,
        sendProgress);
}

bool SetTypedMask(
    const PartScalarView& validity,
    std::vector<PartLabelId>& labels,
    const std::function<bool()>& getStopRequested,
    const PartProgressCallback& sendProgress)
{
    switch (validity.scalarType) {
    case PartScalarType::Int8:
        return SetTypedMask<std::int8_t>(
            validity, labels, getStopRequested, sendProgress);
    case PartScalarType::UInt8:
        return SetTypedMask<std::uint8_t>(
            validity, labels, getStopRequested, sendProgress);
    case PartScalarType::Int16:
        return SetTypedMask<std::int16_t>(
            validity, labels, getStopRequested, sendProgress);
    case PartScalarType::UInt16:
        return SetTypedMask<std::uint16_t>(
            validity, labels, getStopRequested, sendProgress);
    case PartScalarType::Int32:
        return SetTypedMask<std::int32_t>(
            validity, labels, getStopRequested, sendProgress);
    case PartScalarType::UInt32:
        return SetTypedMask<std::uint32_t>(
            validity, labels, getStopRequested, sendProgress);
    case PartScalarType::Int64:
        return SetTypedMask<std::int64_t>(
            validity, labels, getStopRequested, sendProgress);
    case PartScalarType::UInt64:
        return SetTypedMask<std::uint64_t>(
            validity, labels, getStopRequested, sendProgress);
    case PartScalarType::Float32:
        return SetTypedMask<float>(
            validity, labels, getStopRequested, sendProgress);
    case PartScalarType::Float64:
        return SetTypedMask<double>(
            validity, labels, getStopRequested, sendProgress);
    }
    return false;
}

std::array<double, 3> GetInputPhysical(
    const PartVolumeView& volume,
    const std::array<double, 3>& image)
{
    const std::array<double, 3> scaled{
        image[0] * volume.spacing[0],
        image[1] * volume.spacing[1],
        image[2] * volume.spacing[2]
    };
    std::array<double, 3> physical{};
    for (std::size_t row = 0; row < physical.size(); ++row) {
        physical[row] = volume.origin[row]
            + volume.direction[row * 3] * scaled[0]
            + volume.direction[row * 3 + 1] * scaled[1]
            + volume.direction[row * 3 + 2] * scaled[2];
    }
    return physical;
}

struct PartStats final {
    std::array<int, 3> minimum{
        std::numeric_limits<int>::max(),
        std::numeric_limits<int>::max(),
        std::numeric_limits<int>::max()
    };
    std::array<int, 3> maximum{
        std::numeric_limits<int>::min(),
        std::numeric_limits<int>::min(),
        std::numeric_limits<int>::min()
    };
    std::array<long double, 3> indexSum{};
};

void SetStats(
    PartStats& stats,
    const std::array<int, 3>& point)
{
    for (std::size_t axis = 0; axis < 3; ++axis) {
        stats.minimum[axis] = std::min(stats.minimum[axis], point[axis]);
        stats.maximum[axis] = std::max(stats.maximum[axis], point[axis]);
        stats.indexSum[axis] += point[axis];
    }
}

PartMetrics BuildMetrics(
    const PartVolumeView& volume,
    const std::size_t voxelCount,
    const PartStats& stats)
{
    PartMetrics metrics;
    metrics.voxelCount = static_cast<std::uint64_t>(voxelCount);
    metrics.physicalVolumeMM3 = static_cast<double>(voxelCount)
        * std::abs(
            volume.spacing[0]
            * volume.spacing[1]
            * volume.spacing[2]);
    metrics.voxelExtent = {
        stats.minimum[0], stats.maximum[0],
        stats.minimum[1], stats.maximum[1],
        stats.minimum[2], stats.maximum[2]
    };

    std::array<double, 3> centroidImage{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        centroidImage[axis] = static_cast<double>(
            stats.indexSum[axis] / static_cast<long double>(voxelCount));
    }
    metrics.centroidInputPhysical = GetInputPhysical(volume, centroidImage);

    metrics.inputPhysicalBounds = {
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()
    };
    for (int zSide = 0; zSide < 2; ++zSide) {
        for (int ySide = 0; ySide < 2; ++ySide) {
            for (int xSide = 0; xSide < 2; ++xSide) {
                const std::array<double, 3> image{
                    static_cast<double>(xSide == 0
                        ? stats.minimum[0] : stats.maximum[0]),
                    static_cast<double>(ySide == 0
                        ? stats.minimum[1] : stats.maximum[1]),
                    static_cast<double>(zSide == 0
                        ? stats.minimum[2] : stats.maximum[2])
                };
                const auto physical = GetInputPhysical(volume, image);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    metrics.inputPhysicalBounds[axis * 2] = std::min(
                        metrics.inputPhysicalBounds[axis * 2],
                        physical[axis]);
                    metrics.inputPhysicalBounds[axis * 2 + 1] = std::max(
                        metrics.inputPhysicalBounds[axis * 2 + 1],
                        physical[axis]);
                }
            }
        }
    }
    return metrics;
}

bool GetMetricsValid(const PartMetrics& metrics)
{
    if (metrics.voxelCount == 0
        || !std::isfinite(metrics.physicalVolumeMM3)
        || metrics.physicalVolumeMM3 <= 0.0) {
        return false;
    }
    return std::all_of(
            metrics.inputPhysicalBounds.begin(),
            metrics.inputPhysicalBounds.end(),
            [](const double value) { return std::isfinite(value); })
        && std::all_of(
            metrics.centroidInputPhysical.begin(),
            metrics.centroidInputPhysical.end(),
            [](const double value) { return std::isfinite(value); });
}

} // namespace

PartAlgorithmResult ClassicalPartSegmenter::BuildLabels(
    const PartVolumeView& volume,
    const PartAlgorithmParams& params,
    const std::function<bool()>& getStopRequested,
    const PartProgressCallback& sendProgress)
{
    PartAlgorithmResult result;
    WorkingBudget budget(params.maxWorkingBytes);
    std::size_t voxelCount = 0;
    std::size_t partCapacity = 0;
    const PartAlgorithmError inputError =
        GetVolumeError(volume, params, voxelCount, partCapacity);
    if (inputError != PartAlgorithmError::None) {
        result.error = inputError;
        return result;
    }
    SendProgress(sendProgress, 0.0);
    if (GetStopped(getStopRequested)) {
        result.error = PartAlgorithmError::Cancelled;
        return result;
    }

    std::size_t labelBytes = 0;
    std::size_t catalogBytes = 0;
    std::size_t baseBytes = 0;
    const std::size_t metricsCapacity = partCapacity + 1U;
    if (!GetProduct(voxelCount, sizeof(PartLabelId), labelBytes)
        || !GetProduct(
            metricsCapacity, sizeof(PartMetrics), catalogBytes)
        || !GetSum(labelBytes, catalogBytes, baseBytes)) {
        budget.SetOverflow();
        SetFailure(result, PartAlgorithmError::BudgetExceeded, budget);
        return result;
    }
    if (!budget.Add(baseBytes)) {
        SetFailure(result, PartAlgorithmError::BudgetExceeded, budget);
        return result;
    }

    try {
        result.labels.resize(voxelCount);
        result.metricsByLabel.reserve(metricsCapacity);
        result.metricsByLabel.emplace_back();
        std::size_t actualLabelBytes = 0;
        std::size_t actualCatalogBytes = 0;
        std::size_t actualBaseBytes = 0;
        if (!GetProduct(
                result.labels.capacity(),
                sizeof(PartLabelId),
                actualLabelBytes)
            || !GetProduct(
                result.metricsByLabel.capacity(),
                sizeof(PartMetrics),
                actualCatalogBytes)
            || !GetSum(
                actualLabelBytes,
                actualCatalogBytes,
                actualBaseBytes)) {
            budget.SetOverflow();
            SetFailure(result, PartAlgorithmError::BudgetExceeded, budget);
            return result;
        }
        if (actualBaseBytes > baseBytes
            && !budget.Add(actualBaseBytes - baseBytes)) {
            SetFailure(result, PartAlgorithmError::BudgetExceeded, budget);
            return result;
        }
        result.metrics.labelBytes = actualLabelBytes;
        result.metrics.catalogBytes = actualCatalogBytes;

        if (!SetTypedValues(
                volume,
                params,
                result.labels,
                getStopRequested,
                sendProgress)
            || (volume.validity
                && !SetTypedMask(
                    *volume.validity,
                    result.labels,
                    getStopRequested,
                    sendProgress))) {
            SetFailure(result, PartAlgorithmError::Cancelled, budget);
            return result;
        }

        IndexBuffer currentFrontier;
        IndexBuffer nextFrontier;
        IndexBuffer filteredIndices;
        PartLabelId nextPartId = 0;
        std::size_t foregroundDone = 0;
        const std::size_t width =
            static_cast<std::size_t>(volume.dimensions[0]);
        const std::size_t height =
            static_cast<std::size_t>(volume.dimensions[1]);
        const std::size_t sliceSize = width * height;

        for (std::size_t seed = 0; seed < voxelCount; ++seed) {
            if (seed % cancelBatch == 0
                && GetStopped(getStopRequested)) {
                SetFailure(result, PartAlgorithmError::Cancelled, budget);
                return result;
            }
            if (seed % progressBatch == 0) {
                const std::size_t done = std::max(seed, foregroundDone);
                SendProgress(
                    sendProgress,
                    0.30 + 0.65 * static_cast<double>(done)
                        / static_cast<double>(voxelCount));
            }
            if (result.labels[seed] != unvisitedLabel) continue;

            currentFrontier.Clear();
            nextFrontier.Clear();
            filteredIndices.Clear();
            bool didGrow = false;
            if (!currentFrontier.Push(seed, budget, didGrow)) {
                SetFailure(
                    result, PartAlgorithmError::BudgetExceeded, budget);
                return result;
            }
            if (didGrow) {
                SetFrontierPeak(
                    result.metrics, currentFrontier, nextFrontier);
            }
            result.labels[seed] = pendingLabel;

            PartStats stats;
            std::size_t componentCount = 0;
            PartLabelId partId = 0;
            bool isAccepted = false;
            PartAlgorithmError traversalError = PartAlgorithmError::None;

            while (currentFrontier.GetSize() > 0) {
                std::size_t levelIndex = 0;
                const bool didVisit = currentFrontier.SendItems(
                    [&](const std::size_t current) {
                        if (levelIndex % cancelBatch == 0
                            && GetStopped(getStopRequested)) {
                            traversalError = PartAlgorithmError::Cancelled;
                            return false;
                        }
                        ++levelIndex;
                        ++componentCount;
                        ++foregroundDone;

                        const std::size_t currentZ = current / sliceSize;
                        const std::size_t inSlice = current % sliceSize;
                        const std::size_t currentY = inSlice / width;
                        const std::size_t currentX = inSlice % width;
                        SetStats(stats, {
                            volume.extent[0]
                                + static_cast<int>(currentX),
                            volume.extent[2]
                                + static_cast<int>(currentY),
                            volume.extent[4]
                                + static_cast<int>(currentZ)
                        });

                        if (!isAccepted
                            && static_cast<std::uint64_t>(componentCount)
                                < params.minPartVoxels) {
                            bool didFilterGrow = false;
                            if (!filteredIndices.Push(
                                    current, budget, didFilterGrow)) {
                                traversalError =
                                    PartAlgorithmError::BudgetExceeded;
                                return false;
                            }
                            if (didFilterGrow) {
                                result.metrics.filteredPeakBytes = std::max(
                                    result.metrics.filteredPeakBytes,
                                    filteredIndices.GetBytes());
                            }
                        }
                        else if (!isAccepted) {
                            if (nextPartId == params.maxPartCount) {
                                traversalError =
                                    PartAlgorithmError::LabelOverflow;
                                return false;
                            }
                            ++nextPartId;
                            partId = nextPartId;
                            isAccepted = true;
                            std::size_t restoreCount = 0;
                            if (!filteredIndices.SendItems(
                                    [&](const std::size_t index) {
                                        if (restoreCount % cancelBatch == 0
                                            && GetStopped(
                                                getStopRequested)) {
                                            traversalError =
                                                PartAlgorithmError::Cancelled;
                                            return false;
                                        }
                                        ++restoreCount;
                                        result.labels[index] = partId;
                                        return true;
                                    })) {
                                return false;
                            }
                            filteredIndices.Clear();
                            result.labels[current] = partId;
                        }
                        else {
                            result.labels[current] = partId;
                        }

                        const auto addNeighbor =
                            [&](const std::size_t neighbor) {
                                if (result.labels[neighbor]
                                    != unvisitedLabel) {
                                    return true;
                                }
                                result.labels[neighbor] = isAccepted
                                    ? partId : pendingLabel;
                                bool didNeighborGrow = false;
                                if (!nextFrontier.Push(
                                        neighbor,
                                        budget,
                                        didNeighborGrow)) {
                                    traversalError =
                                        PartAlgorithmError::BudgetExceeded;
                                    return false;
                                }
                                if (didNeighborGrow) {
                                    SetFrontierPeak(
                                        result.metrics,
                                        currentFrontier,
                                        nextFrontier);
                                }
                                return true;
                            };
                        if (currentX > 0
                            && !addNeighbor(current - 1)) return false;
                        if (currentX + 1 < width
                            && !addNeighbor(current + 1)) return false;
                        if (currentY > 0
                            && !addNeighbor(current - width)) return false;
                        if (currentY + 1 < height
                            && !addNeighbor(current + width)) return false;
                        if (currentZ > 0
                            && !addNeighbor(current - sliceSize)) return false;
                        if (currentZ + 1
                                < static_cast<std::size_t>(
                                    volume.dimensions[2])
                            && !addNeighbor(current + sliceSize)) {
                            return false;
                        }
                        return true;
                    });
                if (!didVisit) {
                    if (traversalError == PartAlgorithmError::None) {
                        traversalError = PartAlgorithmError::InternalError;
                    }
                    SetFailure(result, traversalError, budget);
                    return result;
                }
                currentFrontier.Clear();
                currentFrontier.Swap(nextFrontier);
                nextFrontier.Clear();
                SendProgress(
                    sendProgress,
                    0.30 + 0.65
                        * static_cast<double>(
                            std::max(seed + 1U, foregroundDone))
                        / static_cast<double>(voxelCount));
            }

            if (!isAccepted) {
                std::size_t restoreCount = 0;
                const bool didRestore = filteredIndices.SendItems(
                    [&](const std::size_t index) {
                        if (restoreCount % cancelBatch == 0
                            && GetStopped(getStopRequested)) {
                            return false;
                        }
                        ++restoreCount;
                        result.labels[index] = 0U;
                        return true;
                    });
                if (!didRestore) {
                    SetFailure(
                        result, PartAlgorithmError::Cancelled, budget);
                    return result;
                }
                continue;
            }
            auto metrics = BuildMetrics(volume, componentCount, stats);
            if (!GetMetricsValid(metrics)
                || result.metricsByLabel.size()
                    != static_cast<std::size_t>(partId)) {
                SetFailure(
                    result, PartAlgorithmError::InvalidInput, budget);
                return result;
            }
            result.metricsByLabel.push_back(std::move(metrics));
        }

        SendProgress(sendProgress, 0.95);
        const bool hasSentinel = std::any_of(
            result.labels.begin(), result.labels.end(),
            [](const PartLabelId label) {
                return label == unvisitedLabel || label == pendingLabel;
            });
        if (hasSentinel) {
            SetFailure(result, PartAlgorithmError::InternalError, budget);
            return result;
        }
        result.error = PartAlgorithmError::None;
        result.requiredBytes = budget.GetRequired();
        result.metrics.peakWorkingBytes = budget.GetPeak();
        SendProgress(sendProgress, 1.0);
        return result;
    }
    catch (const std::bad_alloc&) {
        SetFailure(result, PartAlgorithmError::BudgetExceeded, budget);
        return result;
    }
    catch (...) {
        SetFailure(result, PartAlgorithmError::InternalError, budget);
        return result;
    }
}
