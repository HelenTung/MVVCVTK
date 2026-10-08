#pragma once

#include "Data/DataGraphTypes.h"
#include "Host/Types/HostViewTypes.h"
#include "Host/VoxelVolume.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using AlignmentPoint = std::array<double, 3>;
using AlignmentMatrix = std::array<double, 16>;
inline constexpr AlignmentMatrix alignmentIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

enum class AlignmentUnit : std::uint8_t {
    ModelUnit,
    Millimeter,
    Meter
};
enum class AlignmentGeometryKind : std::uint8_t {
    Point,
    Line,
    Plane,
    Circle,
    Sphere,
    Cylinder
};
enum class AlignmentMethod : std::uint8_t {
    SequentialPlanes,
    PlaneTwoHoles,
    Rps,
    ConstrainedBestFit
};
enum class AlignmentAssociation : std::uint8_t {
    LeastSquares,
    SequentialLeastSquares,
    Contact
};
enum class AlignmentConstraintKind : std::uint8_t {
    Hard,
    Soft,
    Check
};
enum class AlignmentStatus : std::uint8_t {
    FullyDetermined,
    Underconstrained,
    Degenerate,
    Conflicting,
    Ambiguous,
    NotConverged,
    Cancelled,
    DeadlineExceeded,
    Stale,
    InvalidInput,
    Unsupported,
    BudgetExceeded,
    Unavailable,
    InternalError,
    IoError
};

struct AlignmentReferenceRequest final {
    std::string filePath;
    std::string frameId;
    std::string coordinateFrame;
    // STL 不存长度单位。ModelUnit 仅能保持为 ModelUnit，不能推测物理单位。
    AlignmentUnit fileUnit = AlignmentUnit::ModelUnit;
    AlignmentUnit unit = AlignmentUnit::ModelUnit;
};

struct AlignmentReferenceSnapshot final {
    DataRevisionRef mesh;
    std::string filePath;
    std::string sha256;
    std::string frameId;
    std::string coordinateFrame;
    AlignmentUnit fileUnit = AlignmentUnit::ModelUnit;
    AlignmentUnit unit = AlignmentUnit::ModelUnit;
    std::uint64_t sourceBytes = 0;
    std::uint64_t pointCount = 0;
    std::uint64_t triangleCount = 0;
};

struct AlignmentInput final {
    // frame/长度单位声明与精确源修订绑定；宿主改变声明时必须产生新的 source/scope 修订。
    DataRevisionRef source;
    DataRevisionRef mesh;
    DataRevisionRef scopeData;
    std::string sourceFrameId;
    std::string coordinateFrame;
    std::string scope;
    AlignmentUnit unit = AlignmentUnit::ModelUnit;
    // 可选绑定名必须在接纳时指向对应精确修订；revision 与 target 同时参与 CAS。
    std::string sourceBinding;
    std::string meshBinding;
    std::string scopeBinding;
};

struct AlignmentRegion final {
    // 顶点选择与 targetBounds 二选一；顶点编号只能在 pinnedMesh 修订使用。
    DataRevisionRef pinnedMesh;
    std::vector<std::uint64_t> vertexIds;
    std::optional<std::array<double, 6>> targetBounds;
};

struct AlignmentGeometrySpec final {
    std::string id;
    AlignmentGeometryKind kind = AlignmentGeometryKind::Plane;
    AlignmentAssociation association = AlignmentAssociation::LeastSquares;
    AlignmentRegion region;
    // 在目标空间给出符号/初始轴提示，通过本次初始刚体姿态恢复到源空间。
    AlignmentPoint targetDirection{0, 0, 1};
    std::optional<std::array<double, 2>> radiusRange;
    double maxFitRms = 1.0;
    double minCoverage = 1.0;
    double minDirectionCosine = 0.01;
};

struct AlignmentConstraint final {
    std::size_t geometryIndex = 0;
    AlignmentPoint nominalPoint{};
    AlignmentPoint targetDirection{0, 0, 1};
    AlignmentConstraintKind kind = AlignmentConstraintKind::Hard;
    std::uint32_t priority = 0;
    double weight = 1.0;
    double tolerance = 1e-6;
    // 无限直线/圆柱必须指定目标截面，交点才有可追溯的轴向位置。
    std::optional<std::size_t> sectionPlane;
};

