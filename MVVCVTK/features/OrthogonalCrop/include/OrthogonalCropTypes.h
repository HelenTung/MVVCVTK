#pragma once
// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/OrthogonalCropTypes.h
// 分类: Math / Data Types
// OrthogonalCropTypes.h — 正交裁切独立插件纯数据结构
// =====================================================================
// 类型层只保存公式节点、输入快照、shader transaction 与按需物化结果；
// 不依赖 App runtime、Renderer、Interactor、mapper 或具体窗口对象。

#include "Host/TrustedDataPort.h"
#include "Data/DataPayloads.h"
#include "Render/Contracts/RenderEffect.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using CropDocumentId = std::uint64_t;
using CropNodeId = std::uint64_t;
using CropRequestId = std::uint64_t;
using CropResultId = std::uint64_t;

struct CropPredicateTable;

struct CropShaderPayload final {
    std::uint64_t revision = 0;
    RenderInputStamp sourceStamp;
    std::size_t nodeCount = 0;
    std::shared_ptr<const CropPredicateTable> predicateTable;
    CropNodeId nodeId = 0;
};

// active input model AABB，布局固定为 [minX, maxX, minY, maxY, minZ, maxZ]。
using CropBoundsDouble6Array = std::array<double, 6>;
// 三维点或向量，布局固定为 [x, y, z]；具体坐标系由字段名或接口注释限定。
using CropVectorDouble3Array = std::array<double, 3>;
using CropMatrixDouble16Array = std::array<double, 16>; // 4x4 仿射变换矩阵，按 VTK DeepCopy 约定展开。
using CropPointFloat3Array = std::array<float, 3>;
// 闭区间 voxel index bounds，布局固定为 [minI, maxI, minJ, maxJ, minK, maxK]。
using CropIndexBoundsInt6Array = std::array<int, 6>;

// 裁切几何的 Inside 由几何轴决定：Box 是标准盒 [-1,1]^3 内部，
// Plane 是 active input model 中法线指向的正半空间。
enum class CropRemovalMode {
    // 当前 widget 只允许定位，不生成或改写裁切历史。
    None,
    // mapper shader 保留 Box 的 6 个朝内半空间交集，或 Plane 的法线正半空间。
    KeepInside,
    // shader/物化移除上述 Inside，保留其余区域。
    RemoveInside
};

struct CropHistoryState final {
    CropDocumentId documentId = 0;
    std::uint64_t stateRevision = 0;
    CropNodeId requestedHead = 0;
    CropNodeId appliedHead = 0;
    CropNodeId renderedHead = 0;
    std::size_t nodeCount = 0;
    std::size_t operationCount = 0;
    CropRemovalMode editMode = CropRemovalMode::None;
    bool hasEditableOp = false;
    bool isEditing = false;
    bool isDragging = false;
    CropRequestId lastRequestId = 0;
    std::size_t pendingRequestCount = 0;
};

// 裁切几何类型；router 用它和数据源、动作一起决定可执行路径。
enum class CropShape {
    // 已接入的有向盒裁切。
    Box = 0,
    // 法线正半空间，保留旧的严格边界语义。
    Plane = 1,
    Cylinder = 2,
    Sphere = 3
};

// widget 生产、bridge 消费的瞬时交互轴；它与 shader revision、异步导出状态相互独立。
enum class CropInteractionPhase {
    // 当前没有任何交互发生。
    Idle,
    // 已进入可交互区域，但尚未开始真正拖拽。
    Hover,
    // 正在拖拽；bridge 记录最新 world 几何，但阻止 shader 提交和导出。
    Dragging,
    // 一次拖拽刚结束；bridge 消费当前几何，导出也重新允许。
    Released
};

