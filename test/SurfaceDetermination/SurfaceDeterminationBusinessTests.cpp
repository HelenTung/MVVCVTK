#include "SurfaceDeterminationTestCases.h"
#include "SurfaceDeterminationTestSupport.h"
#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceProfileSolver.h"
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

SurfaceProfileWorkspace Profile(const std::function<double(double)> &field)
{
    SurfaceProfileWorkspace p;
    p.step = 0.05;
    for (int i = -100; i <= 100; ++i)
    {
        p.offsets.push_back(i * p.step);
        p.raw.push_back(field(i * p.step));
        p.support.push_back(SurfaceSampleStatus::Valid);
    }
    p.validRatio = 1;
    return p;
}
SurfaceLocalParams Local(SurfaceDeterminationMethod method)
{
    SurfaceLocalParams p;
    p.method = method;
    p.profileHalfLengthModel = 5;
    p.profileSampleStepModel = .05;
    p.maximumOffsetModel = 2;
    p.profileSmoothingSigmaModel = 0;
    p.minimumEdgeWidthModel = .05;
    p.maximumEdgeWidthModel = 2;
    p.minimumEdgeSeparationModel = .2;
    return p;
}

void TestPureRecipe(Checks &c)
{
    const std::string legacy = "surface-parameters 1 \"RAS\" 67108864\n"
                               "1 0 2 0 \"\" \"mm\" 1 500 0 0 0 0 0 0 1 50\n"
                               "1 0 2 0 \"\" \"mm\" 1 500 0 0 0 0 0 0 1 50\n";
    SurfaceDeterminationStartParams oldRequested, oldResolved;
    std::string frame;
    std::size_t working = 0;
    c.Get(SurfaceContract::GetParameters(legacy, oldRequested, oldResolved, frame, working) &&
              oldResolved.localFraction == .5 && oldResolved.materialPairs.empty(),
          "v1 recipe decoding explicitly supplies the old ISO50 semantics");

    SurfaceRecipe recipe;
    recipe.method = SurfaceDeterminationMethod::LocalRelativeIso;
    recipe.localFraction = .43;
    recipe.seedFraction = .61;
    recipe.maximumOffsetModel = 0;
    recipe.profileSmoothingSigmaModel = 0;
    recipe.grayPair = SurfaceGrayPair{{900, 1100}, {-100, 100}};
    SurfaceRegionOverride rule;
    rule.id = "edge/rule";
    rule.priority = 2;
    rule.boundsModel = {1, 2, 3, 4, 5, 6};
    rule.minimumCnr = 4;
    recipe.regionOverrides.push_back(rule);
    const auto text = SurfaceRecipeCodec::BuildText(recipe);
    const auto parsed = SurfaceRecipeCodec::GetRecipe(text);
    c.Get(parsed.recipe && SurfaceRecipeCodec::BuildText(*parsed.recipe) == text,
          "pure recipe round trip retains fractions, zero scales, gray direction and rules");
    c.Get(!SurfaceRecipeCodec::GetRecipe("surface-recipe 99\n").recipe,
          "unsupported recipe version cannot silently restore");
    c.Get(!SurfaceRecipeCodec::GetRecipe(text + "bad").recipe, "pure recipe rejects trailing content");
    auto invalid = recipe;
    invalid.regionOverrides.push_back(rule);
    invalid.regionOverrides.back().id = "other";
    c.Get(!SurfaceRecipeCodec::GetError(invalid).empty(), "equal-priority overlapping regions are ambiguous");
    invalid.regionOverrides.back().priority = 3;
    c.Get(SurfaceRecipeCodec::GetError(invalid).empty(), "overlap with explicit priority is deterministic");
    invalid = recipe;
    invalid.method = SurfaceDeterminationMethod::LocalAdaptiveIso50;
    c.Get(!SurfaceRecipeCodec::GetError(invalid).empty(), "ISO50 cannot silently become relative ISO43");
    invalid = recipe;
    invalid.componentSelection = SurfaceComponentSelection::All;
    invalid.materialPairs = {{2, 7}, {7, 2}};
    c.Get(!SurfaceRecipeCodec::GetError(invalid).empty(),
          "reverse pair duplicates cannot produce duplicate interfaces");
}

