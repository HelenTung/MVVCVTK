#pragma once

#include "Data/DataVersion.h"
#include "Host/Types/HostViewTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using PartId = std::uint32_t;

enum class PartSegmentationAction : std::uint8_t {
    None,
    Start,
    Stop,
    SetVisibility,
    Clear
};

enum class PartAdmissionStatus : std::uint8_t {
    Accepted,
    InvalidRequest,
    Busy,
    Stopping,
    Unavailable
};

enum class PartResultStatus : std::uint8_t {
    Succeeded,
    Cancelled,
    Failed
};

enum class PartFailureReason : std::uint8_t {
    None,
    InvalidSource,
    InvalidGeometry,
    UnsupportedScalar,
    BudgetExceeded,
    Cancelled,
    SourceChanged,
    DisplayFailed,
    InternalError
};

enum class PartSegmentationStatus : std::uint8_t {
    Idle,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    Stale,
    Stopping
};

struct PartSegmentationStartParams final {
    HostViewTargets targetViews;
    double threshold = 0.5;
    std::uint64_t minPartVoxels = 1;
};

struct PartSegmentationConfig final {
    PartSegmentationStartParams defaultStart;
    std::size_t maxWorkingBytes = 512U * 1024U * 1024U;
    bool isOverlayVisible = true;
};

struct PartSegmentationRequest final {
    PartSegmentationAction action = PartSegmentationAction::None;
    std::optional<PartSegmentationStartParams> start;
    std::optional<bool> isVisible;
};

struct PartSegmentationAdmission final {
    PartAdmissionStatus status = PartAdmissionStatus::InvalidRequest;
    std::uint64_t requestId = 0;
};

struct PartRecord final {
    PartId partId = 0;
    std::uint64_t voxelCount = 0;
    double physicalVolumeMM3 = 0.0;
    // extent 为包含端点的 VTK point-index 范围；world 统计同样以体素采样点为准。
    std::array<int, 6> voxelExtent{};
    // direction 变换后所有采样点中心的世界轴对齐包围盒，不表示半体素外扩的 cell bounds。
    std::array<double, 6> worldBounds{};
    std::array<double, 3> centroidWorld{};
    std::optional<double> confidence;
    bool isReviewed = false;
    bool isEdited = false;
};

struct PartSegmentationResult final {
    std::uint64_t requestId = 0;
    PartResultStatus status = PartResultStatus::Failed;
    PartFailureReason failureReason = PartFailureReason::InternalError;
    DataVersion sourceVersion = 0;
    std::uint64_t resultRevision = 0;
    std::size_t partCount = 0;
    std::string message;
};

// Accepted 且 callback 非空时在 Host owner thread 恰好调用一次；空 callback 不产生通知。
using PartSegmentationCallback =
    std::function<void(PartSegmentationResult)>;

struct PartSegmentationState final {
    PartSegmentationStatus status = PartSegmentationStatus::Idle;
    PartFailureReason failureReason = PartFailureReason::None;
    std::uint64_t requestId = 0;
    DataVersion sourceVersion = 0;
    std::uint64_t resultRevision = 0;
    double progress = 0.0;
    bool isOverlayVisible = true;
    std::vector<PartRecord> parts;
};
