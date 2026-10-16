// 固定三角核的整数滑动窗口；范围外补零，等高时仍由调用方保留最左桶。
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
class SurfaceHistogramWindow final {
public:
    // 此界内旧 double 逐项乘加的整数分子也精确，允许逐位保持原平滑结果。
    static bool IsExact(std::uint64_t population, std::size_t radius) noexcept {
        return population <= (std::uint64_t{1} << 53)/(radius+1);
    }
    SurfaceHistogramWindow(const std::vector<std::uint64_t>& counts, std::size_t radius, std::size_t index)
        : m_counts(counts), m_radius(radius), m_index(index) {
        const auto center=static_cast<std::int64_t>(index), r=static_cast<std::int64_t>(radius);
        for (auto j=center-r; j<=center+r; ++j) {
            const auto distance=j>center ? j-center : center-j;
            m_sum+=(radius+1-static_cast<std::size_t>(distance))*Get(j);
            if (j<=center) m_left+=Get(j);
            else m_right+=Get(j);
        }
        m_right+=Get(center+r+1);
    }
    double GetValue() const noexcept {
        const auto width=static_cast<double>(m_radius+1);
        return static_cast<double>(m_sum)/(width*width);
    }
    void Advance() noexcept {
        const auto i=static_cast<std::int64_t>(m_index), r=static_cast<std::int64_t>(m_radius);
        m_sum=m_sum-m_left+m_right;
        m_left=m_left-Get(i-r)+Get(i+1);
        m_right=m_right-Get(i+1)+Get(i+r+2);
        ++m_index;
    }
private:
    std::uint64_t Get(std::int64_t index) const noexcept {
        return index<0 || static_cast<std::size_t>(index)>=m_counts.size() ? 0 : m_counts[static_cast<std::size_t>(index)];
    }
    const std::vector<std::uint64_t>& m_counts;
    std::size_t m_radius, m_index;
    std::uint64_t m_sum=0, m_left=0, m_right=0;
};
