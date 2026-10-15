#pragma once

#include "Data/DataGraphTypes.h"
#include "Data/DataPayloads.h"
#include "SurfaceRecipe.h"
#include "Host/Types/HostViewTypes.h"

#include <array>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <variant>

// 业务模式与计算方法分离；高级模式只预留入口，未实现时明确拒绝。
enum class SurfaceBusinessMode : std::uint8_t { Production, Advanced };

struct SurfaceDeterminationStartParams final : SurfaceRecipe
{
    SurfaceBusinessMode businessMode = SurfaceBusinessMode::Production;
    HostViewTargets targetViews;
    // 省略只在接纳时解析主卷，计算不再查询当前选择。
    std::optional<DataRevisionRef> sourceVolume;
    std::optional<DataRevisionRef> analysisRoi;
    // 显式复用已验证的完整父表面；只重新选择面片，不重新测量。
    std::optional<DataRevisionRef> parentGeneration;
    // 执行分块不降低分辨率，不影响算法结果的参数指纹。
    std::uint32_t seedBlockDepth = 16;
    // 省略表示正式测量；预览也执行同一材料等值面算法。
    std::optional<SurfaceTaskPurpose> purpose;
    std::string resultScope;
    DataPublishPolicy sourcePolicy = DataPublishPolicy::RequireCurrentInputs;
    // 调用方对几何长度单位的声明；空表示未知，绝不借用灰度单位。
    std::string modelUnit;
};

struct SurfaceSelectionPiece final {
    std::uint64_t parentTriangle = 0;
    std::array<std::array<double,3>,3> points{};
    std::array<std::array<double,3>,3> barycentric{};
};
struct SurfaceObjectPage final {
    DataRevisionRef generation;
    std::uint64_t total = 0, next = 0;
    std::vector<SurfaceObjectRecord> objects;
};
struct SurfaceObjectPointPage final {
    DataRevisionRef generation;
    std::uint32_t objectIndex = 0;
    std::uint64_t firstPoint = 0, total = 0, next = 0;
    std::vector<SurfacePointRecord> points;
};
struct SurfaceTriangleRecord final {
    // 全局正式点编号，不是当前读取页内的局部编号。
    std::array<std::uint32_t,3> vertices{};
    std::uint8_t validity=0;
};
struct SurfaceObjectTrianglePage final {
    DataRevisionRef generation;
    std::uint32_t objectIndex=0;
    std::uint64_t firstTriangle=0,total=0,next=0;
    std::vector<SurfaceTriangleRecord> triangles;
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
    DataRevisionRef selectionMeshRevision;
    std::shared_ptr<const MeshReadArray<SurfaceSelectionPiece>> selection;
    double selectedAreaMM2 = 0;
    double selectionToleranceMM = 0;
    DataRevisionRef sourceRevision;
    std::uint64_t resultRevision = 0;
    std::uint64_t parameterFingerprint = 0;
    std::uint32_t algorithmRevision = 0;
    SurfaceDeterminationMethod method =
        SurfaceDeterminationMethod::GlobalAutomatic;
    std::shared_ptr<const MeshReadArray<SurfacePointRecord>> points;
    std::shared_ptr<const MeshReadArray<std::uint32_t>> triangleIndices;
    std::shared_ptr<const MeshReadArray<SurfaceObjectRecord>> objects;
    std::optional<SurfaceIsoEstimate> isoEstimate;
    // 每三角形一个值；仅证明本 Feature 明确检查的有效性条件。
    std::shared_ptr<const MeshReadArray<std::uint8_t>> triangleValidity;
    std::vector<DataInputRef> inputs;
    SurfaceExecutionStats execution;
    std::shared_ptr<const std::vector<SurfaceInterfaceRecord>> interfaces;

