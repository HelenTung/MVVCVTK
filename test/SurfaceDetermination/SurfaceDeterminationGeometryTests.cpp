#include "SurfaceDeterminationTestCases.h"

#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceDeterminationTestSupport.h"

#include <vtkType.h>
#include <vtkSMPTools.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace {

using namespace SurfaceTest;

SurfaceAlgorithmResult Build(
    const VtkImageGridSnapshot& source,
    const SurfaceDeterminationStartParams& params)
{
    return SurfaceDeterminationAlgorithm::BuildSurface(
        source,
        params,
        128U * 1024U * 1024U,
        [] { return false; },
        {});
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

struct MeshAudit final {
    bool hasValidIndices = true;
    bool hasDegenerateTriangle = false;
    std::uint64_t boundaryEdgeCount = 0;
    std::uint64_t nonManifoldEdgeCount = 0;
    bool isOrientationValid = true;
};

MeshAudit GetMeshAudit(const SurfaceAlgorithmResult& result)
{
    struct EdgeUse final {
        std::uint32_t count = 0;
        int direction = 0;
    };
    std::unordered_map<std::uint64_t, EdgeUse> edges;
    MeshAudit audit;
    if (result.triangleIndices.size() % 3U != 0U) {
        audit.hasValidIndices = false;
        return audit;
    }
    for (std::size_t index = 0;
        index < result.triangleIndices.size(); index += 3) {
        const std::array<std::uint32_t, 3> ids{
            result.triangleIndices[index],
            result.triangleIndices[index + 1],
            result.triangleIndices[index + 2]
        };
        if (ids[0] >= result.points.size()
            || ids[1] >= result.points.size()
            || ids[2] >= result.points.size()) {
            audit.hasValidIndices = false;
            continue;
        }
        const auto& first = result.points[ids[0]].positionModel;
        const auto& second = result.points[ids[1]].positionModel;
        const auto& third = result.points[ids[2]].positionModel;
        const std::array<double, 3> left{
            second[0] - first[0],
            second[1] - first[1],
            second[2] - first[2]
        };
        const std::array<double, 3> right{
            third[0] - first[0],
            third[1] - first[1],
            third[2] - first[2]
        };
        const std::array<double, 3> cross{
            left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]
        };
        const double area2 = std::sqrt(
            cross[0] * cross[0]
            + cross[1] * cross[1]
            + cross[2] * cross[2]);
        if (!(area2 > 1.0e-12)) audit.hasDegenerateTriangle = true;
        for (std::size_t edgeIndex = 0; edgeIndex < 3; ++edgeIndex) {
            const std::uint32_t from = ids[edgeIndex];
            const std::uint32_t to = ids[(edgeIndex + 1) % 3];
            const std::uint32_t minimum = std::min(from, to);
            const std::uint32_t maximum = std::max(from, to);
            const std::uint64_t key =
                (static_cast<std::uint64_t>(minimum) << 32U) | maximum;
            auto& use = edges[key];
            ++use.count;
            use.direction += from < to ? 1 : -1;
        }
    }
    for (const auto& item : edges) {
        if (item.second.count == 1) ++audit.boundaryEdgeCount;
        if (item.second.count > 2) ++audit.nonManifoldEdgeCount;
        if (item.second.count == 2 && item.second.direction != 0) {
            audit.isOrientationValid = false;
        }
    }
    return audit;
}

