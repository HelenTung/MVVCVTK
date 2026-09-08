#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using DataGeneration = std::uint64_t;
using DataCommitId = std::uint64_t;
using DataBindingRevision = std::uint64_t;
using DataObserverId = std::uint64_t;

enum class ImageValueType : std::uint8_t {
    Unknown,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64
};

inline std::size_t GetImageValueBytes(
    const ImageValueType valueType) noexcept
{
    switch (valueType) {
    case ImageValueType::Int8:
    case ImageValueType::UInt8:
        return 1;
    case ImageValueType::Int16:
    case ImageValueType::UInt16:
        return 2;
    case ImageValueType::Int32:
    case ImageValueType::UInt32:
    case ImageValueType::Float32:
        return 4;
    case ImageValueType::Int64:
    case ImageValueType::UInt64:
    case ImageValueType::Float64:
        return 8;
    case ImageValueType::Unknown:
    default:
        return 0;
    }
}

inline constexpr std::string_view primaryVolumeBinding =
    "session.primary.volume";

struct DataEntityId final {
    std::array<std::uint8_t, 16> bytes{};
};

inline bool operator==(
    const DataEntityId& left,
    const DataEntityId& right) noexcept
{
    return left.bytes == right.bytes;
}

inline bool operator!=(
    const DataEntityId& left,
    const DataEntityId& right) noexcept
{
    return !(left == right);
}

inline bool operator<(
    const DataEntityId& left,
    const DataEntityId& right) noexcept
{
    return left.bytes < right.bytes;
}

inline bool GetDataEntityIdValid(const DataEntityId& value) noexcept
{
    for (const auto byte : value.bytes) {
        if (byte != 0) return true;
    }
    return false;
}

struct DataRevisionRef final {
    DataEntityId entityId;
    DataGeneration generation = 0;
};

inline bool operator==(
    const DataRevisionRef& left,
    const DataRevisionRef& right) noexcept
{
    return left.entityId == right.entityId
        && left.generation == right.generation;
}

inline bool operator!=(
    const DataRevisionRef& left,
    const DataRevisionRef& right) noexcept
{
    return !(left == right);
}

inline bool operator<(
    const DataRevisionRef& left,
    const DataRevisionRef& right) noexcept
{
    return left.entityId < right.entityId
        || (!(right.entityId < left.entityId)
            && left.generation < right.generation);
}

inline bool GetDataRevisionRefValid(
    const DataRevisionRef& value) noexcept
{
    return GetDataEntityIdValid(value.entityId)
        && value.generation != 0;
}

struct DataTypeId final {
    std::string name;
    std::uint32_t schemaVersion = 1;
};

inline bool operator==(
    const DataTypeId& left,
    const DataTypeId& right) noexcept
{
    return left.name == right.name
        && left.schemaVersion == right.schemaVersion;
}

inline bool operator!=(
    const DataTypeId& left,
    const DataTypeId& right) noexcept
{
    return !(left == right);
}

inline bool operator<(
    const DataTypeId& left,
    const DataTypeId& right) noexcept
{
    return left.name < right.name
        || (left.name == right.name
            && left.schemaVersion < right.schemaVersion);
}

inline bool GetDataTypeIdValid(const DataTypeId& value) noexcept
{
    return !value.name.empty() && value.schemaVersion != 0;
}

struct DataFacetId final {
    std::string name;
};

inline bool operator==(
    const DataFacetId& left,
    const DataFacetId& right) noexcept
{
    return left.name == right.name;
}

inline bool operator!=(
    const DataFacetId& left,
    const DataFacetId& right) noexcept
{
    return !(left == right);
}

inline bool operator<(
    const DataFacetId& left,
    const DataFacetId& right) noexcept
{
    return left.name < right.name;
}

struct DataInputRef final {
    std::string role;
    DataRevisionRef source;
};

struct DataProvenance final {
    std::string producerId;
    std::string operationId;
    std::string algorithmVersion;
    std::string canonicalParameters;
};

