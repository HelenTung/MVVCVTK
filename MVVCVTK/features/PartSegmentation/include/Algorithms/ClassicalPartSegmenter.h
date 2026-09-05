#pragma once

#include "Host/PartSegmentationHostTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

enum class PartScalarType : std::uint8_t {
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64
};

struct PartScalarView final {
    const void* data = nullptr;
    std::size_t valueCount = 0;
    PartScalarType scalarType = PartScalarType::Float32;
};

struct PartVolumeView final {
    std::array<int, 6> extent{};
    std::array<int, 3> dimensions{};
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
    std::array<double, 3> origin{};
    std::array<double, 9> direction{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    PartScalarView values;
    std::optional<PartScalarView> validity;
};

struct PartAlgorithmMetrics final {
    std::size_t labelBytes = 0;
    std::size_t frontierPeakBytes = 0;
    std::size_t filteredPeakBytes = 0;
    std::size_t catalogBytes = 0;
    std::size_t peakWorkingBytes = 0;
};

using PartProgressCallback = std::function<void(double)>;

struct PartAlgorithmParams final {
    double threshold = 0.5;
    std::uint64_t minPartVoxels = 1;
    std::uint32_t maxPartCount =
        std::numeric_limits<std::uint32_t>::max() - 2U;
    std::size_t maxWorkingBytes = 512U * 1024U * 1024U;
};

enum class PartAlgorithmError : std::uint8_t {
    None,
    InvalidInput,
    BudgetExceeded,
    Cancelled,
    LabelOverflow,
    InternalError
};

struct PartAlgorithmResult final {
    PartAlgorithmError error = PartAlgorithmError::InvalidInput;
    std::size_t requiredBytes = 0;
    PartAlgorithmMetrics metrics;
    std::vector<PartLabelId> labels;
    std::vector<PartMetrics> metricsByLabel;
};

class ClassicalPartSegmenter final {
public:
    static PartAlgorithmResult BuildLabels(
        const PartVolumeView& volume,
        const PartAlgorithmParams& params,
        const std::function<bool()>& getStopRequested = nullptr,
        const PartProgressCallback& sendProgress = nullptr);
};