void TestSphereMetrics(Checks& checks)
{
    constexpr double radius = 8.0;
    const auto result = Build(BuildSphere(), GetParams());
    checks.Get(
        result.status == SurfaceResultStatus::Succeeded,
        "sphere surface succeeds");
    checks.Get(result.objects.size() == 1, "sphere has one object");
    if (result.objects.empty()) return;
    const SurfaceObjectRecord& object = result.objects.front();
    checks.Get(object.isClosed, "sphere has no boundary edges");
    checks.Get(object.isManifold, "sphere is manifold");
    checks.Get(object.isOrientationValid, "sphere orientation is valid");
    checks.Get(!object.isTruncated, "centered sphere is not truncated");
    checks.Get(
        object.areaValidity == SurfaceMetricValidity::Valid
            && object.areaModelUnit2.has_value(),
        "sphere area is valid");
    checks.Get(
        object.volumeValidity == SurfaceMetricValidity::Valid
            && object.volumeModelUnit3.has_value(),
        "sphere volume is valid");
    const double expectedArea = 4.0 * 3.14159265358979323846
        * radius * radius;
    const double expectedVolume = 4.0 / 3.0 * 3.14159265358979323846
        * radius * radius * radius;
    checks.Get(
        object.areaModelUnit2
            && std::abs(*object.areaModelUnit2 - expectedArea)
                / expectedArea <= 0.05,
        "sphere area agrees with the analytic value within 5 percent");
    checks.Get(
        object.volumeModelUnit3
            && std::abs(*object.volumeModelUnit3 - expectedVolume)
                / expectedVolume <= 0.05,
        "sphere volume agrees with the analytic value within 5 percent");
    std::vector<double> radialErrors;
    radialErrors.reserve(result.points.size());
    for (const SurfacePointRecord& point : result.points) {
        if (!GetPointAccepted(point)) continue;
        const double dx = point.positionModel[0] - 15.5;
        const double dy = point.positionModel[1] - 15.5;
        const double dz = point.positionModel[2] - 15.5;
        radialErrors.push_back(std::abs(
            std::sqrt(dx * dx + dy * dy + dz * dz) - radius));
    }
    std::sort(radialErrors.begin(), radialErrors.end());
    const double radialMean = radialErrors.empty()
        ? std::numeric_limits<double>::infinity()
        : std::accumulate(radialErrors.begin(), radialErrors.end(), 0.0)
            / static_cast<double>(radialErrors.size());
    const double radialP95 = radialErrors.empty()
        ? std::numeric_limits<double>::infinity()
        : radialErrors[std::min(
            radialErrors.size() - 1,
            static_cast<std::size_t>(0.95 * radialErrors.size()))];
    checks.Get(
        radialMean <= 0.15 && radialP95 <= 0.30,
        "sphere point distance meets mean/P95 subvoxel limits");
    checks.Get(
        result.acceptedPointCount == radialErrors.size()
            && result.acceptedPointCount + result.rejectedPointCount
                == result.points.size()
            && result.truncatedPointCount == 0,
        "sphere quality counters agree with independently accepted points");
    const MeshAudit audit = GetMeshAudit(result);
    checks.Get(
        audit.hasValidIndices
            && !audit.hasDegenerateTriangle
            && audit.boundaryEdgeCount == 0
            && audit.nonManifoldEdgeCount == 0
            && audit.isOrientationValid,
        "independent sphere mesh audit confirms closed oriented topology");
    const double diameterX = object.boundsModel[1] - object.boundsModel[0];
    const double diameterY = object.boundsModel[3] - object.boundsModel[2];
    const double diameterZ = object.boundsModel[5] - object.boundsModel[4];
    checks.Get(
        std::abs(diameterX - 2.0 * radius) <= 0.20
            && std::abs(diameterY - 2.0 * radius) <= 0.20
            && std::abs(diameterZ - 2.0 * radius) <= 0.20,
        "sphere diameter bias is at most 0.20 voxel");
}

void TestCylinderDiameter(Checks& checks)
{
    constexpr Point3 center{ 19.5, 19.5, 19.5 };
    constexpr double radius = 7.0;
    constexpr double halfLength = 10.0;
    const auto cylinder = BuildSnapshot(
        { 40, 40, 40 },
        { 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 0.0,
          0.0, 1.0, 0.0,
          0.0, 0.0, 1.0 },
        VTK_FLOAT,
        [center, radius, halfLength](const Point3& point) {
            const double dx = point[0] - center[0];
            const double dy = point[1] - center[1];
            const double radial = std::sqrt(dx * dx + dy * dy) - radius;
            const double axial = std::abs(point[2] - center[2]) - halfLength;
            return GetSmoothInside(std::max(radial, axial));
        });
    const auto result = Build(cylinder, GetParams());
    checks.Get(
        result.status == SurfaceResultStatus::Succeeded
            && result.objects.size() == 1,
        "cylinder surface succeeds");
    if (result.objects.empty()) return;
    const auto& object = result.objects.front();
    const double diameterX = object.boundsModel[1] - object.boundsModel[0];
    const double diameterY = object.boundsModel[3] - object.boundsModel[2];
    checks.Get(
        std::abs(diameterX - 2.0 * radius) <= 0.20
            && std::abs(diameterY - 2.0 * radius) <= 0.20,
        "cylinder diameter bias is at most 0.20 voxel");
}

