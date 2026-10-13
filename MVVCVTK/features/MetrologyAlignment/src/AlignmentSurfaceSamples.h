#pragma once
#include "AlignmentGeometry.h"

struct AlignmentSurfaceSample final {
    std::uint64_t vertex = 0;
    double area = 0;
    double validArea = 0;
    std::optional<AlignmentPoint> sourcePosition;
    std::optional<AlignmentPoint> sourceNormal;
};
struct AlignmentSurfaceSamples final {
    std::vector<AlignmentSurfaceSample> points;
    std::vector<std::int8_t> directions;
    std::size_t GetWorkingBytes() const noexcept {
        return points.capacity() * sizeof(AlignmentSurfaceSample) + directions.capacity();
    }
};
class AlignmentSurfaceSampling final {
  public:
    static AlignmentSurfaceSamples Build(const AlignmentWork &work, const AlignmentMatrix &initial,
                                          std::size_t indexBytes, bool needsDirections);
    static std::vector<AlignmentSurfaceSample> Reduce(const AlignmentWork &work,
        const AlignmentSurfaceSamples &source, double voxelSize, std::size_t indexBytes);
};