    // 对象目录与点目录分别读取；只复制当前页，不扫描完整点集。
    std::optional<SurfaceObjectPage> GetObjectPage(std::uint64_t offset, std::size_t count) const {
        if (!objects || !count || offset > objects->size()) return {};
        SurfaceObjectPage page; page.generation = dataRevision; page.total = objects->size();
        page.next = offset + std::min<std::uint64_t>({count,4096,page.total - offset});
        page.objects.assign(objects->begin() + static_cast<std::size_t>(offset),
            objects->begin() + static_cast<std::size_t>(page.next));
        return page;
    }
    std::optional<SurfaceObjectRecord> GetObjectById(std::uint32_t id) const {
        if (!objects) return {};
        std::size_t first=0,last=objects->size();
        while(first<last){const auto middle=first+(last-first)/2;const auto object=(*objects)[middle];
            if(object.objectIndex<id)first=middle+1;else last=middle;}
        if(first==objects->size())return {};
        const auto object=(*objects)[first];return object.objectIndex==id?std::optional<SurfaceObjectRecord>(object):std::nullopt;
    }
    std::optional<SurfaceObjectPointPage> GetObjectPoints(std::uint32_t id, std::uint64_t offset, std::size_t count) const {
        const auto object = GetObjectById(id);
        if (!object || !points || !count || offset > object->pointCount
            || object->firstPoint > points->size() || object->pointCount > points->size() - object->firstPoint) return {};
        SurfaceObjectPointPage page; page.generation = dataRevision; page.objectIndex = id;
        page.total = object->pointCount; page.next = offset + std::min<std::uint64_t>({count,65536,page.total - offset});
        page.firstPoint = object->firstPoint + offset;
        page.points.assign(points->begin() + static_cast<std::size_t>(page.firstPoint),
            points->begin() + static_cast<std::size_t>(object->firstPoint + page.next));
        return page;
    }
    std::optional<SurfaceObjectTrianglePage> GetObjectTriangles(std::uint32_t id,std::uint64_t offset,std::size_t count)const {
        const auto object=GetObjectById(id);
        if(!object||!triangleIndices||!triangleValidity||!count||triangleIndices->size()%3
            ||triangleValidity->size()!=triangleIndices->size()/3||offset>object->triangleCount
            ||object->firstTriangle>triangleValidity->size()
            ||object->triangleCount>triangleValidity->size()-object->firstTriangle)return {};
        SurfaceObjectTrianglePage page;page.generation=dataRevision;page.objectIndex=id;
        page.total=object->triangleCount;page.next=offset+std::min<std::uint64_t>({count,65536,page.total-offset});
        page.firstTriangle=object->firstTriangle+offset;page.triangles.reserve(static_cast<std::size_t>(page.next-offset));
        for(auto triangle=page.firstTriangle;triangle<object->firstTriangle+page.next;++triangle)
            page.triangles.push_back({{(*triangleIndices)[triangle*3],(*triangleIndices)[triangle*3+1],(*triangleIndices)[triangle*3+2]},(*triangleValidity)[triangle]});
        return page;
    }
};

struct SurfaceDeterminationConfig final {
    SurfaceDeterminationStartParams defaultStart;
    // 仅选择工作缓存和文件后备；不是正式结果大小或任务准入上限。
    std::optional<std::size_t> maxWorkingBytes {};
    bool isOverlayVisible = true;
};

struct SurfaceStartRequest final {
    // 缺省时沿用 Feature 构造配置；具体 Start 参数由这个动作独占。
    std::optional<SurfaceDeterminationStartParams> start;
};
struct SurfaceStopRequest final {
    // Stop 时为 0 表示当前最新请求，否则必须与仍在执行的请求匹配。
    std::uint64_t targetRequestId = 0;
};
struct SurfaceVisibilityRequest final { bool isVisible = true; };
struct SurfaceClearRequest final {};
struct SurfaceClearPreviewRequest final {};
struct SurfaceObjectSelectionRequest final {
    DataRevisionRef generation;
    // 0 是合法表面对象编号；缺省表示清除本代选择。
    std::optional<std::uint32_t> objectIndex;
    bool isIsolated=false;
    HostViewTargets targetViews;
};
struct SurfaceObjectPickModeRequest final {bool isEnabled=false;};
namespace SurfaceObjectBindingKeys {inline constexpr std::string_view Select="surface.object.select.pointer";}
using SurfaceDeterminationRequest = std::variant<SurfaceStartRequest, SurfaceStopRequest,
    SurfaceVisibilityRequest, SurfaceClearRequest, SurfaceClearPreviewRequest,
    SurfaceObjectSelectionRequest,SurfaceObjectPickModeRequest>;

enum class SurfaceAdmissionStatus : std::uint8_t {
    Accepted,
    UnsupportedRoi,
    InvalidRequest,
    Busy,
    Stopping,
    Unavailable,
    UnsupportedMode
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
    UnsupportedRoi,
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
    // 正式测定终态与当前视域几何的准备状态分开。
    bool isDisplayComplete=true,hasDisplayFailure=false;
    std::uint64_t displayRequestedBatches=0,displayLoadedBatches=0;
    std::string displayMessage;
    DataRevisionRef selectionGeneration;
    std::optional<std::uint32_t> selectedObject;
    bool isSelectionIsolated=false,isObjectPickingEnabled=false;
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
