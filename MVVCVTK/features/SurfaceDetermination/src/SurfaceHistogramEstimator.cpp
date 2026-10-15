#include "SurfaceHistogramEstimator.h"
#include "Host/SurfaceDeterminationHostTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace {
constexpr std::size_t histogramBinCount = 512;
constexpr double geometryEpsilon = 1.0e-12;
std::size_t GetIndex(const SurfaceHistogramInput& input, const std::int64_t x,
    const std::int64_t y, const std::int64_t z)
{
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(input.dimensions[0])
        * (static_cast<std::size_t>(y) + static_cast<std::size_t>(input.dimensions[1]) * static_cast<std::size_t>(z));
}
}

SurfaceFailureReason SurfaceHistogramEstimator::BuildEstimate(const SurfaceHistogramInput& input,
    const WorkLimit budget, const std::function<bool()>& cancelled,
    std::optional<SurfaceIsoEstimate>& estimate, std::string& message)
{
    estimate.reset();
    message.clear();
    if (!input.values || !input.readScalar) return SurfaceFailureReason::InvalidSource;
    std::size_t count = 1;
    for (const auto dimension : input.dimensions) {
        if (dimension < 2 || count > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(dimension))
            return SurfaceFailureReason::InvalidGeometry;
        count *= static_cast<std::size_t>(dimension);
    }
    if (budget < GetWorkingBytes()) {
        message = "Surface histogram workspace exceeds the budget.";
        return SurfaceFailureReason::BudgetExceeded;
    }
    double isoValue = 0;
    double minimum = std::numeric_limits<double>::max(), maximum = std::numeric_limits<double>::lowest();
    std::uint64_t validCount = 0, excludedCount = 0;
    const std::array<std::int64_t, 6> extent{0, input.dimensions[0]-1, 0, input.dimensions[1]-1, 0, input.dimensions[2]-1};
    std::array<std::int64_t, 3> sampleSteps{};
    for (unsigned a = 0; a < 3; ++a)
    {
        sampleSteps[a] = (static_cast<std::int64_t>(input.dimensions[a]) + 127) / 128;
    }
    // 范围和直方图使用同一采样集合。独立的 16^3 预采样会避开薄壁材料，
    // 随后把真正的材料峰作为越界值丢弃，并把背景附近的次峰误认作材料。
    // 只保留每侧 17 个极值，以固定空间排除最多 16 个孤立极端样本。
    constexpr std::size_t tailCapacity = 17;
    std::array<double, tailCapacity> lowest{}, highest{};
    lowest.fill(std::numeric_limits<double>::max());
    highest.fill(std::numeric_limits<double>::lowest());
    for (std::int64_t z = extent[4] + sampleSteps[2] / 2; z <= extent[5]; z += sampleSteps[2])
        for (std::int64_t y = extent[2] + sampleSteps[1] / 2; y <= extent[3]; y += sampleSteps[1])
        {
            if ((cancelled && cancelled()))
                return SurfaceFailureReason::Cancelled;
            for (std::int64_t x = extent[0] + sampleSteps[0] / 2; x <= extent[1]; x += sampleSteps[0])
            {
                const auto id = GetIndex(input, x, y, z);
                const auto value = input.readScalar(input.values, id);
                if ((!input.validity || input.validity[id] != 0) &&
                    std::isfinite(value)) {
                    ++validCount;
                    if (value < lowest.back()) {
                        const auto at = std::lower_bound(lowest.begin(), lowest.end(), value);
                        std::move_backward(at, lowest.end()-1, lowest.end()); *at = value;
                    }
                    if (value > highest.back()) {
                        const auto at = std::lower_bound(highest.begin(), highest.end(), value, std::greater<double>{});
                        std::move_backward(at, highest.end()-1, highest.end()); *at = value;
                    }
                }
            }
        }
    if (validCount >= 64)
    {
        const auto trim = std::min<std::uint64_t>(tailCapacity-1, validCount / 1000);
        minimum = lowest[trim];
        maximum = highest[trim];
    }
    if (validCount < 64 || !std::isfinite(minimum)
        || !std::isfinite(maximum) || !std::isfinite(maximum - minimum)
        || maximum - minimum <= geometryEpsilon
            * std::max({ 1.0, std::abs(minimum), std::abs(maximum) })) {
        message = "Surface material range estimation requires a non-degenerate bimodal histogram.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }

    validCount = 0;
    std::array<std::uint64_t, histogramBinCount> histogram{};
    const double scale = static_cast<double>(histogramBinCount - 1)
        / (maximum - minimum);
    for (std::int64_t zValue = extent[4] + sampleSteps[2] / 2;
        zValue <= static_cast<std::int64_t>(extent[5]); zValue += sampleSteps[2]) {
        const int z = static_cast<int>(zValue);
        if ((cancelled && cancelled())) {
            message = "Surface threshold estimation was cancelled.";
            return SurfaceFailureReason::Cancelled;
        }
        for (std::int64_t yValue = extent[2] + sampleSteps[1] / 2;
            yValue <= static_cast<std::int64_t>(extent[3]); yValue += sampleSteps[1]) {
            const int y = static_cast<int>(yValue);
            for (std::int64_t xValue = extent[0] + sampleSteps[0] / 2;
                xValue <= static_cast<std::int64_t>(extent[1]); xValue += sampleSteps[0]) {
                const int x = static_cast<int>(xValue);
                const std::size_t tupleIndex = GetIndex(input, x, y, z);
                if (input.validity && input.validity[tupleIndex] == 0) {
                    continue;
                }
                const double value = input.readScalar(input.values, tupleIndex);
                if (!std::isfinite(value) || value < minimum || value > maximum)
                {
                    ++excludedCount;
                    continue;
                }
                ++validCount;
                const auto bin = static_cast<std::size_t>(std::clamp(
                    std::llround((value - minimum) * scale),
                    0LL,
                    static_cast<long long>(histogramBinCount - 1)));
                ++histogram[bin];
            }
        }
    }

    std::array<double, histogramBinCount> smooth{};
    for (std::size_t index = 0; index < histogramBinCount; ++index) {
        double sum = 0.0;
        double weight = 0.0;
        const std::size_t begin = index > 2 ? index - 2 : 0;
        const std::size_t end = std::min(
            histogramBinCount - 1, index + 2);
        for (std::size_t sample = begin; sample <= end; ++sample) {
            const double localWeight = sample == index ? 3.0
                : (sample + 1 == index || sample == index + 1 ? 2.0 : 1.0);
            sum += localWeight * static_cast<double>(histogram[sample]);
            weight += localWeight;
        }
        smooth[index] = sum / weight;
    }

    struct Peak final {
        std::size_t index = 0;
        double height = 0.0;
    };
    std::vector<Peak> peaks;
    peaks.reserve(histogramBinCount);
    for (std::size_t index = 0; index < histogramBinCount; ++index) {
        const double left = index == 0 ? -1.0 : smooth[index - 1];
        const double right = index + 1 == histogramBinCount
            ? -1.0 : smooth[index + 1];
        if (smooth[index] >= left && smooth[index] >= right
            && smooth[index] > 0.0) {
            peaks.push_back({ index, smooth[index] });
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& left, const Peak& right) {
        if (left.height != right.height) return left.height > right.height;
        return left.index < right.index;
    });

    std::optional<std::pair<Peak, Peak>> selected;
    const std::size_t minimumSeparation = histogramBinCount / 10;
    const double minimumPeakHeight = std::max(
        2.0, static_cast<double>(validCount) * 1.0e-5);
    if (!peaks.empty()) {
        // 按谷分群；近背景起伏和同一材料上的小尖峰合并，
        // 非空气材料按原始直方图积分样本数比较，而不是按峰高或离背景的距离比较。
        std::vector<Peak> groups;
        for (const auto& peak : peaks) {
            if (peak.height < minimumPeakHeight) continue;
            const bool separated = std::all_of(groups.begin(), groups.end(), [&](const Peak& group) {
                const auto low = std::min(peak.index, group.index), high = std::max(peak.index, group.index);
                if (high-low < minimumSeparation) return false;
                const auto valley = *std::min_element(smooth.begin()+low, smooth.begin()+high+1);
                return valley <= 0.7 * std::min(peak.height, group.height);
            });
            if (separated) groups.push_back(peak);
        }
        std::sort(groups.begin(), groups.end(), [](const Peak& a, const Peak& b) { return a.index < b.index; });
        std::vector<std::uint64_t> counts;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const auto left = i == 0 ? std::ptrdiff_t{0}
                : std::min_element(smooth.begin()+groups[i-1].index, smooth.begin()+groups[i].index+1)-smooth.begin();
            const auto right = i+1 < groups.size()
                ? std::min_element(smooth.begin()+groups[i].index, smooth.begin()+groups[i+1].index+1)-smooth.begin()
                : static_cast<std::ptrdiff_t>(histogramBinCount);
            counts.push_back(std::accumulate(histogram.begin()+left, histogram.begin()+right, std::uint64_t{0}));
        }
        if (!counts.empty()) {
            // 背景取有显著峰高与样本支持的最低灰度群；最高峰可能属于材料。
            // 低值小伪影不能顶替空气。范围需包含足够背景，才能识别这对灰度群。
            const auto maximumCount = *std::max_element(counts.begin(), counts.end());
            std::size_t background = 0;
            while (background < groups.size()
                && (groups[background].height < 0.01*peaks.front().height || counts[background] < 0.05*maximumCount)) ++background;
            std::uint64_t largestCount = 0;
            for (std::size_t i = background+1; i < groups.size(); ++i) {
                if (counts[i] > largestCount) { largestCount = counts[i]; selected = std::make_pair(groups[background], groups[i]); }
            }
        }
    }
    if (!selected) {
        message = "Surface material range estimation could not identify two reliable peaks.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }

    const double binWidth = (maximum - minimum)
        / static_cast<double>(histogramBinCount - 1);
    const double background = minimum
        + static_cast<double>(selected->first.index) * binWidth;
    const double material = minimum
        + static_cast<double>(selected->second.index) * binWidth;
    isoValue = 0.5 * (background + material);
    if (!std::isfinite(isoValue) || material <= background) {
        message = "Surface material range estimation produced an invalid threshold.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }
    estimate = SurfaceIsoEstimate{
        isoValue,      background,
        material,      validCount,
        excludedCount, double(selected->second.index - selected->first.index) / (histogramBinCount - 1)};
    estimate->histogramCounts=histogram;
    estimate->histogramEdges.front()=minimum;
    estimate->histogramEdges.back()=maximum;
    for(std::size_t i=1;i<histogramBinCount;++i) estimate->histogramEdges[i]=minimum+(static_cast<double>(i)-0.5)*binWidth;
    for(std::size_t axis=0;axis<3;++axis) estimate->histogramSampleSteps[axis]=static_cast<int>(sampleSteps[axis]);
    return SurfaceFailureReason::None;
}

