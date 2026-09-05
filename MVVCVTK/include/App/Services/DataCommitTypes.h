#pragma once

#include "App/AppTypes.h"

#include <array>
#include <cstdint>

enum class DataStageStatus {
    Idle,
    Preparing,
    Ready,
    Failed,
    Cancelled
};

enum class LoadCommitStatus {
    Preparing,
    Succeeded,
    Failed,
    Cancelled
};

enum class LoadCommitFailure {
    None,
    InvalidRequest,
    StageFailed,
    StaleInput,
    Cancelled,
    Stopping,
    CommitFailed,
    PublishFailed
};

struct LoadCommitResult final {
    LoadCommitStatus status = LoadCommitStatus::Failed;
    LoadCommitFailure failureReason = LoadCommitFailure::InvalidRequest;
    std::uint64_t transactionRevision = 0;
    DataVersion sourceVersion = 0;
};

// DataManager 最终发布成功后一次写入共享状态的值快照。
// View 候选阶段只计算本值，不得提前修改 SharedInteractionState。
struct DataReadyState final {
    DataVersion version = 0;
    std::array<double, 2> scalarRange = { 0.0, 0.0 };
    std::array<double, 3> spacing = { 1.0, 1.0, 1.0 };
    std::array<double, 3> cursorWorld = { 0.0, 0.0, 0.0 };
};
