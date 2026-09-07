#pragma once

#include "Data/DataGraphTypes.h"
#include "SurfaceRecipe.h"
#include "Host/Types/HostViewTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct SurfaceDeterminationStartParams final : SurfaceRecipe
{
    HostViewTargets targetViews;
    // 省略只在接纳时解析主卷，计算不再查询当前选择。
    std::optional<DataRevisionRef> sourceVolume;
    std::optional<DataRevisionRef> materialLabels;
    std::optional<DataRevisionRef> initialSurface;
    // 执行分块不降低分辨率，不影响算法结果的参数指纹。
    std::uint32_t seedBlockDepth = 16;
    // 省略沿用方法的既有用途：Automatic→Estimate、Global→Preview。
    std::optional<SurfaceTaskPurpose> purpose;
    std::string resultScope;
    DataPublishPolicy sourcePolicy = DataPublishPolicy::RequireCurrentInputs;
    // 调用方对几何长度单位的声明；空表示未知，绝不借用灰度单位。
    std::string modelUnit;
};

struct SurfaceGenerationSnapshot final {
    // Preview 的两个图引用为空；requestId 标识临时候选。
    std::uint64_t requestId = 0;
    SurfaceTaskPurpose purpose = SurfaceTaskPurpose::Determine;
    std::string resultScope;
    std::string coordinateFrame;
    std::string modelUnit;
    SurfaceDeterminationStartParams requestedParams;
    SurfaceDeterminationStartParams resolvedParams;
    std::string canonicalParameters;
    // 仅便利主卷入口保存该期望；显式输入不绑定当前主卷。
    std::optional<DataBinding> sourceBinding;
    DataRevisionRef dataRevision;
    DataRevisionRef meshRevision;
    DataRevisionRef sourceRevision;
    std::uint64_t resultRevision = 0;
    std::uint64_t parameterFingerprint = 0;
    std::uint32_t algorithmRevision = 0;
    SurfaceDeterminationMethod method =
        SurfaceDeterminationMethod::LocalAdaptiveIso50;
    std::shared_ptr<const std::vector<SurfacePointRecord>> points;
    std::shared_ptr<const std::vector<std::uint32_t>> triangleIndices;
    std::shared_ptr<const std::vector<SurfaceObjectRecord>> objects;
    std::optional<SurfaceIsoEstimate> isoEstimate;
    // 每三角形一个值；仅证明本 Feature 明确检查的有效性条件。
    std::shared_ptr<const std::vector<std::uint8_t>> triangleValidity;
    std::vector<DataInputRef> inputs;
    SurfaceExecutionStats execution;
    std::shared_ptr<const std::vector<SurfaceInterfaceRecord>> interfaces;
};

struct SurfaceDeterminationConfig final {
    SurfaceDeterminationStartParams defaultStart;
    std::size_t maxWorkingBytes = 512U * 1024U * 1024U;
    bool isOverlayVisible = true;
};

enum class SurfaceDeterminationAction : std::uint8_t {
    None,
    Start,
    Stop,
    SetVisibility,
    Clear,
    ClearPreview
};

struct SurfaceDeterminationRequest final {
    SurfaceDeterminationAction action = SurfaceDeterminationAction::None;
    std::optional<SurfaceDeterminationStartParams> start;
    std::optional<bool> isVisible;
    // Stop 时为 0 表示当前最新请求，否则必须与仍在执行的请求匹配。
    std::uint64_t targetRequestId = 0;
};

enum class SurfaceAdmissionStatus : std::uint8_t {
    Accepted,
    InvalidRequest,
    Busy,
    Stopping,
    Unavailable
};

struct SurfaceDeterminationAdmission final {
    SurfaceAdmissionStatus status = SurfaceAdmissionStatus::InvalidRequest;
    std::uint64_t requestId = 0;
};

enum class SurfaceResultStatus : std::uint8_t {
    Succeeded,
    Cancelled,
    Failed
};

enum class SurfaceFailureReason : std::uint8_t {
    None,
    InvalidSource,
    InvalidGeometry,
    UnsupportedScalar,
    InvalidRoi,
    ThresholdUnreliable,
    NoSurface,
    BudgetExceeded,
    Cancelled,
    SourceChanged,
    DisplayFailed,
    InternalError,
    PublishFailed
};

enum class SurfaceDeterminationStage : std::uint8_t {
    Idle,
    Preparing,
    ThresholdEstimation,
    SeedExtraction,
    SubvoxelRefinement,
    TopologyValidation,
    Committing,
    Ready,
    Cancelled,
    Stale,
    Failed,
    Stopping
};

struct SurfaceDeterminationResult final {
    SurfaceTaskPurpose purpose = SurfaceTaskPurpose::Determine;
    std::string resultScope;
    DataRevisionRef dataRevision;
    DataRevisionRef meshRevision;
    bool isPublished = false;
    bool isActivated = false;
    std::uint64_t requestId = 0;
    SurfaceResultStatus status = SurfaceResultStatus::Failed;
    SurfaceFailureReason failureReason = SurfaceFailureReason::InternalError;
    DataRevisionRef sourceRevision;
    std::uint64_t resultRevision = 0;
    std::uint64_t pointCount = 0;
    std::uint32_t objectCount = 0;
    std::string message;
    std::optional<SurfaceIsoEstimate> isoEstimate;
};

// Accepted 且 callback 非空时在 Host owner thread 恰好调用一次。
using SurfaceDeterminationCallback =
    std::function<void(SurfaceDeterminationResult)>;

struct SurfaceDeterminationState final {
    SurfaceTaskPurpose purpose = SurfaceTaskPurpose::Determine;
    std::string resultScope;
    std::optional<SurfaceIsoEstimate> isoEstimate;
    SurfaceDeterminationStage stage = SurfaceDeterminationStage::Idle;
    SurfaceFailureReason failureReason = SurfaceFailureReason::None;
    std::uint64_t requestId = 0;
    DataRevisionRef sourceRevision;
    std::uint64_t resultRevision = 0;
    double progress01 = 0.0;
    std::uint64_t pointCount = 0;
    std::uint64_t acceptedPointCount = 0;
    std::uint64_t lowContrastPointCount = 0;
    std::uint64_t rejectedPointCount = 0;
    std::uint64_t truncatedPointCount = 0;
    std::uint32_t objectCount = 0;
    std::uint32_t nonManifoldObjectCount = 0;
    bool isOverlayVisible = true;
    std::string errorMessage;
};

enum class SurfaceRestoreStatus : std::uint8_t
{
    Current,
    Historical,
    MissingInput,
    IncompatibleRecipe,
    MissingResult
};

struct SurfaceRestoreState final
{
    SurfaceRestoreStatus status = SurfaceRestoreStatus::MissingResult;
    bool canDisplay = false;
    bool canRecompute = false;
    bool canMeasure = false;
    std::vector<DataInputRef> inputs;
    std::string message;
};