void TestProfileModels(Checks &c)
{
    const auto step = [](double x) { return 0.5 * (1 + std::tanh((x - .37) / .42)); };
    auto profile = Profile([&](double x) { return 10 + 2 * x + 100 * step(x); });
    auto params = Local(SurfaceDeterminationMethod::EdgeModelFit);
    const auto fit = SurfaceProfileSolver::BuildFit(profile, params);
    c.Get(fit.flags == SurfacePointFlags::None && std::abs(fit.offset - .37) < .01 &&
              std::abs(fit.width - .42) < .02 && fit.normalizedResidual < .005,
          "single-edge model recovers position and width with a sloping background");
    auto pairProfile = Profile(
        [](double x) { return 20 + x + 80 * .5 * (std::tanh((x + .8) / .25) - std::tanh((x - 1.1) / .25)); });
    params = Local(SurfaceDeterminationMethod::PairedEdgeModelFit);
    auto paired = SurfaceProfileSolver::BuildFit(pairProfile, params);
    c.Get(paired.flags == SurfacePointFlags::None && std::abs(paired.offset + .8) < .02 &&
              std::abs(paired.separation - 1.9) < .03 && std::abs(paired.width - .25) < .03,
          "paired model jointly recovers two transitions and their separation");
    params.minimumEdgeSeparationModel = 2.5;
    paired = SurfaceProfileSolver::BuildFit(pairProfile, params);
    c.Get(GetSurfaceFlag(paired.flags, SurfacePointFlags::Unresolved),
          "paired spacing below resolution threshold is rejected");
    auto relative = Profile([&](double x) { return 100 * step(x); });
    params = Local(SurfaceDeterminationMethod::LocalRelativeIso);
    params.localFraction = .43;
    auto fraction = SurfaceProfileSolver::BuildFit(relative, params);
    const double expected = .37 + .42 * std::atanh(2 * .43 - 1);
    c.Get(fraction.flags == SurfacePointFlags::None && std::abs(fraction.offset - expected) < .002,
          "relative ISO43 changes the actual crossing");
    relative.offsets.pop_back();
    c.Get(GetSurfaceFlag(SurfaceProfileSolver::BuildFit(relative, params).flags,
                         SurfacePointFlags::FitRejected),
          "malformed profile lengths are rejected before indexing");
}

