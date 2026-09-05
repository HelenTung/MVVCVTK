#include "SurfaceDeterminationTestCases.h"

#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceDeterminationTestSupport.h"

#include <vtkType.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace {

using namespace SurfaceTest;

SurfaceAlgorithmResult Build(
    const VtkImageGridSnapshot& source,
    const SurfaceDeterminationStartParams& params,
    const std::size_t budget = 64U * 1024U * 1024U)
{
    return SurfaceDeterminationAlgorithm::BuildSurface(
        source, params, budget, [] { return false; }, {});
}

vtkSmartPointer<vtkImageData> BuildMaskLike(
    vtkImageData& source,
    const int scalarType)
{
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->SetExtent(source.GetExtent());
    mask->SetSpacing(source.GetSpacing());
    mask->SetOrigin(source.GetOrigin());
    mask->SetDirectionMatrix(source.GetDirectionMatrix());
    mask->AllocateScalars(scalarType, 1);
    auto* scalars = mask->GetPointData()->GetScalars();
    scalars->FillComponent(0, 1.0);
    return mask;
}

double GetPlaneMeanError(
    const SurfaceAlgorithmResult& result,
    const double boundary)
{
    double error = 0.0;
    std::size_t count = 0;
    for (const SurfacePointRecord& point : result.points) {
        if (!GetPointAccepted(point)) continue;
        error += std::abs(point.positionModel[0] - boundary);
        ++count;
    }
    return count == 0
        ? std::numeric_limits<double>::infinity()
        : error / static_cast<double>(count);
}

void TestExplicitPlaneAndScalarTypes(Checks& checks)
{
    constexpr double boundary = 15.35;
    const std::array<int, 4> scalarTypes{
        VTK_UNSIGNED_CHAR, VTK_UNSIGNED_SHORT, VTK_SHORT, VTK_FLOAT
    };
    for (const int scalarType : scalarTypes) {
        auto params = GetParams();
        params.initialIsoValue = scalarType == VTK_UNSIGNED_CHAR
            ? 127.5 : 500.0;
        params.minimumContrast = scalarType == VTK_UNSIGNED_CHAR
            ? 20.0 : 50.0;
        const auto result = Build(BuildPlane(scalarType, boundary), params);
        checks.Get(
            result.status == SurfaceResultStatus::Succeeded,
            "explicit plane supports scalar type "
                + std::to_string(scalarType));
        checks.Get(
            result.acceptedPointCount > 0,
            "explicit plane accepts points "
                + std::to_string(scalarType));
        const double meanError = GetPlaneMeanError(result, boundary);
        checks.Get(
            meanError <= 0.05,
            "explicit plane reaches 0.05 voxel mean error "
                + std::to_string(scalarType)
                + " error=" + std::to_string(meanError));
    }
}