struct AlignmentFitPair final {
    std::uint64_t vertexId = 0;
    AlignmentPoint nominalPoint{};
    // 空值代表三个点到点分量；否则为目标空间点到平面残差。
    std::optional<AlignmentPoint> targetNormal;
    double weight = 1.0;
};

struct AlignmentSurfaceScale final {
    // 源坐标网格单元边长；0 使用全部选中顶点，不改变原计量网格。
    double voxelSize = 0;
    double maxPairDistance = 1;
    std::size_t iterationLimit = 40;
};
struct AlignmentSurfaceStage final {
    double voxelSize = 0, maxPairDistance = 0;
    std::size_t selectedVertices = 0, samples = 0, iterations = 0, rejectedSamples = 0;
    // 聚合后按代表点的配对状态累计组面积；acceptedArea 不是完整表面的逐点覆盖率。
    double selectedArea = 0, acceptedArea = 0;
    bool isConverged = false;
};
struct AlignmentSurfaceFit final {
    // 空选区使用完整实测网格；ROI 在初始姿态下确定后固定，不随迭代漂移。
    std::optional<AlignmentRegion> sourceRegion;
    // 编号属于 nominalData 指向的参考三角网格；空集合使用全部三角形。
    std::vector<std::uint64_t> referenceTriangles;
    std::size_t iterationLimit = 40;
    bool isPointToPlane = true;
    // 空列表保留全顶点单级行为；显式列表使用各级迭代上限，按粗到细且距离门槛不逐级放宽。
    std::vector<AlignmentSurfaceScale> scales;
};

struct AlignmentPrealignParams final {
    // 长度使用配方单位；sampleSpacing^2 是每个工作样本的目标面积，不是最小点间距。
    double sampleSpacing = 0;
    double normalRadius = 0;
    double featureRadius = 0;
    double maxPairDistance = 0;
    double minCoverage = 0.25;
    double edgeLengthRatio = 0.9;
    // 粗候选的分群距离；0 使用 maxPairDistance，正式解仍用求解容差判别。
    double candidateSeparation = 0;
    std::size_t neighborLimit = 64;
    std::size_t matchCount = 4;
    std::size_t iterationLimit = 10000;
    std::size_t candidateLimit = 8;
    std::uint64_t randomSeed = 0;
    // 空集合覆盖整个表面；源编号绑定 exactMesh，参考编号绑定 nominalData。
    std::vector<std::uint64_t> sourceTriangles;
    std::vector<std::uint64_t> referenceTriangles;
};

struct AlignmentPrealignStats final {
    std::size_t sourceSamples = 0, referenceSamples = 0;
    std::size_t sourceFeatures = 0, referenceFeatures = 0;
    std::size_t trials = 0, candidates = 0;
    double bestCoverage = 0;
    // 源选区面积使用配方单位的平方；质量过滤的面与面积单独报告。
    std::size_t sourceRejectedTriangles = 0;
    double sourceSelectedArea = 0, sourceRejectedArea = 0;
    bool isCandidateLimitReached = false;
};

struct AlignmentRecipe final {
    std::string id;
    std::string targetFrameId;
    AlignmentUnit unit = AlignmentUnit::ModelUnit;
    AlignmentMethod method = AlignmentMethod::SequentialPlanes;
    std::vector<AlignmentGeometrySpec> geometries;
    std::vector<AlignmentConstraint> constraints;
    // 最佳拟合的对应顶点绑定到 exactMesh；不允许在另一网格复用编号。
    DataRevisionRef exactMesh;
    std::vector<AlignmentFitPair> fitPairs;
    // 与显式 fitPairs 互斥。nominalData 必须为同单位、同坐标轴约定的参考表面。
    std::optional<AlignmentSurfaceFit> surfaceFit;
    std::optional<AlignmentPrealignParams> prealignment;
    DataRevisionRef nominalData;
    // ConstrainedBestFit 时前 0..3 个几何为 A/B/C 基准，其余点对只优化余下运动。
    std::size_t datumCount = 3;
    AlignmentPoint datumOffsets{}; // 目标 x/y/z 基准位置。
    double lengthScale = 1.0;
    double rankTolerance = 1e-10;
    double conditionLimit = 1e8;
    double solveTolerance = 1e-8;
    double maxAlignmentRms = 1e-3;
    // 对应关系的三维欧氏距离门槛，独立于点到平面残差；不能用无限平面吸附远处数据。
    double maxPairDistance = 1.0;
    double minPairCoverage = 1.0;
    std::optional<double> minPairNormalCosine;
    double huberDistance = 1.0;
    std::size_t iterationLimit = 100;
    bool requiresMeasurementQuality = false;
    double minSupportRatio = 0.0;
    double maxLocalizationSigma = 1.0;
    double maxSurfaceFitResidual = 1.0;
};

