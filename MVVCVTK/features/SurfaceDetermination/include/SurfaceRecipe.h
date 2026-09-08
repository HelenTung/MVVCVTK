#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class SurfaceDeterminationMethod : std::uint8_t
{
    GlobalIsoPreview,
    LocalAdaptiveIso50,
    GradientPeak,
    // 仅估计空气/单材料双峰 ISO50，不构造测量网格。
    AutomaticIso50,
    LocalRelativeIso,
    EdgeModelFit,
    PairedEdgeModelFit
};

enum class SurfaceTaskPurpose : std::uint8_t
{
    Estimate,
    Preview,
    Determine
};

enum class SurfaceComponentSelection : std::uint8_t
{
    Largest,
    Seeded,
    All
};

enum class SurfacePointFlags : std::uint32_t
{
    None = 0,
    LowContrast = 1U << 0,
    MultipleCrossings = 1U << 1,
    InvalidSupport = 1U << 2,
    ProfileClipped = 1U << 3,
    ExcessiveOffset = 1U << 4,
    FitRejected = 1U << 5,
    TriangleFlipRisk = 1U << 6,
    SeedRetained = 1U << 7,
    Unresolved = 1U << 8,
    MaterialJunction = 1U << 9,
    SharpCorner = 1U << 10,
    OverrideBoundary = 1U << 11,
    PlateauUnstable = 1U << 12,
    DirectionMismatch = 1U << 13,
    RoiBoundary = 1U << 14
};

constexpr SurfacePointFlags operator|(const SurfacePointFlags left, const SurfacePointFlags right) noexcept
{
    return static_cast<SurfacePointFlags>(static_cast<std::uint32_t>(left) |
                                          static_cast<std::uint32_t>(right));
}

constexpr SurfacePointFlags operator&(const SurfacePointFlags left, const SurfacePointFlags right) noexcept
{
    return static_cast<SurfacePointFlags>(static_cast<std::uint32_t>(left) &
                                          static_cast<std::uint32_t>(right));
}

inline SurfacePointFlags &operator|=(SurfacePointFlags &left, const SurfacePointFlags right) noexcept
{
    left = left | right;
    return left;
}

constexpr bool GetSurfaceFlag(const SurfacePointFlags value, const SurfacePointFlags flag) noexcept
{
    return (value & flag) != SurfacePointFlags::None;
}

enum class SurfaceMetricValidity : std::uint8_t
{
    Valid,
    OpenSurface,
    NonManifold,
    Truncated,
    UnitUnknown,
    InsufficientQuality
};

struct SurfacePointRecord final
{
    std::array<double, 3> seedPositionModel{};
    std::array<double, 3> seedNormalModel{};
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
    std::uint32_t interfaceIndex = 0;
    // 0 为全局配方，n+1 对应 regionOverrides[n]。
    std::uint32_t overrideIndex = 0;
    float transitionWidthModel = 0.0F;
    float pairedSeparationModel = 0.0F;
    SurfacePointFlags flags = SurfacePointFlags::None;
};

struct SurfaceObjectRecord final
{
    std::uint32_t objectIndex = 0;
    std::uint32_t interfaceIndex = 0;
    std::uint64_t firstPoint = 0;
    std::uint64_t pointCount = 0;
    std::uint64_t firstTriangle = 0;
    std::uint64_t triangleCount = 0;
    std::array<double, 6> boundsModel{};
    double validAreaRatio = 0.0;
    bool isClosed = false;
    bool isManifold = false;
    bool isTruncated = false;
    bool isOrientationValid = false;
    SurfaceMetricValidity areaValidity = SurfaceMetricValidity::InsufficientQuality;
    SurfaceMetricValidity volumeValidity = SurfaceMetricValidity::InsufficientQuality;
    std::optional<double> areaModelUnit2;
    std::optional<double> volumeModelUnit3;
};

struct SurfaceIsoEstimate final
{
    double isoValue = 0.0;
    double backgroundValue = 0.0;
    double materialValue = 0.0;
    std::uint64_t sampleCount = 0;
    std::uint64_t excludedSampleCount = 0;
    double separationRatio = 0.0;
};

struct SurfaceGrayPair final
{
    std::array<double, 2> sideA{};
    std::array<double, 2> sideB{};
};

struct SurfaceMaterialPair final
{
    std::uint32_t materialA = 0;
    std::uint32_t materialB = 0;
};

