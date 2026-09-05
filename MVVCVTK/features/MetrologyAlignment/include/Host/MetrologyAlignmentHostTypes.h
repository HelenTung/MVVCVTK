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
    InternalError
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
};

struct AlignmentArchive final {
    std::uint32_t schemaVersion = 1;
    std::string algorithmVersion = "metrology-alignment-1";
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
    Restore
};
struct AlignmentRequest final {
    AlignmentAction action = AlignmentAction::Start;
    std::optional<AlignmentRecipe> recipe;
    std::optional<AlignmentInput> input;
    DataRevisionRef recipeRef;
    DataRevisionRef resultRef;
    // row-major，列向量：pTarget = sourceToTarget * pSource；不得包含尺度。
    std::vector<AlignmentMatrix> initialPoses{alignmentIdentity};
    std::optional<AlignmentArchive> archive;
    // Restore 时显式映射名义数据；source/mesh/scope 由 input 映射。
    DataRevisionRef restoredNominal;
    std::optional<bool> isVisible;
    std::uint64_t targetRequestId = 0;
    bool isActivationRequested = true;
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
    std::size_t pointLimit = 100000;
    std::size_t constraintLimit = 4096;
    std::size_t workingBytes = 256U * 1024U * 1024U;
    std::uint64_t deadlineMs = 30000;
    double axisLength = 10.0;
    bool isOverlayVisible = true;
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
};
