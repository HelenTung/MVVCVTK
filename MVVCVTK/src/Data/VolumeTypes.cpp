#include "Data/VolumeTypes.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

std::optional<VolumeLayout> VolumeLayout::Create(
    std::array<int, 3> dimensions,
    std::array<float, 3> spacing,
    std::array<float, 3> origin)
{
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
        dimensions, spacing, origin, voxelCount, byteCount);
}

VolumeLayout::VolumeLayout(
    std::array<int, 3> dimensions,
    std::array<float, 3> spacing,
    std::array<float, 3> origin,
    std::size_t voxelCount,
    std::size_t byteCount) noexcept
    : m_dimensions(dimensions), m_spacing(spacing), m_origin(origin),
      m_voxelCount(voxelCount), m_byteCount(byteCount) {}

const std::array<int, 3>& VolumeLayout::GetDimensions() const noexcept { return m_dimensions; }
const std::array<float, 3>& VolumeLayout::GetSpacing() const noexcept { return m_spacing; }
const std::array<float, 3>& VolumeLayout::GetOrigin() const noexcept { return m_origin; }
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
