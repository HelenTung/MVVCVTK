#include "Algorithms/ClassicalPartSegmenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace {

constexpr std::uint32_t filteredLabel =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t cancelBatch = 4096;

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

std::array<double, 3> GetWorld(
    const PartVolumeData& volume,
    const std::array<double, 3>& image)
{
    const std::array<double, 3> scaled{
        image[0] * volume.spacing[0],
        image[1] * volume.spacing[1],
        image[2] * volume.spacing[2]
    };
    std::array<double, 3> world{};
    for (std::size_t row = 0; row < world.size(); ++row) {
        world[row] = volume.origin[row]
            + volume.direction[row * 3] * scaled[0]
            + volume.direction[row * 3 + 1] * scaled[1]
            + volume.direction[row * 3 + 2] * scaled[2];
    }
    return world;
}

PartAlgorithmError GetVolumeError(
    const PartVolumeData& volume,
    const PartAlgorithmParams& params,
    std::size_t& voxelCount,
    std::size_t& partCapacity)
{
    if (!std::isfinite(params.threshold)
        || params.minPartVoxels == 0
        || params.maxPartCount == 0
        || params.maxPartCount >= filteredLabel
        || params.maxWorkingBytes == 0) {
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
    if (product != volume.values.size()
        || (!volume.validity.empty()
            && volume.validity.size() != product)) {
        return PartAlgorithmError::InvalidInput;
    }

    const std::size_t separatedCapacity = product / 2 + product % 2;
    const std::size_t sizeCapacity = params.minPartVoxels > product
        ? 0
        : product / static_cast<std::size_t>(params.minPartVoxels);
    partCapacity = std::min({
        separatedCapacity,
        sizeCapacity,
        static_cast<std::size_t>(params.maxPartCount)
    });

    std::size_t valueBytes = 0;
    std::size_t labelBytes = 0;
    std::size_t queueBytes = 0;
    std::size_t catalogBytes = 0;
    std::size_t workingBytes = 0;
    if (!GetProduct(product, sizeof(double), valueBytes)
        || !GetProduct(product, sizeof(std::uint32_t), labelBytes)
        || !GetProduct(product, sizeof(std::size_t), queueBytes)
        || !GetProduct(partCapacity, sizeof(PartRecord), catalogBytes)
        || !GetSum(valueBytes, volume.validity.size(), workingBytes)
        || !GetSum(workingBytes, labelBytes, workingBytes)
        || !GetSum(workingBytes, queueBytes, workingBytes)
        || !GetSum(workingBytes, catalogBytes, workingBytes)
        || workingBytes > params.maxWorkingBytes) {
        return PartAlgorithmError::BudgetExceeded;
    }
    voxelCount = product;
    return PartAlgorithmError::None;
}

bool GetForeground(
    const PartVolumeData& volume,
    const PartAlgorithmParams& params,
    const std::size_t index)
{
    if (!volume.validity.empty() && volume.validity[index] == 0) {
        return false;
    }
    const double value = volume.values[index];
    return std::isfinite(value) && value >= params.threshold;
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

PartRecord BuildRecord(
    const PartVolumeData& volume,
    const PartId partId,
    const std::size_t voxelCount,
    const PartStats& stats)
{
    PartRecord record;
    record.partId = partId;
    record.voxelCount = static_cast<std::uint64_t>(voxelCount);
    record.physicalVolumeMM3 = static_cast<double>(voxelCount)
        * std::abs(
            volume.spacing[0]
            * volume.spacing[1]
            * volume.spacing[2]);
    record.voxelExtent = {
        stats.minimum[0], stats.maximum[0],
        stats.minimum[1], stats.maximum[1],
        stats.minimum[2], stats.maximum[2]
    };

    std::array<double, 3> centroidImage{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        centroidImage[axis] = static_cast<double>(
            stats.indexSum[axis] / static_cast<long double>(voxelCount));
    }
    record.centroidWorld = GetWorld(volume, centroidImage);

    record.worldBounds = {
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
                const auto world = GetWorld(volume, image);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    record.worldBounds[axis * 2] = std::min(
                        record.worldBounds[axis * 2], world[axis]);
                    record.worldBounds[axis * 2 + 1] = std::max(
                        record.worldBounds[axis * 2 + 1], world[axis]);
                }
            }
        }
    }
    return record;
}

} // namespace