struct AlignmentGeometrySupport final {
    // 有效拟合样本相对质心的三个主方向 RMS 展宽，降序排列，使用模型长度单位。
    // 描述离散样本分布，不代表工件尺寸、完整表面积覆盖率或计量不确定度。
    std::array<double,3> principalRmsSpreads{};
    // Line/Cylinder：样本投影到拟合轴上的最大值减最小值，不是整件轴向长度。
    std::optional<double> axialSpan;
    // Circle/Cylinder：容纳全部样本方位角的最小圆弧（度），等于 360 度减最大空隙。
    // 有限离散整圈样本通常仍小于 360；不得据此宣称表面闭合或周向连续。
    std::optional<double> angularSpanDegrees;
    // Sphere：球心至样本的单位向量均值之长度，[0,1]；接近 1 表示方向集中。
    std::optional<double> radialDirectionMeanLength;
};
struct AlignmentGeometry final {
    std::string id;
    AlignmentGeometryKind kind = AlignmentGeometryKind::Point;
    // Line/Cylinder 的 center 是轴上一点，以样本质心截面锚定；不是工件原点。
    AlignmentPoint sourceCenter{};
    AlignmentPoint sourceDirection{0, 0, 1};
    double radius = 0.0;
    double fitRms = 0.0;
    double fitMax = 0.0;
    std::size_t sampleCount = 0;
    std::size_t rejectedCount = 0;
    // 旧结果或名义定义可以缺失；新成功拟合保留有效样本的支撑信息。
    std::optional<AlignmentGeometrySupport> support;
};

struct AlignmentResidual final {
    std::string id;
    AlignmentConstraintKind kind = AlignmentConstraintKind::Hard;
    std::uint32_t priority = 0;
    double value = 0.0;
    double tolerance = 0.0;
    bool isUsed = true;
};

struct AlignmentDiagnostics final {
    AlignmentStatus status = AlignmentStatus::InvalidInput;
    std::size_t hardRemaining = 6;
    std::size_t remaining = 6;
    std::vector<double> singularValues;
    // twist 顺序为绕目标原点旋转 / lengthScale、目标平移；仅供局部可观测性诊断。
    std::vector<std::array<double, 6>> nullspace;
    std::vector<std::size_t> stageRemaining;
    double lengthScale = 1.0;
    double rankTolerance = 1e-10;
    double condition = 1.0;
    double rms = 0.0;
    double maximum = 0.0;
    std::size_t iterations = 0;
    std::size_t rejectedPairs = 0;
    bool isQualityPassed = false;
    std::string message;
    std::optional<AlignmentPrealignStats> prealignment;
    std::vector<AlignmentSurfaceStage> surfaceStages;
};

