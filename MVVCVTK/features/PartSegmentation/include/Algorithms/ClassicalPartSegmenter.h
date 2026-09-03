#pragma once

#include "Host/PartSegmentationHostTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

struct PartVolumeData final {
    std::array<int, 6> extent{};
    std::array<int, 3> dimensions{};
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
    std::array<double, 3> origin{};
    std::array<double, 9> direction{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    std::vector<double> values;
    std::vector<std::uint8_t> validity;
};

struct PartAlgorithmParams final {
    double threshold = 0.5;
    std::uint64_t minPartVoxels = 1;
    std::uint32_t maxPartCount =
        std::numeric_limits<std::uint32_t>::max() - 1U;
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
    std::vector<std::uint32_t> labels;
    std::vector<PartRecord> parts;
};

class ClassicalPartSegmenter final {
public:
    static PartAlgorithmResult BuildLabels(
        const PartVolumeData& volume,
        const PartAlgorithmParams& params,
        const std::function<bool()>& getStopRequested = nullptr);
};
