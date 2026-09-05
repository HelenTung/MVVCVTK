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

// 点细化可由 VTK SMP worker 并发查询取消状态；调用方必须提供可并发调用的
// 只读检查。进度回调只在 BuildSurface 的调用线程执行。

struct SurfaceAlgorithmResult final {
    SurfaceResultStatus status = SurfaceResultStatus::Failed;
    SurfaceFailureReason failureReason =
        SurfaceFailureReason::InternalError;
    std::string message;
    DataRevisionRef sourceRevision;
    std::uint64_t parameterFingerprint = 0;
    std::uint32_t algorithmRevision = 1;
    SurfaceDeterminationMethod method =
        SurfaceDeterminationMethod::LocalAdaptiveIso50;
    double initialIsoValue = 0.0;
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
    static SurfaceAlgorithmResult BuildSurface(
        const VtkImageGridSnapshot& source,
        const SurfaceDeterminationStartParams& params,
        std::size_t maxWorkingBytes,
        const SurfaceCancelCheck& getCancelled,
        const SurfaceProgressCallback& onProgress);
};
