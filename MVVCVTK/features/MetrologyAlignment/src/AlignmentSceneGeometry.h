#pragma once
#include "AlignmentSceneDisplay.h"
#include <chrono>
namespace AlignmentSceneGeometry {
std::vector<std::size_t> GetItemGeometries(const InspectionPlan &plan,
                                           std::size_t item);
void Build(AlignmentSceneProduct &product, AlignmentScenePartData &part,
           const AlignmentScenePart &request,
           const SurfaceMeshPayload &measured,
           const SurfaceMeshPayload *reference, double glyphLength,
           std::size_t markerLimit, std::size_t viewCount,
           std::chrono::steady_clock::time_point deadline,
           const std::function<void(std::size_t, std::size_t)> &charge);
} // namespace AlignmentSceneGeometry
