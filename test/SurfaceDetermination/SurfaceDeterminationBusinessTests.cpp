#include "SurfaceDeterminationTestCases.h"
#include "SurfaceDeterminationTestSupport.h"
#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceContracts.h"

#include <limits>
#include <map>

namespace
{
using namespace SurfaceTest;
constexpr std::size_t budget = 128U * 1024U * 1024U;
SurfaceAlgorithmResult Run(const VtkImageGridSnapshot &source, const SurfaceDeterminationStartParams &params,
                           const SurfaceAlgorithmInputs &inputs = {})
{
    return SurfaceDeterminationAlgorithm::BuildSurface(
        source, params, budget, [] { return false; }, {}, inputs);
}

void TestPureRecipe(Checks &c)
{
    const std::string legacy = "surface-parameters 1 \"RAS\" 67108864\n";
    SurfaceDeterminationStartParams requested, resolved;
    std::string frame; WorkLimit working;
    c.Get(!SurfaceContract::GetParameters(legacy, requested, resolved, frame, working),
          "retired parameter schema is not interpreted as a material recipe");
    SurfaceRecipe recipe; recipe.materialRange = std::array<double, 2>{-100, 900};
    recipe.initialIsoValue = 250;
    const auto text = SurfaceRecipeCodec::BuildText(recipe);
    const auto parsed = SurfaceRecipeCodec::GetRecipe(text);
    c.Get(parsed.recipe && SurfaceRecipeCodec::BuildText(*parsed.recipe) == text,
          "material calibration round trips without a legacy method mapping");
    c.Get(!SurfaceRecipeCodec::GetRecipe("surface-recipe 2\n").recipe,
          "retired recipe schema is rejected");
    c.Get(!SurfaceRecipeCodec::GetRecipe(text + "bad").recipe, "trailing recipe data is rejected");
    for (unsigned method=0; method<7; ++method) {
        recipe.method=static_cast<SurfaceDeterminationMethod>(method);
        c.Get(!SurfaceRecipeCodec::GetError(recipe).empty(), "retired method cannot select a new algorithm");
    }
}




void TestMaterialsAndReplay(Checks &c)
{
    const auto source = BuildPlane();
    auto params = GetParams(); params.targetViews = {};
    params.materialRange = std::array<double, 2>{0, 1000};
    const auto result = Run(source, params);
    c.Get(result.status == SurfaceResultStatus::Succeeded && result.acceptedPointCount > 0,
          "material surface uses scalar calibration");
    if (!result.points.empty()) {
        const auto diagnostic = SurfaceDeterminationAlgorithm::GetPointDiagnostic(
            source, result.resolvedParams, result.points[result.points.size()/2], {});
        c.Get(diagnostic.isAvailable &&
              diagnostic.point.positionModel == result.points[result.points.size()/2].positionModel,
              "material replay has no local fitting candidate");
    }

}


void TestBlocksRoiOverridesAndInitialMesh(Checks &c)
{
    const auto source = BuildSphere();
    auto params = GetParams();
    params.targetViews = {};
    params.seedBlockDepth = 1;
    const std::array<double, 6> roiBounds{9.25, 22.75, 9.1, 23.1, 9.2, 23.2};
    SurfaceAlgorithmInputs roiInputs;
    roiInputs.roi = BuildRoi(source, roiBounds);
    params.analysisRoi = roiInputs.roi->GetRevision();
    const auto first = Run(source, params, roiInputs);
    params.seedBlockDepth = 4096;
    const auto second = Run(source, params, roiInputs);
    bool same = first.status == SurfaceResultStatus::Succeeded && first.points.size() == second.points.size();
    if (same)
        for (std::size_t i = 0; i < first.points.size(); ++i)
            same = same && first.points[i].positionModel == second.points[i].positionModel &&
                   first.points[i].flags == second.points[i].flags;
    c.Get(same && first.triangleIndices == second.triangleIndices &&
              first.triangleValidity == second.triangleValidity &&
              first.execution.blockCount > second.execution.blockCount,
          "ROI geometry and global indices are identical across block boundaries");
    bool inside = true;
    bool keptBoundary = false;
    for (const auto &point : first.points)
        for (unsigned a = 0; a < 3; ++a)
            inside = inside && point.positionModel[a] >= roiBounds[a * 2] - 1e-10 &&
                     point.positionModel[a] <= roiBounds[a * 2 + 1] + 1e-10;
    c.Get(inside && !first.objects.empty() && !first.objects[0].isClosed,
          "final refined geometry stays inside ROI and adds no caps");
    for (const auto &point : first.points)
        if (GetSurfaceFlag(point.flags, SurfacePointFlags::RoiBoundary))
            keptBoundary = keptBoundary || (point.positionModel == point.seedPositionModel &&
                                            GetSurfaceFlag(point.flags, SurfacePointFlags::SeedRetained));
    c.Get(keptBoundary,
          "artificial ROI boundary retains its seed and explicit nonmeasurement quality reason");
    auto otherRoiInputs = roiInputs;
    otherRoiInputs.roi = BuildRoi(source, roiBounds);
    c.Get(Run(source, params, otherRoiInputs).failureReason == SurfaceFailureReason::InvalidRoi,
          "equal ROI geometry cannot substitute a different frozen revision identity");
    auto otherRoiParams = params;
    otherRoiParams.analysisRoi = otherRoiInputs.roi->GetRevision();
    const auto otherRoiResult = Run(source, otherRoiParams, otherRoiInputs);
    c.Get(otherRoiResult.status == SurfaceResultStatus::Succeeded
        && otherRoiResult.parameterFingerprint != first.parameterFingerprint,
        "ROI identity participates in the surface fingerprint even for equal geometry");
    const auto refused =
        SurfaceDeterminationAlgorithm::BuildSurface(source, GetParams(), 64 * 1024, [] { return false; }, {});
    c.Get(refused.failureReason == SurfaceFailureReason::BudgetExceeded && refused.points.empty(),
          "explicit mesh budget refuses allocation before publication");
}
void TestReviewedFailureBoundaries(Checks &c)
{
    const auto source = BuildSnapshot(
        {32, 32, 32}, {1, 1, 1}, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, VTK_DOUBLE,
        [](const Point3 &p) { return p == Point3{1, 1, 1} ? 1e30 : GetSmoothInside(p[0] - 10.35); });
    auto automatic = GetParams(SurfaceDeterminationMethod::MaterialIso);
    automatic.initialIsoValue.reset();
    const auto estimate = Run(source, automatic);
    c.Get(estimate.status == SurfaceResultStatus::Succeeded && estimate.isoEstimate &&
              estimate.isoEstimate->excludedSampleCount > 0 && std::abs(estimate.initialIsoValue - 500) < 50,
          "trimmed automatic range survives an extreme finite hot voxel and reports exclusion");
    const auto box = BuildSnapshot(
        {24, 24, 24}, {1, 1, 1}, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, VTK_FLOAT, [](const Point3 &p) {
            return GetSmoothInside(
                std::max({std::abs(p[0] - 11.5), std::abs(p[1] - 11.5), std::abs(p[2] - 11.5)}) - 5.2);
        });
    auto sharp = GetParams();
    sharp.sharpCornerAngleDeg = 30;
    const auto corners = Run(box, sharp);
    bool flagged = false, kept = true;
    for (const auto &point : corners.points)
        if (GetSurfaceFlag(point.flags, SurfacePointFlags::SharpCorner))
        {
            flagged = true;
            kept = kept && point.positionModel == point.seedPositionModel;
        }
    c.Get(flagged && kept, "sharp seed corners remain rejected seeds rather than accepted rounded geometry");
    std::atomic<bool> topology{false};
    const auto cancelled = SurfaceDeterminationAlgorithm::BuildSurface(
        BuildSphere(), GetParams(), budget, [&] { return topology.load(); },
        [&](auto stage, double) {
            if (stage == SurfaceDeterminationStage::TopologyValidation)
                topology = true;
        });
    c.Get(cancelled.status == SurfaceResultStatus::Cancelled && cancelled.points.empty() && topology,
          "topology cancellation discards every partial business result");
}

void TestMultipleInterfacesAndLocality(Checks &c)
{
    const auto large =
        BuildSnapshot({64, 64, 64}, {1, 1, 1}, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, VTK_FLOAT,
                      [](const Point3 &p) { return GetSmoothInside(p[0] - 31.35); });
    auto roi = GetParams();
    roi.targetViews = {};
    SurfaceAlgorithmInputs roiInputs;
    roiInputs.roi = BuildRoi(large, {30, 32, 30, 32, 30, 32});
    roi.analysisRoi = roiInputs.roi->GetRevision();
    const auto local = Run(large, roi, roiInputs);
    c.Get(local.status == SurfaceResultStatus::Succeeded &&
              local.execution.scannedCellCount < 63U * 63U * 63U / 4 &&
              local.execution.processedExtent[0] > 0 && local.execution.processedExtent[1] < 63,
          "small model ROI scans only its inverse-transformed halo region");
    const auto cylinder = BuildSnapshot(
        {32, 32, 32}, {1, 1, 1}, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, VTK_FLOAT,
        [](const Point3 &p) { return GetSmoothInside(std::hypot(p[0] - 15.5, p[1] - 15.5) - 8); });
    auto cylindrical = Run(cylinder, GetParams());
    double error = 0;
    std::size_t count = 0;
    for (const auto &point : cylindrical.points)
        if (point.flags == SurfacePointFlags::None)
        {
            error += std::abs(std::hypot(point.positionModel[0] - 15.5, point.positionModel[1] - 15.5) - 8);
            ++count;
        }
    c.Get(count > 0 && error / count < .1 && !cylindrical.objects.empty() &&
              !cylindrical.objects[0].isClosed && !cylindrical.objects[0].volumeModelUnit3,
          "open cylinder reaches subvoxel radial tolerance without a fabricated closed volume");
}

} // namespace

int GetSurfaceBusinessFailCount()
{
    Checks checks;
    TestMultipleInterfacesAndLocality(checks);
    TestReviewedFailureBoundaries(checks);
    TestPureRecipe(checks);
    TestMaterialsAndReplay(checks);
    TestBlocksRoiOverridesAndInitialMesh(checks);
    return checks.failureCount;
}
