#pragma once

#include <vtkImageData.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

// 两种 CPU 产品共用的溢出检查；只累计本任务工作集，不管理资源或调度。
class RenderWorkBudget final {
public:
    bool Add(const std::uint64_t count, const std::uint64_t elementBytes)
    {
        const auto limit = (std::numeric_limits<std::uint64_t>::max)();
        if (!m_isValid || (count != 0 && elementBytes > limit / count)
            || count * elementBytes > limit - m_bytes) {
            m_isValid = false;
            return false;
        }
        m_bytes += count * elementBytes;
        return true;
    }

    bool AddGrid(vtkImageData* image, const std::array<int, 3>& dimensions)
    {
        if (!image) return true;
        const auto count = GetVoxelCount(dimensions);
        const int components = image->GetNumberOfScalarComponents();
        const int scalarBytes = image->GetScalarSize();
        if (!count || components <= 0 || scalarBytes <= 0) {
            m_isValid = false;
            return false;
        }
        return Add(*count, static_cast<std::uint64_t>(components) * scalarBytes);
    }

    std::optional<std::uint64_t> GetBytes() const
    {
        return m_isValid ? std::optional<std::uint64_t>{m_bytes} : std::nullopt;
    }

    static std::optional<std::uint64_t> GetVoxelCount(
        const std::array<int, 3>& dimensions)
    {
        std::uint64_t count = 1;
        for (const int dimension : dimensions) {
            if (dimension <= 0 || count >
                (std::numeric_limits<std::uint64_t>::max)() / dimension) return {};
            count *= dimension;
        }
        return count;
    }

private:
    std::uint64_t m_bytes = 0;
    bool m_isValid = true;
};
