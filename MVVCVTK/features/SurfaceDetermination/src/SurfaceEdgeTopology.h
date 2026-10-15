#pragma once
#include "Data/DataPayloads.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

struct SurfaceEdgeTopology final {
    std::uint64_t boundaryEdgeCount = 0;
    std::uint64_t nonManifoldEdgeCount = 0;
    bool isOrientationValid = true;
};

// 调用方先验证两个局部编号均小于 2^31；最低位保留原 from<to 方向规则。
inline std::uint64_t GetSurfaceDirectedEdgeKey(std::uint32_t from, std::uint32_t to) noexcept
{
    return (std::uint64_t(std::min(from, to)) << 32U) |
           (std::uint64_t(std::max(from, to)) << 1U) | (from < to ? 1U : 0U);
}

inline SurfaceEdgeTopology GetSortedSurfaceEdgeTopology(DataArray<std::uint64_t> &keys,
                                                         const std::function<void()> &checkpoint)
{
    std::size_t work = 0;
    const auto check = [&] { if ((work++ & 1023U) == 0 && checkpoint) checkpoint(); };
    std::sort(keys.begin(), keys.end(), [&](auto left, auto right) {
        check();
        return left < right;
    });
    SurfaceEdgeTopology result;
    for (std::size_t begin = 0; begin < keys.size();) {
        check();
        const auto key = keys[begin] & ~std::uint64_t(1);
        auto end = begin + 1;
        while (end < keys.size() && (keys[end] & ~std::uint64_t(1)) == key) { check(); ++end; }
        const auto count = end - begin;
        if (count == 1) ++result.boundaryEdgeCount;
        if (count > 2) { ++result.nonManifoldEdgeCount; result.isOrientationValid = false; }
        // 两条边的方向相反才一致；self-edge 始终是负向，与原判据相同。
        if (count == 2 && (keys[begin] & 1U) == (keys[begin + 1] & 1U))
            result.isOrientationValid = false;
        begin = end;
    }
    return result;
}
