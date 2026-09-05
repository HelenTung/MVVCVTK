#pragma once

#include "Data/DataGraphTypes.h"
#include "Host/Types/HostViewTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using PartLabelId = std::uint32_t;

struct PartSetId final {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
};

constexpr bool operator==(
    const PartSetId& left,
    const PartSetId& right) noexcept
{
    return left.high == right.high && left.low == right.low;
}

constexpr bool operator!=(
    const PartSetId& left,
    const PartSetId& right) noexcept
{
    return !(left == right);
}

struct PartObjectId final {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
};

constexpr bool operator==(
    const PartObjectId& left,
    const PartObjectId& right) noexcept
{
    return left.high == right.high && left.low == right.low;
}

constexpr bool operator!=(
    const PartObjectId& left,
    const PartObjectId& right) noexcept
{
    return !(left == right);
}

struct PartObjectRef final {
    PartSetId partSetId;
    PartObjectId objectId;
};

constexpr bool operator==(
    const PartObjectRef& left,
    const PartObjectRef& right) noexcept
{
    return left.partSetId == right.partSetId
        && left.objectId == right.objectId;
}

constexpr bool operator!=(
    const PartObjectRef& left,
    const PartObjectRef& right) noexcept
{
    return !(left == right);
}

struct PartBindingRef final {
    PartObjectRef object;
    std::uint64_t resultRevision = 0;
};

constexpr bool operator==(
    const PartBindingRef& left,
    const PartBindingRef& right) noexcept
{
    return left.object == right.object
        && left.resultRevision == right.resultRevision;
}

constexpr bool operator!=(
    const PartBindingRef& left,
    const PartBindingRef& right) noexcept
{
    return !(left == right);
}

struct PartMetrics final {
    std::uint64_t voxelCount = 0;
    double physicalVolumeMM3 = 0.0;
    // extent 为包含端点的 VTK point-index 范围。
    std::array<int, 6> voxelExtent{};
    // source image 物理空间中采样点中心的轴对齐包围盒。
    std::array<double, 6> inputPhysicalBounds{};
    std::array<double, 3> centroidInputPhysical{};
    std::optional<double> confidence;
};

inline bool operator==(
    const PartMetrics& left,
    const PartMetrics& right)
{
    return left.voxelCount == right.voxelCount
        && left.physicalVolumeMM3 == right.physicalVolumeMM3
        && left.voxelExtent == right.voxelExtent
        && left.inputPhysicalBounds == right.inputPhysicalBounds
        && left.centroidInputPhysical == right.centroidInputPhysical
        && left.confidence == right.confidence;
}

inline bool operator!=(
    const PartMetrics& left,
    const PartMetrics& right)
{
    return !(left == right);
}

struct PartUserState final {
    std::string name;
    bool isReviewed = false;
};

inline bool operator==(
    const PartUserState& left,
    const PartUserState& right)
{
    return left.name == right.name
        && left.isReviewed == right.isReviewed;
}

inline bool operator!=(
    const PartUserState& left,
    const PartUserState& right)
{
    return !(left == right);
}

enum class PartColorUse : std::uint8_t {
    Stable,
    Custom
};

struct PartPresentation final {
    bool isVisible = true;
    bool isSelected = false;
    double opacity = 0.85;
    PartColorUse colorUse = PartColorUse::Stable;
    std::array<double, 4> color{ 0.0, 0.0, 0.0, 0.85 };
};

inline bool operator==(
    const PartPresentation& left,
    const PartPresentation& right)
{
    return left.isVisible == right.isVisible
        && left.isSelected == right.isSelected
        && left.opacity == right.opacity
        && left.colorUse == right.colorUse
        && left.color == right.color;
}

inline bool operator!=(
    const PartPresentation& left,
    const PartPresentation& right)
{
    return !(left == right);
}

struct PartSnapshot final {
    PartBindingRef binding;
    PartLabelId labelId = 0;
    PartMetrics metrics;
    PartUserState userState;
    PartPresentation presentation;
};

