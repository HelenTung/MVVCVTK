#include "SurfaceHistogramEstimator.h"
#include "SurfaceHistogramWindow.h"
#include "SurfaceCpuSampling.h"
#include "Host/SurfaceDeterminationHostTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace {
constexpr std::size_t histogramBinCount = 512;
constexpr double geometryEpsilon = 1.0e-12;
// 冻结的统一计数起伏规则；它用于识别浅谷，不是空间独立性或物理置信度声明。
constexpr double peakCountMargin = 5.0;
template<class Counts> double GetSmoothCount(const Counts& counts, const std::size_t index,
    const std::size_t radius=2, const bool isEdgeNormalized=true)
{
    double sum = 0, weight = 0;
    const auto begin = index > radius ? index-radius : 0;
    const auto end = std::min(counts.size()-1,index+radius);
    for (auto i=begin; i<=end; ++i) {
        const auto distance=i>index ? i-index : index-i;
        const auto w=static_cast<double>(radius+1-distance);
        sum += w*static_cast<double>(counts[i]);
        weight += w;
    }
    // 细桶用固定核质量，范围外为零；不能在首尾重新放大截断核，使端点尖峰压过邻近总体。
    const auto fullWeight=static_cast<double>(radius+1)*static_cast<double>(radius+1);
    return sum/(isEdgeNormalized ? weight : fullWeight);
}
std::size_t GetIndex(const SurfaceHistogramInput& input, const std::int64_t x,
    const std::int64_t y, const std::int64_t z)
{
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(input.dimensions[0])
        * (static_cast<std::size_t>(y) + static_cast<std::size_t>(input.dimensions[1]) * static_cast<std::size_t>(z));
}
}

