#pragma once

#include "Data/DataGraphTypes.h"

#include <cstddef>
#include <array>
#include <map>
#include <optional>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <string_view>

enum class GapReadError { None, Unavailable, InvalidRequest, NotFound, Cancelled, Failed };
struct GapObjectRecord final {
    std::int32_t id = 0;
    std::uint64_t voxelCount = 0;
    double volumeMM3 = 0, parentVolumeMM3 = 0, intersectionVolumeMM3 = 0;
    double equivalentDiameterMM=0;
    bool isRoiPartial = false, hasWholeObjectGeometry = false;
    std::array<double,6> boundsModel{};
    std::uint64_t firstPoint = 0, pointCount = 0, firstTriangle = 0, triangleCount = 0;
};
struct GapResultDescriptor final {
    DataRevisionRef result, source;
    std::string coordinateFrame;
    std::uint64_t objectCount = 0, pointCount = 0, triangleCount = 0;
};
struct GapObjectPage final {
    GapReadError error = GapReadError::Unavailable;
    DataRevisionRef result;
    std::uint64_t total = 0, next = 0;
    std::vector<GapObjectRecord> objects;
};
struct GapObjectReadResult final {
    GapReadError error = GapReadError::Unavailable;
    DataRevisionRef result;
    std::optional<GapObjectRecord> object;
};
struct GapGeometryChunk final {
    GapReadError error = GapReadError::Unavailable;
    DataRevisionRef result;
    std::int32_t objectId = 0;
    std::uint64_t firstPoint = 0, firstTriangle = 0;
    std::uint64_t nextPoint = 0, nextTriangle = 0, totalPoints = 0, totalTriangles = 0;
    std::vector<std::array<double,3>> points;
    // 保留正式结果的全局点编号，不将显示批次局部编号当作正式编号。
    std::vector<std::uint64_t> triangleIndices;
};
// 读取器固定一代正式结果，持有后备存储有效性；只读方法可在 worker 使用。
// 对象页最多 4096 条并保持正式表顺序；几何页每类最多 65536 条，按 next 继续读取。
class GapResultReader {
public:
    virtual ~GapResultReader() noexcept = default;
    virtual const GapResultDescriptor& GetMetadata() const noexcept = 0;
    // 目录读者保留同一修订的摘要及索引，不保活网格；其几何读取返回 Unavailable。
    virtual std::shared_ptr<const GapResultReader> CreateObjectReader() const = 0;
    virtual GapObjectPage GetObjects(std::uint64_t offset,std::size_t count,
        const std::function<bool()>& getCancelled = {}) const = 0;
    virtual GapObjectReadResult GetObjectById(std::int32_t id) const = 0;
    virtual GapGeometryChunk GetGeometry(std::int32_t id,std::uint64_t pointOffset,
        std::uint64_t triangleOffset,std::size_t count,const std::function<bool()>& getCancelled = {}) const = 0;
};

// worker 执行轴；Host 只读取已发布状态，不借此控制 overlay 显示。
enum class GapAnalysisState {
    Idle,
    Running,
    Succeeded,
    Failed,
    Stale
};

enum class GapResultStatus {
    Succeeded,
    SucceededWithDisplayFailure,
    Failed,
    SourceChanged
};
enum class GapAdmissionStatus { Accepted, InvalidRequest, Busy, Stopping, Unavailable };
struct GapAdmission final {
    GapAdmissionStatus status = GapAdmissionStatus::InvalidRequest;
    std::uint64_t requestId = 0;
};

// Gap Feature 的等值面阈值来源，不携带 Host 窗口或实现对象。
enum class GapIsoMode {
    DataRangeRatio,
    AbsoluteValue
};

struct GapSurfaceConfig {
    GapIsoMode isoMode = GapIsoMode::DataRangeRatio;
    // DataRangeRatio 下按 min + (max-min)*ratio 解析；当前不钳制范围。
    double dataRangeRatio = 0.0;
    // AbsoluteValue 下直接使用输入标量域中的阈值。
    double absoluteIsoValue = 0.0;
    // DefX 材料定义的背景与材料均值；Feature 只做请求映射。
    float backgroundMean = 0.0f;
    float materialMean = 0.0f;
};

