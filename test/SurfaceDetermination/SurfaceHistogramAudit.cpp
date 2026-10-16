#include "SurfaceDeterminationTestCases.h"
#include "SurfaceHistogramEstimator.h"
#include "Host/SurfaceDeterminationHostTypes.h"
#include "Platform/MemMappedFile.h"

#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
template<class T> double ReadValue(const void* values, const std::size_t index)
{
    return static_cast<double>(static_cast<const T*>(values)[index]);
}
}

int GetSurfaceHistogramAuditResult(const int argc, char* argv[])
{
    const bool samples = argc == 7 && std::string_view(argv[1]) == "--histogram-samples";
    const bool raw = argc == 8 && std::string_view(argv[1]) == "--histogram-raw";
    if (!samples && !raw) return 2;
    SurfaceHistogramInput input;
    const std::string_view type = samples ? "float32" : argv[3];
    std::size_t scalarBytes = 0;
    if (type == "float32") { input.readScalar = ReadValue<float>; scalarBytes = sizeof(float); }
    else if (type == "uint16") { input.readScalar = ReadValue<std::uint16_t>; scalarBytes = sizeof(std::uint16_t); }
    else return 2;
    std::size_t count = 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto* text = argv[(samples ? 3 : 4) + axis];
        const auto* end = text + std::strlen(text);
        const auto parsed = std::from_chars(text, end, input.dimensions[axis]);
        const auto dimension = input.dimensions[axis];
        if (parsed.ec != std::errc{} || parsed.ptr != end || dimension < 2 || (samples && dimension > 128)
            || count > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(dimension)) return 2;
        count *= static_cast<std::size_t>(dimension);
    }
    if (count > std::numeric_limits<std::size_t>::max() / scalarBytes) return 2;
    // 映射是只读 RAII owner；同步估计结束前不释放，不创建整卷测试副本。
    MemMappedFile mapping;
    if (!mapping.Load(argv[2]) || mapping.GetSize() != count * scalarBytes) return 2;
    input.values = mapping.GetData();
    input.allowParallel = true;
    SurfaceHistogramStats stats;
    std::optional<SurfaceIsoEstimate> estimate;
    std::string message;
    const auto started = std::chrono::steady_clock::now();
    const auto failure = SurfaceHistogramEstimator::BuildEstimate(input, [] { return false; }, estimate, message, &stats);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-started).count();
    if (failure != SurfaceFailureReason::None || !estimate) {
        std::cerr << "threshold-rejected reason=" << static_cast<unsigned>(failure) << " " << message << '\n';
        return 1;
    }
    std::ofstream output(argv[samples ? 6 : 7]);
    output << std::setprecision(17) << "bin,lower,upper,count\n";
    for (std::size_t i = 0; i < estimate->histogramCounts.size(); ++i)
        output << i << ',' << estimate->histogramEdges[i] << ',' << estimate->histogramEdges[i+1]
               << ',' << estimate->histogramCounts[i] << '\n';
    if (!output) return 2;
    std::cout << std::setprecision(17) << "iso=" << estimate->isoValue << " background=" << estimate->backgroundValue
              << " material=" << estimate->materialValue << " samples=" << estimate->sampleCount
              << " excluded=" << estimate->excludedSampleCount << " stepX=" << estimate->histogramSampleSteps[0]
              << " stepY=" << estimate->histogramSampleSteps[1] << " stepZ=" << estimate->histogramSampleSteps[2]
              << " workingBytes=" << stats.workingBytes << " elapsedMs=" << elapsed
              << " rangeMs=" << stats.rangeMs << " countMs=" << stats.countMs << " peakMs=" << stats.peakMs
              << " workers=" << stats.workers << '\n';
    return 0;
}
