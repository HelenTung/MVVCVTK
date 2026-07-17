#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

// float32 三维体数据的已验证几何描述；Create 同时计算并校验 voxel/byte 数，避免调用方重复乘法溢出。
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

    std::array<int, 3> m_dimensions{}; // X/Y/Z 体素数，均为正数。
    std::array<float, 3> m_spacing{};  // X/Y/Z 物理间距，有限且大于零。
    std::array<float, 3> m_origin{};   // 物理原点，必须为有限值。
    std::size_t m_voxelCount = 0;      // dimensions 安全乘积。
    std::size_t m_byteCount = 0;       // voxelCount * sizeof(float) 的安全乘积。
};

// 拥有 voxel 存储及其匹配布局的不可分割值对象；Create 拒绝元素数不一致的组合。
class VolumeBuffer final {
public:
    static std::optional<VolumeBuffer> Create(
        std::vector<float> voxels,
        VolumeLayout layout);

    const std::vector<float>& GetVoxels() const noexcept;
    const VolumeLayout& GetLayout() const noexcept;

private:
    VolumeBuffer(std::vector<float> voxels, VolumeLayout layout) noexcept;

    std::vector<float> m_voxels; // X 最快的连续单分量 float32 标量。
    VolumeLayout m_layout;       // 与 voxels 数量严格匹配的几何和字节计数。
};