struct SurfaceRegionOverride final
{
    std::string id;
    std::int32_t priority = 0;
    std::array<double, 6> boundsModel{};
    std::optional<SurfaceDeterminationMethod> method;
    std::optional<double> localFraction;
    std::optional<double> profileHalfLengthModel;
    std::optional<double> profileSampleStepModel;
    std::optional<double> maximumOffsetModel;
    std::optional<double> profileSmoothingSigmaModel;
    std::optional<double> minimumContrast;
    std::optional<double> minimumCnr;
};

struct SurfaceRecipe
{
    SurfaceDeterminationMethod method = SurfaceDeterminationMethod::LocalAdaptiveIso50;
    SurfaceComponentSelection componentSelection = SurfaceComponentSelection::Largest;
    std::optional<double> initialIsoValue;
    std::optional<std::array<double, 3>> seedModelPoint;
    std::optional<double> profileHalfLengthModel;
    std::optional<double> profileSampleStepModel;
    std::optional<double> maximumOffsetModel;
    std::optional<double> profileSmoothingSigmaModel;
    // 闭合初始表面按体积/体素体积估计；开放/截断表面不伪造 voxel count。
    std::uint64_t minimumObjectVoxels = 1;
    double minimumContrast = 0.0;

    double seedFraction = 0.5;
    double localFraction = 0.5;
    std::optional<SurfaceGrayPair> grayPair;
    double minimumCnr = 0.0;
    double maximumPlateauNoiseRatio = 1.0;
    double maximumNormalizedResidual = 0.25;
    std::optional<double> minimumEdgeWidthModel;
    std::optional<double> maximumEdgeWidthModel;
    std::optional<double> minimumEdgeSeparationModel;
    double sharpCornerAngleDeg = 75.0;
    double maximumNormalTurnDeg = 75.0;
    std::vector<SurfaceMaterialPair> materialPairs;
    std::vector<SurfaceRegionOverride> regionOverrides;
};

enum class SurfaceSampleStatus : std::uint8_t
{
    Valid,
    Clipped,
    InvalidSupport,
    OtherMaterial
};

struct SurfaceEdgeCandidate final
{
    double offsetModel = 0.0;
    double gradient = 0.0;
    double widthModel = 0.0;
    bool isSelected = false;
};

struct SurfaceProfileDiagnostic final
{
    bool isAvailable = false;
    SurfacePointRecord point;
    std::array<double, 3> profileCenterModel{};
    std::array<double, 3> directionModel{};
    std::vector<double> offsetsModel;
    std::vector<double> rawValues;
    std::vector<double> filteredValues;
    std::vector<SurfaceSampleStatus> support;
    std::vector<std::uint32_t> materialLabels;
    std::vector<SurfaceEdgeCandidate> candidates;
    double sideA = 0.0;
    double sideB = 0.0;
    double noiseSigma = 0.0;
    double normalizedResidual = 0.0;
    double minimumOffsetModel = 0.0;
    double maximumOffsetModel = 0.0;
    std::string message;
};

struct SurfaceInterfaceRecord final
{
    SurfaceMaterialPair materials;
    // 身份按标签排序；法向仍严格为上面的 materialA -> materialB。
    std::string canonicalId;
    std::uint64_t firstPoint = 0;
    std::uint64_t pointCount = 0;
    std::uint64_t firstTriangle = 0;
    std::uint64_t triangleCount = 0;
};

struct SurfaceExecutionStats final
{
    std::uint64_t scannedCellCount = 0;
    std::uint64_t skippedCellCount = 0;
    std::uint64_t blockCount = 0;
    std::uint64_t sampledProfileCount = 0;
    std::uint64_t sampledValueCount = 0;
    std::size_t estimatedWorkingBytes = 0;
    std::size_t retainedBytes = 0;
    std::array<int, 6> processedExtent{};
    double seedMs = 0.0;
    double refinementMs = 0.0;
    double publicationPreparationMs = 0.0;
};

struct SurfaceRecipeReadResult final
{
    std::optional<SurfaceRecipe> recipe;
    std::string errorMessage;
};

// 纯内存业务编解码；不读写文件，不依赖 Host 或应用对象。
class SurfaceRecipeCodec final
{
  public:
    static std::string BuildText(const SurfaceRecipe &recipe);
    static SurfaceRecipeReadResult GetRecipe(std::string_view text);
    static std::string GetError(const SurfaceRecipe &recipe);
};
