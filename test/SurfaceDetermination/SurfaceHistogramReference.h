#pragma once

#include "SurfaceHistogramEstimator.h"

// 冻结 c722897 实现，仅作为提取前的数值回归参考。
class SurfaceHistogramReference final
{
public:
    static constexpr std::size_t GetWorkingBytes(unsigned histogramCount = 1) noexcept
    {
        return 64U * 1024U + fineBinCount * sizeof(std::uint64_t)
            + (histogramCount - 1) * (fineBinCount + 512) * sizeof(std::uint64_t);
    }

    static SurfaceFailureReason BuildEstimate(const SurfaceHistogramInput& input,
        const std::function<bool()>& cancelled, std::optional<SurfaceIsoEstimate>& estimate,
        std::string& message, SurfaceHistogramStats* statistics = nullptr);

private:
    static constexpr std::size_t fineBinCount = 65536;
};
