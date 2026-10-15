#include "AlignmentMeshQuery.h"
#include "AlignmentSolver.h"
#include "AlignmentMath.h"
#include "AlignmentSurfaceSamples.h"
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
    const auto original = mesh->GetPointValues();
    AlignmentMeshQuery query(mesh, work);
    const auto inside = query.GetClosest({1, 1, 3});
    Check(inside.isValid && std::abs(inside.distanceSquared - 9) < numericTolerance &&
          inside.point == AlignmentPoint{1, 1, 0}, "closest point lies inside triangle");
    const auto edge = query.GetClosest({3, 3, 1});
    Check(edge.isValid && edge.point == AlignmentPoint{2, 2, 0} &&
          std::abs(edge.distanceSquared - 3) < numericTolerance, "closest point lies on edge");
    Check(query.GetClosest({-1, -2, 0}).point == AlignmentPoint{0, 0, 0}, "closest vertex region");
    Check(mesh->GetPointValues() == original, "query leaves original positions unchanged");
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
    const auto positions = work.mesh->GetPointValues();
    auto result = AlignmentSolver::BuildResult(work);
    std::cerr << "surface status " << static_cast<int>(result.diagnostics.status) << ' '
              << result.diagnostics.message << '\n';
    Check(result.diagnostics.status == AlignmentStatus::FullyDetermined, "dynamic ICP fully determined");
    Check(std::abs(result.sourceToTarget[3] - 0.1) < numericTolerance &&
          std::abs(result.sourceToTarget[7] + 0.2) < numericTolerance &&
          std::abs(result.sourceToTarget[11] - 0.15) < numericTolerance &&
          std::abs(result.sourceToTarget[1] + std::sin(0.02)) < numericTolerance,
          "dynamic plane correspondences recover analytic rigid transform");
    Check(work.mesh->GetPointValues() == positions, "ICP preserves source mesh coordinates");
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
AlignmentWork DensePlanes() {
    auto work = Planes();
    std::vector<double> vertices;
    std::vector<std::uint64_t> triangles;
    constexpr int n = 60;
    const double c = std::cos(0.02), s = std::sin(0.02);
    for (int face = 0; face < 3; ++face) {
        const auto start = vertices.size() / 3;
        for (int y = 0; y <= n; ++y) for (int x = 0; x <= n; ++x) {
            const double u = 2 + 6.0 * x / n, v = 2 + 6.0 * y / n;
            const AlignmentPoint q = face == 0 ? AlignmentPoint{u,v,0}
                : face == 1 ? AlignmentPoint{0,u,v} : AlignmentPoint{v,0,u};
            const double px = q[0] - .1, py = q[1] + .2;
            vertices.insert(vertices.end(), {c*px+s*py, -s*px+c*py, q[2]-.15});
        }
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
            const auto a = static_cast<std::uint64_t>(start + y*(n+1) + x);
            const auto b = a + 1, d = a + n + 1, e = d + 1;
            triangles.insert(triangles.end(), {a,b,d, b,e,d});
        }
    }
    work.mesh = std::make_shared<const SurfaceMeshPayload>(std::move(vertices), std::move(triangles));
    return work;
}
void MultiscaleAlignment() {
    auto work = DensePlanes();
    const auto original = work.mesh->GetPointValues();
    Check(AlignmentSolver::BuildResult(work).diagnostics.status == AlignmentStatus::FullyDetermined,"dense equations run without a resource quota");
    work.recipe.surfaceFit->scales = {{1,2,40},{.5,.1,40}};
    const auto result = AlignmentSolver::BuildResult(work);
    std::cerr << "multiscale status " << static_cast<int>(result.diagnostics.status) << ' '
              << result.diagnostics.message << '\n';
    Check(result.diagnostics.status == AlignmentStatus::FullyDetermined && result.diagnostics.isQualityPassed,
          "bounded multiscale fit solves dense surface");
    Check(std::abs(result.sourceToTarget[3]-.1) < numericTolerance &&
          std::abs(result.sourceToTarget[7]+.2) < numericTolerance &&
          std::abs(result.sourceToTarget[11]-.15) < numericTolerance,
          "multiscale fit recovers analytic translation");
    const auto &stages = result.diagnostics.surfaceStages;
    Check(stages.size() == 2, "all explicit surface stages recorded");
    if (stages.size() == 2) {
        Check(stages[0].samples < stages[1].samples && stages[1].samples < stages[1].selectedVertices &&
              stages[1].selectedVertices == original.size()/3 && stages[1].isConverged,
              "diagnostics distinguish full selection from final working sample count");
        Check(std::abs(stages[0].selectedArea-108) < numericTolerance &&
              std::abs(stages[1].acceptedArea-108) < numericTolerance,
              "coarsening preserves analytic selected surface area");
    }
    Check(work.mesh->GetPointValues() == original, "multiscale keeps full source immutable");
    work.recipe.surfaceFit->scales.push_back({0,.1,40});
    Check(AlignmentSolver::BuildResult(work).diagnostics.status == AlignmentStatus::FullyDetermined,
          "full final stage runs without a point quota");
    work = Planes(.5);
    AlignmentGeometrySpec datum; datum.id = "A";
    datum.association = AlignmentAssociation::SequentialLeastSquares;
    datum.region.pinnedMesh = work.input.mesh; datum.region.vertexIds = {0,1,2,3,4,5};
    work.recipe.geometries = {datum}; work.recipe.datumCount = 1;
    work.recipe.surfaceFit->scales = {{1,2,40},{0,1,40}};
    const auto constrained = AlignmentSolver::BuildResult(work);
    Check(constrained.diagnostics.status == AlignmentStatus::FullyDetermined &&
          std::abs(constrained.sourceToTarget[11]-.15) < numericTolerance,
          "multiscale preserves primary datum across stages");
    work.recipe.surfaceFit->scales = {{1,1,40},{2,1,40}};
    Check(!AlignmentGeometryFit::GetRecipeValid(work.recipe, work.config), "increasing voxel size rejected");
    work.recipe.surfaceFit->scales = {{1,1,40},{0,3,40}};
    Check(!AlignmentGeometryFit::GetRecipeValid(work.recipe, work.config), "widening final distance rejected");
}
void SurfaceGrouping() {
    auto work = Work();
    std::vector<double> vertices, normals, valid, interfaces, overrides;
    std::vector<std::uint64_t> triangles;
    for (int group = 0; group < 5; ++group) {
        const double z = .01 * group;
        vertices.insert(vertices.end(), {0,0,z, .1,0,z, 0,.1,z});
        for (int corner = 0; corner < 3; ++corner) {
            triangles.push_back(static_cast<std::uint64_t>(group*3+corner));
            normals.insert(normals.end(), {0,0,group == 1 ? -1.0 : 1.0});
            valid.push_back(group == 2 ? 0 : 1);
            interfaces.push_back(group == 3 ? 1 : 0);
            overrides.push_back(group == 4 ? 1 : 0);
        }
    }
    work.mesh = std::make_shared<const SurfaceMeshPayload>(vertices, triangles,
        std::vector<MeshAttribute>{{"measurement.normal",3,normals},{"measurement.valid",1,valid},
            {"surface.interface-index",1,interfaces},{"surface.override-index",1,overrides}});
    const auto dense = AlignmentSurfaceSampling::Build(work, alignmentIdentity, 0, true);
    const auto reduced = AlignmentSurfaceSampling::Reduce(work, dense, 1, 0);
    Check(reduced.size() == 5, "same voxel separates opposite normals, invalid samples and interface identities");
    double area = 0;
    for (const auto &sample : reduced) {
        area += sample.area;
        Check(sample.vertex % 3 == 0, "representative is stable original vertex");
    }
    Check(std::abs(area-.025) < numericTolerance, "grouping conserves selected area including invalid area");
    AlignmentRegion region; region.targetBounds = std::array<double,6>{-.1,.2,-.1,.2,-.001,.011};
    work.recipe.surfaceFit->sourceRegion = region;
    const auto roi = AlignmentSurfaceSampling::Build(work, alignmentIdentity, 0, true);
    Check(roi.points.size() == 6 && AlignmentSurfaceSampling::Reduce(work, roi, 1, 0).size() == 2,
          "ROI grouping uses only original selected vertices");
    work.cancelled->store(true);
    bool cancelled = false;
    try { (void)AlignmentSurfaceSampling::Reduce(work, dense, 1, 0); }
    catch (const std::runtime_error &) { cancelled = true; }
    Check(cancelled, "surface reduction observes cancellation");
}
void SelectedCellQuality() {
    auto work=Work();
    const std::vector<double> vertices{0,0,0, 1,0,0, 0,1,0, 10,0,0, 10,0,0, 10,0,0};
    const std::vector<std::uint64_t> triangles{0,1,2, 3,4,5};
    work.mesh=std::make_shared<const SurfaceMeshPayload>(vertices,triangles);
    work.recipe.surfaceFit->sourceRegion=AlignmentRegion{work.input.mesh,{0,1,2},{}};
    const auto selected=AlignmentSurfaceSampling::Build(work,alignmentIdentity,0,false);
    double area=0;for(const auto& p:selected.points)area+=p.area;
    Check(selected.points.size()==3 && std::abs(area-.5)<numericTolerance,
          "outside-selection degenerate triangles cannot invalidate selected surface area");
    work.mesh=std::make_shared<const SurfaceMeshPayload>(std::vector<double>{0,0,0, 1,0,0, 0,1,0},
        std::vector<std::uint64_t>{0,1,2, 0,0,1},std::vector<MeshAttribute>{},"RAS",
        std::vector<MeshAttribute>{{"measurement.valid",1,{1,0}}});
    work.recipe.requiresMeasurementQuality=true;
    const auto quality=AlignmentSurfaceSampling::Build(work,alignmentIdentity,0,false);
    area=0;for(const auto& p:quality.points)area+=p.area;
    Check(std::abs(area-.5)<numericTolerance,"explicitly invalid measured cells do not add area or block valid selected cells");
    work.mesh=std::make_shared<const SurfaceMeshPayload>(std::vector<double>{0,0,0, 1,0,0, 0,1,0},
        std::vector<std::uint64_t>{0,1,2, 0,0,1});
    bool rejected=false;try{(void)AlignmentSurfaceSampling::Build(work,alignmentIdentity,0,false);}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"selected degenerate triangle without explicit invalid quality is still rejected");
    work.mesh=std::make_shared<const SurfaceMeshPayload>(std::vector<double>{0,0,0, 1,0,0, 0,1,0},
        std::vector<std::uint64_t>{0,1,2},std::vector<MeshAttribute>{},"RAS",
        std::vector<MeshAttribute>{{"measurement.valid",2,{1,1}}});
    rejected=false;try{(void)AlignmentSurfaceSampling::Build(work,alignmentIdentity,0,false);}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"invalid measured cell quality schema is rejected");
}
void DynamicObservability() {
    auto work = Work();
    work.mesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{2,2,0, 3,2,0, 2,3,0}, std::vector<std::uint64_t>{0,1,2});
    work.referenceMesh = Triangle();
    // 小面完全处于大平面内部。面内平移和绕法向旋转都不改变曲面距离。
    work.referenceMesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{0,0,0, 20,0,0, 0,20,0}, std::vector<std::uint64_t>{0,1,2});
    work.recipe.surfaceFit->isPointToPlane = false;
    for (const auto pose : {alignmentIdentity, AlignmentMatrix{std::cos(.1),-std::sin(.1),0,1,
         std::sin(.1),std::cos(.1),0,0, 0,0,1,0, 0,0,0,1}}) {
        work.poses = {pose};
        const auto result = AlignmentSolver::BuildResult(work);
        Check(result.diagnostics.status == AlignmentStatus::Underconstrained && result.diagnostics.remaining == 3,
              "dynamic point-to-point planar ICP cannot turn closest points into fixed correspondences");
    }
    work.recipe.surfaceFit.reset(); work.recipe.exactMesh = work.input.mesh; work.poses = {alignmentIdentity};
    work.recipe.fitPairs = {{0,{2,2,0},{},1}, {1,{3,2,0},{},1}, {2,{2,3,0},{},1}};
    Check(AlignmentSolver::BuildResult(work).diagnostics.status == AlignmentStatus::FullyDetermined,
          "explicit fixed point pairs still determine a planar rigid pose");
    AlignmentMeshQuery query(Triangle(), work);
    Check(query.GetClosest({1,1,2},true).observableCount == 1 && query.GetClosest({3,3,1},true).observableCount == 2 &&
          query.GetClosest({-1,-2,1},true).observableCount == 3 && query.GetClosest({0,0,0},true).observableCount == 1,
          "interior, strict edge and vertex exterior, and zero-distance boundary have distinct observability");
}
void FaceCoverageAndPoseDistance() {
    auto work = Work();
    work.mesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{0,0,0, 1,0,0, 0,1,0, 3,0,0, 4,0,0, 3,1,0},
        std::vector<std::uint64_t>{0,1,2,3,4,5}, std::vector<MeshAttribute>{}, "RAS",
        std::vector<MeshAttribute>{{"measurement.valid",1,{1,0}}});
    const auto samples = AlignmentSurfaceSampling::Build(work, alignmentIdentity, 0, false);
    double area = 0, validArea = 0;
    for (const auto &sample : samples.points) { area += sample.area; validArea += sample.validArea; }
    Check(std::abs(area-1) < numericTolerance && std::abs(validArea-.5) < numericTolerance,
          "ICP keeps invalid selected area in coverage but excludes it from fit weight without requiring point quality");
    AlignmentMeshQuery query(work.mesh, work);
    Check(query.GetClosest({3.1,.1,0}).triangle == 0, "invalid cell is excluded from nearest-face query");
    for (const auto center : {AlignmentMath::Vec{}, AlignmentMath::Vec(100,200,300)}) {
        const double angle = 1e-9;
        AlignmentMatrix pose{std::cos(angle),-std::sin(angle),0,0, std::sin(angle),std::cos(angle),0,0,
                             0,0,1,0, 0,0,0,1};
        const auto translation = center - AlignmentMath::Rotate(pose, center);
        pose[3]=translation[0]; pose[7]=translation[1]; pose[11]=translation[2];
        Check(AlignmentMath::GetPoseDistance(pose, alignmentIdentity, center, std::sqrt(2.0)) < numericTolerance,
              "pose equivalence uses actual part displacement at any coordinate origin");
        auto pairs = Work(); pairs.recipe.surfaceFit.reset(); pairs.recipe.exactMesh = pairs.input.mesh;
        std::vector<double> vertices;
        for (const auto offset : {AlignmentMath::Vec{}, AlignmentMath::Vec(1,0,0),
                                 AlignmentMath::Vec(0,1,0), AlignmentMath::Vec(0,0,1)}) {
            const auto point = AlignmentMath::Point(center + offset);
            pairs.recipe.fitPairs.push_back({vertices.size()/3,point,{},1});
            vertices.insert(vertices.end(), point.begin(), point.end());
        }
        pairs.mesh = std::make_shared<const SurfaceMeshPayload>(std::move(vertices), std::vector<std::uint64_t>{});
        pairs.poses = {alignmentIdentity, pose};
        Check(AlignmentSolver::BuildResult(pairs).diagnostics.status == AlignmentStatus::FullyDetermined,
              "near-identical converged candidates are not Ambiguous when the part is shifted from the origin");
    }
}
} // namespace
int main() {
    try { QueryGeometry(); QueryIndex(); SurfaceAlignment(); MultiscaleAlignment(); SurfaceGrouping(); SelectedCellQuality();
          DynamicObservability(); FaceCoverageAndPoseDistance(); }
    catch (const std::exception &error) { ++failures; std::cerr << error.what() << '\n'; }
    return failures == 0 ? 0 : 1;
}