SurfaceFailureReason SurfaceHistogramEstimator::BuildEstimate(const SurfaceHistogramInput& input,
    const std::function<bool()>& cancelled,
    std::optional<SurfaceIsoEstimate>& estimate, std::string& message, SurfaceHistogramStats* statistics)
{
    const auto started=std::chrono::steady_clock::now();
    SurfaceHistogramStats stats;
    stats.workingBytes=GetWorkingBytes();
    struct StatsGuard final {
        SurfaceHistogramStats& value; SurfaceHistogramStats* output;
        ~StatsGuard() { if (output) *output=value; }
    } statsGuard{stats,statistics};
    estimate.reset();
    message.clear();
    if (!input.values || !input.readScalar) return SurfaceFailureReason::InvalidSource;
    std::size_t count = 1;
    for (const auto dimension : input.dimensions) {
        if (dimension < 2 || count > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(dimension))
            return SurfaceFailureReason::InvalidGeometry;
        count *= static_cast<std::size_t>(dimension);
    }
    if (cancelled && cancelled()) return SurfaceFailureReason::Cancelled;
    double isoValue = 0;
    double minimum = std::numeric_limits<double>::max(), maximum = std::numeric_limits<double>::lowest();
    std::uint64_t validCount = 0, excludedCount = 0;
    const std::array<std::int64_t, 6> extent{0, input.dimensions[0]-1, 0, input.dimensions[1]-1, 0, input.dimensions[2]-1};
    std::array<std::int64_t, 3> sampleSteps{};
    for (unsigned a = 0; a < 3; ++a)
    {
        sampleSteps[a] = (static_cast<std::int64_t>(input.dimensions[a]) + 127) / 128;
    }
    // 固定格点先界定健壮灰度范围；不使用更稀疏的独立16^3预采样，避免漏掉薄材料。
    // 随后的全有效域计数明确排除越界值，粗细桶共用同一个范围和体素集合。
    // 定范围时每侧只保留17个采样极值，最多跳过其中16个；全卷越界体素另计排除数。
    constexpr std::size_t tailCapacity = 17;
    std::array<double, tailCapacity> lowest{}, highest{};
    lowest.fill(std::numeric_limits<double>::max());
    highest.fill(std::numeric_limits<double>::lowest());
    for (std::int64_t z = extent[4] + sampleSteps[2] / 2; z <= extent[5]; z += sampleSteps[2])
        for (std::int64_t y = extent[2] + sampleSteps[1] / 2; y <= extent[3]; y += sampleSteps[1])
        {
            if (cancelled && cancelled())
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

    // 同一原始有效集合同时进入粗细桶；越出健壮范围和非有限值计入排除数。
    validCount = 0;
    std::array<std::uint64_t, histogramBinCount> histogram{};
    std::vector<std::uint64_t> fineHistogram(fineBinCount);
    const double scale = static_cast<double>(histogramBinCount-1)/(maximum-minimum);
    const auto fineWidth = (maximum-minimum)/static_cast<double>(fineBinCount-1);
    const auto fineScale = static_cast<double>(fineBinCount-1)/(maximum-minimum);
    const auto rangeFinished=std::chrono::steady_clock::now();
    stats.rangeMs=std::chrono::duration<double,std::milli>(rangeFinished-started).count();
    const auto limit=input.allowParallel ? static_cast<unsigned>(std::min<std::size_t>(
        SurfaceCpu::GetCpuSamplingPlan().threadLimit,std::max<std::size_t>(1,count/8192))) : 1U;
    std::atomic<bool> wasCancelled{false};
    std::mutex mergeMutex;
    const auto scan=[&](std::size_t begin, std::size_t end, auto& coarseCounts, auto& fineCounts,
                        std::uint64_t& localValid, std::uint64_t& localExcluded) {
        for (std::size_t index=begin; index<end; ++index) {
            if ((index & 65535U)==0 && (wasCancelled.load(std::memory_order_relaxed) || (cancelled && cancelled()))) {
                wasCancelled.store(true,std::memory_order_relaxed);
                return;
            }
            if (input.validity && input.validity[index]==0) continue;
            const auto value = input.readScalar(input.values,index);
            if (!std::isfinite(value) || value<minimum || value>maximum) {
                ++localExcluded;
                continue;
            }
            ++localValid;
            const auto coarse = static_cast<std::size_t>(std::clamp(std::llround((value-minimum)*scale),
                0LL,static_cast<long long>(histogramBinCount-1)));
            const auto fine = static_cast<std::size_t>(std::clamp(std::llround((value-minimum)*fineScale),
                0LL,static_cast<long long>(fineBinCount-1)));
            ++coarseCounts[coarse];
            ++fineCounts[fine];
        }
    };
    const auto serial=[&] {
        histogram.fill(0); std::fill(fineHistogram.begin(),fineHistogram.end(),0);
        validCount=excludedCount=0;
        scan(0,count,histogram,fineHistogram,validCount,excludedCount);
        stats.workers=1;
    };
    if (limit==1) serial();
    else {
        // 预先记录私有桶的规划峰值；失败/取消仍交回工作区估算，不将其当硬上限。
        stats.workingBytes=GetWorkingBytes(1+limit);
        try {
            stats.workers=SurfaceCpu::RunPointSampling(count,limit,cancelled,[&](std::size_t begin,std::size_t end) {
                std::array<std::uint64_t,histogramBinCount> localCoarse{};
                std::vector<std::uint64_t> localFine(fineBinCount);
                std::uint64_t localValid=0,localExcluded=0;
                scan(begin,end,localCoarse,localFine,localValid,localExcluded);
                std::lock_guard<std::mutex> lock(mergeMutex);
                validCount+=localValid; excludedCount+=localExcluded;
                for (std::size_t i=0;i<histogramBinCount;++i) histogram[i]+=localCoarse[i];
                for (std::size_t i=0;i<fineBinCount;++i) fineHistogram[i]+=localFine[i];
            });
        } catch (const std::bad_alloc&) {
            // 只回退加速工作区，所有已启动 worker 已由共享调度器 join；完整重算，不降采样。
            serial();
        } catch (const SurfaceCpu::SurfaceCancelled&) {
            wasCancelled.store(true,std::memory_order_relaxed);
        }
    }
    if (wasCancelled.load(std::memory_order_relaxed)) {
        message="Surface histogram estimation was cancelled.";
        return SurfaceFailureReason::Cancelled;
    }
    const auto countFinished=std::chrono::steady_clock::now();
    stats.countMs=std::chrono::duration<double,std::milli>(countFinished-rangeFinished).count();

    std::array<double, histogramBinCount> smooth{};
    for (std::size_t index = 0; index < histogramBinCount; ++index)
        smooth[index] = GetSmoothCount(histogram,index);

    struct Peak final {
        std::size_t index = 0;
        double height = 0.0;
        std::size_t left = 0, right = histogramBinCount;
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
                const auto lowerPeak = std::min(peak.height, group.height);
                // 保留深谷及薄材料原规则；计数充足的浅谷也能与局部计数起伏区分。
                return valley <= 0.7 * lowerPeak
                    || lowerPeak-valley >= peakCountMargin*std::sqrt(lowerPeak+valley);
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
            groups[i].left = static_cast<std::size_t>(left);
            groups[i].right = static_cast<std::size_t>(right);
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
    // 同一集合的512桶用于分群及诊断，细桶只在已选峰的局部范围细化峰位。
    // 不使用最终表面或参考答案重建灰度分布。
    // 粗核[1,2,3,2,1]/9的方差为4/3个粗桶平方。连续三角核方差为r²/6，
    // 因此用sqrt(8)个粗桶确定细核半径；不能把整数灰度量化的小起伏当成更精细的峰。
    const auto fineRadius=static_cast<std::size_t>(std::ceil(std::sqrt(8.0)*
        static_cast<double>(fineBinCount-1)/static_cast<double>(histogramBinCount-1)));
    bool refinementCancelled=false;
    const auto getPeakValue = [&](const Peak& peak) -> std::optional<double> {
        if (refinementCancelled) return std::nullopt;
        const auto valleyLeft = peak.left==0 ? minimum : minimum+(static_cast<double>(peak.left)-0.5)*binWidth;
        const auto valleyRight = peak.right==histogramBinCount ? maximum : minimum+(static_cast<double>(peak.right)-0.5)*binWidth;
        // 细化不越过粗峰的半个分离间距，避免把已合并的背景次峰重新选成薄材料。
        const auto halfDistance = 0.5*static_cast<double>(minimumSeparation);
        const auto left = std::max(valleyLeft,minimum+(static_cast<double>(peak.index)-halfDistance)*binWidth);
        const auto right = std::min(valleyRight,minimum+(static_cast<double>(peak.index)+halfDistance)*binWidth);
        const auto first = static_cast<std::size_t>(std::clamp(std::ceil((left-minimum)/fineWidth),
            0.0,static_cast<double>(fineBinCount-1)));
        const auto last = static_cast<std::size_t>(std::clamp(std::floor((right-minimum)/fineWidth),
            0.0,static_cast<double>(fineBinCount-1)));
        double height = 0;
        auto selectedIndex = first;
        std::optional<SurfaceHistogramWindow> window;
        if (SurfaceHistogramWindow::IsExact(validCount,fineRadius)) window.emplace(fineHistogram,fineRadius,first);
        for (auto i=first; i<=last; ++i) {
            if ((i&63U)==0 && cancelled && cancelled()) {refinementCancelled=true;return std::nullopt;}
            const auto value = window ? window->GetValue() : GetSmoothCount(fineHistogram,i,fineRadius,false);
            if (value>height) { height=value; selectedIndex=i; }
            if (window && i<last) window->Advance();
        }
        return height>0 ? std::optional<double>{minimum+static_cast<double>(selectedIndex)*fineWidth} : std::nullopt;
    };
    const auto backgroundPeak = getPeakValue(selected->first), materialPeak = getPeakValue(selected->second);
    if (refinementCancelled) {
        message="Surface peak refinement was cancelled.";
        return SurfaceFailureReason::Cancelled;
    }
    if (!backgroundPeak || !materialPeak) {
        message = "Selected material groups have no finite full-volume histogram support.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }
    const double background = *backgroundPeak, material = *materialPeak;
    isoValue = 0.5 * background + 0.5 * material;
    if (!std::isfinite(isoValue) || material <= background) {
        message = "Surface material range estimation produced an invalid threshold.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }
    estimate = SurfaceIsoEstimate{
        isoValue,      background,
        material,      validCount,
        excludedCount, (material-background)/(maximum-minimum)};
    estimate->histogramCounts=histogram;
    estimate->histogramEdges.front()=minimum;
    estimate->histogramEdges.back()=maximum;
    for(std::size_t i=1;i<histogramBinCount;++i) estimate->histogramEdges[i]=minimum+(static_cast<double>(i)-0.5)*binWidth;
    estimate->histogramSampleSteps = {1,1,1};
    stats.peakMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-countFinished).count();
    return SurfaceFailureReason::None;
}