void TestAutomaticIsoAndPeakMode(Checks& checks)
{
    auto automatic = GetParams();
    automatic.initialIsoValue.reset();
    const auto automaticResult = Build(BuildSphere(), automatic);
    checks.Get(
        automaticResult.status == SurfaceResultStatus::Succeeded,
        "automatic ISO50 accepts a separated bimodal sphere");
    checks.Get(
        automaticResult.initialIsoValue > 350.0
            && automaticResult.initialIsoValue < 650.0,
        "automatic ISO50 remains between material peaks");

    const auto flat = BuildSnapshot(
        { 16, 16, 16 },
        { 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 0.0,
          0.0, 1.0, 0.0,
          0.0, 0.0, 1.0 },
        VTK_FLOAT,
        [](const Point3&) { return 10.0; });
    const auto flatResult = Build(flat, automatic);
    checks.Get(
        flatResult.failureReason
            == SurfaceFailureReason::ThresholdUnreliable,
        "automatic ISO50 rejects a single flat peak");

    auto threshold = GetParams(SurfaceDeterminationMethod::AutomaticIso50);
    threshold.initialIsoValue.reset();
    // 大于旧网格预检预算的体积仍能用固定空间估计阈值；低值伪影不得取代主空气峰。
    const auto artifact = BuildSnapshot(
        {129, 129, 129}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}, VTK_FLOAT,
        [](const Point3& point) { return point[0] < -8.0 ? -900.0
            : point[0] > 90.0 ? 1000.0 : 0.0; }, {}, 1, {-10, -10, -10});
    const auto estimate = Build(artifact, threshold, 64U * 1024U);
    const auto repeated = Build(artifact, threshold, 64U * 1024U);
    checks.Get(estimate.status == SurfaceResultStatus::Succeeded
        && estimate.isoEstimate && estimate.isoEstimate->isoValue > 450.0
        && estimate.isoEstimate->isoValue < 550.0
        && estimate.isoEstimate->sampleCount <= 128U * 128U * 128U
        && estimate.requiredBytes <= 64U * 1024U
        && estimate.points.empty() && estimate.triangleIndices.empty(),
        "bounded threshold mode excludes a low artifact and allocates no mesh");
    checks.Get(estimate.isoEstimate && repeated.isoEstimate
        && estimate.isoEstimate->isoValue == repeated.isoEstimate->isoValue
        && estimate.parameterFingerprint == repeated.parameterFingerprint,
        "threshold sampling is deterministic with nonzero negative extents");
    checks.Get(Build(flat, threshold).failureReason == SurfaceFailureReason::ThresholdUnreliable,
        "threshold-only mode refuses degenerate data instead of fabricating an iso");
    checks.Get(Build(artifact, threshold, 1024U).failureReason == SurfaceFailureReason::BudgetExceeded,
        "threshold workspace budget is enforced independently of mesh budget");
    const auto cancelled = SurfaceDeterminationAlgorithm::BuildSurface(artifact, threshold,
        64U * 1024U, [] { return true; }, {});
    checks.Get(cancelled.status == SurfaceResultStatus::Cancelled && !cancelled.isoEstimate,
        "threshold cancellation publishes no estimate");

    constexpr double boundary = 15.35;
    auto peakParams = GetParams(SurfaceDeterminationMethod::GradientPeak);
    const auto peakResult = Build(BuildPlane(VTK_FLOAT, boundary), peakParams);
    checks.Get(
        peakResult.status == SurfaceResultStatus::Succeeded,
        "gradient peak mode succeeds");
    checks.Get(
        GetPlaneMeanError(peakResult, boundary) <= 0.15,
        "gradient peak mode reaches subvoxel plane tolerance");
}

