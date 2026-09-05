#pragma once

#include "Data/DataVersion.h"
#include "Host/Types/HostViewTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class SurfaceDeterminationMethod : std::uint8_t {
    GlobalIsoPreview,
    LocalAdaptiveIso50,
    GradientPeak
};

enum class SurfaceComponentSelection : std::uint8_t {
    Largest,
    Seeded,
    All
};

enum class SurfacePointFlags : std::uint32_t {
    None = 0,
    LowContrast = 1U << 0,
    MultipleCrossings = 1U << 1,
    InvalidSupport = 1U << 2,
    ProfileClipped = 1U << 3,
    ExcessiveOffset = 1U << 4,
    FitRejected = 1U << 5,
    TriangleFlipRisk = 1U << 6
};

constexpr SurfacePointFlags operator|(
    const SurfacePointFlags left,
    const SurfacePointFlags right) noexcept
{
    return static_cast<SurfacePointFlags>(
        static_cast<std::uint32_t>(left)
        | static_cast<std::uint32_t>(right));
}

constexpr SurfacePointFlags operator&(
    const SurfacePointFlags left,
    const SurfacePointFlags right) noexcept
{
    return static_cast<SurfacePointFlags>(
        static_cast<std::uint32_t>(left)
        & static_cast<std::uint32_t>(right));
}

inline SurfacePointFlags& operator|=(
    SurfacePointFlags& left,
    const SurfacePointFlags right) noexcept
{
    left = left | right;
    return left;
}

constexpr bool GetSurfaceFlag(
    const SurfacePointFlags value,
    const SurfacePointFlags flag) noexcept
{
    return (value & flag) != SurfacePointFlags::None;
}

enum class SurfaceMetricValidity : std::uint8_t {
    Valid,
    OpenSurface,
    NonManifold,
    Truncated,
    UnitUnknown,
    InsufficientQuality
};

struct SurfacePointRecord final {
    std::array<double, 3> positionModel{};
    std::array<float, 3> normalModel{};
    float localThreshold = 0.0F;
    float contrast = 0.0F;
    float gradientMagnitude = 0.0F;
    float fitResidual = 0.0F;
    float offsetFromSeed = 0.0F;
    float validSupportRatio = 0.0F;
    // 只表示局部定位稳定性估计，不等于完整计量不确定度。
    float estimatedLocalizationSigma = 0.0F;
    std::uint32_t crossingCount = 0;
    std::uint32_t objectIndex = 0;
    SurfacePointFlags flags = SurfacePointFlags::None;
};

struct SurfaceObjectRecord final {
    std::uint32_t objectIndex = 0;
    std::uint64_t firstPoint = 0;
    std::uint64_t pointCount = 0;
    std::uint64_t firstTriangle = 0;
    std::uint64_t triangleCount = 0;
    std::array<double, 6> boundsModel{};
    bool isClosed = false;
    bool isManifold = false;
    bool isTruncated = false;
    bool isOrientationValid = false;
    SurfaceMetricValidity areaValidity =
        SurfaceMetricValidity::InsufficientQuality;
    SurfaceMetricValidity volumeValidity =
        SurfaceMetricValidity::InsufficientQuality;
    std::optional<double> areaModelUnit2;
    std::optional<double> volumeModelUnit3;
};

struct SurfaceGenerationSnapshot final {
    DataVersion sourceVersion = 0;
    std::uint64_t resultRevision = 0;
    std::uint64_t parameterFingerprint = 0;
    std::uint32_t algorithmRevision = 0;
    SurfaceDeterminationMethod method =
        SurfaceDeterminationMethod::LocalAdaptiveIso50;
    std::shared_ptr<const std::vector<SurfacePointRecord>> points;
    std::shared_ptr<const std::vector<std::uint32_t>> triangleIndices;
    std::shared_ptr<const std::vector<SurfaceObjectRecord>> objects;
};

struct SurfaceDeterminationStartParams final {
    HostViewTargets targetViews;
    SurfaceDeterminationMethod method =
        SurfaceDeterminationMethod::LocalAdaptiveIso50;
    SurfaceComponentSelection componentSelection =
        SurfaceComponentSelection::Largest;
    std::optional<double> initialIsoValue;
    std::optional<std::array<double, 3>> seedModelPoint;
    std::optional<std::array<double, 6>> roiModelBounds;
    std::optional<double> profileHalfLengthModel;
    std::optional<double> profileSampleStepModel;
    std::optional<double> maximumOffsetModel;
    std::optional<double> profileSmoothingSigmaModel;
    // 闭合初始表面按体积/体素体积估计；开放/截断表面不伪造 voxel count。
    std::uint64_t minimumObjectVoxels = 1;
    double minimumContrast = 0.0;
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
    Clear
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
    InternalError
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
    std::uint64_t requestId = 0;
    SurfaceResultStatus status = SurfaceResultStatus::Failed;
    SurfaceFailureReason failureReason = SurfaceFailureReason::InternalError;
    DataVersion sourceVersion = 0;
    std::uint64_t resultRevision = 0;
    std::uint64_t pointCount = 0;
    std::uint32_t objectCount = 0;
    std::string message;
};

// Accepted 且 callback 非空时在 Host owner thread 恰好调用一次。
using SurfaceDeterminationCallback =
    std::function<void(SurfaceDeterminationResult)>;

struct SurfaceDeterminationState final {
    SurfaceDeterminationStage stage = SurfaceDeterminationStage::Idle;
    SurfaceFailureReason failureReason = SurfaceFailureReason::None;
    std::uint64_t requestId = 0;
    DataVersion sourceVersion = 0;
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