struct GapVoidParams {
    // 显式控制 Gap 结果的最小体积筛选；关闭时 minVolumeMM3 不参与筛选。
    bool isFilterEnabled = false;
    // 最小缺陷体积，单位 mm^3；保留体积大于或等于此值的检测区域。
    double minVolumeMM3 = 0.00;
};

struct GapStatistics final {
    std::size_t objectVoxelCount = 0;
    std::size_t voidVoxelCount = 0;
    double objectVolumeMM3 = 0.0;
    double voidVolumeMM3 = 0.0;
    double porosityRatio = 0.0;
    bool isPorosityAvailable = true;
    std::size_t evaluationVoxelCount = 0, actualSupportVoxelCount = 0;
    std::optional<DataRevisionRef> evaluationRoi, detectionRoi;
};
enum class GapRoiPolicy : std::uint8_t { InsideOnly, WholeIfIntersects };

enum class GapColorMode { Constant, Gradient, Rainbow, InverseRainbow, HueLoop };
enum class GapRangeMode { Result, SelectedInterval };
enum class GapDisplayStyle { Constant, Inclined, InverseInclined };
struct GapColorSegment final {
    // 缺省端点分别表示负／正无穷；无穷端段只支持恒定色，邻段必须连续。
    std::optional<double> lower, upper;
    GapColorMode mode=GapColorMode::Gradient;
    std::array<double,3> lowColor{0.294,0.294,0.84},highColor{0.84,0.294,0.294};
};

// 色带只投影已发布的缺陷体积；不改变标签、筛选、网格和统计。
struct GapDisplayParams final {
    GapColorMode mode = GapColorMode::Rainbow;
    bool isReversed=false,hasLegend=true;
    std::vector<std::string> hiddenLegendViews;
    std::array<double,2> legendPosition{.025,.20};
    std::map<std::string,std::array<double,2>> legendPositions;
    double opacity=1;
    std::array<double, 3> constantColor{0.70, 0.70, 0.70};
    std::array<double, 3> lowColor{0.294, 0.294, 0.84};
    std::array<double, 3> highColor{0.84, 0.294, 0.294};
    std::array<double, 3> belowColor{0.64, 0.29, 0.78};
    std::array<double, 3> aboveColor{0.84, 0.29, 0.65};
    GapRangeMode rangeMode = GapRangeMode::Result;
    std::array<double, 2> range{0, 1};
    std::vector<GapColorSegment> segments;
    GapDisplayStyle style=GapDisplayStyle::Constant;
    // 按本 Feature 当前体积范围渐变的相对叠加强度，仍乘以各视图原有透明度。
    std::array<double,2> opacityRange{0.15,1.0};
    // 当前视域的精确几何批次驻留目标，不限制正式对象数量。
};

struct GapObjectSelection final {
    DataRevisionRef result;
    std::optional<std::int32_t> regionId;
    bool isIsolated=false;
};
namespace GapObjectBindingKeys {inline constexpr std::string_view Select="gap.object.select.pointer";}

struct GapHostState final {
    GapAnalysisState analysisState = GapAnalysisState::Idle;
    GapStatistics statistics;
    DataCommitId commitId = 0;
    DataRevisionRef sourceRevision;
    DataRevisionRef labelMap;
    DataRevisionRef voidTable;
    DataRevisionRef voidMesh;
    DataRevisionRef statisticsData;
    DataRevisionRef resultSet;
    bool isViewActive = false;
    bool isExitPending = false;
    bool isOverlayVisible = false;
    GapDisplayParams display;
    std::optional<std::array<double,2>> displayRange;
    bool isDisplayComplete=true,hasDisplayFailure=false;
    std::uint64_t displayRequestedBatches=0,displayLoadedBatches=0;
    std::string displayMessage;
    DataRevisionRef selectionResult;
    std::optional<std::int32_t> selectedObject;
    bool isSelectionIsolated=false,isObjectPickingEnabled=false;
};

struct GapHostResult final {
    GapResultStatus status = GapResultStatus::Failed;
    DataCommitId commitId = 0;
    DataRevisionRef sourceRevision;
    DataRevisionRef labelMap;
    DataRevisionRef voidTable;
    DataRevisionRef voidMesh;
    DataRevisionRef statisticsData;
    DataRevisionRef resultSet;
    GapStatistics statistics;
    std::string message;
    std::uint64_t requestId = 0;
};
