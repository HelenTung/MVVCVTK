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
};

class SurfaceHistogramEstimator final
{
public:
    static constexpr std::size_t GetWorkingBytes() noexcept { return 64U * 1024U + fineBinCount*sizeof(std::uint64_t); }
    static SurfaceFailureReason BuildEstimate(const SurfaceHistogramInput& input,
        const std::function<bool()>& cancelled, std::optional<SurfaceIsoEstimate>& estimate,
        std::string& message);
private:
    static constexpr std::size_t fineBinCount = 65536;
};