// 以下评价绑定已发布的刚体对齐结果；检测区域独立于配准选区。
// NormalProjection 是最近名义三角面法向上的点积，不是全局内外符号距离或沿法向射线求交。
enum class AlignmentDeviationMetric : std::uint8_t { UnsignedDistance, NominalNormalProjection };
enum class AlignmentDeviationState : std::uint8_t {
    NotSelected, Valid, InvalidSource, InvalidTarget, TooFar, NormalMismatch, NormalUnavailable
};
enum class AlignmentInspectionVerdict : std::uint8_t { NotEvaluated, Pass, Fail, Indeterminate };
struct AlignmentDeviationRequest final {
    DataRevisionRef alignmentResult;
    AlignmentDeviationMetric metric = AlignmentDeviationMetric::UnsignedDistance;
    // 编号属于 alignmentResult 固定的实测/名义网格，空集合使用全部三角形。
    std::vector<std::uint64_t> sourceTriangles, referenceTriangles;
    double maxDistance = 1;
    std::optional<double> minNormalCosine;
    // 与 metric 同定义、同长度单位；空值只报告偏差，不作合格判定。
    std::optional<std::array<double, 2>> tolerance;
    bool reverseCoverage = true;
    bool requiresMeasurementQuality = false;
    double minSupportRatio = 0, maxLocalizationSigma = 1, maxSurfaceFitResidual = 1;
};
struct AlignmentDeviationSample final {
    // 下标直接对应原网格顶点；NotSelected/InvalidSource 时 value、distance 不可解释为测量值。
    double value = 0, distance = 0, area = 0;
    std::uint64_t closestTriangle = 0;
    AlignmentDeviationState state = AlignmentDeviationState::NotSelected;
};
struct AlignmentDeviationStatistics final {
    std::array<std::size_t, 7> counts{};
    std::array<double, 7> areas{};
    // 三角形面积三等分至顶点的离散面积积分；不是连续曲面误差极值保证。
    double selectedArea = 0, evaluatedArea = 0, outOfToleranceArea = 0;
    double minimum = 0, maximum = 0, mean = 0, rms = 0, absoluteP95 = 0;
    bool hasValues = false;
};
struct AlignmentDeviationField final {
    std::vector<AlignmentDeviationSample> samples;
    AlignmentDeviationStatistics statistics;
};
struct AlignmentDeviationSnapshot final {
    DataRevisionRef result, alignmentResult, sourceMesh, referenceMesh, transform;
    // 选区编号也只读共享，状态轮询不复制大型 ROI。
    std::shared_ptr<const AlignmentDeviationRequest> parameters;
    AlignmentUnit unit = AlignmentUnit::ModelUnit;
    AlignmentMatrix sourceToTarget = alignmentIdentity;
    // 只读共享数组；查询不会再次复制完整场。reverse 只表示名义到实测的无符号覆盖距离。
    std::shared_ptr<const AlignmentDeviationField> forward, reverse;
    AlignmentInspectionVerdict verdict = AlignmentInspectionVerdict::NotEvaluated;
    bool isCurrent = false;
};

struct AlignmentDeviationDisplay final {
    DataRevisionRef result;
    // 固定显示范围，截断只影响颜色。若启用公差色，范围必须覆盖正式公差区间。
    std::array<double,2> range{0,1};
    bool useToleranceColors = true;
    bool isVisible = true;
    std::optional<std::uint64_t> selectedVertex;
};
struct AlignmentDeviationSelection final {
    DataRevisionRef result;
    std::uint64_t vertex = 0;
    AlignmentPoint sourcePoint{}, alignedPoint{};
    AlignmentDeviationSample sample;
    bool isCurrent = false;
};