void TestQualityFlags(Checks& checks)
{
    auto invalidParams = GetParams();
    const auto invalidResult = Build(
        BuildPlane(
            VTK_FLOAT,
            15.35,
            { 1.0, 1.0, 1.0 },
            { 1.0, 0.0, 0.0,
              0.0, 1.0, 0.0,
              0.0, 0.0, 1.0 },
            [](const Point3& point) {
                return point[0] < 14.0 || point[0] > 17.0;
            }),
        invalidParams);
    const bool hasInvalid = std::any_of(
        invalidResult.points.begin(), invalidResult.points.end(),
        [](const SurfacePointRecord& point) {
            return GetSurfaceFlag(
                point.flags, SurfacePointFlags::InvalidSupport);
        });
    checks.Get(hasInvalid, "invalid mask support is flagged");
    const std::uint64_t invalidCount = static_cast<std::uint64_t>(
        std::count_if(
            invalidResult.points.begin(), invalidResult.points.end(),
            [](const SurfacePointRecord& point) {
                return GetSurfaceFlag(
                    point.flags, SurfacePointFlags::InvalidSupport);
            }));
    checks.Get(
        invalidCount > invalidResult.points.size() / 2U
            && invalidResult.acceptedPointCount
                + invalidResult.rejectedPointCount
                == invalidResult.points.size(),
        "invalid support affects the expected band and counters stay consistent");

    auto clippedParams = GetParams();
    clippedParams.profileHalfLengthModel = 3.0;
    const auto clippedResult = Build(
        BuildPlane(VTK_FLOAT, 0.55), clippedParams);
    const bool hasClipped = std::any_of(
        clippedResult.points.begin(), clippedResult.points.end(),
        [](const SurfacePointRecord& point) {
            return GetSurfaceFlag(
                point.flags, SurfacePointFlags::ProfileClipped);
        });
    checks.Get(hasClipped, "data-edge profile clipping is flagged");

    const auto thinWall = BuildSnapshot(
        { 32, 16, 16 },
        { 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 0.0,
          0.0, 1.0, 0.0,
          0.0, 0.0, 1.0 },
        VTK_FLOAT,
        [](const Point3& point) {
            return GetSmoothInside(std::abs(point[0] - 15.5) - 1.25);
        });
    auto wallParams = GetParams();
    wallParams.componentSelection = SurfaceComponentSelection::All;
    wallParams.profileHalfLengthModel = 4.0;
    const auto wallResult = Build(thinWall, wallParams);
    const bool hasMultiple = std::any_of(
        wallResult.points.begin(), wallResult.points.end(),
        [](const SurfacePointRecord& point) {
            return GetSurfaceFlag(
                point.flags, SurfacePointFlags::MultipleCrossings);
        });
    checks.Get(hasMultiple, "thin-wall profiles report multiple crossings");

    auto offsetParams = GetParams();
    offsetParams.initialIsoValue = 100.0;
    offsetParams.maximumOffsetModel = 0.10;
    const auto offsetResult = Build(BuildPlane(), offsetParams);
    const bool hasExcessive = std::any_of(
        offsetResult.points.begin(), offsetResult.points.end(),
        [](const SurfacePointRecord& point) {
            return GetSurfaceFlag(
                point.flags, SurfacePointFlags::ExcessiveOffset);
        });
    checks.Get(hasExcessive, "excessive seed displacement is flagged");
    checks.Get(
        offsetResult.rejectedPointCount > 0,
        "excessive displacement is excluded from accepted points");

    auto previewParams = GetParams(
        SurfaceDeterminationMethod::GlobalIsoPreview);
    const auto previewResult = Build(
        BuildPlane(
            VTK_FLOAT,
            15.35,
            { 1.0, 1.0, 1.0 },
            { 1.0, 0.0, 0.0,
              0.0, 1.0, 0.0,
              0.0, 0.0, 1.0 },
            [](const Point3& point) { return point[1] > 12.0; }),
        previewParams);
    const bool previewHasInvalid = std::any_of(
        previewResult.points.begin(), previewResult.points.end(),
        [](const SurfacePointRecord& point) {
            return GetSurfaceFlag(
                point.flags, SurfacePointFlags::InvalidSupport);
        });
    checks.Get(
        previewHasInvalid,
        "GlobalIsoPreview still reports invalid mask support");
}