// 统一归档请求/执行失败原因，便于 bridge 日志和上层 UI 提示复用同一套语义。
enum class CropFailure {
    // 没有失败，当前请求可以视作正常完成。
    None,
    // image 路径需要 vtkImageData，但当前没有绑定输入图像。
    NoImage,
    // polydata 路径需要 vtkPolyData，但当前没有绑定输入网格。
    NoPolyData,
    // 请求里的 bounds 自身就不合法，例如 min >= max。
    BadBounds,
    // bounds 虽合法，但超出了输入数据允许的范围。
    OutOfBounds,
    // 请求三元组没有可执行路径，或与当前算法输入不匹配。
    NoBackend,
    // image 物化的保留语义无法由当前后端执行。
    BadBuildMode,
    // 预估或执行时发现内存不足，无法安全完成裁切。
    LowRam,
    // image 物化需要输出与输入体数据对齐的三维 mask 时，生成 mask 失败。
    MaskFailed,
    // image 物化需要输出主数据 image 时，生成输出 image 失败。
    ImageFailed,
    // Box 几何需要生成 outlinePolyData 时，轮廓 artifact 生成失败。
    ClipFailed,
    // 公式节点、输入快照或导出参数不满足稳定契约。
    BadInput,
    // 有效前缀计算后没有任何点被保留。
    EmptyResult,
    // 同一 Bridge 已有一个导出任务执行中。
    Busy,
    // 导出请求与捕获输入的版本或数据源不一致。
    VersionMismatch,
    // packaged_task 已创建，但 joinable worker 未能启动。
    WorkerStartFailed,
    // worker 已启动，但任务以异常终止。
    WorkerFailed,
    NodeNotFound,
    StateVersionMismatch,
    PublishedResultDependency,
    ReturnToSourceRequired,
    ResultInUse,
    ResultReleasing,
    PreviewNotReady,
    SourceMismatch,
    ResourceLimit,
    ResultRetired,
    PrecisionNotMet,
    NoCropOperations,
    Cancelled,
    InvalidRequest,
    RequestExpired,
    ParentFailed
};

// history 的唯一节点类型；只保存可序列化数学参数，不保存 VTK 对象或 GPU 资源。
struct CropOpItem final {
    std::uint64_t operationIndex = 0;
    CropShape geometryType = CropShape::Box;
    CropRemovalMode removalMode = CropRemovalMode::KeepInside;
    CropMatrixDouble16Array boxToInputModelMatrix = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    CropVectorDouble3Array planeCenterInInputModel = { 0.0, 0.0, 0.0 };
    CropVectorDouble3Array planeNormalInInputModel = { 0.0, 0.0, 1.0 };
    CropVectorDouble3Array centerInInputModel = { 0.0, 0.0, 0.0 };
    CropVectorDouble3Array axisInInputModel = { 0.0, 0.0, 1.0 };
    double radius = 1.0;
    double height = 1.0;
    std::uint32_t recipeVersion = 1;
    std::uint32_t boundaryPolicyVersion = 1;
};

// Host 在 owner thread 从同一图快照捕获的不可拆分快照；正式 ref、binding 与 typed view 一起换代。
struct CropInputSnapshot final {
    DataGraphSnapshot graph;
    std::optional<DataBinding> binding;
    DataSnapshot data;
    CropBoundsDouble6Array inputModelBounds = {};
    VtkImageGridSnapshot image;
    VtkSurfaceMeshSnapshot mesh;
};

struct CropBuildOptions final {
    std::size_t availableRamBytes = 512ULL * 1024 * 1024;
    double meshTolerance = 0.05;
    std::size_t maxCells = 1000000;
    std::uint32_t maxDepth = 64;
    bool operator==(const CropBuildOptions& other) const noexcept {
        return availableRamBytes==other.availableRamBytes && meshTolerance==other.meshTolerance
            && maxCells==other.maxCells && maxDepth==other.maxDepth;
    }
};

struct CropBuildParams final {
    CropDocumentId documentId = 0;
    CropNodeId nodeId = 0;
    CropRequestId requestId = 0;
    DataRevisionRef sourceRevision;
    std::vector<CropOpItem> operations;
    std::size_t nodeCount = 0;
    std::size_t availableRamBytes = 0;
    double meshTolerance = 0.05;
    std::size_t maxCells = 1000000;
    std::uint32_t maxDepth = 64;
};

struct CropBuildResult final {
    CropRequestId requestId = 0;
    std::uint64_t stateRevision = 0;
    CropDocumentId documentId = 0;
    CropNodeId nodeId = 0;
    CropResultId resultId = 0;
    DataEntityId scopeId;
    std::vector<DataLifetimeBlocker> blockers;
    bool isSucceeded = false;
    CropFailure failureReason = CropFailure::None;
    std::uint64_t failureOperationIndex = 0;
    std::size_t nodeCount = 0;
    DataCommitId commitId = 0;
    DataRevisionRef sourceRevision;
    DataRevisionRef recipeRevision;
    DataRevisionRef outputRevision;
    std::string message;
    double meshErrorBound = 0;
    double meshAreaErrorBound = 0;
    std::size_t meshTriangleCount = 0;
};