void TestComponentSelection(Checks& checks)
{
    constexpr Point3 largeCenter{ 10.0, 15.5, 15.5 };
    constexpr Point3 smallCenter{ 25.0, 15.5, 15.5 };
    const auto twoObjects = BuildSnapshot(
        { 36, 32, 32 },
        { 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 0.0,
          0.0, 1.0, 0.0,
          0.0, 0.0, 1.0 },
        VTK_FLOAT,
        [largeCenter, smallCenter](const Point3& point) {
            const auto distance = [&point](
                const Point3& center, const double radius) {
                const double dx = point[0] - center[0];
                const double dy = point[1] - center[1];
                const double dz = point[2] - center[2];
                return std::sqrt(dx * dx + dy * dy + dz * dz) - radius;
            };
            return GetSmoothInside(std::min(
                distance(largeCenter, 5.0),
                distance(smallCenter, 3.0)));
        });

    auto allParams = GetParams();
    allParams.componentSelection = SurfaceComponentSelection::All;
    const auto all = Build(twoObjects, allParams);
    checks.Get(all.objects.size() == 2, "All selects both separated objects");

    auto largestParams = GetParams();
    largestParams.componentSelection = SurfaceComponentSelection::Largest;
    const auto largest = Build(twoObjects, largestParams);
    checks.Get(largest.objects.size() == 1, "Largest selects one object");
    if (!largest.objects.empty()) {
        const double centerX = 0.5 * (
            largest.objects[0].boundsModel[0]
            + largest.objects[0].boundsModel[1]);
        checks.Get(
            std::abs(centerX - largeCenter[0]) < 0.5,
            "Largest selects the larger sphere");
    }

    auto seededParams = GetParams();
    seededParams.componentSelection = SurfaceComponentSelection::Seeded;
    seededParams.seedModelPoint = smallCenter;
    const auto seeded = Build(twoObjects, seededParams);
    checks.Get(seeded.objects.size() == 1, "Seeded selects one object");
    if (!seeded.objects.empty()) {
        const double centerX = 0.5 * (
            seeded.objects[0].boundsModel[0]
            + seeded.objects[0].boundsModel[1]);
        checks.Get(
            std::abs(centerX - smallCenter[0]) < 0.5,
            "Seeded selects the nearest sphere");
    }
}

void TestRoiAndInvalidGeometry(Checks& checks)
{
    auto roiParams = GetParams();
    roiParams.roiModelBounds = std::array<double, 6>{
        0.0, 15.5, 0.0, 31.0, 0.0, 31.0
    };
    const auto truncated = Build(BuildSphere(), roiParams);
    checks.Get(
        truncated.status == SurfaceResultStatus::Succeeded
            && !truncated.objects.empty(),
        "ROI produces a partial surface");
    if (!truncated.objects.empty()) {
        const auto& object = truncated.objects.front();
        checks.Get(object.isTruncated, "ROI object is marked truncated");
        checks.Get(
            object.areaValidity == SurfaceMetricValidity::Truncated
                && object.areaModelUnit2.has_value(),
            "ROI object reports only a qualified local open area");
        checks.Get(
            object.volumeValidity == SurfaceMetricValidity::Truncated
                && !object.volumeModelUnit3,
            "ROI object does not report closed volume");
    }
    const bool allPointsInRoi = std::all_of(
        truncated.points.begin(), truncated.points.end(),
        [](const SurfacePointRecord& point) {
            return point.positionModel[0] >= -1.0e-8
                && point.positionModel[0] <= 15.5 + 1.0e-8
                && point.positionModel[1] >= -1.0e-8
                && point.positionModel[1] <= 31.0 + 1.0e-8
                && point.positionModel[2] >= -1.0e-8
                && point.positionModel[2] <= 31.0 + 1.0e-8;
        });
    checks.Get(allPointsInRoi, "all ROI output points remain inside model bounds");

    auto outsideParams = GetParams();
    outsideParams.roiModelBounds = std::array<double, 6>{
        100.0, 110.0, 100.0, 110.0, 100.0, 110.0
    };
    const auto outside = Build(BuildSphere(), outsideParams);
    checks.Get(
        outside.failureReason == SurfaceFailureReason::InvalidRoi,
        "non-intersecting ROI is rejected");

    const auto singular = BuildSnapshot(
        { 8, 8, 8 },
        { 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 0.0,
          0.0, 0.0, 0.0,
          0.0, 0.0, 1.0 },
        VTK_FLOAT,
        [](const Point3& point) { return point[0] < 3.5 ? 1000.0 : 0.0; });
    const auto singularResult = Build(singular, GetParams());
    checks.Get(
        singularResult.failureReason
            == SurfaceFailureReason::InvalidGeometry,
        "non-invertible direction is rejected");
}

