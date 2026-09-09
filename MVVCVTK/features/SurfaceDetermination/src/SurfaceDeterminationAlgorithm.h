#pragma once

#include "Host/TrustedDataPort.h"
#include "Host/SurfaceDeterminationHostTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using SurfaceCancelCheck = std::function<bool()>;
using SurfaceProgressCallback =
    std::function<void(SurfaceDeterminationStage, double)>;
inline constexpr std::uint32_t surfaceAlgorithmRevision = 5;

// 点细化可由 VTK SMP worker 并发查询取消状态；调用方必须提供可并发调用的
// 只读检查。进度回调只在 BuildSurface 的调用线程执行。

struct SurfaceAlgorithmInputs final
{
    DataSnapshot materialLabels;
    DataSnapshot initialSurface;
    RoiReadSnapshot roi;
};

struct SurfaceAlgorithmResult final {
    SurfaceExecutionStats execution;
    std::vector<SurfaceInterfaceRecord> interfaces;
    SurfaceResultStatus status = SurfaceResultStatus::Failed;
    SurfaceFailureReason failureReason =
        SurfaceFailureReason::InternalError;
    std::string message;
    DataRevisionRef sourceRevision;
    std::uint64_t parameterFingerprint = 0;
    std::uint32_t algorithmRevision = surfaceAlgorithmRevision;
    SurfaceDeterminationStartParams resolvedParams;
    std::vector<std::uint8_t> triangleValidity;
    SurfaceDeterminationMethod method =
        SurfaceDeterminationMethod::LocalAdaptiveIso50;
    double initialIsoValue = 0.0;
    std::optional<SurfaceIsoEstimate> isoEstimate;
    std::size_t requiredBytes = 0;
    std::vector<SurfacePointRecord> points;
    std::vector<std::uint32_t> triangleIndices;
    std::vector<SurfaceObjectRecord> objects;
    std::uint64_t acceptedPointCount = 0;
    std::uint64_t lowContrastPointCount = 0;
    std::uint64_t rejectedPointCount = 0;
    std::uint64_t truncatedPointCount = 0;
    std::uint32_t nonManifoldObjectCount = 0;
};

class SurfaceDeterminationAlgorithm final {
public:
  static SurfaceFailureReason GetInputFailure(const VtkImageGridSnapshot &source,
                                              const SurfaceDeterminationStartParams &params,
                                              const SurfaceAlgorithmInputs &inputs);
  static SurfaceAlgorithmResult BuildSurface(const VtkImageGridSnapshot &source,
                                             const SurfaceDeterminationStartParams &params,
                                             std::size_t maxWorkingBytes,
                                             const SurfaceCancelCheck &getCancelled,
                                             const SurfaceProgressCallback &onProgress,
                                             const SurfaceAlgorithmInputs &inputs = {});
  static SurfaceProfileDiagnostic GetProfileDiagnostic(const VtkImageGridSnapshot &source,
                                                       const SurfaceDeterminationStartParams &resolved,
                                                       const SurfacePointRecord &point,
                                                       const SurfaceAlgorithmInputs &inputs = {});
};
