#pragma once
#include "ArtifactReductionAlgorithm.h"

namespace ArtifactReduction {
ArtifactError BuildQuality(const AlgorithmInput& input,
    const std::vector<float>& output, TaskControl& control,
    ArtifactQuality& quality, std::array<double, 2>& scalarRange);
std::shared_ptr<const RecordTablePayload> CreateQualityReport(
    const ArtifactQuality& quality);
}