enum class InspectionGeometrySource : std::uint8_t { MeasuredSurface, ReferenceSurface, NominalDefinition };
struct InspectionIdealGeometry final {
    AlignmentPoint center{}, direction{0,0,1};
    double radius = 0;
};
struct InspectionSection final {
    // 截面位于对齐目标坐标系；索引属于该精确计量网格，空集合遍历全网格。
    DataRevisionRef pinnedMesh;
    AlignmentPoint planePoint{}, planeNormal{0,0,1};
    std::vector<std::uint64_t> triangles;
    // 闭环等弧长取样的最大间距，至少 8 点。容差只用于平面分类和数值检查，不做空间焊接。
    double sampleSpacing=.1, planeTolerance=1e-8;
    // 多个截线分量时按距此点最近的分量选取；并列、开口或分支均不能静默成为圆。
    std::optional<AlignmentPoint> selectionPoint;
    double maxSelectionDistance=1;
};
struct InspectionSectionSample final {
    std::uint64_t triangle=0;
    std::array<double,3> barycentric{};
    // 由原三角形重心坐标重建的点沿源截面法向投影至平面的带符号距离。
    double planeCorrection=0;
};
struct InspectionSectionTrace final {
    AlignmentPoint sourcePlanePoint{}, sourcePlaneNormal{0,0,1};
    std::size_t components=0, closedLoops=0, selectedSegments=0;
    std::size_t sourceTriangleCount=0;
    double perimeter=0, sampleSpacing=0, maxPlaneCorrection=0;
    std::vector<InspectionSectionSample> samples;
};
struct InspectionGeometrySpec final {
    // NominalDefinition 仅使用 fit.id/kind 与 ideal；其他来源从独立 ROI 按最小二乘拟合。
    AlignmentGeometrySpec fit;
    InspectionGeometrySource source = InspectionGeometrySource::MeasuredSurface;
    std::optional<InspectionIdealGeometry> ideal;
    std::string definitionSource;
    // 仅 Circle 支持截面模式；与 fit.region 互斥，平面方向采用 section.planeNormal。
    std::optional<InspectionSection> section;
};
enum class InspectionMetric : std::uint8_t {
    Radius, Diameter, CenterX, CenterY, CenterZ, CenterDistance,
    OrientedDirectionAngleDegrees, SignedParallelPlaneDistance, FitRms, FitMaximum
};
struct InspectionOperand final {
    std::size_t geometry = 0;
    // Line/Cylinder 的位置必须以轴线与这个 Plane 的交点构造，不能采用任意轴心。
    std::optional<std::size_t> sectionPlane;
};
struct InspectionMeasure final {
    InspectionMetric metric = InspectionMetric::Diameter;
    InspectionOperand first;
    std::optional<InspectionOperand> second;
    double maxParallelAngleDegrees = .001;
};
struct InspectionItem final {
    std::string id, name;
    InspectionMeasure measured;
    // 二选一：明确名义标量，或在同一对齐坐标系中从名义几何计算相同量。
    std::optional<double> nominal;
    std::optional<InspectionMeasure> nominalMeasure;
    std::string nominalSource;
    // 实测值减名义值的允许区间；角度为度，其余量使用对齐长度单位。
    std::optional<std::array<double,2>> deviationLimits;
};
struct InspectionPlan final {
    std::string id;
    DataRevisionRef alignmentResult;
    std::vector<InspectionGeometrySpec> geometries;
    std::vector<InspectionItem> items;
    double lengthScale=1, rankTolerance=1e-10, conditionLimit=1e8, solveTolerance=1e-8;
    std::size_t iterationLimit=100;
    bool requiresMeasurementQuality=false;
    double minSupportRatio=0, maxLocalizationSigma=1, maxSurfaceFitResidual=1;
};
struct InspectionGeometry final {
    InspectionGeometrySource source=InspectionGeometrySource::MeasuredSurface;
    AlignmentStatus status=AlignmentStatus::InvalidInput;
    AlignmentGeometry fitted;
    AlignmentPoint alignedCenter{}, alignedDirection{};
    std::shared_ptr<const InspectionSectionTrace> section;
    std::string message;
};
struct InspectionItemResult final {
    std::string id;
    std::optional<double> measured, nominal, deviation;
    AlignmentInspectionVerdict verdict=AlignmentInspectionVerdict::Indeterminate;
    std::string message;
};
struct InspectionReport final {
    std::vector<InspectionGeometry> geometries;
    std::vector<InspectionItemResult> items;
    AlignmentInspectionVerdict verdict=AlignmentInspectionVerdict::NotEvaluated;
    std::string algorithmVersion="metrology-inspection-2";
    std::int64_t evaluatedUnixMilliseconds=0;
    // FitRms/FitMaximum 是拟合残差，不是平面度、圆度、圆柱度或测量不确定度。
};
struct InspectionSnapshot final {
    DataRevisionRef result, alignmentResult, sourceMesh, referenceData, transform;
    AlignmentUnit unit=AlignmentUnit::ModelUnit;
    AlignmentMatrix sourceToTarget=alignmentIdentity;
    std::shared_ptr<const InspectionPlan> plan;
    std::shared_ptr<const InspectionReport> report;
    bool isCurrent=false;
};