PartAlgorithmResult ClassicalPartSegmenter::BuildLabels(
    const PartVolumeData& volume,
    const PartAlgorithmParams& params,
    const std::function<bool()>& getStopRequested)
{
    PartAlgorithmResult result;
    std::size_t voxelCount = 0;
    std::size_t partCapacity = 0;
    const PartAlgorithmError inputError =
        GetVolumeError(volume, params, voxelCount, partCapacity);
    if (inputError != PartAlgorithmError::None) {
        result.error = inputError;
        return result;
    }
    if (GetStopped(getStopRequested)) {
        result.error = PartAlgorithmError::Cancelled;
        return result;
    }

    try {
        result.labels.assign(voxelCount, 0U);
        result.parts.reserve(partCapacity);
        std::vector<std::size_t> queue;
        queue.reserve(voxelCount);
        PartId nextPartId = 0;

        const std::size_t width =
            static_cast<std::size_t>(volume.dimensions[0]);
        const std::size_t height =
            static_cast<std::size_t>(volume.dimensions[1]);
        const std::size_t sliceSize = width * height;

        for (int localZ = 0; localZ < volume.dimensions[2]; ++localZ) {
            if (GetStopped(getStopRequested)) {
                result.labels.clear();
                result.parts.clear();
                result.error = PartAlgorithmError::Cancelled;
                return result;
            }
            for (int localY = 0; localY < volume.dimensions[1]; ++localY) {
                for (int localX = 0; localX < volume.dimensions[0]; ++localX) {
                    const std::size_t seed =
                        static_cast<std::size_t>(localX)
                        + width * (static_cast<std::size_t>(localY)
                            + height * static_cast<std::size_t>(localZ));
                    if (result.labels[seed] != 0
                        || !GetForeground(volume, params, seed)) {
                        continue;
                    }

                    queue.clear();
                    queue.push_back(seed);
                    result.labels[seed] = filteredLabel;
                    PartStats stats;

                    for (std::size_t head = 0; head < queue.size(); ++head) {
                        if (head % cancelBatch == 0
                            && GetStopped(getStopRequested)) {
                            result.labels.clear();
                            result.parts.clear();
                            result.error = PartAlgorithmError::Cancelled;
                            return result;
                        }
                        const std::size_t current = queue[head];
                        const std::size_t currentZ = current / sliceSize;
                        const std::size_t inSlice = current % sliceSize;
                        const std::size_t currentY = inSlice / width;
                        const std::size_t currentX = inSlice % width;
                        SetStats(stats, {
                            volume.extent[0] + static_cast<int>(currentX),
                            volume.extent[2] + static_cast<int>(currentY),
                            volume.extent[4] + static_cast<int>(currentZ)
                        });

                        const auto addNeighbor = [&](const std::size_t neighbor) {
                            if (result.labels[neighbor] == 0
                                && GetForeground(volume, params, neighbor)) {
                                result.labels[neighbor] = filteredLabel;
                                queue.push_back(neighbor);
                            }
                        };
                        if (currentX > 0) addNeighbor(current - 1);
                        if (currentX + 1 < width) addNeighbor(current + 1);
                        if (currentY > 0) addNeighbor(current - width);
                        if (currentY + 1 < height) addNeighbor(current + width);
                        if (currentZ > 0) addNeighbor(current - sliceSize);
                        if (currentZ + 1
                            < static_cast<std::size_t>(volume.dimensions[2])) {
                            addNeighbor(current + sliceSize);
                        }
                    }

                    if (queue.size() < params.minPartVoxels) {
                        continue;
                    }
                    if (nextPartId == params.maxPartCount) {
                        result.labels.clear();
                        result.parts.clear();
                        result.error = PartAlgorithmError::LabelOverflow;
                        return result;
                    }
                    ++nextPartId;
                    for (const std::size_t index : queue) {
                        result.labels[index] = nextPartId;
                    }
                    result.parts.push_back(BuildRecord(
                        volume, nextPartId, queue.size(), stats));
                }
            }
        }

        std::replace(
            result.labels.begin(), result.labels.end(),
            filteredLabel, 0U);
        result.error = PartAlgorithmError::None;
        return result;
    }
    catch (const std::bad_alloc&) {
        result.labels.clear();
        result.parts.clear();
        result.error = PartAlgorithmError::BudgetExceeded;
        return result;
    }
    catch (...) {
        result.labels.clear();
        result.parts.clear();
        result.error = PartAlgorithmError::InternalError;
        return result;
    }
}