inline bool operator==(
    const PartSnapshot& left,
    const PartSnapshot& right)
{
    return left.binding == right.binding
        && left.labelId == right.labelId
        && left.metrics == right.metrics
        && left.userState == right.userState
        && left.presentation == right.presentation;
}

inline bool operator!=(
    const PartSnapshot& left,
    const PartSnapshot& right)
{
    return !(left == right);
}

enum class PartRelationKind : std::uint8_t {
    ContinuedFrom,
    SplitFrom,
    MergedFrom
};

struct PartRelation final {
    PartBindingRef current;
    PartBindingRef previous;
    PartRelationKind kind = PartRelationKind::ContinuedFrom;
    double overlapScore = 0.0;
};

inline bool operator==(
    const PartRelation& left,
    const PartRelation& right)
{
    return left.current == right.current
        && left.previous == right.previous
        && left.kind == right.kind
        && left.overlapScore == right.overlapScore;
}

inline bool operator!=(
    const PartRelation& left,
    const PartRelation& right)
{
    return !(left == right);
}

struct PartSetSnapshot final {
    PartSetId partSetId;
    DataRevisionRef sourceRevision;
    std::uint64_t resultRevision = 0;
    std::uint64_t catalogRevision = 0;
    bool isStale = false;
    std::vector<PartSnapshot> parts;
    std::vector<PartRelation> relationsFromPrevious;
    std::vector<PartBindingRef> retiredFromPrevious;
};

inline bool operator==(
    const PartSetSnapshot& left,
    const PartSetSnapshot& right)
{
    return left.partSetId == right.partSetId
        && left.sourceRevision == right.sourceRevision
        && left.resultRevision == right.resultRevision
        && left.catalogRevision == right.catalogRevision
        && left.isStale == right.isStale
        && left.parts == right.parts
        && left.relationsFromPrevious == right.relationsFromPrevious
        && left.retiredFromPrevious == right.retiredFromPrevious;
}

inline bool operator!=(
    const PartSetSnapshot& left,
    const PartSetSnapshot& right)
{
    return !(left == right);
}

struct PartColorPatch final {
    PartColorUse colorUse = PartColorUse::Stable;
    std::array<double, 4> color{ 0.0, 0.0, 0.0, 0.85 };
};

inline bool operator==(
    const PartColorPatch& left,
    const PartColorPatch& right)
{
    return left.colorUse == right.colorUse
        && left.color == right.color;
}

inline bool operator!=(
    const PartColorPatch& left,
    const PartColorPatch& right)
{
    return !(left == right);
}

struct PartStatePatch final {
    std::optional<std::string> name;
    std::optional<bool> isVisible;
    std::optional<bool> isSelected;
    std::optional<bool> isReviewed;
    std::optional<double> opacity;
    std::optional<PartColorPatch> color;
};

enum class PartMutationStatus : std::uint8_t {
    Succeeded,
    InvalidRequest,
    NotFound,
    StaleReference,
    RevisionConflict,
    Busy,
    Unavailable,
    DisplayFailed
};

struct PartMutationResult final {
    PartMutationStatus status = PartMutationStatus::InvalidRequest;
    std::uint64_t catalogRevision = 0;
};

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
    SucceededWithDisplayFailure,
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

struct PartSegmentationResult final {
    std::uint64_t resultRevision = 0;
    std::uint64_t catalogRevision = 0;
    std::uint64_t requestId = 0;
    PartResultStatus status = PartResultStatus::Failed;
    PartFailureReason failureReason = PartFailureReason::InternalError;
    DataCommitId commitId = 0;
    DataRevisionRef sourceRevision;
    DataRevisionRef labelMap;
    DataRevisionRef partTable;
    DataRevisionRef resultSet;
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
    PartSetId partSetId;
    std::uint64_t resultRevision = 0;
    std::uint64_t catalogRevision = 0;
    std::size_t partCount = 0;
    DataCommitId commitId = 0;
    DataRevisionRef sourceRevision;
    DataRevisionRef labelMap;
    DataRevisionRef partTable;
    DataRevisionRef resultSet;
    double progress = 0.0;
    bool isOverlayVisible = true;
};
