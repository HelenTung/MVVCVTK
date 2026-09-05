#pragma once

#include "Algorithms/ClassicalPartSegmenter.h"
#include "Host/PartSegmentationHostTypes.h"

#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct PartSurfaceBuildRequest final {
    std::array<int, 6> extent{};
    std::array<int, 3> dimensions{};
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
    std::array<double, 3> origin{};
    std::array<double, 9> direction{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    std::shared_ptr<const std::vector<std::uint32_t>> labels;
    // 已验证目录中所有非背景部件的并集；提取时再扩展一体素背景边界。
    std::optional<std::array<int, 6>> foregroundExtent;
    std::uint32_t partCount = 0;
    std::size_t maxWorkingBytes = 0;
};

struct PartSurfaceProduct final {
    vtkSmartPointer<vtkPolyData> surface;
    std::size_t actualBytes = 0;
};

struct PartSurfaceBuildResult final {
    PartFailureReason failureReason = PartFailureReason::None;
    std::shared_ptr<const PartSurfaceProduct> product;
    std::size_t requiredBytes = 0;
    std::string message;
};

class PartSurfaceProductBuilder final {
public:
    static PartSurfaceBuildResult BuildProduct(
        const PartSurfaceBuildRequest& request,
        const std::function<bool()>& getStopRequested,
        const PartProgressCallback& sendProgress);
};
