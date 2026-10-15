#pragma once

#include "Data/ImageReadTypes.h"
#include "Data/DataGraphTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <chrono>
#include <limits>
#include <optional>
#include <memory>
#include <map>
#include <variant>
#include <string>
#include <vector>

// 节点索引的表示上限；不按历史长度或 ROI 数量另设业务门槛。
inline constexpr std::size_t roiNodeLimit = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t roiSchemaVersion = 4;
// 归档编码独立演进，不改变几何类型身份或算法参数指纹。
inline constexpr std::uint32_t roiArchiveSchemaVersion = 5;
inline constexpr std::size_t roiChunkBytes = 8ULL * 1024 * 1024;
inline constexpr std::array<double, 16> roiIdentityMatrix = {
    1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
inline constexpr std::string_view roiCatalogBinding = "session.roi.catalog";

enum class RoiShape : std::uint8_t {
    Box, HalfSpace, MaskReference, Sphere, Cylinder, Ellipsoid,
    Contour, ProjectedContour, Brush, VoxelSet, EllipticCylinder
};

struct RoiVoxel final {
    std::array<int,3> index{};
    std::uint32_t label = 1;
};

// 同一 (Y,Z) 行上的闭 X 区间，first[0] <= last；标签沿整个区间保持不变。
struct RoiVoxelRun final {
    std::array<int,3> first{};
    int last = 0;
    std::uint32_t label = 1;
};

// 正式载荷保留行区间；拆分结果共享一次标记的数据。
class RoiVoxelSet final {
public:
    explicit RoiVoxelSet(std::vector<RoiVoxelRun> runs);
    // 只读借用，引用有效期不超过本载荷的生命期。
    const std::vector<RoiVoxelRun>& GetRuns() const noexcept;
    std::size_t GetVoxelCount() const noexcept;
    // 仅展开明确请求的块，按 (Y,Z,X) 排序；不在对象中缓存展开结果。
    std::vector<RoiVoxel> GetVoxelChunk(std::size_t offset, std::size_t count) const;
    std::uint32_t GetLabel(const std::array<int,3>& index) const noexcept;
    std::array<double,6> GetIndexBounds(const std::vector<std::uint64_t>& labels) const noexcept;
private:
    class Impl;
    std::shared_ptr<Impl> m_impl;
};
// 有序区间直接追加；外部逐体素输入可无序分块接入并即时合并，不保留完整体素数组。
// 一次构建只使用一种输入方式；出错后不可发布，Finish 后不可继续使用。
class RoiVoxelBuilder final {
public:
    RoiVoxelBuilder();~RoiVoxelBuilder();
    void AppendRuns(const std::vector<RoiVoxelRun>& runs);
    void AppendVoxels(const std::vector<RoiVoxel>& voxels);
    std::shared_ptr<const RoiVoxelSet> Finish();
private:
    class Impl;std::unique_ptr<Impl> m_impl;
};
// Closed 保持已有 ROI 语义；CropV1 保留裁切 Box 容差和 Plane 严格正侧。
enum class RoiBoundaryPolicy : std::uint8_t { Closed, CropV1 };
enum class RoiNodeKind : std::uint8_t {
    Empty, SourceDomain, Primitive, Union, Intersection, Difference
};

struct RoiPrimitive final {
    RoiShape shape = RoiShape::Box;
    // sourcePoint = localToSource * localPoint；Box 局部域为 [-1,1]^3。
    std::array<double, 16> localToSource = roiIdentityMatrix;
    // HalfSpace 在源物理坐标中定义，法线正侧（含平面）属于区域。
    std::array<double, 3> origin = { 0, 0, 0 };
    std::array<double, 3> normal = { 0, 0, 1 };
    // 同源同网格的精确掩码修订，非零为前景；不接受额外空间变换。
    std::optional<DataRevisionRef> mask;
    RoiBoundaryPolicy boundaryPolicy = RoiBoundaryPolicy::Closed;
    // 曲面使用 origin 为中心、normal 为柱轴；球体 height 必须为 0。
    double radius = 0;
    double height = 0;
    // 轮廓局部 XY 平面，多环奇偶填充；深度沿局部 Z，包含端面。
    std::vector<std::vector<std::array<double,2>>> contours;
    std::array<double,2> depth = {-0.5,0.5};
    // 创建时冻结的 source->view 和 view->clip；投影轮廓为 NDC。
    std::array<double,16> sourceToView = roiIdentityMatrix;
    std::array<double,16> viewToClip = roiIdentityMatrix;
    // 画笔源空间路径，radius 为 mm；轮廓及椭球由 localToSource 定位。
    std::vector<std::array<double,3>> path;
    std::shared_ptr<const RoiVoxelSet> voxels;
    // 空集合表示全部非零标签，非空时只选择这些固定标签。
    std::vector<std::uint64_t> labels;
};

struct RoiNode final {
    RoiNodeKind kind = RoiNodeKind::Primitive;
    RoiPrimitive primitive;
    // 二元节点只引用更早节点；叶节点两项必须为 0，最后节点是根。
    std::uint32_t left = 0;
    std::uint32_t right = 0;
};

struct RoiDefinition final {
    DataRevisionRef source;
    std::vector<RoiNode> nodes;
};

struct RoiMetadata final {
    std::string name;
    std::string group;
    std::string description;
    bool isArchived = false;
};

struct RoiDescriptor final {
    DataRevisionRef revision;
    DataRevisionRef currentRevision;
    RoiDefinition definition;
    // 元信息为当前目录投影，几何可为指定的历史修订。
    RoiMetadata metadata;
    DataBindingRevision catalogRevision = 0;
};

enum class RoiError : std::uint8_t {
    None, Unavailable, WrongThread, InvalidRequest, InvalidGeometry,
    UnsupportedRoi, MissingInput, SourceMismatch, SourceUnresolved,
    RevisionConflict, TooLarge, Cancelled, CommitFailed, Pending, DeadlineExceeded
};

enum class RoiAction : std::uint8_t { Create, SetGeometry, SetMetadata, Copy, Remove };

struct RoiRequest final {
    RoiAction action = RoiAction::Create;
    RoiDefinition definition;
    RoiMetadata metadata;
    std::optional<DataRevisionRef> expectedRoi;
    std::optional<DataBindingRevision> expectedCatalogRevision;
    std::optional<DataRevisionRef> copyFrom;
    // 当前绑定驱动的操作可冻结来源绑定；显式历史来源操作可省略。
    std::optional<DataBinding> expectedSourceBinding;
    std::vector<DataInputRef> inputs;
    std::optional<DataProvenance> provenance;
};

struct RoiResult final {
    RoiError error = RoiError::Unavailable;
    std::optional<RoiDescriptor> roi;
    std::size_t requiredBytes = 0;
    std::string message;
    DataCommitId commitId = 0;
};

enum class RoiSelectionMode : std::uint8_t { Replace, Add, Subtract, Intersect };
enum class RoiOutputMode : std::uint8_t { UpdateTarget, CreateNew };
enum class RoiBooleanOperation : std::uint8_t { Union, Intersection, Difference };
enum class RoiMorphologyOperation : std::uint8_t { Dilate, Erode, Open, Close };
enum class RoiRadiusUnit : std::uint8_t { Millimetres, Voxels };
enum class RoiConnectivity : std::uint8_t { Faces, Full };
enum class RoiComponentOperation : std::uint8_t { RemoveSmall, KeepLargest, KeepSelected, Split, FillHoles };
struct RoiBooleanParams final {
    RoiBooleanOperation operation = RoiBooleanOperation::Union;
    std::vector<DataRevisionRef> operands;
};
struct RoiMorphologyParams final {
    RoiMorphologyOperation operation = RoiMorphologyOperation::Dilate;
    double radius = 0;
    RoiRadiusUnit unit = RoiRadiusUnit::Millimetres;
};
struct RoiComponentParams final {
    RoiComponentOperation operation = RoiComponentOperation::RemoveSmall;
    RoiConnectivity connectivity = RoiConnectivity::Faces;
    double minimumVolume = 0; // mm^3
    double maximumHoleVolume = 0; // 0 表示不限大小
    std::vector<std::uint32_t> labels;
    bool keepInput = true;
};
enum class RoiThresholdMode : std::uint8_t { Threshold, SeedGrow };
struct RoiThresholdParams final {
    RoiThresholdMode mode=RoiThresholdMode::Threshold;
    std::array<double,2> range{};
    std::vector<std::array<double,3>> seeds; // 连通生长的源物理坐标种子；阈值模式必须为空
    RoiConnectivity connectivity = RoiConnectivity::Faces;
};
struct RoiInvertParams final { std::optional<DataRevisionRef> domain; };
struct RoiClearParams final {};
struct RoiTransformParams final {
    std::array<double,16> sourceToTarget = roiIdentityMatrix;
    std::optional<DataRevisionRef> targetSource;
    // 掩码或不能精确保留的几何必须由调用方显式选择按目标体素中心重采样。
    bool rasterize = false;
};
enum class RoiMeshMode : std::uint8_t { Interior, Layer };
struct RoiMeshParams final {
    DataRevisionRef mesh;
    std::array<double,16> meshToSource = roiIdentityMatrix;
    RoiMeshMode mode = RoiMeshMode::Interior;
    double inward = 0; // mm
    double outward = 0; // mm
    bool hasOrientation = false;
};
using RoiOperationParams = std::variant<RoiBooleanParams,RoiMorphologyParams,
    RoiComponentParams,RoiThresholdParams,RoiInvertParams,RoiClearParams,
    RoiTransformParams,RoiMeshParams>;
struct RoiOperationRequest final {
    DataRevisionRef source;
    std::optional<DataRevisionRef> target;
    std::optional<RoiDefinition> input;
    RoiOperationParams params;
    RoiOutputMode output = RoiOutputMode::UpdateTarget;
    RoiMetadata metadata;
    std::optional<DataBinding> expectedSourceBinding;
    std::function<bool()> getCancelled;
    std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::time_point::max();
};
struct RoiOperationResult final {
    RoiError error = RoiError::Unavailable;
    std::vector<RoiDescriptor> rois;
    std::vector<std::size_t> componentVoxelCounts;
    DataCommitId commitId = 0;
    std::string message;
    // 临时候选绑定输入、参数和输出几何；不进入正式 ROI 或归档。
    std::string preparedSignature;
    DataProvenance provenance;
};

// 来源的外部稳定身份由调用应用解析；image 内的运行期 revision 必须清除。
struct RoiSourceDescriptor final {
    DataTypeId type;
    std::string coordinateFrame;
    std::optional<ImageDescriptor> image;
    std::array<double, 6> sourceBounds{};
    std::size_t pointCount = 0;
    std::size_t triangleCount = 0;
    std::string meshGeometryDigest; // 规范化点与拓扑的 SHA-256；同包围盒不能代替来源身份。
};

struct RoiArchiveMask final {
    // v5 保留原整数类型；v3/v4 的掩码存储类型固定为 UInt64。
    ImageDescriptor grid;
    std::vector<std::uint32_t> nodeIndices;
    std::optional<std::uint32_t> sharedPayload;
};

struct RoiArchiveVoxels final {
    std::vector<std::uint32_t> nodeIndices;
    std::optional<std::uint32_t> sharedPayload;
};

struct RoiArchiveItem final {
    std::uint32_t schemaVersion = roiArchiveSchemaVersion;
    std::string sourceKey;
    RoiSourceDescriptor source;
    RoiMetadata metadata;
    DataRevisionRef originalRevision;
    DataProvenance provenance;
    // mask 引用清空，由 masks.nodeIndices 关联归档内数据，不持久化运行期 ID。
    std::vector<RoiNode> nodes;
    std::vector<RoiArchiveMask> masks;
    std::vector<RoiArchiveVoxels> voxels;
};

struct RoiArchiveSelection final {DataRevisionRef roi;std::string sourceKey;};
struct RoiSourceMapping final {std::string sourceKey;DataRevisionRef source;};
// 多对象文件只保存一次共享标签/稀疏载荷，节点通过包内索引引用。
struct RoiArchiveDescriptor final {
    std::uint32_t schemaVersion=roiArchiveSchemaVersion;
    std::vector<RoiArchiveItem> rois;
    std::vector<RoiArchiveMask> masks;
    std::vector<RoiArchiveVoxels> voxels;
};
struct RoiArchiveRestoreResult final {
    RoiError error=RoiError::Unavailable;
    DataCommitId commitId=0;
    std::vector<RoiDescriptor> rois;
    std::vector<std::pair<DataRevisionRef,DataRevisionRef>> revisionMapping;
};

struct RoiArchiveChunkRequest final {
    std::size_t payloadIndex=0,elementOffset=0,chunkElements=65536;
    std::function<bool()> getCancelled;
};
struct RoiArchiveMaskChunk final {
    RoiError error=RoiError::Unavailable;
    std::size_t nextOffset=0;bool isComplete=false;
    // 标签值逐块传输；有符号负数按转换到 uint64_t 的模 2^64 值编码。
    // v5 恢复时按 masks[payloadIndex].grid 的原类型校验并还原，不截断标签。
    std::vector<std::uint64_t> values;
};
struct RoiArchiveVoxelPayloadChunk final {
    RoiError error=RoiError::Unavailable;
    std::size_t nextOffset=0;bool isComplete=false;
    // v4/v5 只允许 runs；v3 只允许 legacyVoxels。元素偏移按所选版本的元素计数。
    std::vector<RoiVoxelRun> runs;
    std::vector<RoiVoxel> legacyVoxels;
};
// 元数据先返回，共享载荷逐批读取；只读实例可以跨 worker 持有。
class RoiArchiveReader {
public:
    virtual ~RoiArchiveReader() noexcept=default;
    virtual const RoiArchiveDescriptor& GetMetadata()const noexcept=0;
    virtual std::optional<std::size_t> GetMaskCount(std::size_t payloadIndex)const noexcept=0;
    virtual std::optional<std::size_t> GetVoxelPayloadCount(std::size_t payloadIndex)const noexcept=0;
    virtual RoiArchiveMaskChunk GetMaskChunk(const RoiArchiveChunkRequest&)const=0;
    virtual RoiArchiveVoxelPayloadChunk GetVoxelPayloadChunk(const RoiArchiveChunkRequest&)const=0;
};
struct RoiArchiveReadResult final {
    RoiError error=RoiError::Unavailable;
    std::shared_ptr<const RoiArchiveReader> reader;
};

// 所有恢复候选完整验证后一次发布；未指定目录版本时仍检查加载期间的目录变更。
struct RoiArchiveRestoreOptions final {
    std::optional<DataBindingRevision> expectedCatalogRevision;
    // 返回 true 或抛异常均视为取消；只丢弃未提交候选。
    std::function<bool()> getCancelled;
    std::size_t chunkElements=65536;
};
