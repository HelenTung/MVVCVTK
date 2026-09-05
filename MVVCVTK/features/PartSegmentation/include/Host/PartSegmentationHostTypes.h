#pragma once

#include "Data/DataGraphTypes.h"
#include "Host/Types/HostViewTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
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
    Unavailable,
    RevisionConflict,
    BudgetExceeded
};

enum class PartResultStatus : std::uint8_t {
    Succeeded,
    SucceededWithDisplayFailure,
    Cancelled,
    Failed,
    PreviewReady
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
    InternalError,
    InvalidEdit,
    ConstraintConflict,
    UnassignedVoxels,
    RevisionConflict,
    NoChange,
    TimedOut
};

enum class PartSegmentationStatus : std::uint8_t {
    Idle,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    Stale,
    Stopping,
    Committing
};

// 编辑坐标均属于原始数据网格；extent 含端点，物理位置使用 mm。
struct PartEditScope final {
    std::optional<std::array<int, 6>> extent;
    std::optional<DataRevisionRef> roiMask;
    std::vector<PartBindingRef> protectedParts;
    std::optional<DataRevisionRef> protectionMask;
};

struct PartBrushPlane final {
    std::array<double, 3> origin{};
    std::array<double, 3> normal{ 0.0, 0.0, 1.0 };
    double thicknessMM = 1.0;
};

struct PartBrushEdit final {
    PartBindingRef target;
    bool isErase = false;
    double radiusMM = 1.0;
    std::vector<std::array<double, 3>> sourcePoints;
    std::optional<PartBrushPlane> slice;
    std::vector<PartBindingRef> overwriteParts;
    bool isBackgroundAllowed = true;
};

struct PartFillEdit final {
    PartBindingRef target;
    std::array<int, 3> seed{};
};

struct PartIslandEdit final {
    PartBindingRef target;
    std::uint64_t minIslandVoxels = 1;
};

struct PartGrowEdit final {
    PartBindingRef target;
    std::vector<std::array<int, 3>> seeds;
    double minimum = 0.0;
    double maximum = 1.0;
    std::vector<PartBindingRef> overwriteParts;
    bool isBackgroundAllowed = true;
};

struct PartSplitSeed final {
    std::array<int, 3> imageIndex{};
    // 必须覆盖连续的 1..N，表示子对象序号，不是标签或 PartObjectId。
    std::uint32_t target = 0;
};

struct PartBarrier final {
    std::array<int, 3> imageIndex{};
    // 无向邻接边：imageIndex 与沿 axis 正向的相邻体素，不删除端点。
    std::uint8_t axis = 0;
};

struct PartSplitEdit final {
    PartBindingRef target;
    std::vector<PartSplitSeed> seeds;
    std::vector<PartBarrier> barriers;
};

struct PartMergeEdit final {
    std::vector<PartBindingRef> parts;
};

struct PartHistoryEdit final {
    bool isRedo = false;
};

using PartEditOperation = std::variant<PartBrushEdit, PartFillEdit,
    PartIslandEdit, PartGrowEdit, PartSplitEdit, PartMergeEdit, PartHistoryEdit>;

struct PartEditRequest final {
    DataRevisionRef expectedLabelMap;
    std::uint64_t expectedCatalogRevision = 0;
    PartEditScope scope;
    PartEditOperation operation;
};

struct PartEditPreview final {
    std::uint64_t previewId = 0;
    DataRevisionRef sourceRevision;
    DataRevisionRef baseLabels;
    // 临时候选，不是正式 DataGraph 修订；共享冻结 owner，不允许写入别名。
    std::shared_ptr<const std::vector<PartLabelId>> labels;
    std::shared_ptr<const PartSetSnapshot> parts;
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
    std::size_t maxHistoryBytes = 256U * 1024U * 1024U;
    std::size_t maxUndoSteps = 16;
    std::uint64_t editTimeoutMs = 30000;
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
