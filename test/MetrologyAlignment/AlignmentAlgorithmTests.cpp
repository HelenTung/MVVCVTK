#include "AlignmentSolver.h"
#include "AlignmentMath.h"
#include <cmath>
#include <iostream>

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
    w.input = {Ref(1), Ref(2), Ref(3), "scan-1", "RAS", "part-1", AlignmentUnit::ModelUnit};
    w.recipe.id = "fixture";
    w.recipe.targetFrameId = "workpiece";
    w.recipe.nominalData = Ref(4);
    w.cancelled = std::make_shared<std::atomic<bool>>(false);
    w.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    w.poses = {alignmentIdentity};
    return w;
}
void Sequential() {
    auto w = Work();
    std::vector<double> vertices;
    for (int face = 0; face < 3; ++face) {
        AlignmentGeometrySpec spec;
        spec.id = std::to_string(face);
        spec.association = AlignmentAssociation::SequentialLeastSquares;
        spec.region.pinnedMesh = w.input.mesh;
        spec.targetDirection = face == 0   ? AlignmentPoint{0, 0, 1}
                               : face == 1 ? AlignmentPoint{0, 1, 0}
                                           : AlignmentPoint{1, 0, 0};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                AlignmentPoint p = face == 0   ? AlignmentPoint{double(i), double(j), 30}
                                   : face == 1 ? AlignmentPoint{double(i), 20, double(j)}
                                               : AlignmentPoint{10, double(i), double(j)};
                spec.region.vertexIds.push_back(vertices.size() / 3);
                vertices.insert(vertices.end(), p.begin(), p.end());
            }
        w.recipe.geometries.push_back(spec);
    }
    w.mesh = std::make_shared<const SurfaceMeshPayload>(vertices, std::vector<std::uint64_t>{});
    auto r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.status == AlignmentStatus::FullyDetermined, "sequential fully determined");
    Check(r.diagnostics.remaining == 0, "sequential rank six");
    Check(std::abs(r.sourceToTarget[3] + 10) < 1e-7 && std::abs(r.sourceToTarget[7] + 20) < 1e-7 &&
              std::abs(r.sourceToTarget[11] + 30) < 1e-7,
          "sequential translation");
    Check(r.diagnostics.stageRemaining == std::vector<std::size_t>{3, 1, 0}, "sequential stages");
    w.recipe.datumCount = 1;
    w.recipe.geometries.resize(1);
    r = AlignmentSolver::BuildResult(w);
    Check(r.diagnostics.status == AlignmentStatus::Underconstrained && r.diagnostics.remaining == 3,
          "single plane three DOF");
}
void Rps() {
    auto w = Work();
    w.recipe.method = AlignmentMethod::Rps;
    w.recipe.datumCount = 0;
    const double c = std::cos(0.2), s = std::sin(0.2);
    const AlignmentMatrix truth{c, -s, 0, 2, s, c, 0, -3, 0, 0, 1, 4, 0, 0, 0, 1};
    std::vector<AlignmentPoint> points{{0, 0, 0}, {2, 0, 0}, {0, 3, 0}, {0, 0, 4}};
    std::vector<double> vertices;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto p = points[i];
        vertices.insert(vertices.end(), p.begin(), p.end());
        AlignmentGeometrySpec g;
        g.id = std::to_string(i);
        g.kind = AlignmentGeometryKind::Point;
        g.region.pinnedMesh = w.input.mesh;
        g.region.vertexIds = {i};
        w.recipe.geometries.push_back(g);
        for (int axis = 0; axis < 3; ++axis) {
            AlignmentConstraint e;
            e.geometryIndex = i;
            e.nominalPoint = Point(Transform(truth, Vector(p)));
            e.targetDirection = {0, 0, 0};
            e.targetDirection[axis] = 1;
            w.recipe.constraints.push_back(e);
        }
    }
    w.mesh = std::make_shared<const SurfaceMeshPayload>(vertices, std::vector<std::uint64_t>{});
    auto result = AlignmentSolver::BuildResult(w);
    Check(result.diagnostics.status == AlignmentStatus::FullyDetermined, "RPS transform recovery");
    for (std::size_t i = 0; i < 16; ++i)
        Check(std::abs(result.sourceToTarget[i] - truth[i]) < 1e-6, "known matrix entry");
    const auto roundtrip = Multiply(Inverse(result.sourceToTarget), result.sourceToTarget);
    for (std::size_t i = 0; i < 16; ++i)
        Check(std::abs(roundtrip[i] - alignmentIdentity[i]) < 1e-8, "inverse roundtrip");
    w.recipe.constraints.push_back(w.recipe.constraints.front());
    result = AlignmentSolver::BuildResult(w);
    Check(result.diagnostics.remaining == 0, "duplicate constraint does not increase rank");
    w.recipe.constraints.back().nominalPoint[0] += 1;
    result = AlignmentSolver::BuildResult(w);
    Check(result.diagnostics.status == AlignmentStatus::Conflicting, "hard conflict rejected");
    w.cancelled->store(true);
    result = AlignmentSolver::BuildResult(w);
    Check(result.diagnostics.status == AlignmentStatus::Cancelled, "cancelled request");
}
void Shapes() {
    auto w = Work();
    for (auto kind : {AlignmentGeometryKind::Circle, AlignmentGeometryKind::Sphere,
                      AlignmentGeometryKind::Cylinder}) {
        AlignmentSamples samples;
        for (int a = 0; a < 16; ++a)
            for (int b = 0; b < (kind == AlignmentGeometryKind::Circle ? 1 : 7); ++b) {
                const double angle = 6.283185307179586 * a / 16;
                AlignmentPoint p{};
                if (kind == AlignmentGeometryKind::Sphere) {
                    const double polar = 3.141592653589793 * (b + 1) / 8;
                    p = {2 * std::sin(polar) * std::cos(angle) + 4,
                         2 * std::sin(polar) * std::sin(angle) - 3, 2 * std::cos(polar) + 5};
                } else
                    p = {2 * std::cos(angle) + 4, 2 * std::sin(angle) - 3,
                         kind == AlignmentGeometryKind::Circle ? 5.0 : double(b)};
                samples.points.push_back(p);
            }
        AlignmentGeometrySpec spec;
        spec.id = "round";
        spec.kind = kind;
        spec.maxFitRms = 1e-5;
        try {
            const auto g = AlignmentGeometryFit::BuildGeometry(w, spec, samples, alignmentIdentity);
            Check(std::abs(g.radius - 2) < 1e-6, "round fitted radius");
            Check(g.fitRms < 1e-7, "round fitted residual");
        } catch (const std::exception &e) {
            ++failures;
            std::cerr << "Shape exception: " << e.what() << '\n';
        }
    }
}
} // namespace
int TestConstraints();
int main() {
    Sequential();
    Rps();
    Shapes();
    return failures + TestConstraints() ? 1 : 0;
}