struct AlignmentScenePart final {
    std::string id;
    DataRevisionRef alignmentResult;
    bool showMeasured=true,showReference=true;
    std::optional<DataRevisionRef> inspection;
    // 选择项目会高亮其 measured/nominal 两侧及构造截面涉及的几何。
    std::optional<std::string> selectedItem;
};
struct AlignmentSceneDisplay final {
    // 拟合线框使用 AlignmentConfig.axisLength 作为示意长度，圆柱线框长度不是高度测量。
    // 黄色为选中元素，青绿为其他拟合，紫红为区域标记；这些颜色不用于公差判定。
    // 场景目标坐标经 anchor 的逆变换进入 Host model 坐标；再由 Host modelToWorld 定位。
    // measured: M * inverse(T_anchor) * T_part，reference: M * inverse(T_anchor)。
    DataRevisionRef anchorAlignment;
    std::vector<AlignmentScenePart> parts;
    std::array<double,3> measuredColor{.15,.75,.9},referenceColor{.75,.75,.75};
    double referenceOpacity=.5;
    bool referenceWireframe=true,isVisible=true;
    std::optional<std::string> selectedPart;
    // 每个所选区域最多绘制这些采样标记；0 不绘制标记，区域边框仍显示。原始计划/截面样本不变。
    std::size_t roiMarkerLimit=4096;
};
enum class AlignmentSceneRegionKind : std::uint8_t { VertexCandidates, TargetBounds, SectionSamples };
struct AlignmentSceneRegionDisplay final {
    std::size_t geometry=0;
    AlignmentSceneRegionKind kind=AlignmentSceneRegionKind::VertexCandidates;
    // bounds 不扫描全网格计数，缺失的截面 trace 也为空；不能用 0 冒充没有样本。
    std::optional<std::uint64_t> sourceCount;
    std::uint64_t drawnMarkers=0;
};
struct AlignmentSceneItem final {
    std::string partId;
    DataRevisionRef inspectionResult,alignmentResult,anchorAlignment;
    std::size_t item=0;
    std::vector<std::size_t> geometries;
    // 共享冻结结果，可从 plan.geometries 追溯完整区域；不复制选区/截面数组。
    std::shared_ptr<const InspectionPlan> plan;
    std::shared_ptr<const InspectionReport> report;
    std::vector<AlignmentSceneRegionDisplay> displayedRegions;
    AlignmentMatrix targetToModel=alignmentIdentity;
    AlignmentUnit unit=AlignmentUnit::ModelUnit;
    bool isCurrent=false,isSelected=false;
};
struct AlignmentSceneVertex final {
    std::string partId;
    DataRevisionRef alignmentResult,anchorAlignment,mesh;
    std::uint64_t vertex=0;
    bool isReference=false,isCurrent=false;
    AlignmentUnit unit=AlignmentUnit::ModelUnit;
    AlignmentPoint nativePoint{},targetPoint{},modelPoint{};
};
struct AlignmentResult final {
    std::uint64_t requestId = 0;
    AlignmentStatus status = AlignmentStatus::Unavailable;
    DataRevisionRef recipe;
    DataRevisionRef result;
    DataRevisionRef transform;
    AlignmentDiagnostics diagnostics;
    bool isActivated = false;
    bool isDisplayReady = false;
    std::string message;
    DataRevisionRef referenceMesh;
    DataRevisionRef deviationResult;
    DataRevisionRef inspectionResult;
    DataRevisionRef voxelVolumeResult;
    bool isModelPoseApplied=false;
};

struct AlignmentArchive final {
    std::uint32_t schemaVersion = 1;
    std::string algorithmVersion = "metrology-alignment-3";
    AlignmentRecipe recipe;
    AlignmentInput input;
    // 导出值只用于交换；恢复不直接激活旧矩阵，必须以重映射输入重新求解。
    AlignmentMatrix sourceToTarget = alignmentIdentity;
};

struct AlignmentSnapshot final {
    DataRevisionRef result;
    DataRevisionRef transform;
    DataRevisionRef recipe;
    AlignmentInput input;
    std::vector<AlignmentGeometry> geometries;
    std::vector<AlignmentResidual> residuals;
    AlignmentDiagnostics diagnostics;
    bool isCurrent = false;
};