void TestDirectionExtentAndDeterminism(Checks& checks)
{
    constexpr double angle = 0.35;
    const std::array<double, 9> direction{
        std::cos(angle), -std::sin(angle), 0.0,
        std::sin(angle), std::cos(angle), 0.0,
        0.0, 0.0, 1.0
    };
    constexpr double boundary = 5.25;
    const auto rotated = BuildPlane(
        VTK_FLOAT,
        boundary,
        { 0.7, 1.3, 2.0 },
        direction);
    auto params = GetParams();
    vtkSMPTools::Initialize(1);
    const auto first = Build(rotated, params);
    vtkSMPTools::Initialize(4);
    const auto second = Build(rotated, params);
    checks.Get(
        first.status == SurfaceResultStatus::Succeeded,
        "rotated non-equidistant geometry succeeds");
    checks.Get(
        GetPlaneMeanError(first, boundary) <= 0.15,
        "rotated geometry uses model-space gradients");
    checks.Get(
        first.parameterFingerprint == second.parameterFingerprint
            && first.points.size() == second.points.size()
            && first.triangleIndices == second.triangleIndices,
        "thread-count changes preserve identity and topology order");
    bool coordinatesEqual = first.points.size() == second.points.size();
    for (std::size_t index = 0;
        coordinatesEqual && index < first.points.size(); ++index) {
        coordinatesEqual = first.points[index].positionModel
            == second.points[index].positionModel;
    }
    checks.Get(coordinatesEqual, "thread-count changes preserve point coordinates");

    auto changedParams = params;
    changedParams.maximumOffsetModel = 1.1;
    const auto changed = Build(rotated, changedParams);
    checks.Get(
        changed.status == SurfaceResultStatus::Succeeded
            && changed.parameterFingerprint != first.parameterFingerprint,
        "resolved parameter changes produce a new fingerprint");
    auto changedMethod = params;
    changedMethod.method = SurfaceDeterminationMethod::GradientPeak;
    const auto changedMethodResult = Build(rotated, changedMethod);
    checks.Get(
        changedMethodResult.status == SurfaceResultStatus::Succeeded
            && changedMethodResult.parameterFingerprint
                != first.parameterFingerprint,
        "method changes produce a new fingerprint");

    const auto nonzeroExtent = BuildSnapshot(
        { 16, 12, 10 },
        { 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 0.0,
          0.0, 1.0, 0.0,
          0.0, 0.0, 1.0 },
        VTK_FLOAT,
        [](const Point3& point) {
            return GetSmoothInside(point[0] - 10.35);
        },
        {},
        1,
        { 3, -2, 4 });
    const auto extentResult = Build(nonzeroExtent, GetParams());
    checks.Get(
        extentResult.status == SurfaceResultStatus::Succeeded,
        "nonzero VTK extent is supported");
}

} // namespace

int GetSurfaceGeometryFailCount()
{
    Checks checks;
    TestSphereMetrics(checks);
    TestCylinderDiameter(checks);
    TestComponentSelection(checks);
    TestRoiAndInvalidGeometry(checks);
    TestDirectionExtentAndDeterminism(checks);
    return checks.failureCount;
}
