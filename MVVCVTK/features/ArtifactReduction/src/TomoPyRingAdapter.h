#pragma once
#include "ArtifactReductionAlgorithm.h"
#include "remove_ring.h"

namespace ArtifactReduction {
struct RingLayout final {
    std::array<int, 2> planeAxes{};
    std::array<float, 2> center{};
    MvvcvtkTomoPyLayout native{};
};
ArtifactError GetRingLayout(const GridGeometry3D& grid,
    const ArtifactRingParams& params, RingLayout& layout) noexcept;
ArtifactError BuildRingCorrection(const GridGeometry3D& grid,
    const ArtifactRingParams& params, std::vector<float>& values,
    TaskControl& control, ArtifactQuality& quality);
}