// 作用域只约束可撤销数据的读取和资源持有，不包含具体 Feature 业务。
enum class DataLifetimeStatus : std::uint8_t {
    Unknown, Published, Releasing, Released
};

struct DataLifetimeBlocker final {
    DataRevisionRef revision;
    std::string owner;
};

struct DataLifetimeState final {
    DataEntityId scopeId;
    DataLifetimeStatus status = DataLifetimeStatus::Unknown;
    std::vector<DataRevisionRef> ownedRevisions;
    std::vector<DataLifetimeBlocker> blockers;
};

enum class DataResourceKind : std::uint8_t { Reader, RenderObject };

class DataResourceLease {
public:
    virtual ~DataResourceLease() noexcept = default;
};

class DataLifetimeAccess {
public:
    virtual ~DataLifetimeAccess() noexcept = default;
    virtual bool GetIsPublished() const = 0;
    // 取得后由实际资源 owner 持有；退役后不得再取得新租约。
    virtual std::shared_ptr<const DataResourceLease> StartResourceUse(
        const DataRevisionRef& revision, std::string owner,
        DataResourceKind kind = DataResourceKind::Reader) = 0;
};

class DataChangeBatch {
public:
    // 仅 owner thread 使用；最外层析构在完整业务状态交换后派发通知。
    virtual ~DataChangeBatch() noexcept = default;
};

class IDataPayload {
public:
    virtual ~IDataPayload() noexcept = default;

    virtual DataTypeId GetDataType() const = 0;
    // Store 在提交锁外调用；返回值不得与原对象共享可写业务状态。
    virtual std::shared_ptr<const IDataPayload> CreateSnapshot() const = 0;
    // 列出可被调用方单独持有的共享 allocation；内嵌值由 payload 自身覆盖。
    virtual std::vector<std::shared_ptr<const void>> GetDataResources() const
    {
        return {};
    }
};

struct DataRevision final {
    DataRevisionRef self;
    DataTypeId type;
    std::vector<DataInputRef> inputs;
    std::shared_ptr<const IDataPayload> payload;
    std::optional<DataProvenance> provenance;
    DataEntityId lifetimeScope;
    std::weak_ptr<DataLifetimeAccess> lifetime;
};

using DataSnapshot = std::shared_ptr<const DataRevision>;

struct DataBinding final {
    std::string name;
    std::optional<DataRevisionRef> target;
    DataBindingRevision revision = 0;
};

struct DataTypeDescriptor final {
    DataTypeId id;
    std::vector<DataFacetId> facets;
    std::function<bool(const IDataPayload&, std::string&)> validate;
};

enum class DataExpectationKind : std::uint8_t {
    EntityHead,
    Binding
};

enum class DataExpectationUse : std::uint8_t {
    Required,
    Activation
};

struct DataExpectation final {
    DataExpectationKind kind = DataExpectationKind::EntityHead;
    DataExpectationUse use = DataExpectationUse::Required;
    DataEntityId entityId;
    DataGeneration expectedGeneration = 0;
    std::string binding;
    DataBindingRevision expectedBindingRevision = 0;
    bool isTargetChecked = false;
    std::optional<DataRevisionRef> expectedTarget;
};

struct DataRevisionDraft final {
    DataEntityId entityId;
    DataGeneration expectedGeneration = 0;
    DataTypeId type;
    std::vector<DataInputRef> inputs;
    std::shared_ptr<const IDataPayload> payload;
    std::optional<DataProvenance> provenance;
    // 同一事务内以新身份创建；已发布或退役的作用域均不能追加输出。
    std::optional<DataEntityId> lifetimeScope;
};

struct DataLifetimeRetirement final {
    DataEntityId scopeId;
    DataLifetimeStatus expectedStatus = DataLifetimeStatus::Published;
    std::vector<DataRevisionRef> expectedRevisions;
    // View 过渡已准备好：渲染对象可进入延迟清理；Reader/下游依赖仍严格阻塞。
    // 此标记不等于释放完成；RenderObject 租约全部结束前状态保持 Releasing。
    bool isResourceTransition = false;
};

