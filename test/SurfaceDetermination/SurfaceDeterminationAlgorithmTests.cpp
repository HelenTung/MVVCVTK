// 测试用途：验证阈值估计、等值面和梯度定位、质量标志、参数校验、取消与预算。
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

void TestMaterialRangeEstimation(Checks& checks)
{
    auto automatic = GetParams();
    automatic.initialIsoValue.reset();
    const auto automaticResult = Build(BuildSphere(), automatic);
    checks.Get(
        automaticResult.status == SurfaceResultStatus::Succeeded,
        "material range estimation accepts a separated bimodal sphere");
    checks.Get(
        automaticResult.initialIsoValue > 350.0
            && automaticResult.initialIsoValue < 650.0,
        "material range estimation remains between material peaks");

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
        "material range estimation rejects a single flat peak");

    auto threshold = GetParams(SurfaceDeterminationMethod::GlobalAutomatic);
    threshold.initialIsoValue.reset();
    // 大于旧网格预检预算的体积仍能用固定空间估计阈值；低值伪影不得取代主空气峰。
    const auto artifact = BuildSnapshot(
        {129, 129, 129}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}, VTK_FLOAT,
        [](const Point3& point) { return point[0] < -8.0 ? -900.0
            : point[0] > 90.0 ? 1000.0 : 0.0; }, {}, 1, {-10, -10, -10});
    const auto estimate = Build(artifact, threshold);
    const auto repeated = Build(artifact, threshold);
    checks.Get(estimate.status == SurfaceResultStatus::Succeeded
        && estimate.isoEstimate && estimate.isoEstimate->isoValue > 450.0
        && estimate.isoEstimate->isoValue < 550.0
        && estimate.isoEstimate->sampleCount <= 128U * 128U * 128U
        && !estimate.points.empty() && !estimate.triangleIndices.empty(),
        "material estimation excludes low artifacts and publishes the same new surface");
    checks.Get(estimate.isoEstimate && repeated.isoEstimate
        && estimate.isoEstimate->isoValue == repeated.isoEstimate->isoValue
        && estimate.parameterFingerprint == repeated.parameterFingerprint,
        "threshold sampling is deterministic with nonzero negative extents");
    checks.Get(Build(flat, threshold).failureReason == SurfaceFailureReason::ThresholdUnreliable,
        "material estimation refuses degenerate data instead of fabricating an iso");
    checks.Get(Build(artifact, threshold, 1024U).failureReason == SurfaceFailureReason::BudgetExceeded,
        "threshold workspace budget is enforced independently of mesh budget");
    const auto cancelled = SurfaceDeterminationAlgorithm::BuildSurface(artifact, threshold,
        64U * 1024U, [] { return true; }, {});
    checks.Get(cancelled.status == SurfaceResultStatus::Cancelled && !cancelled.isoEstimate,
        "threshold cancellation publishes no estimate");

    const auto thinMaterial = BuildSnapshot(
        {128,128,128}, {1,1,1}, {0,0,0}, {1,0,0,0,1,0,0,0,1}, VTK_FLOAT,
        [](const Point3& point) { return point[0] == 61 || point[0] == 62 ? 1000.0 : point[0] >= 112 ? 0.0 : -100.0; });
    const auto thinEstimate = Build(thinMaterial, threshold);
    checks.Get(thinEstimate.status == SurfaceResultStatus::Succeeded && thinEstimate.isoEstimate
        && std::abs(thinEstimate.isoEstimate->isoValue-450.0) < 3.0
        && thinEstimate.isoEstimate->materialValue > 990.0,
        "histogram range retains thin material missed by the old coarse pilot instead of selecting a background secondary peak");
    const auto multipleMaterials = BuildSnapshot(
        {128,128,128}, {1,1,1}, {0,0,0}, {1,0,0,0,1,0,0,0,1}, VTK_FLOAT,
        [](const Point3& point) { return point[0] < 80 ? -100.0 : point[0] < 112
            ? 600.0 + .8*(point[1]+point[2]-127.0) : 2000.0; });
    const auto dominantEstimate = Build(multipleMaterials, threshold);
    checks.Get(dominantEstimate.status == SurfaceResultStatus::Succeeded && dominantEstimate.isoEstimate
        && std::abs(dominantEstimate.isoEstimate->isoValue-250.0) < 5.0,
        "material range estimation selects the largest material population instead of a sharper brighter minority peak");
    const auto materialDominated = BuildSnapshot(
        {128,128,128}, {1,1,1}, {0,0,0}, {1,0,0,0,1,0,0,0,1}, VTK_FLOAT,
        [](const Point3& point) { return point[0] < 32 ? 0.0 : point[0] < 116 ? 800.0 : 1200.0; });
    const auto materialDominatedEstimate = Build(materialDominated, threshold);
    checks.Get(materialDominatedEstimate.status == SurfaceResultStatus::Succeeded && materialDominatedEstimate.isoEstimate
        && std::abs(materialDominatedEstimate.isoEstimate->isoValue-400.0) < 3.0,
        "the tallest material peak is not mistaken for air when material occupies most of the input");
    const auto negativeMaterial = BuildSnapshot(
        {32,32,32}, {1,1,1}, {0,0,0}, {1,0,0,0,1,0,0,0,1}, VTK_FLOAT,
        [](const Point3& point) { return point[0] < 24 ? -1000.0 : -200.0; });
    const auto negativeEstimate = Build(negativeMaterial, threshold);
    checks.Get(negativeEstimate.status == SurfaceResultStatus::Succeeded && negativeEstimate.isoEstimate
        && std::abs(negativeEstimate.isoEstimate->isoValue+600.0) < 2.0,
        "ISO50 remains in the input scalar domain and never clamps or takes the absolute value of valid negative thresholds");


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
    checks.Get(invalidResult.failureReason == SurfaceFailureReason::NoSurface && invalidResult.points.empty(),
               "fully invalid interface support produces no invented surface");
    checks.Get(invalidResult.execution.skippedCellCount > 0 && invalidResult.acceptedPointCount == 0,
               "invalid cells are counted and cannot become accepted seeds");

    const auto material = Build(BuildPlane(), GetParams());
    bool unchanged = !material.points.empty();
    for (const auto &point : material.points)
        unchanged = unchanged && point.positionModel == point.seedPositionModel && point.offsetFromSeed == 0;
    checks.Get(unchanged && material.execution.sampledProfileCount == 0,
               "material surface never relocates seeds through a retired local fit");
    auto previewParams = GetParams(
        SurfaceDeterminationMethod::GlobalAutomatic);
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
    bool supported = !previewResult.points.empty();
    for (const auto &point : previewResult.points)
        supported = supported && point.positionModel[1] >= 13;
    checks.Get(supported && previewResult.execution.skippedCellCount > 0,
               "preview leaves invalid-mask cells empty without adding a cap");
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

    auto invalidRange = GetParams();
    invalidRange.materialRange = std::array<double, 2>{500, 500};
    checks.Get(Build(BuildPlane(), invalidRange).failureReason == SurfaceFailureReason::InvalidGeometry,
               "degenerate material range remains invalid");

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

