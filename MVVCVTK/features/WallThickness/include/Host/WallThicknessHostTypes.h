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
#include <vector>

using ThicknessPoint = std::array<double, 3>;

enum class ThicknessUnit : std::uint8_t
{
    Unknown,
    Millimeter,
    Meter
};
enum class ThicknessValidity : std::uint8_t
{
    Valid,
    LowSurfaceQuality,
    AmbiguousOpposite,
    SharpFeature,
    TruncatedSupport,
    SearchLimitReached,
    InvalidMaterialPath,
    OutsideEvaluation,
    Count
};
enum class ThicknessStatus : std::uint8_t
{
    Succeeded,
    InvalidInput,
    IncompleteBoundary,
    NoValidSamples,
    Cancelled,
    DeadlineExceeded,
    BudgetExceeded,
    StaleInput,
    Unavailable,
    DisplayFailed,
    InternalError
};
enum class ThicknessAdmissionStatus : std::uint8_t
{
    Accepted,
    InvalidRequest,
    Busy,
    Stopping,
    Unavailable
};
enum class ThicknessDisplayMode : std::uint8_t
{
    Continuous,
    Tolerance
};

struct ThicknessInput final
{
    DataRevisionRef source, labels, mesh;
    std::string sourceBinding, labelsBinding, meshBinding;
    // 只有此正整数标签是材料；所有其他标签均不允许穿越。
    std::uint64_t materialLabel = 0;
    // 所有坐标、长度参数与输出使用该单位；不隐式改变源数据尺度。
    ThicknessUnit unit = ThicknessUnit::Unknown;
};

struct ThicknessParams final
{
    double maxDistance = 0.0;
    double sampleSpacing = 0.0;
    double reverseTolerance = 0.0;
    double maxFitResidual = 0.0; // measurement.fit-residual 的原灰度量纲，不是长度。
    double maxLocalizationSigma = 0.0;
    double minSupportRatio = 0.8;
    double coneAngleDegrees = 10.0;
    std::uint32_t directionCount = 9;
    double minOppositeCosine = 0.5;
    double sharpNormalCosine = 0.5;
    double ambiguityAbsolute = 0.0;
    double ambiguityRelative = 0.15;
    // 只允许两端连续的离散边界误差，不能跳过线段内部孔隙。
    double maxBoundaryError = 0.0;
    std::optional<std::array<double, 6>> evaluationBounds;
};

struct ThicknessEvaluation final
{
    double lower = 0.0;
    double upper = 1.0;
    std::array<double, 2> histogramRange{0.0, 1.0};
    std::size_t histogramBins = 32;
    double minRegionArea = 0.0;
};

struct ThicknessDisplay final
{
    HostViewTargets targetViews;
    ThicknessDisplayMode mode = ThicknessDisplayMode::Continuous;
    std::array<double, 2> range{0.0, 1.0};
    double opacity = 1.0;
    bool isVisible = true;
    bool hasLegend = true;
};

struct ThicknessConfig final
{
    std::size_t maxWorkingBytes = 512U * 1024U * 1024U;
    std::size_t maxSamples = 1000000;
    std::uint32_t deadlineMilliseconds = 60000;
    std::uint32_t stopTimeoutMilliseconds = 250;
};

struct ThicknessSample final
{
    std::uint64_t sourceTriangle = 0;
    std::uint64_t oppositeTriangle = 0;
    // 源三角形上的子三角形足迹；不复制源网格坐标。
    std::array<ThicknessPoint, 3> barycentricCorners{};
    ThicknessPoint source{}, opposite{};
    double area = 0.0;
    double thickness = 0.0;
    std::array<double, 2> endpointTrim{};
    std::uint32_t directionIndex = 0;
    ThicknessValidity validity = ThicknessValidity::LowSurfaceQuality;
};

struct ThicknessStatistics final
{
    std::size_t sampleCount = 0;
    std::size_t validCount = 0;
    double evaluatedArea = 0.0;
    double validArea = 0.0;
    double coverage = 0.0;
    std::optional<double> minimum, maximum, mean;
    std::optional<std::size_t> minimumSample, maximumSample;
    std::array<std::optional<double>, 3> quantiles{}; // Q05/Q50/Q95，逆累计面积规则。
    std::vector<double> histogramAreas;
    double histogramBelowArea = 0.0;
    double histogramAboveArea = 0.0;
    std::array<std::size_t, static_cast<std::size_t>(ThicknessValidity::Count)> reasonCounts{};
    std::array<double, static_cast<std::size_t>(ThicknessValidity::Count)> reasonAreas{};
};

struct ThicknessRegion final
{
    std::size_t id = 0;
    bool isBelowLower = false;
    bool isDisplayed = true;
    double area = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    std::array<double, 6> sampleBounds{};
    std::vector<std::size_t> sampleIds;
};

struct ThicknessArchive final
{
    std::uint32_t schemaVersion = 1;
    std::string algorithmVersion = "wall-thickness-ray-1";
    ThicknessInput input;
    ThicknessParams params;
    ThicknessEvaluation evaluation;
    ThicknessConfig limits;
};

struct ThicknessSnapshot final
{
    DataRevisionRef result;
    ThicknessArchive archive;
    std::shared_ptr<const std::vector<ThicknessSample>> samples;
    ThicknessStatistics statistics;
    std::vector<ThicknessRegion> regions;
    std::uint32_t subdivisionCount = 0;
    bool isCurrent = false;
};

enum class ThicknessAction : std::uint8_t
{
    None,
    Start,
    Cancel,
    Clear,
    SetEvaluation,
    SetDisplay,
    SetActive,
    SelectSample
};
struct ThicknessRequest final
{
    ThicknessAction action = ThicknessAction::None;
    std::optional<ThicknessInput> input;
    std::optional<ThicknessParams> params;
    std::optional<ThicknessEvaluation> evaluation;
    std::optional<ThicknessDisplay> display;
    std::optional<DataRevisionRef> resultRevision;
    std::optional<std::size_t> sampleIndex;
    std::uint64_t targetRequestId = 0;
};
struct ThicknessAdmission final
{
    ThicknessAdmissionStatus status = ThicknessAdmissionStatus::InvalidRequest;
    std::uint64_t requestId = 0;
};
struct ThicknessResult final
{
    std::uint64_t requestId = 0;
    ThicknessStatus status = ThicknessStatus::Unavailable;
    DataRevisionRef result;
    bool isActivated = false;
    bool isDisplayReady = false;
    std::string message;
};
using ThicknessCallback = std::function<void(const ThicknessResult &)>;

struct ThicknessState final
{
    bool isAttached = false;
    bool isBusy = false;
    bool isStopping = false;
    bool isCurrent = false;
    bool isDisplayReady = false;
    std::uint64_t requestId = 0;
    DataRevisionRef result;
    ThicknessStatus status = ThicknessStatus::Unavailable;
    std::optional<std::size_t> selectedSample;
};