enum class AlignmentAction : std::uint8_t {
    SaveRecipe,
    Start,
    Cancel,
    Activate,
    Deactivate,
    SetVisibility,
    Restore,
    ImportReference,
    // FullyDetermined 表示计算/发布成功；合格判定读取 DeviationSnapshot.verdict。
    EvaluateSurface,
    SetDeviationDisplay,
    Inspect,
    EvaluateVoxelVolume,
    // 以 resultRef 的绝对计量矩阵替换当前 Host 模型显示姿态；不是单部件变换。
    // 仅当前显示数据等于结果源体或源网格时接纳，帧提交完成后才回调成功；不重采样。
    ApplyModelPose,
    SetSceneDisplay
};
struct AlignmentRequest final {
    AlignmentAction action = AlignmentAction::Start;
    std::optional<AlignmentRecipe> recipe;
    std::optional<AlignmentInput> input;
    DataRevisionRef recipeRef;
    DataRevisionRef resultRef;
    // row-major，列向量：pTarget = sourceToTarget * pSource；不得包含尺度。
    // 启用自动预对齐时保持默认；实际生成的候选保存在正式结果的输入记录中。
    std::vector<AlignmentMatrix> initialPoses{alignmentIdentity};
    std::optional<AlignmentArchive> archive;
    // Restore 时显式映射名义数据；source/mesh/scope 由 input 映射。
    DataRevisionRef restoredNominal;
    std::optional<bool> isVisible;
    std::uint64_t targetRequestId = 0;
    bool isActivationRequested = true;
    std::optional<AlignmentReferenceRequest> reference;
    std::optional<AlignmentDeviationRequest> deviation;
    std::optional<AlignmentDeviationDisplay> deviationDisplay;
    std::optional<InspectionPlan> inspection;
    std::optional<VoxelVolumeRequest> voxelVolume;
    std::optional<AlignmentSceneDisplay> sceneDisplay;
};
enum class AlignmentAdmissionStatus : std::uint8_t {
    Accepted,
    InvalidRequest,
    Busy,
    Unavailable
};
struct AlignmentAdmission final {
    AlignmentAdmissionStatus status = AlignmentAdmissionStatus::InvalidRequest;
    std::uint64_t requestId = 0;
};
// Accepted 且 callback 有效时恰好一次；可能同步完成，始终 owner thread。
// callback 内允许查询/新请求/Detach；提交期间 observer 重入写请求返回 Busy。
using AlignmentCallback = std::function<void(AlignmentResult)>;

struct AlignmentConfig final {
    HostViewTargets targetViews;
    // 0 不启用数量预算。导入限制唯一顶点，预对齐限制两侧样本总数，求解限制工作样本。
    std::size_t pointLimit = 0;
    std::size_t constraintLimit = 0;
    std::optional<std::size_t> workingBytes {};
    std::optional<std::size_t> deadlineMs{};
    double axisLength = 10.0;
    bool isOverlayVisible = true;
};

struct AlignmentModelPoseState final {
    DataRevisionRef alignmentResult,displayedData;
    AlignmentMatrix modelToWorld=alignmentIdentity;
    std::uint64_t transformRevision=0,sessionGeneration=0;
    DataBindingRevision bindingRevision=0;
    AlignmentStatus status=AlignmentStatus::Unavailable;
    bool isPending=false,isCurrent=false;
};
struct AlignmentState final {
    bool isAttached = false;
    bool isBusy = false;
    bool isStopping = false;
    bool isOverlayVisible = true;
    bool isDisplayReady = false;
    bool isCurrent = false;
    std::uint64_t requestId = 0;
    DataRevisionRef activeResult;
    AlignmentStatus lastStatus = AlignmentStatus::Unavailable;
    std::optional<AlignmentDeviationDisplay> deviationDisplay;
    bool isDeviationDisplayReady = false;
    AlignmentModelPoseState modelPose;
    std::shared_ptr<const AlignmentSceneDisplay> sceneDisplay;
    bool isSceneDisplayReady=false;
    // 包含保守的渲染缓存计账；不是实测显存/工作集。旧新场景并存时均计入准入。
    std::size_t sceneWorkingBytes=0;
};
