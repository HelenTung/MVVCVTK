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
    NoValidSource,
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
    // 源灰度支持 Float32/Float64 及不超过 32 位的整数；标签编号仍按整数精确比较。
    DataRevisionRef source, labels, mesh;
    std::string sourceBinding, labelsBinding, meshBinding;
    // 单一高灰度材料；标签用于身份与边界覆盖核验，材料路径由原始灰度定义。
    std::uint64_t materialLabel = 0;
    // 所有坐标、长度参数与输出使用该单位；不隐式改变源数据尺度。
    ThicknessUnit unit = ThicknessUnit::Unknown;
};

enum class ThicknessBoundaryPolicy : std::uint8_t
{
    Complete,
    // 允许网格在源体素盒面上开放；只测量有完整搜索邻域的来源。
    SourceExtentLocal
};

struct ThicknessParams final
{
    double maxDistance = 0.0;
    // 只控制显示/面积统计采样；节点积分固定为原始三角形上的 3×3 Gauss 规则。
    double sampleSpacing = 0.0;
    // 必须显式指定，与 source 的原始存储灰度同量纲；材料为 trilinear(source) > threshold。
    std::optional<double> materialThreshold;
    double coneAngleDegrees = 30.0; // 围绕源法向的搜索锥半角。
    std::uint32_t directionCount = 9;
    // 只允许两端连续的灰度边界误差，不能跳过线段内部孔隙。
    double maxBoundaryError = 0.0;
    ThicknessBoundaryPolicy boundaryPolicy = ThicknessBoundaryPolicy::Complete;
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
    // 源三角形上的子三角形足迹；不复制源网格坐标。
    std::array<ThicknessPoint, 3> barycentricCorners{};
    // 节点场的查询位置，不代表某条唯一测量射线的起点。
    ThicknessPoint source{};
    double area = 0.0;
    double thickness = 0.0;
    ThicknessValidity validity = ThicknessValidity::LowSurfaceQuality;
};

struct ThicknessNode final
{
    std::array<int, 3> index{};
    // 原始源三角形面积 × Gauss 权重 × Q1 帽函数；无效来源不进入分母。
    double validWeight = 0.0;
    double totalWeight = 0.0;
    // 仅 validWeight > 0 时有效；没有有效来源的节点不以零厚度参与插值。
    double thickness = 0.0;
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
    std::uint32_t schemaVersion = 2;
    std::string algorithmVersion = "wall-thickness-normal-offset-field-2";
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
    // 按 index 字典序排列；索引属于 archive.input.source 的原始网格。
    std::shared_ptr<const std::vector<ThicknessNode>> nodes;
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
