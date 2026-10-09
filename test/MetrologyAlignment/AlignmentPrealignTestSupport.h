#pragma once
#include "AlignmentGeometry.h"
#include <cmath>

namespace AlignmentTestModels {
inline DataRevisionRef Ref(unsigned char id) {
    DataRevisionRef r; r.entityId.bytes[0] = id; r.generation = 1; return r;
}
inline AlignmentWork Work() {
    AlignmentWork w;
    w.input = {Ref(1), Ref(2), Ref(3), "scan", "RAS", "part", AlignmentUnit::ModelUnit};
    w.recipe.id = "automatic"; w.recipe.targetFrameId = "nominal"; w.recipe.nominalData = Ref(4);
    w.recipe.method = AlignmentMethod::ConstrainedBestFit; w.recipe.datumCount = 0;
    w.recipe.surfaceFit = AlignmentSurfaceFit{};
    w.recipe.prealignment = AlignmentPrealignParams{};
    auto &p = *w.recipe.prealignment;
    p.sampleSpacing = 0.18; p.normalRadius = 0.65; p.featureRadius = 1.2;
    p.maxPairDistance = 0.35; p.minCoverage = 0.8; p.randomSeed = 419;
    p.iterationLimit = 4000;
    w.recipe.maxPairDistance = 0.8; w.recipe.maxAlignmentRms = 1e-6;
    w.cancelled = std::make_shared<std::atomic<bool>>(false);

    w.poses = {alignmentIdentity};
    return w;
}
inline AlignmentMatrix Truth() {
    const double a = 0.73, b = -0.48;
    return {std::cos(a), -std::sin(a)*std::cos(b), std::sin(a)*std::sin(b), 8,
            std::sin(a), std::cos(a)*std::cos(b), -std::cos(a)*std::sin(b), -6,
            0, std::sin(b), std::cos(b), 3, 0,0,0,1};
}
inline void SetMeshes(AlignmentWork &w, std::vector<double> target, std::vector<std::uint64_t> triangles) {
    auto measured = target;
    const auto truth = Truth();
    for (std::size_t i = 0; i < measured.size(); i += 3) {
        const double x = target[i] - 8, y = target[i+1] + 6, z = target[i+2] - 3;
        for (std::size_t j = 0; j < 3; ++j)
            measured[i+j] = truth[j]*x + truth[4+j]*y + truth[8+j]*z;
    }
    w.mesh = std::make_shared<const SurfaceMeshPayload>(std::move(measured), triangles);
    w.referenceMesh = std::make_shared<const SurfaceMeshPayload>(std::move(target), std::move(triangles));
}
inline AlignmentWork Model() {
    auto w = Work();
    std::vector<double> points;
    std::vector<std::uint64_t> triangles;
    constexpr std::size_t nx = 12, ny = 10;
    for (std::size_t j = 0; j <= ny; ++j) for (std::size_t i = 0; i <= nx; ++i) {
        const double x = static_cast<double>(i)*.25, y = static_cast<double>(j)*.25;
        const double z = .12*x*x + .06*x*y + .15*std::sin(1.3*x)*std::cos(.7*y) + .05*y*y*y;
        points.insert(points.end(), {x,y,z});
    }
    for (std::size_t j = 0; j < ny; ++j) for (std::size_t i = 0; i < nx; ++i) {
        const auto a = j*(nx+1)+i, b = a+1, c = a+nx+1, d = c+1;
        triangles.insert(triangles.end(), {a,b,d, a,d,c});
    }
    SetMeshes(w, std::move(points), std::move(triangles));
    return w;
}
} // namespace AlignmentTestModels
