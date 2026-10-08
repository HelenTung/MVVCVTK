#include "AlignmentMeshQuery.h"
#include "AlignmentSolver.h"
#include "AlignmentMath.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
int failures = 0;
// 解析平面与刚体双精度运算的数值回归容差，不代表 CT 测量精度。
constexpr double numericTolerance = 1e-7;
void Check(bool value, const char *message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
DataRevisionRef Ref(unsigned char id) {
    DataRevisionRef ref;
    ref.entityId.bytes[0] = id;
    ref.generation = 1;
    return ref;
}
AlignmentWork Work() {
    AlignmentWork work;
    work.deadline = std::chrono::steady_clock::time_point::max();
    work.cancelled = std::make_shared<std::atomic<bool>>(false);
    work.input = {Ref(1), Ref(2), Ref(3), "scan", "RAS", "part", AlignmentUnit::ModelUnit};
    work.recipe.id = "surface-fixture";
    work.recipe.targetFrameId = "reference";
    work.recipe.nominalData = Ref(4);
    work.recipe.method = AlignmentMethod::ConstrainedBestFit;
    work.recipe.datumCount = 0;
    work.recipe.surfaceFit = AlignmentSurfaceFit{};
    work.recipe.maxPairDistance = 2;
    work.recipe.maxAlignmentRms = 1;
    work.poses = {alignmentIdentity};
    return work;
}
std::shared_ptr<const SurfaceMeshPayload> Triangle(double height = 0) {
    return std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{0, 0, height, 4, 0, height, 0, 4, height},
        std::vector<std::uint64_t>{0, 1, 2});
}
void QueryGeometry() {
    auto work = Work();
    auto mesh = Triangle();
    const auto original = mesh->GetVertices();
    AlignmentMeshQuery query(mesh, work);
    const auto inside = query.GetClosest({1, 1, 3});
    Check(inside.isValid && std::abs(inside.distanceSquared - 9) < numericTolerance &&
          inside.point == AlignmentPoint{1, 1, 0}, "closest point lies inside triangle");
    const auto edge = query.GetClosest({3, 3, 1});
    Check(edge.isValid && edge.point == AlignmentPoint{2, 2, 0} &&
          std::abs(edge.distanceSquared - 3) < numericTolerance, "closest point lies on edge");
    Check(query.GetClosest({-1, -2, 0}).point == AlignmentPoint{0, 0, 0}, "closest vertex region");
    Check(mesh->GetVertices() == original, "query leaves original positions unchanged");
    std::weak_ptr<const SurfaceMeshPayload> retained = mesh;
    mesh.reset();
    Check(!retained.expired() && query.GetClosest({1, 1, 3}).isValid, "query pins immutable input owner");
    work.cancelled->store(true);
    bool cancelled = false;
    try { (void)query.GetClosest({0, 0, 0}); } catch (const std::runtime_error &) { cancelled = true; }
    Check(cancelled, "query observes cancellation after construction");
    const AlignmentMatrix halfTurn{-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Check(std::abs(AlignmentMath::GetRotationAngle(halfTurn) - std::acos(-1.0)) < numericTolerance,
          "half-turn is not treated as a converged zero-angle increment");
}
void QueryIndex() {
    auto work = Work();
    std::vector<double> vertices;
    std::vector<std::uint64_t> triangles;
    // 分离平面提供独立解析真值，覆盖索引内部节点与受限参考区域。
    for (std::uint64_t i = 0; i < 65; ++i) {
        const auto z = static_cast<double>(i) * 2;
        vertices.insert(vertices.end(), {0, 0, z, 4, 0, z, 0, 4, z});
        triangles.insert(triangles.end(), {3 * i, 3 * i + 1, 3 * i + 2});
    }
    auto mesh = std::make_shared<const SurfaceMeshPayload>(std::move(vertices), std::move(triangles));
    AlignmentMeshQuery query(mesh, work);
    for (std::uint64_t i = 0; i < 65; ++i) {
        const auto hit = query.GetClosest({1, 1, 2 * static_cast<double>(i) + 0.25});
        Check(hit.triangle == i && std::abs(hit.distanceSquared - 0.0625) < numericTolerance,
              "tree nearest plane identity and distance");
    }
    AlignmentMeshQuery selected(mesh, work, {0});
    Check(selected.GetClosest({1, 1, 20}).triangle == 0, "reference region restricts correspondence");
    work.config.workingBytes = 1;
    bool limited = false;
    try { AlignmentMeshQuery rejected(mesh, work); } catch (const std::length_error &) { limited = true; }
    Check(limited, "index rejects insufficient explicit working budget");
    work.config.workingBytes.reset();
    bool duplicate = false;
    try { AlignmentMeshQuery rejected(mesh, work, {0, 0}); } catch (const std::invalid_argument &) { duplicate = true; }
    Check(duplicate, "duplicate reference selection rejected");
}
AlignmentWork Planes(double referenceZ = 0) {
    auto work = Work();
    work.referenceMesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{0, 0, referenceZ, 20, 0, referenceZ, 0, 20, referenceZ,
                            0, 0, 0, 0, 20, 0, 0, 0, 20,
                            0, 0, 0, 0, 0, 20, 20, 0, 0},
        std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8});
    std::vector<double> vertices;
    std::vector<std::uint64_t> triangles;
    const double c = std::cos(0.02), s = std::sin(0.02);
    for (int face = 0; face < 3; ++face)
        for (int patch = 0; patch < 2; ++patch) {
            const std::array<std::array<double, 2>, 3> uv{{{2.0 + 4 * patch, 2},
                                                        {4.0 + 4 * patch, 2},
                                                        {2.0 + 4 * patch, 4}}};
            for (const auto &p : uv) {
                const AlignmentPoint q = face == 0 ? AlignmentPoint{p[0], p[1], 0}
                                         : face == 1 ? AlignmentPoint{0, p[0], p[1]}
                                                     : AlignmentPoint{p[1], 0, p[0]};
                const double x = q[0] - 0.1, y = q[1] + 0.2, z = q[2] - 0.15;
                triangles.push_back(static_cast<std::uint64_t>(vertices.size() / 3));
                vertices.insert(vertices.end(), {c * x + s * y, -s * x + c * y, z});
            }
        }
    work.mesh = std::make_shared<const SurfaceMeshPayload>(std::move(vertices), std::move(triangles));
    return work;
}
void SurfaceAlignment() {
    auto work = Planes();
    const auto positions = work.mesh->GetVertices();
    auto result = AlignmentSolver::BuildResult(work);
    std::cerr << "surface status " << static_cast<int>(result.diagnostics.status) << ' '
              << result.diagnostics.message << '\n';
    Check(result.diagnostics.status == AlignmentStatus::FullyDetermined, "dynamic ICP fully determined");
    Check(std::abs(result.sourceToTarget[3] - 0.1) < numericTolerance &&
          std::abs(result.sourceToTarget[7] + 0.2) < numericTolerance &&
          std::abs(result.sourceToTarget[11] - 0.15) < numericTolerance &&
          std::abs(result.sourceToTarget[1] + std::sin(0.02)) < numericTolerance,
          "dynamic plane correspondences recover analytic rigid transform");
    Check(work.mesh->GetVertices() == positions, "ICP preserves source mesh coordinates");
    work.recipe.surfaceFit->referenceTriangles = {0};
    work.recipe.maxPairDistance = 20;
    work.recipe.maxAlignmentRms = 20;
    result = AlignmentSolver::BuildResult(work);
    Check(result.diagnostics.status == AlignmentStatus::Underconstrained && result.diagnostics.remaining == 3,
          "one reference plane preserves three unconstrained degrees");

    work = Planes(0.5);
    AlignmentGeometrySpec datum;
    datum.id = "A";
    datum.association = AlignmentAssociation::SequentialLeastSquares;
    datum.region.pinnedMesh = work.input.mesh;
    datum.region.vertexIds = {0, 1, 2, 3, 4, 5};
    work.recipe.geometries = {datum};
    work.recipe.datumCount = 1;
    result = AlignmentSolver::BuildResult(work);
    Check(result.diagnostics.status == AlignmentStatus::FullyDetermined &&
          std::abs(result.sourceToTarget[11] - 0.15) < numericTolerance,
          "dynamic best fit cannot move established primary datum toward offset reference");
    work.cancelled->store(true);
    Check(AlignmentSolver::BuildResult(work).diagnostics.status == AlignmentStatus::Cancelled,
          "dynamic alignment cancellation status");
}
} // namespace
int main() {
    try { QueryGeometry(); QueryIndex(); SurfaceAlignment(); }
    catch (const std::exception &error) { ++failures; std::cerr << error.what() << '\n'; }
    return failures == 0 ? 0 : 1;
}