DataSnapshot Labels(const VtkImageGridSnapshot &source, const bool junction = false)
{
    const auto *image = dynamic_cast<const ImageGrid3DPayload *>(source->data->payload.get());
    const auto geometry = image->GetGeometry();
    auto values = std::make_shared<std::vector<std::uint16_t>>();
    for (int z = geometry.extent[4]; z <= geometry.extent[5]; ++z)
        for (int y = geometry.extent[2]; y <= geometry.extent[3]; ++y)
            for (int x = geometry.extent[0]; x <= geometry.extent[1]; ++x)
                values->push_back(junction && y == 12 ? 9 : x <= 15 ? 2 : 7);
    return std::make_shared<const DataRevision>(
        DataRevision{GetTestDataRef(91),
                     DataTypes::labelMap3D,
                     {{"source-volume", source->data->self}},
                     std::make_shared<const LabelMap3DPayload>(geometry, LabelMapValues{values}),
                     {}});
}
void TestMaterialsAndReplay(Checks &c)
{
    const auto source = BuildPlane();
    SurfaceAlgorithmInputs inputs;
    inputs.materialLabels = Labels(source);
    auto params = GetParams();
    params.targetViews = {};
    params.materialLabels = inputs.materialLabels->self;
    params.materialPairs = {{2, 7}};
    params.componentSelection = SurfaceComponentSelection::All;
    params.initialIsoValue.reset();
    const auto forward = Run(source, params, inputs);
    c.Get(forward.status == SurfaceResultStatus::Succeeded && forward.acceptedPointCount > 0 &&
              forward.interfaces.size() == 1 && forward.interfaces[0].canonicalId == "2:7",
          "generic uint16 labels create a named material interface without global histogram");
    params.materialPairs = {{7, 2}};
    const auto reverse = Run(source, params, inputs);
    bool same = forward.points.size() == reverse.points.size() && !forward.points.empty();
    if (same)
        for (std::size_t i = 0; i < forward.points.size(); ++i)
        {
            for (unsigned a = 0; a < 3; ++a)
                same = same && std::abs(forward.points[i].positionModel[a] -
                                        reverse.points[i].positionModel[a]) < 1e-8;
            same = same && forward.points[i].normalModel[0] * reverse.points[i].normalModel[0] < -.99;
        }
    c.Get(same && reverse.interfaces[0].canonicalId == "2:7",
          "reverse material direction shares canonical geometry and reverses normals");
    if (!forward.points.empty())
    {
        const auto selected =
            std::find_if(forward.points.begin(), forward.points.end(),
                         [](const auto &point) { return point.flags == SurfacePointFlags::None; });
        if (selected != forward.points.end())
        {
            const auto diagnostic = SurfaceDeterminationAlgorithm::GetProfileDiagnostic(
                source, forward.resolvedParams, *selected, inputs);
            c.Get(diagnostic.isAvailable && diagnostic.rawValues.size() == diagnostic.offsetsModel.size() &&
                      diagnostic.rawValues.size() == diagnostic.materialLabels.size() &&
                      diagnostic.sideA > diagnostic.sideB &&
                      diagnostic.point.positionModel == selected->positionModel,
                  "diagnostic replays the actual frozen labeled profile and final point");
        }
    }
    inputs.materialLabels = Labels(source, true);
    params.materialPairs = {{2, 7}};
    const auto junction = Run(source, params, inputs);
    bool gap = !junction.points.empty();
    for (const auto &point : junction.points)
        gap = gap && (point.seedPositionModel[1] <= 11 || point.seedPositionModel[1] >= 13);
    c.Get(gap && junction.execution.skippedCellCount > 0,
          "third material cells leave a gap instead of inventing an A-B interface");
    auto bad = std::make_shared<DataRevision>(*inputs.materialLabels);
    bad->inputs.clear();
    inputs.materialLabels = bad;
    c.Get(Run(source, params, inputs).failureReason == SurfaceFailureReason::InvalidSource,
          "labels require exact source lineage");
}

