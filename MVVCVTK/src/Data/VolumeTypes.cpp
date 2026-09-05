#include "Data/VolumeTypes.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace {

constexpr double directionTolerance = 1.0e-6;

bool GetRequiredTextValid(
    const std::string& value,
    const std::size_t limit) noexcept
{
    return !value.empty() && value.size() <= limit;
}

bool GetOptionalTextValid(
    const std::optional<std::string>& value,
    const std::size_t limit) noexcept
{
    return !value || GetRequiredTextValid(*value, limit);
}

bool GetDirectionValid(const std::array<double, 9>& value) noexcept
{
    for (const double element : value) {
        if (!std::isfinite(element)) return false;
    }
    for (std::size_t column = 0; column < 3; ++column) {
        double normSquared = 0.0;
        for (std::size_t row = 0; row < 3; ++row) {
            const double element = value[row * 3 + column];
            normSquared += element * element;
        }
        if (std::abs(normSquared - 1.0) > directionTolerance) {
            return false;
        }
        for (std::size_t other = column + 1; other < 3; ++other) {
            double dot = 0.0;
            for (std::size_t row = 0; row < 3; ++row) {
                dot += value[row * 3 + column]
                    * value[row * 3 + other];
            }
            if (std::abs(dot) > directionTolerance) return false;
        }
    }
    const double determinant =
        value[0] * (value[4] * value[8] - value[5] * value[7])
        - value[1] * (value[3] * value[8] - value[5] * value[6])
        + value[2] * (value[3] * value[7] - value[4] * value[6]);
    return std::abs(std::abs(determinant) - 1.0)
        <= directionTolerance;
}

} // namespace

bool GetImageMetadataValid(const ImageMetadata& value) noexcept
{
    if (!GetRequiredTextValid(
            value.identity.datasetId, imageDatasetIdByteLimit)
        || !GetOptionalTextValid(
            value.identity.inspectionId, imageMetadataTextByteLimit)
        || !GetOptionalTextValid(
            value.identity.objectId, imageMetadataTextByteLimit)
        || !GetOptionalTextValid(
            value.identity.batchId, imageMetadataTextByteLimit)
        || !GetRequiredTextValid(
            value.source.uri, imageMetadataTextByteLimit)
        || !GetOptionalTextValid(
            value.source.digest, imageMetadataTextByteLimit)
        || !GetRequiredTextValid(
            value.scalar.quantity, imageMetadataKeyByteLimit)
        || !GetRequiredTextValid(
            value.scalar.unit, imageMetadataKeyByteLimit)
        || !std::isfinite(value.scalar.slope)
        || value.scalar.slope == 0.0
        || !std::isfinite(value.scalar.intercept)
        || (value.scalar.noData
            && !std::isfinite(*value.scalar.noData))
        || value.attributes.size() > imageMetadataAttributeLimit) {
        return false;
    }

    switch (value.source.kind) {
    case ImageSourceKind::RawFile:
    case ImageSourceKind::TiffSeries:
    case ImageSourceKind::Memory:
        break;
    default:
        return false;
    }

    for (std::size_t index = 0; index < value.attributes.size(); ++index) {
        const auto& attribute = value.attributes[index];
        if (!GetRequiredTextValid(
                attribute.key, imageMetadataKeyByteLimit)) {
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (value.attributes[previous].key == attribute.key) {
                return false;
            }
        }
        if (const auto* number = std::get_if<double>(&attribute.value);
            number && !std::isfinite(*number)) {
            return false;
        }
        if (const auto* text = std::get_if<std::string>(&attribute.value);
            text && text->size() > imageMetadataTextByteLimit) {
            return false;
        }
    }
    return true;
}

std::optional<VolumeLayout> VolumeLayout::Create(
    std::array<int, 3> dimensions,
    std::array<float, 3> spacing,
    std::array<float, 3> origin,
    std::array<double, 9> direction,
    ImageMetadata metadata)
{
    if (!GetDirectionValid(direction)
        || !GetImageMetadataValid(metadata)) {
        return std::nullopt;
    }
    std::size_t voxelCount = 1;
    for (std::size_t index = 0; index < dimensions.size(); ++index) {
        if (dimensions[index] <= 0 || !std::isfinite(spacing[index])
            || spacing[index] <= 0.0f || !std::isfinite(origin[index])) {
            return std::nullopt;
        }
        const auto dimension = static_cast<std::size_t>(dimensions[index]);
        if (voxelCount > std::numeric_limits<std::size_t>::max() / dimension) {
            return std::nullopt;
        }
        voxelCount *= dimension;
    }
    if (voxelCount > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())
        || voxelCount > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        return std::nullopt;
    }
    const std::size_t byteCount = voxelCount * sizeof(float);
    return VolumeLayout(
        dimensions,
        spacing,
        origin,
        direction,
        std::move(metadata),
        voxelCount,
        byteCount);
}

VolumeLayout::VolumeLayout(
    std::array<int, 3> dimensions,
    std::array<float, 3> spacing,
    std::array<float, 3> origin,
    std::array<double, 9> direction,
    ImageMetadata metadata,
    std::size_t voxelCount,
    std::size_t byteCount) noexcept
    : m_dimensions(dimensions)
    , m_spacing(spacing)
    , m_origin(origin)
    , m_direction(direction)
    , m_metadata(std::move(metadata))
    , m_voxelCount(voxelCount)
    , m_byteCount(byteCount)
{
}

const std::array<int, 3>& VolumeLayout::GetDimensions() const noexcept { return m_dimensions; }
const std::array<float, 3>& VolumeLayout::GetSpacing() const noexcept { return m_spacing; }
const std::array<float, 3>& VolumeLayout::GetOrigin() const noexcept { return m_origin; }
const std::array<double, 9>& VolumeLayout::GetDirection() const noexcept { return m_direction; }
const ImageMetadata& VolumeLayout::GetMetadata() const noexcept { return m_metadata; }
std::size_t VolumeLayout::GetVoxelCount() const noexcept { return m_voxelCount; }
std::size_t VolumeLayout::GetByteCount() const noexcept { return m_byteCount; }

std::optional<VolumeBuffer> VolumeBuffer::Create(
    std::vector<float> voxels, VolumeLayout layout)
{
    if (voxels.size() != layout.GetVoxelCount()) return std::nullopt;
    return VolumeBuffer(std::move(voxels), std::move(layout));
}

VolumeBuffer::VolumeBuffer(
    std::vector<float> voxels, VolumeLayout layout) noexcept
    : m_voxels(std::move(voxels)), m_layout(std::move(layout)) {}

const std::vector<float>& VolumeBuffer::GetVoxels() const noexcept { return m_voxels; }
const VolumeLayout& VolumeBuffer::GetLayout() const noexcept { return m_layout; }
