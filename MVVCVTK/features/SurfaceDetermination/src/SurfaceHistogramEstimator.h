#pragma once

#include "SurfaceRecipe.h"

#include <functional>

enum class SurfaceFailureReason : std::uint8_t;

// 借用一次同步计算内不变的原始标量与可选有效域；不持有 Host/VTK identity。
struct SurfaceHistogramInput final
{
    std::array<int, 3> dimensions{};
    const void* values = nullptr;
    double (*readScalar)(const void*, std::size_t) = nullptr;
    const unsigned char* validity = nullptr;
    // 仅当读取函数及取消检查可并发调用时启用；自定义有状态读取默认串行。
    bool allowParallel = false;
};

struct SurfaceHistogramStats final
{
    double rangeMs = 0, countMs = 0, peakMs = 0;
    unsigned workers = 1;
    std::size_t workingBytes = 0;
};

class SurfaceHistogramEstimator final
{
public:
    // 一个归并输出，加上并行时每个区间的私有桶；不包含输入映射和线程栈。
    static constexpr std::size_t GetWorkingBytes(unsigned histogramCount = 1) noexcept { return 64U * 1024U + fineBinCount*sizeof(std::uint64_t)
        + (histogramCount-1)*(fineBinCount+512)*sizeof(std::uint64_t); }
    static SurfaceFailureReason BuildEstimate(const SurfaceHistogramInput& input,
        const std::function<bool()>& cancelled, std::optional<SurfaceIsoEstimate>& estimate,
        std::string& message, SurfaceHistogramStats* statistics = nullptr);
private:
    static constexpr std::size_t fineBinCount = 65536;
};
