#include "AlignmentSolver.h"
#include "AlignmentMath.h"
#include <iostream>
#include <limits>

namespace {
using namespace AlignmentMath;
int failures = 0;
void Check(bool value, const char *message) {
    if (!value) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}
DataRevisionRef Ref(unsigned char id) {
    DataRevisionRef r;
    r.entityId.bytes[0] = id;
    r.generation = 1;
    return r;
}
AlignmentWork Work() {
    AlignmentWork w;
    w.input = {Ref(1), Ref(2), Ref(3), "scan", "RAS", "part", AlignmentUnit::ModelUnit};
    w.recipe.id = "recipe";
    w.recipe.targetFrameId = "target";
    w.recipe.nominalData = Ref(4);
    w.recipe.datumCount = 0;
    w.cancelled = std::make_shared<std::atomic<bool>>(false);
    w.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    w.poses = {alignmentIdentity};
    return w;
}
void SoftPriority() {
    auto w = Work();
    w.recipe.method = AlignmentMethod::Rps;
    AlignmentGeometrySpec g;
    g.id = "point";
    g.kind = AlignmentGeometryKind::Point;
    g.region.pinnedMesh = w.input.mesh;
    g.region.vertexIds = {0};
    w.recipe.geometries = {g};
    w.mesh = std::make_shared<const SurfaceMeshPayload>(std::vector<double>{0, 0, 0},
                                                        std::vector<std::uint64_t>{});
    AlignmentConstraint a;
    a.kind = AlignmentConstraintKind::Soft;
    a.nominalPoint = {1, 0, 0};
    a.targetDirection = {1, 0, 0};
    a.tolerance = 20;
    auto b = a;
    b.priority = 1;
    b.nominalPoint = {10, 0, 0};
    b.weight = 1e9;
    w.recipe.constraints = {a, b};
    w.recipe.maxAlignmentRms = 20;
    auto r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.status == AlignmentStatus::Underconstrained,
          "soft priority partial status");
    Check(std::abs(r.sourceToTarget[3] - 1) < 1e-7,
          "lower priority cannot pull earlier optimum with huge weight");
    Check(r.diagnostics.remaining == 5, "single direction has five unresolved DOF");
    w.recipe.constraints = {a, a};
    r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.remaining == 5, "duplicate soft rows do not determine extra DOF");
}
void BestFit() {
    auto w = Work();
    w.recipe.method = AlignmentMethod::ConstrainedBestFit;
    w.recipe.datumCount = 1;
    w.recipe.exactMesh = w.input.mesh;
    w.recipe.maxPairDistance = 100;
    w.recipe.huberDistance = 100;
    const double c = std::cos(.2), s = std::sin(.2);
    const AlignmentMatrix truth{c, -s, 0, 2, s, c, 0, -1, 0, 0, 1, -3, 0, 0, 0, 1};
    std::vector<AlignmentPoint> points{{0, 0, 3}, {3, 0, 3}, {0, 2, 3}, {2, 4, 3}, {1, 2, 6}};
    std::vector<double> vertices;
    AlignmentGeometrySpec plane;
    plane.id = "A";
    plane.association = AlignmentAssociation::SequentialLeastSquares;
    plane.region.pinnedMesh = w.input.mesh;
    plane.region.vertexIds = {0, 1, 2, 3};
    w.recipe.geometries = {plane};
    for (std::size_t i = 0; i < points.size(); ++i) {
        vertices.insert(vertices.end(), points[i].begin(), points[i].end());
        w.recipe.fitPairs.push_back({i, Point(Transform(truth, Vector(points[i]))), {}, 1});
    }
    w.mesh = std::make_shared<const SurfaceMeshPayload>(vertices, std::vector<std::uint64_t>{});
    auto r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.status == AlignmentStatus::FullyDetermined &&
              r.diagnostics.hardRemaining == 3 && r.diagnostics.remaining == 0,
          "best-fit resolves exactly A remaining motion");
    for (std::size_t i = 0; i < 16; ++i)
        Check(std::abs(r.sourceToTarget[i] - truth[i]) < 1e-6, "constrained transform recovery");
    for (auto &pair : w.recipe.fitPairs)
        pair.nominalPoint[2] += 0.2;
    w.recipe.maxAlignmentRms = 1;
    r = AlignmentSolver::BuildResult(w);
    Check(std::abs(r.sourceToTarget[11] + 3) < 1e-8 &&
              std::abs(r.sourceToTarget[8]) + std::abs(r.sourceToTarget[9]) < 1e-8,
          "incompatible soft Z cannot pull hard A plane");
    w.recipe.fitPairs.back().nominalPoint[0] += 100;
    w.recipe.minPairCoverage = .5;
    w.recipe.maxPairDistance = 10;
    r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.rejectedPairs == 1 && !r.diagnostics.isQualityPassed &&
              r.diagnostics.maximum > 90,
          "outlier remains in final quality evaluation");
    w.recipe.exactMesh.generation = 2;
    r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.status != AlignmentStatus::FullyDetermined,
          "old correspondence mesh rejected");
}
void SelectionAndQuality() {
    auto w = Work();
    w.recipe.method = AlignmentMethod::SequentialPlanes;
    w.recipe.datumCount = 1;
    AlignmentGeometrySpec spec;
    spec.id = "plane";
    spec.association = AlignmentAssociation::SequentialLeastSquares;
    spec.region.targetBounds = std::array<double, 6>{-1, 3, -1, 3, -.1, .1};
    w.recipe.geometries = {spec};
    w.mesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{10, 0, 2, 12, 0, 2, 10, 2, 2, 12, 2, 2, 100, 100, 100},
        std::vector<std::uint64_t>{});
    w.poses[0][3] = -10;
    w.poses[0][11] = -2;
    auto r = AlignmentSolver::BuildResult(w);
    Check(r.geometries.size() == 1 && r.geometries[0].sampleCount == 4,
          "repeatable target-space selection respects initial pose");
    w.recipe.requiresMeasurementQuality = true;
    r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.status != AlignmentStatus::FullyDetermined && r.geometries.empty(),
          "required missing quality rejected");
    auto vertices = w.mesh->GetVertices();
    std::vector<MeshAttribute> attrs{
        {"measurement.valid", 1, {1, 1, 1, 0, 1}},
        {"measurement.support-ratio", 1, {1, 1, 1, 1, 1}},
        {"measurement.localization-sigma", 1, {.01, .01, .01, .01, .01}},
        {"measurement.fit-residual", 1, {0, 0, 0, 0, 0}},
        {"measurement.normal", 3, {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}}};
    w.mesh =
        std::make_shared<const SurfaceMeshPayload>(vertices, std::vector<std::uint64_t>{}, attrs);
    w.recipe.geometries[0].minCoverage = .7;
    r = AlignmentSolver::BuildResult(w);
    Check(r.geometries.size() == 1 && r.geometries[0].sampleCount == 3 &&
              r.geometries[0].rejectedCount == 1,
          "quality validity filters and records rejected measurement");
    w.deadline = std::chrono::steady_clock::now();
    r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.status == AlignmentStatus::DeadlineExceeded,
          "deadline distinct from cancellation");
    w.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    w.config.pointLimit = 2;
    r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.status == AlignmentStatus::BudgetExceeded, "selection size reports budget rather than geometric degeneracy");
    w.config.pointLimit = 100000;
    w.config.workingBytes = 1024;
    r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.status == AlignmentStatus::BudgetExceeded,
          "work allocation budget enforced");
}
void ScaleAndDegeneracy() {
    for (double scale : {1e-3, 1.0, 1e3}) {
        auto w = Work();
        w.recipe.lengthScale = scale;
        w.recipe.solveTolerance = scale * 1e-8;
        AlignmentSamples samples;
        for (auto p : std::vector<AlignmentPoint>{{0, 0, 5}, {2, 0, 5}, {0, 3, 5}, {2, 3, 5}}) {
            for (auto &v : p)
                v *= scale;
            samples.points.push_back(p);
        }
        AlignmentGeometrySpec spec;
        spec.id = "A";
        spec.maxFitRms = scale * 1e-4;
        const auto g = AlignmentGeometryFit::BuildGeometry(w, spec, samples, alignmentIdentity);
        Check(std::abs(g.sourceCenter[2] / scale - 5) < 1e-10 &&
                  std::abs(g.sourceDirection[2] - 1) < 1e-10,
              "scale normalized geometry invariance");
        samples.points = {{0, 0, 0}, {scale, 0, 0}, {2 * scale, 0, 0}};
        bool rejected = false;
        try {
            AlignmentGeometryFit::BuildGeometry(w, spec, samples, alignmentIdentity);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        Check(rejected, "collinear plane rejected");
    }
}
} // namespace
namespace {
void HolesAndAmbiguity() {
    auto work = Work();
    work.recipe.method = AlignmentMethod::PlaneTwoHoles;
    work.recipe.datumCount = 3;
    std::vector<double> vertices;
    for (std::size_t index = 0; index < 3; ++index) {
        AlignmentGeometrySpec spec;
        spec.id = "datum-" + std::to_string(index);
        spec.kind = index == 0 ? AlignmentGeometryKind::Plane : AlignmentGeometryKind::Cylinder;
        spec.region.pinnedMesh = work.input.mesh;
        spec.maxFitRms = 1e-6;
        const auto add = [&](AlignmentPoint point) {
            spec.region.vertexIds.push_back(vertices.size() / 3);
            vertices.insert(vertices.end(), point.begin(), point.end());
        };
        if (index == 0) {
            for (double x : {-5.0, 5.0})
                for (double y : {-5.0, 5.0})
                    add({x, y, 5});
        } else {
            for (int a = 0; a < 16; ++a)
                for (double z : {3.0, 5.0, 7.0}) {
                    const double angle = 6.283185307179586 * a / 16;
                    add({(index == 1 ? 10.0 : 40.0) + std::cos(angle), 20 + std::sin(angle), z});
                }
        }
        work.recipe.geometries.push_back(spec);
    }
    work.mesh = std::make_shared<const SurfaceMeshPayload>(vertices, std::vector<std::uint64_t>{});
    const auto holes = AlignmentSolver::BuildResult(work);
    Check(holes.diagnostics.status == AlignmentStatus::FullyDetermined &&
              holes.diagnostics.remaining == 0,
          "plane and two hole axes form complete frame");
    Check(std::abs(holes.sourceToTarget[3] + 10) < 1e-6 &&
              std::abs(holes.sourceToTarget[7] + 20) < 1e-6 &&
              std::abs(holes.sourceToTarget[11] + 5) < 1e-6,
          "hole origin is axis-plane intersection, not arbitrary axial fit point");
    work = Work();
    work.recipe.method = AlignmentMethod::Rps;
    const std::vector<AlignmentPoint> target{{0, 0, 0}, {2, 0, 0}, {0, 3, 0}};
    vertices.clear();
    for (double offset : {0.0, 100.0})
        for (auto point : target) {
            point[0] += offset;
            vertices.insert(vertices.end(), point.begin(), point.end());
        }
    for (std::size_t i = 0; i < target.size(); ++i) {
        AlignmentGeometrySpec spec;
        spec.id = std::to_string(i);
        spec.kind = AlignmentGeometryKind::Point;
        spec.region.targetBounds = std::array<double, 6>{
            target[i][0] - .1, target[i][0] + .1, target[i][1] - .1, target[i][1] + .1, -.1, .1};
        work.recipe.geometries.push_back(spec);
        for (int axis = 0; axis < 3; ++axis) {
            AlignmentConstraint constraint;
            constraint.geometryIndex = i;
            constraint.nominalPoint = target[i];
            constraint.targetDirection = {0, 0, 0};
            constraint.targetDirection[axis] = 1;
            work.recipe.constraints.push_back(constraint);
        }
    }
    work.mesh = std::make_shared<const SurfaceMeshPayload>(vertices, std::vector<std::uint64_t>{});
    auto second = alignmentIdentity;
    second[3] = -100;
    work.poses = {alignmentIdentity, second};
    auto result = AlignmentSolver::BuildResult(work);
    Check(result.diagnostics.status == AlignmentStatus::Ambiguous &&
              !result.diagnostics.isQualityPassed,
          "two repeated feature candidates cannot be activated as unique alignment");
    work.poses = {alignmentIdentity, alignmentIdentity};
    result = AlignmentSolver::BuildResult(work);
    Check(result.diagnostics.status == AlignmentStatus::FullyDetermined,
          "duplicate identical candidates are not false ambiguity");
}
} // namespace
int TestConstraints() {
    SoftPriority();
    BestFit();
    SelectionAndQuality();
    ScaleAndDegeneracy();
    HolesAndAmbiguity();
    return failures;
}
