#include "AlignmentPrealign.h"
#include "AlignmentPrealignTestSupport.h"
#include "AlignmentSolver.h"
#include "AlignmentMath.h"
#include <iostream>

namespace {
using namespace AlignmentTestModels;
int failures = 0;
// 解析刚体/平面双精度数值回归容差，不是扫描精度或性能门限。
constexpr double numericTolerance = 1e-7;
void Check(bool value, const char *message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
void Features() {
    auto w = Work();
    w.recipe.prealignment->featureRadius = 10;
    AlignmentFeatureCloud plane;
    plane.points = {{0,0,0}, {1,0,0}, {0,1,0}};
    plane.normals = {{0,0,1}, {0,0,1}, {0,0,1}};
    AlignmentPrealign::BuildFeatures(plane, w, false);
    for (std::size_t i = 0; i < plane.points.size(); ++i) {
        Check(plane.valid[i] != 0, "planar descriptor valid");
        for (std::size_t bin = 0; bin < 33; ++bin) {
            const double expected = bin % 11 == 5 ? 200 : 0;
            Check(std::abs(plane.descriptors[i][bin] - expected) < numericTolerance,
                  "analytic planar FPFH bins and self-plus-neighbor normalization");
        }
    }
    AlignmentFeatureCloud cloud;
    cloud.points = {{-1,0,0}, {1,0,0}, {0,2,0}, {0,0,3}, {1,1,1}};
    const auto n = 1 / std::sqrt(3.0);
    cloud.normals = {{-1,0,0}, {1,0,0}, {0,1,0}, {0,0,1}, {n,n,n}};
    auto moved = cloud;
    for (auto &p : moved.points) p = {-p[1] + 4, p[0] - 7, p[2] + 2};
    for (auto &v : moved.normals) v = {-v[1], v[0], v[2]};
    AlignmentPrealign::BuildFeatures(cloud, w, false);
    AlignmentPrealign::BuildFeatures(moved, w, false);
    for (std::size_t i = 0; i < cloud.points.size(); ++i) {
        Check(cloud.valid[i] == moved.valid[i], "rigid motion preserves descriptor validity");
        for (std::size_t j = 0; j < 33; ++j)
            Check(std::abs(cloud.descriptors[i][j] - moved.descriptors[i][j]) < numericTolerance,
                  "FPFH invariant under analytic quarter-turn and translation");
    }
    AlignmentFeatureCloud duplicates;
    duplicates.points = {{0,0,0}, {0,0,0}}; duplicates.normals = {{0,0,1}, {0,0,1}};
    AlignmentPrealign::BuildFeatures(duplicates, w, false);
    Check(duplicates.valid == std::vector<std::uint8_t>{0,0}, "zero-distance pairs do not fabricate a descriptor");
    AlignmentFeatureCloud line;
    line.points = {{0,0,0}, {.1,0,0}, {.2,0,0}, {.3,0,0}, {.4,0,0}};
    line.normals.assign(line.points.size(), {0,0,1});
    AlignmentPrealign::BuildFeatures(line, w);
    Check(std::none_of(line.valid.begin(), line.valid.end(), [](auto v) { return v != 0; }),
          "collinear PCA neighborhoods are rejected");
    AlignmentFeatureCloud regular;
    for (int y = 0; y < 10; ++y) for (int x = 0; x < 10; ++x) {
        regular.points.push_back({x*.1, y*.1, 0}); regular.normals.push_back({0,0,1});
    }
    // 纯描述子阶段不创建匹配表；增大后续 matchCount 不能使该阶段多收费。

    w.recipe.prealignment->matchCount = 1;
    AlignmentPrealign::BuildFeatures(regular, w, false);
    const auto values = regular.descriptors;
    w.recipe.prealignment->matchCount = 64;
    AlignmentPrealign::BuildFeatures(regular, w, false);
    Check(regular.descriptors == values, "feature-only budget excludes unused matching tables");
    regular.normals.assign(regular.points.size(), {});
    AlignmentPrealign::BuildFeatures(regular, w);
    Check(std::none_of(regular.valid.begin(), regular.valid.end(), [](auto v) { return v != 0; }),
          "normal estimation does not invent orientation without a valid mesh hint");
}
void AutomaticAlignment() {
    auto w = Model();
    const auto measured = w.mesh->GetPointValues();
    const auto seeds = AlignmentPrealign::BuildCandidates(w);
    std::cerr << "prealignment: " << seeds.statistics.sourceSamples << '/' << seeds.statistics.referenceSamples
              << " samples, " << seeds.statistics.sourceFeatures << '/' << seeds.statistics.referenceFeatures
              << " features, " << seeds.poses.size() << " candidates, coverage=" << seeds.statistics.bestCoverage << '\n';
    Check(!seeds.poses.empty() && seeds.poses.size() <= 8, "FPFH generates bounded rigid candidates");
    const auto repeated = AlignmentPrealign::BuildCandidates(w);
    Check(seeds.poses == repeated.poses, "same recipe and seed replay candidate generation");
    auto result = AlignmentSolver::BuildResult(w);
    std::cerr << "automatic solver: " << static_cast<int>(result.diagnostics.status) << ' '
              << result.diagnostics.message << " rms=" << result.diagnostics.rms << '\n';
    Check(result.hasSolution && result.diagnostics.status == AlignmentStatus::FullyDetermined &&
          result.diagnostics.isQualityPassed && result.diagnostics.prealignment && !result.initialPoses.empty(),
          "automatic seeds pass through formal constrained solver");
    const auto truth = Truth();
    for (std::size_t i = 0; i < truth.size(); ++i)
        Check(std::abs(result.sourceToTarget[i] - truth[i]) < numericTolerance,
              "large initial pose recovered from asymmetric surface");
    Check(w.mesh->GetPointValues() == measured, "automatic work sampling does not mutate measured surface");
    auto noAutomatic = w; noAutomatic.recipe.prealignment.reset();
    const auto uninitialized = AlignmentSolver::BuildResult(noAutomatic);
    Check(uninitialized.diagnostics.status != AlignmentStatus::FullyDetermined || !uninitialized.diagnostics.isQualityPassed,
          "fixture requires prealignment instead of already being locally aligned");

    w.recipe.surfaceFit.reset(); w.recipe.method = AlignmentMethod::Rps;
    const auto &nominal = w.referenceMesh->GetPointValues();
    for (const std::uint64_t id : {0,12,130}) {
        AlignmentGeometrySpec geometry;
        geometry.id = "point-" + std::to_string(id); geometry.kind = AlignmentGeometryKind::Point;
        geometry.region.pinnedMesh = w.input.mesh; geometry.region.vertexIds = {id};
        w.recipe.geometries.push_back(geometry);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            AlignmentConstraint constraint;
            constraint.geometryIndex = w.recipe.geometries.size()-1;
            constraint.nominalPoint = {nominal[3*id], nominal[3*id+1], nominal[3*id+2]};
            constraint.targetDirection = {}; constraint.targetDirection[axis] = 1;
            w.recipe.constraints.push_back(constraint);
        }
    }
    result = AlignmentSolver::BuildResult(w);
    Check(result.diagnostics.status == AlignmentStatus::FullyDetermined && result.diagnostics.prealignment,
          "automatic prealignment also feeds engineering RPS entry");
    for (std::size_t i = 0; i < truth.size(); ++i)
        Check(std::abs(result.sourceToTarget[i] - truth[i]) < numericTolerance, "RPS retains known nominal engineering constraints");
    w.poses.front()[3] = 1;
    Check(AlignmentSolver::BuildResult(w).diagnostics.status == AlignmentStatus::InvalidInput,
          "automatic stage does not ignore manually supplied initial pose");
}
void FailureAndAmbiguity() {
    auto w = Model();
    w.cancelled->store(true);
    Check(AlignmentSolver::BuildResult(w).diagnostics.status == AlignmentStatus::Cancelled, "prealignment cancellation");
    w.cancelled->store(false);
    w.recipe.prealignment->normalRadius = 1e-5;
    const auto failed = AlignmentSolver::BuildResult(w);
    Check(!failed.hasSolution && failed.diagnostics.status == AlignmentStatus::NotConverged,
          "missing descriptors do not fabricate a formal identity transform");
    w = Work();
    w.recipe.surfaceFit->isPointToPlane = false;
    w.recipe.surfaceFit->iterationLimit = 200;
    w.recipe.prealignment->sampleSpacing = .2;
    w.recipe.prealignment->normalRadius = .6;
    w.recipe.prealignment->featureRadius = 1;
    w.recipe.prealignment->maxPairDistance = .4;
    w.recipe.prealignment->matchCount = 16;
    w.recipe.prealignment->iterationLimit = 12000;
    SetMeshes(w, {1,0,0, -1,0,0, 0,1,0, 0,-1,0, 0,0,1, 0,0,-1},
                 {0,2,4, 2,1,4, 1,3,4, 3,0,4, 2,0,5, 1,2,5, 3,1,5, 0,3,5});
    const auto symmetric = AlignmentSolver::BuildResult(w);
    std::cerr << "symmetric solver: " << static_cast<int>(symmetric.diagnostics.status) << ' '
              << symmetric.diagnostics.message << '\n';
    Check(symmetric.diagnostics.status == AlignmentStatus::Ambiguous && !symmetric.diagnostics.isQualityPassed,
          "symmetric surface retains multiple valid poses instead of choosing arbitrary alignment");
}
void SelectionAndRepresentation() {
    auto w = Model();
    std::vector<std::uint64_t> triangles(w.referenceMesh->GetTriangleIndices().begin(),w.referenceMesh->GetTriangleIndices().end());
    for (std::size_t i = 0; i < triangles.size() / 6; ++i)
        for (std::size_t j = 0; j < 3; ++j)
            std::swap(triangles[3*i+j], triangles[triangles.size()-3*(i+1)+j]);
    w.referenceMesh = std::make_shared<const SurfaceMeshPayload>(w.referenceMesh->GetPointValues(),MeshReadArray<std::uint64_t>(std::move(triangles)),std::vector<MeshField>{},w.referenceMesh->GetCoordinateFrame());
    const auto reordered = AlignmentSolver::BuildResult(w);
    std::cerr << "reordered reference: " << static_cast<int>(reordered.diagnostics.status) << ' '
              << reordered.diagnostics.message << " rms=" << reordered.diagnostics.rms << '\n';
    Check(reordered.diagnostics.status == AlignmentStatus::FullyDetermined && reordered.diagnostics.isQualityPassed,
          "triangle reordering changes work samples but preserves recoverable geometry");
    const auto truth = Truth();
    for (std::size_t i = 0; i < truth.size(); ++i)
        Check(std::abs(reordered.sourceToTarget[i] - truth[i]) < numericTolerance,
              "reordered reference recovers same analytic pose");
    w.recipe.prealignment->sourceTriangles = {0}; w.recipe.exactMesh = Ref(99);
    Check(AlignmentSolver::BuildResult(w).diagnostics.status == AlignmentStatus::InvalidInput,
          "prealignment source triangle selection pinned to exact mesh");
    w.recipe.exactMesh = w.input.mesh; w.recipe.prealignment->sourceTriangles = {0,0};
    Check(AlignmentSolver::BuildResult(w).diagnostics.status == AlignmentStatus::InvalidInput,
          "duplicate prealignment triangle selection rejected");
    w.recipe.prealignment->sourceTriangles.clear();
    w.recipe.prealignment->referenceTriangles = {w.referenceMesh->GetTriangleIndices().size()/3};
    Check(AlignmentSolver::BuildResult(w).diagnostics.status == AlignmentStatus::InvalidInput,
          "reference triangle index bounds checked");
    w = Work();
    const std::vector<double> square{0,0,0, 1,0,0, 0,1,0, 1,1,0};
    const std::vector<std::uint64_t> faces{0,1,2, 1,3,2};
    w.mesh = std::make_shared<const SurfaceMeshPayload>(square, faces,
        std::vector<MeshAttribute>{{"measurement.valid", 1, {0,1,1,1}}});
    w.referenceMesh = std::make_shared<const SurfaceMeshPayload>(square, faces);
    const auto filtered = AlignmentPrealign::BuildCandidates(w);
    Check(filtered.statistics.sourceRejectedTriangles == 1 &&
          std::abs(filtered.statistics.sourceSelectedArea - 1) < numericTolerance &&
          std::abs(filtered.statistics.sourceRejectedArea - .5) < numericTolerance,
          "quality-filtered source area is reported rather than hidden by work sampling");
    w.mesh = std::make_shared<const SurfaceMeshPayload>(square, faces, std::vector<MeshAttribute>{}, "RAS",
        std::vector<MeshAttribute>{{"measurement.valid",1,{0,1}}});
    const auto cellFiltered = AlignmentPrealign::BuildCandidates(w);
    Check(cellFiltered.statistics.sourceRejectedTriangles == 1 &&
          std::abs(cellFiltered.statistics.sourceSelectedArea-1) < numericTolerance &&
          std::abs(cellFiltered.statistics.sourceRejectedArea-.5) < numericTolerance,
          "FPFH rejects invalid cells even when their vertices are usable");
    w.mesh = std::make_shared<const SurfaceMeshPayload>(square, std::vector<std::uint64_t>{0,1,2,1,1,1},
        std::vector<MeshAttribute>{}, "RAS", std::vector<MeshAttribute>{{"measurement.valid",1,{1,0}}});
    const auto invalidDegenerate = AlignmentPrealign::BuildCandidates(w);
    Check(invalidDegenerate.statistics.sourceRejectedTriangles == 1,
          "explicitly invalid degenerate cell cannot block valid prealignment support");
    w.recipe.prealignment->sourceTriangles = {0};
    w.recipe.exactMesh = w.input.mesh;
    w.mesh = std::make_shared<const SurfaceMeshPayload>(square, std::vector<std::uint64_t>{0,1,2,1,1,1});
    Check(AlignmentPrealign::BuildCandidates(w).statistics.sourceRejectedTriangles == 0,
          "unselected degenerate cells do not block local prealignment");
}
} // namespace
int main() {
    try { Features(); AutomaticAlignment(); FailureAndAmbiguity(); SelectionAndRepresentation(); }
    catch (const std::exception &error) { ++failures; std::cerr << error.what() << '\n'; }
    return failures ? 1 : 0;
}
