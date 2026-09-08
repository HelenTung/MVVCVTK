#pragma once
#include "SurfaceRecipe.h"

struct SurfaceLocalParams
{
    SurfaceDeterminationMethod method = SurfaceDeterminationMethod::LocalAdaptiveIso50;
    double initialIsoValue = 0.0;
    double profileHalfLengthModel = 0.0;
    double profileSampleStepModel = 0.0;
    double maximumOffsetModel = 0.0;
    double profileSmoothingSigmaModel = 0.0;
    double localFraction = 0.5;
    double minimumContrast = 0.0;
    double minimumCnr = 0.0;
    double maximumPlateauNoiseRatio = 1.0;
    double maximumNormalizedResidual = 0.25;
    double minimumEdgeWidthModel = 0.0;
    double maximumEdgeWidthModel = 0.0;
    double minimumEdgeSeparationModel = 0.0;
    double maximumNormalTurnDeg = 75.0;
    double expectedDerivativeSign = 0.0;
    std::optional<SurfaceGrayPair> grayPair;
    std::optional<SurfaceMaterialPair> materials;
};

struct SurfaceProfileWorkspace final
{
    std::vector<double> offsets, raw, values, derivatives, plateauA, plateauB, scratch, weights;
    std::vector<SurfaceSampleStatus> support;
    std::vector<std::uint32_t> labels;
    std::vector<SurfaceEdgeCandidate> candidates;
    double step = 0.0;
    double validRatio = 0.0;
    std::uint64_t sampledProfileCount = 0;
    std::uint64_t sampledValueCount = 0;
    void Reserve(std::size_t count);
};

struct SurfaceProfileFit final
{
    SurfacePointFlags flags = SurfacePointFlags::None;
    double offset = 0.0;
    double threshold = 0.0;
    double sideA = 0.0;
    double sideB = 0.0;
    double contrast = 0.0;
    double noise = 0.0;
    double gradient = 0.0;
    double residual = 0.0;
    double normalizedResidual = 0.0;
    double width = 0.0;
    double separation = 0.0;
    std::uint32_t crossingCount = 0;
};

class SurfaceProfileSolver final
{
  public:
    static SurfaceProfileFit BuildFit(SurfaceProfileWorkspace &profile, const SurfaceLocalParams &params);
};