void TestGlobalAutomatic(Checks &checks)
{
    SurfaceDeterminationStartParams params;
    checks.Get(params.method == SurfaceDeterminationMethod::GlobalAutomatic, "material ISO is the default method");
    params.componentSelection = SurfaceComponentSelection::All;
    params.materialRange = std::array<double, 2>{0, 100};
    params.initialIsoValue = 40;
    const auto source = BuildSnapshot({20, 20, 20}, {1, 1, 1}, {0, 0, 0},
        {1, 0, 0, 0, 1, 0, 0, 0, 1}, VTK_FLOAT,
        [](const Point3 &point) { return point[0] <= 7 ? -100.0 : 200.0; });
    const auto result = Build(source, params);
    checks.Get(result.status == SurfaceResultStatus::Succeeded && !result.points.empty() &&
        result.acceptedPointCount > 0, "saturated material ISO publishes measurement points");
    for (const auto &point : result.points)
        checks.Get(std::abs(point.positionModel[0] - 7.4) < 1e-12 &&
                       std::abs(point.normalModel[0] + 1) < 1e-7 && point.offsetFromSeed == 0,
                   "saturation occurs at scalar nodes before crossing and gradient interpolation");
    checks.Get(source->image->GetScalarComponentAsDouble(7, 8, 8, 0) == -100,
               "material surface leaves original RAW unchanged");
    checks.Get(result.resolvedParams.materialRange == params.materialRange && result.isoEstimate &&
                   result.isoEstimate->isoValue == 40, "resolved recipe retains scalar calibration");
    const auto text = SurfaceRecipeCodec::BuildText(result.resolvedParams);
    const auto decoded = SurfaceRecipeCodec::GetRecipe(text);
    checks.Get(decoded.recipe && decoded.recipe->materialRange == params.materialRange,
               "material range survives recipe serialization");
    params.materialRange = std::array<double, 2>{0, 200};
    const auto other = Build(source, params);
    checks.Get(!other.points.empty() && std::abs(other.points.front().positionModel[0] - 7.2) < 1e-12 &&
                   other.parameterFingerprint != result.parameterFingerprint,
               "material range belongs to the request and changes identity");
    if (!result.points.empty())
    {
        const auto replay = SurfaceDeterminationAlgorithm::GetPointDiagnostic(
            source, result.resolvedParams, result.points[result.points.size() / 2], {});
        checks.Get(replay.isAvailable && replay.point.localThreshold == 40,
                   "diagnostic replay uses the first frozen material range");
    }
    auto inferred = params;
    inferred.materialRange.reset();
    const auto inferredResult = Build(source, inferred);
    auto explicitSource = inferred;
    explicitSource.materialRange = std::array<double, 2>{-100, 200};
    const auto explicitSourceResult = Build(source, explicitSource);
    bool same = inferredResult.status == SurfaceResultStatus::Succeeded &&
        inferredResult.points.size() == explicitSourceResult.points.size();
    if (same) for (std::size_t i=0;i<inferredResult.points.size();++i)
        same = same && inferredResult.points[i].positionModel == explicitSourceResult.points[i].positionModel;
    checks.Get(same && inferredResult.resolvedParams.materialRange == explicitSource.materialRange,
               "explicit ISO uses source range without an unrelated peak-estimation rejection");
    params.initialIsoValue = 200;
    checks.Get(Build(source, params).status == SurfaceResultStatus::Failed,
               "material threshold must be strictly inside the material range");
    params.materialRange = std::array<double, 2>{-std::numeric_limits<double>::max(),
                                                std::numeric_limits<double>::max()};
    params.initialIsoValue = 0;
    checks.Get(Build(source, params).status == SurfaceResultStatus::Failed,
               "unrepresentable material span is rejected before interpolation");
}

} // namespace

int GetSurfaceAlgorithmFailCount()
{
    Checks checks;
    TestExplicitPlaneAndScalarTypes(checks);
    TestMaterialRangeEstimation(checks);
    TestQualityFlags(checks);
    TestNoiseAndParameterValidation(checks);
    TestCancellationAndBudget(checks);
    TestGlobalAutomatic(checks);
    return checks.failureCount;
}
