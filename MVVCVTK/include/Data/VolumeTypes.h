#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

class VolumeLayout final {
public:
    static std::optional<VolumeLayout> Create(
        std::array<int, 3> dimensions,
        std::array<float, 3> spacing,
        std::array<float, 3> origin);

    const std::array<int, 3>& GetDimensions() const noexcept;
    const std::array<float, 3>& GetSpacing() const noexcept;
    const std::array<float, 3>& GetOrigin() const noexcept;
    std::size_t GetVoxelCount() const noexcept;
    std::size_t GetByteCount() const noexcept;

private:
    VolumeLayout(
        std::array<int, 3> dimensions,
        std::array<float, 3> spacing,
        std::array<float, 3> origin,
        std::size_t voxelCount,
        std::size_t byteCount) noexcept;

    std::array<int, 3> m_dimensions{};
    std::array<float, 3> m_spacing{};
    std::array<float, 3> m_origin{};
    std::size_t m_voxelCount = 0;
    std::size_t m_byteCount = 0;
};

class VolumeBuffer final {
public:
    static std::optional<VolumeBuffer> Create(
        std::vector<float> voxels,
        VolumeLayout layout);

    const std::vector<float>& GetVoxels() const noexcept;
    const VolumeLayout& GetLayout() const noexcept;

private:
    VolumeBuffer(std::vector<float> voxels, VolumeLayout layout) noexcept;

    std::vector<float> m_voxels;
    VolumeLayout m_layout;
};
