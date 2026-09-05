#pragma once

#include "Data/DataPayloads.h"
#include "Host/MetrologyAlignmentHostTypes.h"
#include <atomic>
#include <chrono>
#include <memory>

struct AlignmentWork final {
    AlignmentInput input;
    AlignmentRecipe recipe;
    AlignmentConfig config;
    // 生产入口只传 DataGraphStore 已验证的不可变 payload；计算不重复扫描整张网格。
    std::shared_ptr<const SurfaceMeshPayload> mesh;
    std::vector<AlignmentMatrix> poses;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::chrono::steady_clock::time_point deadline;
};
struct AlignmentSamples final {
    std::vector<AlignmentPoint> points;
    std::size_t rejected = 0;
};
struct AlignmentCandidate final {
    AlignmentMatrix sourceToTarget = alignmentIdentity;
    std::vector<AlignmentGeometry> geometries;
    std::vector<AlignmentResidual> residuals;
    AlignmentDiagnostics diagnostics;
};

class AlignmentGeometryFit final {
  public:
    static bool GetRecipeValid(const AlignmentRecipe &recipe, const AlignmentConfig &config);
    static AlignmentStatus GetWorkStatus(const AlignmentWork &work);
    static bool GetPointUsable(const AlignmentWork &work, std::size_t index);
    static AlignmentSamples BuildSamples(const AlignmentWork &work, const AlignmentRegion &region,
                                         const AlignmentMatrix &pose);
    static AlignmentGeometry BuildGeometry(const AlignmentWork &work,
                                           const AlignmentGeometrySpec &spec,
                                           const AlignmentSamples &samples,
                                           const AlignmentMatrix &pose,
                                           std::optional<AlignmentPoint> normalConstraint = {},
                                           bool isFixedNormal = false);
};
