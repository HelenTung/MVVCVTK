#pragma once
#include "ArtifactReductionAlgorithm.h"

namespace ArtifactReduction {
ArtifactError BuildDiffusion(const GridGeometry3D& grid,
    const ArtifactDiffusionParams& params, std::vector<float>& values,
    TaskControl& control);
}
