#pragma once

#include "Data/DataGraphTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <limits>

enum class ArtifactError : std::uint8_t {
    None, Unavailable, WrongThread, Busy, InvalidRequest, InvalidData,
    UnsupportedType, SourceChanged, TooLarge, Cancelled, TimedOut,
    InsufficientEvidence, CommitFailed, UnsupportedGeometry, UnsupportedValidity, KernelFailed
};

enum class ArtifactStatus : std::uint8_t {
    Detached, Idle, Running, Cancelling, Ready, Publishing, Failed, Stopping
};

enum class ArtifactInputMode : std::uint8_t {
    CurrentPrimary, ExplicitRevision
};

struct ArtifactConfig final {
    // 本 Feature 保留输入及工作缓冲的保守预算，不代表进程峰值。
    std::size_t memoryBudgetBytes = 512ULL * 1024 * 1024;
    // 同一 Feature 对象累计发布量；Detach/Attach 不重置，Host 历史仍持有输出。
    std::size_t publishBudgetBytes = 512ULL * 1024 * 1024;
    std::uint32_t stopTimeoutMs = 2000;
};

struct ArtifactDiffusionParams final {
    int iterations = 4;
    double threshold = 1.0; // 存储灰度/网格物理单位；VTK按spacing计算邻域阈值。
    double factor = 0.25;
    int slabDepth = 8;
};

enum class ArtifactRingMode : std::uint8_t { Wrap, Reflect };

struct ArtifactRingParams final {
    int axis = 2; // 网格轴；物理方向为direction对应列，不要求世界Z。
    // 两个平面轴按X/Y/Z升序排列；坐标包含原始extent偏移。
    std::array<double, 2> centerIndex = { 0.0, 0.0 };
    double threshMin = std::numeric_limits<double>::quiet_NaN();
    double threshMax = std::numeric_limits<double>::quiet_NaN();
    double threshold = std::numeric_limits<double>::quiet_NaN();
    int angularMin = 30;
    int ringWidth = 3;
    ArtifactRingMode mode = ArtifactRingMode::Wrap;
    double strength = 1.0;
    double maxCorrection = 1.0; // 存储灰度单位，最终校正量上限。
};

struct ArtifactRequest final {
    DataRevisionRef source;
    ArtifactInputMode inputMode = ArtifactInputMode::CurrentPrimary;
    // 三种区域均为绑定 source 精确修订的公共 ROI。
    std::optional<DataRevisionRef> processingRoi;
    std::optional<DataRevisionRef> protectionRoi;
    std::optional<DataRevisionRef> qualityRoi;
    // qualityRoi仅用于区域（material）质量统计，整体输出统计仍覆盖有效数据；两种算法都不是掩码感知滤波器。
    std::optional<ArtifactDiffusionParams> diffusion;
    std::optional<ArtifactRingParams> ring;
    std::uint32_t timeoutMs = 60000;
};

enum class ArtifactAction : std::uint8_t { Prepare, Cancel, Commit, Discard };

struct ArtifactHostRequest final {
    ArtifactAction action = ArtifactAction::Prepare;
    std::optional<ArtifactRequest> prepare;
    std::uint64_t requestId = 0; // Commit必需；其余动作必须为0。
};

struct ArtifactAdmission final {
    ArtifactError error = ArtifactError::Unavailable;
    std::uint64_t requestId = 0; // 仅 error==None 时表示接纳。
};

struct ArtifactQuality final {
    std::size_t validCount = 0;
    std::size_t changedCount = 0;
    std::size_t materialCount = 0;
    std::size_t gradientCount = 0;
    std::size_t guardedCount = 0;
    std::size_t ringSampleCount = 0;
    std::size_t ringRadiusCount = 0;
    std::size_t ringVoxelCount = 0;
    std::size_t ringSkippedSlices = 0;
    double meanBefore = 0.0;
    double meanAfter = 0.0;
    double meanDelta = 0.0;
    double rmsDelta = 0.0;
    double maxDelta = 0.0;
    double materialMeanBefore = 0.0;
    double materialMeanAfter = 0.0;
    double materialStdBefore = 0.0;
    double materialStdAfter = 0.0;
    double gradientDeltaRms = 0.0;
    double ringCorrectionRms = 0.0;
    bool hasMaterial = false;
    bool fidelityVerified = false; // 无干净参考或计量证据，不能由这些统计量置 true。
};

struct ArtifactState final {
    ArtifactStatus status = ArtifactStatus::Detached;
    ArtifactError error = ArtifactError::None;
    std::uint64_t requestId = 0;
    unsigned int progressPercent = 0;
    std::size_t requiredBytes = 0;
    std::size_t publishedBytes = 0;
    ArtifactQuality quality;
    DataCommitStatus commitStatus = DataCommitStatus::Rejected;
    std::optional<DataRevisionRef> correctedVolume;
    std::optional<DataRevisionRef> qualityReport;
};

struct ArtifactCommitResult final {
    ArtifactError error = ArtifactError::Unavailable;
    DataCommitStatus status = DataCommitStatus::Rejected;
    std::optional<DataRevisionRef> correctedVolume;
    std::optional<DataRevisionRef> qualityReport;
};
