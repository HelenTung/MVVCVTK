#pragma once

#include <array>
#include <memory>
#include <utility>

// DefX 输入快照的最小只读载体；只保存连续 float 别名、生命周期 owner 和几何元数据。
// 本类型不提供 mask、采样、插值或任何本地分析入口，避免旧算法语义重新进入生产链。
class GapInputBuffer final {
public:
    bool SetVoxels(
        std::shared_ptr<const void> owner,
        const float* voxelData) noexcept
    {
        if (!owner || !voxelData) {
            return false;
        }
        m_owner = std::move(owner);
        m_voxelData = voxelData;
        return true;
    }

    bool GetVoxelReady() const noexcept
    {
        return m_owner && m_voxelData;
    }

    const float* GetVoxelData() const noexcept
    {
        return m_voxelData;
    }

    // voxel 数量与输入 physical 几何均按 [x, y, z] 保存。
    std::array<int, 3> dims{ 0, 0, 0 };
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
    std::array<double, 3> origin{ 0.0, 0.0, 0.0 };
    // 输入标量闭区间，仅用于校验和解析 DataRangeRatio ISO。
    float minVal = 0.0f;
    float maxVal = 0.0f;

private:
    std::shared_ptr<const void> m_owner;
    const float* m_voxelData = nullptr;
};