enum class CropDocumentStatus : std::uint8_t {
    Ready, Building, Returning, Releasing, Closing, Closed
};

enum class CropResultStatus : std::uint8_t {
    Building, Published, Releasing
};

struct CropResultRecord final {
    CropResultId resultId = 0;
    CropNodeId nodeId = 0;
    CropResultStatus status = CropResultStatus::Building;
    DataEntityId scopeId;
    DataRevisionRef sourceRevision;
    DataRevisionRef recipeRevision;
    DataRevisionRef outputRevision;
    std::uint64_t publicationGeneration = 0;
};

struct CropNodeSnapshot final {
    CropNodeId nodeId = 0;
    CropNodeId parentNodeId = 0;
    // Root 没有操作；正式节点的几何/父关系从不原位改写。
    std::optional<CropOpItem> operation;
};

// Values describe one target view at the owner-thread query instant. A committed
// node may still await its first presented frame; 0 means no known rendered node.
struct CropViewPreviewState final {
    std::string viewId;
    CropNodeId requestedHead=0;
    CropNodeId appliedHead=0;
    CropNodeId renderedHead=0;
    RenderEffectState effect;
    bool isRenderPending=false;
};

struct CropHistorySnapshot final {
    CropDocumentId documentId = 0;
    CropNodeId rootNodeId = 0;
    DataRevisionRef sourceRevision;
    std::uint64_t stateRevision = 0;
    CropNodeId requestedHead = 0;
    CropNodeId appliedHead = 0;
    CropNodeId renderedHead = 0;
    std::size_t totalNodeCount = 0;
    std::vector<CropNodeSnapshot> nodes;
    std::vector<CropResultRecord> results;
    CropNodeId nextPageAfter = 0;
};

enum class CropPruneScope : std::uint8_t { Subtrees, Descendants, OutsidePaths };
enum class CropPruneFallback : std::uint8_t { Reject, NearestSurvivingAncestor, ExplicitNode };

struct CropPruneRequest final {
    CropPruneScope scope = CropPruneScope::Subtrees;
    std::vector<CropNodeId> nodeIds;
    CropPruneFallback fallback = CropPruneFallback::Reject;
    CropNodeId explicitFallbackNode = 0;
};

struct CropPruneBlocker final {
    CropResultId resultId = 0;
    CropNodeId nodeId = 0;
};

struct CropPruneImpact final {
    CropFailure failureReason = CropFailure::None;
    std::uint64_t stateRevision = 0;
    std::size_t deletedCount = 0;
    std::vector<CropNodeId> subtreeRoots;
    std::vector<CropPruneBlocker> blockers;
    CropNodeId fallbackNode = 0;
};

struct CropDocumentArchive final {
    std::uint32_t schemaVersion = 1;
    DataRevisionRef sourceRevision;
    std::optional<GridGeometry3D> imageGeometry;
    std::vector<CropNodeSnapshot> nodes;
    CropNodeId rootNodeId = 0;
    CropNodeId requestedHead = 0;
    CropNodeId appliedHead = 0;
    std::optional<CropResultRecord> result;
};

enum class CropEditKind : std::uint8_t { Append, Replace, Select, Prune };
enum class CropEditStatus : std::uint8_t { Queued, Succeeded, Failed, Cancelled };

struct CropEditRequest final {
    CropDocumentId documentId = 0;
    CropRequestId requestId = 0;
    std::uint64_t expectedRevision = 0;
    CropEditKind kind = CropEditKind::Select;
    // Append 是明确父节点；Replace/Select 是明确目标，不把数量作为身份。
    CropNodeId nodeId = 0;
    CropOpItem operation;
    CropPruneRequest prune;
};
struct CropEditOutcome final {
    CropRequestId requestId = 0;
    CropNodeId nodeId = 0;
    std::uint64_t stateRevision = 0;
    CropEditStatus status = CropEditStatus::Queued;
    CropFailure failureReason = CropFailure::None;
    CropPruneImpact prune;
};
struct CropEditAdmission final {
    explicit operator bool() const noexcept { return isAccepted; }
    bool isAccepted = false;
    bool isReplay = false;
    CropFailure failureReason = CropFailure::None;
    CropRequestId requestId = 0;
    CropNodeId nodeId = 0;
    std::uint64_t stateRevision = 0;
};
