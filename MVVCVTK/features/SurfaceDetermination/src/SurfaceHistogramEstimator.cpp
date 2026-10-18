#include "SurfaceHistogramEstimator.h"
#include "SurfaceCpuSampling.h"
#include "Host/SurfaceDeterminationHostTypes.h"
#include "ImagePeakEstimator.h"

#include <functional>
#include <utility>

SurfaceFailureReason SurfaceHistogramEstimator::BuildEstimate(const SurfaceHistogramInput& input,
    const std::function<bool()>& cancelled,
    std::optional<SurfaceIsoEstimate>& estimate, std::string& message,
    SurfaceHistogramStats* statistics)
{
    estimate.reset();
    message.clear();

    ImagePeakStats peakStats;
    peakStats.workingBytes = GetWorkingBytes();
    struct StatsGuard final {
        SurfaceHistogramStats* output;
        const ImagePeakStats& value;
        ~StatsGuard()
        {
            if (output) {
                *output = {value.rangeMs, value.countMs, value.peakMs,
                    value.workers, value.workingBytes};
            }
        }
    } statsGuard{statistics, peakStats};

    const ImagePeakInput peakInput{
        input.dimensions, input.values, input.readScalar, input.validity};
    std::function<unsigned()> getThreadLimit;
    ImagePeakDispatch dispatch;
    if (input.allowParallel) {
        // 与原实现一样，在输入/取消检查和范围探测后才查询 CPU 规划。
        getThreadLimit = [] { return SurfaceCpu::GetCpuSamplingPlan().threadLimit; };
        dispatch = [](const std::size_t pointCount, const unsigned limit,
            const std::function<bool()>& stopRequested,
            const std::function<void(std::size_t, std::size_t)>& sample) {
            try {
                return SurfaceCpu::RunPointSampling(pointCount, limit, stopRequested, sample);
            } catch (const SurfaceCpu::SurfaceCancelled&) {
                return 0U;
            }
        };
    }

    std::optional<ImagePeakEstimate> peakEstimate;
    const auto failure = ImagePeakEstimator::BuildEstimate(
        peakInput, cancelled, peakEstimate, message, &peakStats, 1, std::move(dispatch), getThreadLimit);
    if (peakEstimate) {
        SurfaceIsoEstimate surfaceEstimate{
            peakEstimate->isoValue,
            peakEstimate->backgroundValue,
            peakEstimate->materialValue,
            peakEstimate->sampleCount,
            peakEstimate->excludedSampleCount,
            peakEstimate->separationRatio};
        surfaceEstimate.histogramCounts = peakEstimate->histogramCounts;
        surfaceEstimate.histogramEdges = peakEstimate->histogramEdges;
        surfaceEstimate.histogramSampleSteps = peakEstimate->histogramSampleSteps;
        estimate = std::move(surfaceEstimate);
    }

    switch (failure) {
    case ImagePeakFailure::None: return SurfaceFailureReason::None;
    case ImagePeakFailure::InvalidSource: return SurfaceFailureReason::InvalidSource;
    case ImagePeakFailure::InvalidGeometry: return SurfaceFailureReason::InvalidGeometry;
    case ImagePeakFailure::ThresholdUnreliable: return SurfaceFailureReason::ThresholdUnreliable;
    case ImagePeakFailure::Cancelled: return SurfaceFailureReason::Cancelled;
    }
    return SurfaceFailureReason::InternalError;
}