void TestNoiseAndParameterValidation(Checks& checks)
{
    constexpr double boundary = 15.35;
    const auto noisy = BuildSnapshot(
        { 32, 24, 20 },
        { 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 0.0,
          0.0, 1.0, 0.0,
          0.0, 0.0, 1.0 },
        VTK_FLOAT,
        [boundary](const Point3& point) {
            const double noise = 8.0 * std::sin(
                0.37 * point[0] + 0.71 * point[1] + 1.13 * point[2]);
            return GetSmoothInside(point[0] - boundary) + noise;
        });
    const auto noisyResult = Build(noisy, GetParams());
    std::vector<double> errors;
    for (const SurfacePointRecord& point : noisyResult.points) {
        if (GetPointAccepted(point)) {
            errors.push_back(std::abs(point.positionModel[0] - boundary));
        }
    }
    std::sort(errors.begin(), errors.end());
    const double mean = errors.empty()
        ? std::numeric_limits<double>::infinity()
        : std::accumulate(errors.begin(), errors.end(), 0.0)
            / static_cast<double>(errors.size());
    const double p95 = errors.empty()
        ? std::numeric_limits<double>::infinity()
        : errors[std::min(
            errors.size() - 1,
            static_cast<std::size_t>(std::floor(
                0.95 * static_cast<double>(errors.size()))))];
    checks.Get(
        noisyResult.status == SurfaceResultStatus::Succeeded
            && mean <= 0.15 && p95 <= 0.30
            && noisyResult.acceptedPointCount
                >= noisyResult.points.size() * 8U / 10U
            && noisyResult.acceptedPointCount
                + noisyResult.rejectedPointCount
                == noisyResult.points.size(),
        "controlled-noise plane meets mean/P95 error mean="
            + std::to_string(mean) + " p95=" + std::to_string(p95));

    auto invalidTypeState = *BuildPlane();
    invalidTypeState.validityMask = BuildMaskLike(
        *invalidTypeState.image, VTK_FLOAT);
    const auto invalidTypeResult = Build(
        std::make_shared<const VtkImageGridView>(
            std::move(invalidTypeState)),
        GetParams());
    checks.Get(
        invalidTypeResult.failureReason
            == SurfaceFailureReason::UnsupportedScalar,
        "matching-geometry mask with wrong scalar type is rejected");

    auto invalidGeometryState = *BuildPlane();
    invalidGeometryState.validityMask = BuildMaskLike(
        *invalidGeometryState.image, VTK_UNSIGNED_CHAR);
    invalidGeometryState.validityMask->SetSpacing(2.0, 1.0, 1.0);
    const auto invalidGeometryResult = Build(
        std::make_shared<const VtkImageGridView>(
            std::move(invalidGeometryState)),
        GetParams());
    checks.Get(
        invalidGeometryResult.failureReason
            == SurfaceFailureReason::InvalidGeometry,
        "correct-type mask with mismatched geometry is rejected");

    auto excessiveSamples = GetParams();
    excessiveSamples.profileHalfLengthModel = 100.0;
    excessiveSamples.profileSampleStepModel = 0.001;
    excessiveSamples.maximumOffsetModel = 1.0;
    const auto sampleResult = Build(BuildPlane(), excessiveSamples);
    checks.Get(
        sampleResult.failureReason == SurfaceFailureReason::InvalidGeometry,
        "profile sample limit is enforced");
}

void TestCancellationAndBudget(Checks& checks)
{
    auto params = GetParams();
    std::atomic<std::uint32_t> cancelChecks{ 0 };
    const auto cancelled = SurfaceDeterminationAlgorithm::BuildSurface(
        BuildSphere(),
        params,
        64U * 1024U * 1024U,
        [&cancelChecks] { return ++cancelChecks > 2; },
        {});
    checks.Get(
        cancelled.status == SurfaceResultStatus::Cancelled
            && cancelled.failureReason == SurfaceFailureReason::Cancelled,
        "algorithm observes cooperative cancellation");
    checks.Get(
        cancelled.points.empty() && cancelled.triangleIndices.empty(),
        "cancelled algorithm does not expose a partial generation");

    const auto budget = Build(BuildSphere(), params, 1024U);
    checks.Get(
        budget.failureReason == SurfaceFailureReason::BudgetExceeded,
        "algorithm rejects insufficient budget before extraction");
}

} // namespace

int GetSurfaceAlgorithmFailCount()
{
    Checks checks;
    TestExplicitPlaneAndScalarTypes(checks);
    TestAutomaticIsoAndPeakMode(checks);
    TestQualityFlags(checks);
    TestNoiseAndParameterValidation(checks);
    TestCancellationAndBudget(checks);
    return checks.failureCount;
}
