#pragma once
#include "ThicknessAlgorithm.h"

namespace ThicknessAlgorithm {
struct SamplingCorner final {
    std::size_t vertex = 0;
    ThicknessPoint barycentric{};
};
struct SamplingTriangle final {
    std::size_t source = 0;
    std::array<SamplingCorner, 3> corners{};
};
// 先裁剪评价范围，再按局部边长细分；共享边使用同一交点/中点身份。
// 失败抛出 ThicknessStatus，不修改原始网格和 Gauss 积分来源。
std::vector<SamplingTriangle> BuildSampling(const Work& work, std::size_t limit);
}