struct DataBindingUpdate final {
    std::string binding;
    DataBindingRevision expectedRevision = 0;
    bool isTargetChecked = false;
    std::optional<DataRevisionRef> expectedTarget;
    std::optional<DataRevisionRef> target;
};

enum class DataPublishPolicy : std::uint8_t {
    RequireCurrentInputs,
    AllowHistoricalResult
};

struct DataTransaction final {
    DataPublishPolicy policy = DataPublishPolicy::RequireCurrentInputs;
    std::vector<DataExpectation> expectations;
    std::vector<DataRevisionDraft> outputs;
    std::vector<DataBindingUpdate> bindings;
    std::vector<DataLifetimeRetirement> retireScopes;
};

enum class DataCommitStatus : std::uint8_t {
    Rejected,
    Succeeded,
    SucceededHistorical
};

enum class DataCommitFailure : std::uint8_t {
    None,
    InvalidTransaction,
    TypeNotRegistered,
    PayloadInvalid,
    MissingInput,
    CycleDetected,
    ExpectationFailed,
    Overflow,
    OutOfMemory,
    ResultInUse,
    ResultRetired
};

struct DataBindingChange final {
    std::string binding;
    std::optional<DataRevisionRef> previous;
    DataBinding current;
};

struct DataChangeSet final {
    DataCommitId commitId = 0;
    std::vector<DataRevisionRef> published;
    std::vector<DataBindingChange> bindings;
    std::vector<DataEntityId> retiredScopes;
    std::vector<DataEntityId> releasedScopes;
};

using DataChangeCallback = std::function<void(const DataChangeSet&)>;

class DataGraphView;
struct DataGraphSnapshot final {
    DataCommitId commitId = 0;
    std::shared_ptr<const DataGraphView> view;
};

struct DataCommitResult final {
    DataCommitStatus status = DataCommitStatus::Rejected;
    DataCommitFailure failureReason = DataCommitFailure::InvalidTransaction;
    DataCommitId commitId = 0;
    bool isActivated = false;
    std::vector<DataSnapshot> published;
    std::vector<DataBinding> bindings;
    std::string message;
    DataGraphSnapshot graph;
    std::vector<DataLifetimeBlocker> blockers;
};

struct DataQuery final {
    std::optional<DataEntityId> entityId;
    std::optional<DataTypeId> type;
    std::optional<DataFacetId> facet;
    std::optional<DataRevisionRef> input;
    std::string inputRole;
    std::string producerId;
    std::size_t limit = 0;
};

struct DataQueryResult final {
    DataCommitId commitId = 0;
    std::vector<DataSnapshot> data;
};

enum class DataRelationStatus : std::uint8_t {
    Unknown,
    ValidForItsInputs,
    OutOfDateRelativeToCurrentBinding
};

class DataGraphView {
public:
    virtual ~DataGraphView() noexcept = default;

    virtual DataSnapshot GetData(const DataRevisionRef& ref) const = 0;
    virtual std::optional<DataBinding> GetDataBinding(
        std::string_view name) const = 0;
    virtual std::vector<DataBinding> GetDataBindings() const = 0;
    virtual DataQueryResult GetDataQuery(const DataQuery& query) const = 0;
    virtual std::vector<DataRevisionRef> GetDerivedData(
        const DataRevisionRef& source) const = 0;
    virtual std::vector<DataFacetId> GetDataFacets(
        const DataTypeId& type) const = 0;
    virtual DataRelationStatus GetDataRelation(
        const DataRevisionRef& data,
        std::string_view inputRole,
        std::string_view binding) const = 0;
};

struct ProjectDataSnapshot final {
    DataCommitId commitId = 0;
    std::vector<DataBinding> bindings;
};