void TestBlocksRoiOverridesAndInitialMesh(Checks &c)
{
    const auto source = BuildSphere();
    auto params = GetParams();
    params.targetViews = {};
    params.seedBlockDepth = 1;
    params.roiModelBounds = std::array<double, 6>{9.25, 22.75, 9.1, 23.1, 9.2, 23.2};
    const auto first = Run(source, params);
    params.seedBlockDepth = 4096;
    const auto second = Run(source, params);
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
            inside = inside && point.positionModel[a] >= (*params.roiModelBounds)[a * 2] - 1e-10 &&
                     point.positionModel[a] <= (*params.roiModelBounds)[a * 2 + 1] + 1e-10;
    c.Get(inside && !first.objects.empty() && !first.objects[0].isClosed,
          "final refined geometry stays inside ROI and adds no caps");
    for (const auto &point : first.points)
        if (GetSurfaceFlag(point.flags, SurfacePointFlags::RoiBoundary))
            keptBoundary = keptBoundary || (point.positionModel == point.seedPositionModel &&
                                            GetSurfaceFlag(point.flags, SurfacePointFlags::SeedRetained));
    c.Get(keptBoundary,
          "artificial ROI boundary retains its seed and explicit nonmeasurement quality reason");
    params = GetParams();
    params.targetViews = {};
    params.method = SurfaceDeterminationMethod::LocalRelativeIso;
    SurfaceRegionOverride rule;
    rule.id = "upper";
    rule.priority = 1;
    rule.boundsModel = {0, 32, 12, 24, 0, 20};
    rule.localFraction = .43;
    params.regionOverrides = {rule};
    const auto plane = BuildPlane();
    const auto overridden = Run(plane, params);
    bool lower = false, upper = false, seam = false;
    for (const auto &point : overridden.points)
    {
        if (point.flags == SurfacePointFlags::None)
        {
            lower = lower || point.overrideIndex == 0;
            upper = upper || (point.overrideIndex == 1 && point.localThreshold > 550);
        }
        seam = seam || GetSurfaceFlag(point.flags, SurfacePointFlags::OverrideBoundary);
    }
    c.Get(lower && upper && seam,
          "region override changes localization and marks shared boundary quality lower=" +
              std::to_string(lower) + " upper=" + std::to_string(upper) + " seam=" + std::to_string(seam) +
              " message=" + overridden.message);
    auto seedParams = GetParams(SurfaceDeterminationMethod::GlobalIsoPreview);
    seedParams.targetViews = {};
    const auto seed = Run(plane, seedParams);
    std::vector<double> vertices;
    for (const auto &point : seed.points)
        vertices.insert(vertices.end(), point.positionModel.begin(), point.positionModel.end());
    std::vector<std::uint64_t> indices(seed.triangleIndices.begin(), seed.triangleIndices.end());
    SurfaceAlgorithmInputs inputs;
    inputs.initialSurface = std::make_shared<const DataRevision>(
        DataRevision{GetTestDataRef(92),
                     DataTypes::surfaceMesh,
                     {{"source-volume", plane->data->self}},
                     std::make_shared<const SurfaceMeshPayload>(vertices, indices),
                     {}});
    params = GetParams();
    params.targetViews = {};
    params.initialSurface = inputs.initialSurface->self;
    params.initialIsoValue.reset();
    const auto imported = Run(plane, params, inputs);
    c.Get(imported.status == SurfaceResultStatus::Succeeded && imported.acceptedPointCount > 0 &&
              imported.execution.scannedCellCount == 0 && imported.points.size() == seed.points.size(),
          "generic initial mesh supports business refinement without iso extraction");
    params.maximumOffsetModel = 0;
    params.profileSmoothingSigmaModel = 0;
    const auto stationary = Run(plane, params, inputs);
    bool retained = true;
    for (const auto &point : stationary.points)
        retained = retained && point.positionModel == point.seedPositionModel;
    c.Get(stationary.status == SurfaceResultStatus::Succeeded && retained,
          "zero offset and zero smoothing are accepted and preserve all seeds");
    const auto refused =
        SurfaceDeterminationAlgorithm::BuildSurface(plane, GetParams(), 64 * 1024, [] { return false; }, {});
    c.Get(refused.failureReason == SurfaceFailureReason::BudgetExceeded && refused.points.empty(),
          "budget covers parallel workspaces before mesh publication");
}
void TestReviewedFailureBoundaries(Checks &c)
{
    auto profile = Profile(
        [](double x) { return 20 + 80 * .5 * (std::tanh((x + 1.1) / .2) - std::tanh((x - .3) / .2)); });
    for (const auto offset : profile.offsets)
        profile.labels.push_back(offset < -1.1 || offset > .3 ? 2 : 7);
    auto params = Local(SurfaceDeterminationMethod::PairedEdgeModelFit);
    params.materials = SurfaceMaterialPair{2, 7};
    const auto reversed = SurfaceProfileSolver::BuildFit(profile, params);
    c.Get(GetSurfaceFlag(reversed.flags, SurfacePointFlags::DirectionMismatch),
          "paired fit rejects the nearest edge when it is B-to-A");
    auto noisy = Profile([](double x) {
        return 20 + 80 * .5 * (std::tanh((x + .8) / .25) - std::tanh((x - 1.1) / .25)) + 2 * std::sin(13 * x);
    });
    params = Local(SurfaceDeterminationMethod::PairedEdgeModelFit);
    params.maximumPlateauNoiseRatio = .001;
    const auto unstable = SurfaceProfileSolver::BuildFit(noisy, params);
    c.Get(GetSurfaceFlag(unstable.flags, SurfacePointFlags::PlateauUnstable),
          "paired fit applies the declared plateau noise limit after estimating amplitude");
    const auto source = BuildSnapshot(
        {32, 32, 32}, {1, 1, 1}, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, VTK_DOUBLE,
        [](const Point3 &p) { return p == Point3{1, 1, 1} ? 1e30 : GetSmoothInside(p[0] - 10.35); });
    auto automatic = GetParams(SurfaceDeterminationMethod::AutomaticIso50);
    automatic.initialIsoValue.reset();
    const auto estimate = Run(source, automatic);
    c.Get(estimate.status == SurfaceResultStatus::Succeeded && estimate.isoEstimate &&
              estimate.isoEstimate->excludedSampleCount > 0 && std::abs(estimate.initialIsoValue - 500) < 50,
          "trimmed automatic range survives an extreme finite hot voxel and reports exclusion");
    const auto triple = BuildSnapshot({32, 32, 32}, {1, 1, 1}, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1},
                                      VTK_FLOAT, [](const Point3 &p) {
                                          return p[0] < 10 ? 0.0 : p[0] < 21 ? 500.0 : 1000.0;
                                      });
    auto seed = GetParams();
    seed.initialIsoValue.reset();
    c.Get(Run(triple, seed).failureReason == SurfaceFailureReason::ThresholdUnreliable,
          "automatic seed refuses three significant material peaks");
    const auto plane = BuildPlane();
    auto gray = GetParams();
    gray.method = SurfaceDeterminationMethod::LocalRelativeIso;
    gray.localFraction = .43;
    gray.initialIsoValue.reset();
    gray.grayPair = SurfaceGrayPair{{-100, 100}, {900, 1100}};
    const auto backward = Run(plane, gray);
    bool directed = backward.acceptedPointCount > 0;
    for (const auto &point : backward.points)
        if (point.flags == SurfacePointFlags::None)
            directed = directed && point.normalModel[0] < -.99 && point.localThreshold < 450;
    c.Get(directed, "explicit low-to-high gray direction determines the ISO43 interpretation");
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
            kept = kept && point.positionModel == point.seedPositionModel &&
                   GetSurfaceFlag(point.flags, SurfacePointFlags::SeedRetained);
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
    const auto source = BuildSnapshot(
        {32, 24, 20}, {1, 1, 1}, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, VTK_FLOAT, [](const Point3 &p) {
            return GetSmoothInside(p[0] - 10.35, 0, 500) + GetSmoothInside(p[0] - 20.35, 0, 500);
        });
    const auto geometry =
        dynamic_cast<const ImageGrid3DPayload *>(source->data->payload.get())->GetGeometry();
    auto values = std::make_shared<std::vector<std::uint16_t>>();
    for (int z = 0; z < 20; ++z)
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 32; ++x)
                values->push_back(x <= 10 ? 2 : x <= 20 ? 7 : 9);
    SurfaceAlgorithmInputs inputs;
    inputs.materialLabels = std::make_shared<const DataRevision>(
        DataRevision{GetTestDataRef(94),
                     DataTypes::labelMap3D,
                     {{"source-volume", source->data->self}},
                     std::make_shared<const LabelMap3DPayload>(geometry, LabelMapValues{values}),
                     {}});
    auto params = GetParams();
    params.targetViews = {};
    params.initialIsoValue.reset();
    params.materialLabels = inputs.materialLabels->self;
    params.materialPairs = {{2, 7}, {7, 9}};
    params.componentSelection = SurfaceComponentSelection::All;
    const auto result = Run(source, params, inputs);
    bool correct = result.interfaces.size() == 2 && result.objects.size() == 2;
    if (correct)
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto &face = result.interfaces[i];
            correct = correct && face.pointCount > 0 && face.triangleCount > 0;
            for (std::size_t j = face.firstPoint; j < face.firstPoint + face.pointCount; ++j)
            {
                const auto &point = result.points[j];
                correct = correct && point.interfaceIndex == i && point.normalModel[0] > .99;
                if (point.flags == SurfacePointFlags::None)
                    correct = correct && std::abs(point.positionModel[0] - (i ? 20.35 : 10.35)) < .05;
            }
        }
    c.Get(correct && result.execution.sampledProfileCount > 0 &&
              result.execution.sampledValueCount > result.execution.sampledProfileCount,
          "one request publishes two distinct material interfaces with original-gray refinement and sample "
          "counts");
    const auto large =
        BuildSnapshot({64, 64, 64}, {1, 1, 1}, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, VTK_FLOAT,
                      [](const Point3 &p) { return GetSmoothInside(p[0] - 31.35); });
    auto roi = GetParams();
    roi.targetViews = {};
    roi.roiModelBounds = std::array<double, 6>{30, 32, 30, 32, 30, 32};
    const auto local = Run(large, roi);
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
    TestProfileModels(checks);
    TestMaterialsAndReplay(checks);
    TestBlocksRoiOverridesAndInitialMesh(checks);
    return checks.failureCount;
}
