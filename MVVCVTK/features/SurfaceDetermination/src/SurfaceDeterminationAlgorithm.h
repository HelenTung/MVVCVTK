#pragma once
#include "FeatureSupport/WorkLimit.h"
#include "SurfaceCudaTypes.h"

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
inline constexpr std::uint32_t surfaceAlgorithmRevision = 8;

// 点细化可由请求内 CPU worker 并发查询取消状态；调用方必须提供可并发调用的
// 只读检查。进度回调只在 BuildSurface 的调用线程执行。

struct SurfaceAlgorithmInputs final
{
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
        SurfaceDeterminationMethod::GlobalAutomatic;
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
    // 仓内审计使用，不进入 generation/API/SPI。
    std::size_t seedClassificationCacheBytes = 0;
    std::uint64_t seedClassifiedNodeCount = 0;
    std::uint64_t seedUniformCellCount = 0;
    std::size_t seedCacheBudgetFallbackCount = 0;
    std::size_t cudaPointCount = 0;
    std::size_t cudaDeviceBytes = 0;
    std::size_t cudaHostBufferBytes = 0;
    std::size_t cudaBatchCount = 0;
    unsigned cudaFinalizeThreads = 1;
    std::size_t cudaFinalizeWorkspaceBound = 0;
    bool cudaProfilingRequested = false;
    double cudaStartMs = 0, cudaIndexMs = 0, cudaSubmitMs = 0, cudaCollectMs = 0;
    double cudaFinalizeMs = 0, cudaCloseMs = 0, cpuSamplingMs = 0;
    // GPU 分支的 wall 时间，包含后处理排空；故障时也包含该分支的 CPU 余段回退。
    double cudaPipelineMs = 0;
    std::size_t adaptiveCpuPrefixPoints = 0, adaptiveGpuPilotPoints = 0;
    bool adaptiveKeptGpu = false, adaptiveEstimateAvailable = false;
    double adaptiveCpuPrefixMs = 0, adaptiveGpuPilotMs = 0, adaptiveStartupMs = 0;
    double adaptiveCpuEstimateMs = 0, adaptiveGpuEstimateMs = 0;
    unsigned adaptiveCpuPrefixThreads = 0;
    // hit 表示查得成本记录；是否实际避免初始化由 skippedStartup 独立说明。
    bool adaptiveCalibrationHit = false, adaptiveSkippedStartup = false;
    std::size_t adaptiveCalibrationBytes = 0;
    SurfaceCudaProfile cudaProfile;
    unsigned cpuThreadCount = 0;
    unsigned availablePhysicalCores = 0;
    unsigned availableLogicalThreads = 0;
    unsigned plannedCpuThreads = 1;
    unsigned componentCpuThreads = 1;
    std::uint64_t componentCpuPointCount = 0;
    std::size_t componentWorkspaceBound = 0, componentBudgetFallbackCount = 0;
    double componentParallelMs = 0;
    double componentsMs = 0;
    double componentSelectionMs = 0, largestPointSamplingMs = 0;
    std::uint64_t componentCandidateCount = 0, componentSelectionPointVisits = 0;
    std::size_t largestPointCount = 0, largestTriangleCount = 0;
    double meshPreparationMs = 0;
    double pointSamplingMs = 0;
    double outputMs = 0;
};

class SurfaceDeterminationAlgorithm final {
public:
  static SurfaceFailureReason GetInputFailure(const VtkImageGridSnapshot &source,
                                              const SurfaceDeterminationStartParams &params,
                                              const SurfaceAlgorithmInputs &inputs);
  static SurfaceAlgorithmResult BuildSurface(const VtkImageGridSnapshot &source,
                                             const SurfaceDeterminationStartParams &params,
                                             WorkLimit maxWorkingBytes,
                                             const SurfaceCancelCheck &getCancelled,
                                             const SurfaceProgressCallback &onProgress,
                                             const SurfaceAlgorithmInputs &inputs = {});
  static SurfacePointDiagnostic GetPointDiagnostic(const VtkImageGridSnapshot &source,
                                                       const SurfaceDeterminationStartParams &resolved,
                                                       const SurfacePointRecord &point,
                                                       const SurfaceAlgorithmInputs &inputs = {});
};
