#include "Data/DataGraphStore.h"

#include "Data/DataPayloads.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <utility>

namespace {

class DataLifetimeScope final : public DataLifetimeAccess {
public:
    struct ResourceUse final : DataResourceLease {};
    struct Probe final {
        DataRevisionRef revision;
        std::string owner;
        std::weak_ptr<const void> resource;
        DataResourceKind kind = DataResourceKind::Reader;
    };

    bool GetIsPublished() const override
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return status == DataLifetimeStatus::Published;
    }

    std::shared_ptr<const DataResourceLease> StartResourceUse(
        const DataRevisionRef& revision, std::string owner, DataResourceKind kind) override
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (status != DataLifetimeStatus::Published || owner.empty()
            || (kind != DataResourceKind::Reader && kind != DataResourceKind::RenderObject)
            || std::find(revisions.begin(), revisions.end(), revision)
                == revisions.end()) return {};
        auto lease = std::make_shared<const ResourceUse>();
        uses.erase(std::remove_if(uses.begin(), uses.end(),
            [](const Probe& probe) { return probe.resource.expired(); }), uses.end());
        uses.push_back({ revision, std::move(owner), lease, kind });
        return lease;
    }

    DataSnapshot GetData(const std::weak_ptr<const DataRevision>& value) const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return status == DataLifetimeStatus::Published ? value.lock() : nullptr;
    }

    // 调用方必须持 mutex；实际 allocation 的探针不持有载荷。
    DataLifetimeState GetState() const
    {
        DataLifetimeState result{ id, status, revisions, {} };
        for (const auto& use : uses) {
            if (!use.resource.expired()) result.blockers.push_back({use.revision, use.owner});
        }
        if (status == DataLifetimeStatus::Releasing) {
            for (const auto& probe : probes) {
                if (!probe.resource.expired()) result.blockers.push_back({probe.revision, probe.owner});
            }
        }
        return result;
    }

    mutable std::mutex mutex;
    DataEntityId id;
    DataLifetimeStatus status = DataLifetimeStatus::Published;
    std::vector<DataRevisionRef> revisions;
    std::vector<DataSnapshot> owned;
    std::vector<Probe> probes;
    std::vector<Probe> uses;
};

struct RevisionEntry final {
    // 普通数据沿用原拥有快照；scoped metadata 不持有实际 payload。
    DataSnapshot metadata;
    std::weak_ptr<const DataRevision> value;
    std::shared_ptr<DataLifetimeScope> scope;
    std::vector<std::weak_ptr<const void>> resources;

    DataSnapshot GetData() const
    {
        return scope ? scope->GetData(value) : metadata;
    }
};

class GraphState final : public DataGraphView {
public:
    DataSnapshot GetData(const DataRevisionRef& ref) const override
    {
        const auto found = revisions.find(ref);
        return found == revisions.end() ? DataSnapshot{} : found->second.GetData();
    }

    std::optional<DataBinding> GetDataBinding(
        const std::string_view name) const override
    {
        const auto found = bindings.find(std::string(name));
        return found == bindings.end()
            ? std::optional<DataBinding>{}
            : std::optional<DataBinding>{ found->second };
    }

    std::vector<DataBinding> GetDataBindings() const override
    {
        std::vector<DataBinding> result;
        result.reserve(bindings.size());
        for (const auto& entry : bindings) result.push_back(entry.second);
        return result;
    }

    DataQueryResult GetDataQuery(const DataQuery& query) const override
    {
        DataQueryResult result;
        result.commitId = commitId;
        for (const auto& entry : revisions) {
            const auto snapshot = entry.second.GetData();
            if (!snapshot) continue;
            if (query.entityId && snapshot->self.entityId != *query.entityId) {
                continue;
            }
            if (query.type && snapshot->type != *query.type) continue;
            if (!query.producerId.empty()
                && (!snapshot->provenance
                    || snapshot->provenance->producerId
                        != query.producerId)) {
                continue;
            }
            if (query.facet) {
                const auto descriptor = types.find(snapshot->type);
                if (descriptor == types.end()
                    || std::find(
                        descriptor->second.facets.begin(),
                        descriptor->second.facets.end(),
                        *query.facet) == descriptor->second.facets.end()) {
                    continue;
                }
            }
            if (query.input) {
                const auto input = std::find_if(
                    snapshot->inputs.begin(), snapshot->inputs.end(),
                    [&query](const DataInputRef& value) {
                        return value.source == *query.input
                            && (query.inputRole.empty()
                                || value.role == query.inputRole);
                    });
                if (input == snapshot->inputs.end()) continue;
            }
            result.data.push_back(snapshot);
            if (query.limit != 0 && result.data.size() >= query.limit) break;
        }
        return result;
    }

    std::vector<DataRevisionRef> GetDerivedData(
        const DataRevisionRef& source) const override
    {
        const auto found = derived.find(source);
        return found == derived.end()
            ? std::vector<DataRevisionRef>{}
            : found->second;
    }

    std::vector<DataFacetId> GetDataFacets(
        const DataTypeId& type) const override
    {
        const auto found = types.find(type);
        return found == types.end()
            ? std::vector<DataFacetId>{}
            : found->second.facets;
    }

    DataRelationStatus GetDataRelation(
        const DataRevisionRef& data,
        const std::string_view inputRole,
        const std::string_view binding) const override
    {
        const auto snapshot = GetData(data);
        const auto current = GetDataBinding(binding);
        if (!snapshot || !current || !current->target || inputRole.empty()) {
            return DataRelationStatus::Unknown;
        }
        const auto input = std::find_if(
            snapshot->inputs.begin(), snapshot->inputs.end(),
            [inputRole](const DataInputRef& value) {
                return value.role == inputRole;
            });
        if (input == snapshot->inputs.end()) {
            return DataRelationStatus::Unknown;
        }
        return input->source == *current->target
            ? DataRelationStatus::ValidForItsInputs
            : DataRelationStatus::OutOfDateRelativeToCurrentBinding;
    }

    DataCommitId commitId = 0;
    std::map<DataRevisionRef, RevisionEntry> revisions;
    std::map<DataEntityId, std::shared_ptr<DataLifetimeScope>> lifetimes;
    std::map<DataEntityId, DataGeneration> heads;
    std::map<DataRevisionRef, std::vector<DataRevisionRef>> derived;
    std::map<std::string, DataBinding> bindings;
    std::map<DataTypeId, DataTypeDescriptor> types;
};

DataCommitResult GetRejected(
    const DataCommitFailure failure,
    std::string message)
{
    DataCommitResult result;
    result.failureReason = failure;
    result.message = std::move(message);
    return result;
}

bool GetExpectationMatched(
    const GraphState& state,
    const DataExpectation& expectation)
{
    if (expectation.kind == DataExpectationKind::EntityHead) {
        if (!GetDataEntityIdValid(expectation.entityId)
            || !expectation.binding.empty()) {
            return false;
        }
        const auto found = state.heads.find(expectation.entityId);
        const auto generation = found == state.heads.end()
            ? DataGeneration{ 0 } : found->second;
        return generation == expectation.expectedGeneration;
    }

    if (expectation.binding.empty()
        || GetDataEntityIdValid(expectation.entityId)
        || expectation.expectedGeneration != 0) {
        return false;
    }
    const auto found = state.bindings.find(expectation.binding);
    const auto revision = found == state.bindings.end()
        ? DataBindingRevision{ 0 } : found->second.revision;
    const auto target = found == state.bindings.end()
        ? std::optional<DataRevisionRef>{} : found->second.target;
    return revision == expectation.expectedBindingRevision
        && (!expectation.isTargetChecked
            || target == expectation.expectedTarget);
}

bool GetBindingMatched(
    const GraphState& state,
    const DataBindingUpdate& update)
{
    const auto found = state.bindings.find(update.binding);
    const auto revision = found == state.bindings.end()
        ? DataBindingRevision{ 0 } : found->second.revision;
    const auto target = found == state.bindings.end()
        ? std::optional<DataRevisionRef>{} : found->second.target;
    return revision == update.expectedRevision
        && (!update.isTargetChecked || target == update.expectedTarget);
}

bool GetReferenceExists(
    const GraphState& state,
    const std::map<DataRevisionRef, std::size_t>& provisional,
    const DataRevisionRef& ref)
{
    return provisional.find(ref) != provisional.end() || state.GetData(ref) != nullptr;
}

bool GetGraphAcyclic(
    const GraphState& state,
    const std::vector<DataSnapshot>& outputs)
{
    std::map<DataRevisionRef, const DataRevision*> graph;
    for (const auto& entry : state.revisions) {
        if (entry.second.metadata) graph.emplace(entry.first, entry.second.metadata.get());
    }
    for (const auto& output : outputs) {
        if (output) graph[output->self] = output.get();
    }

    std::map<DataRevisionRef, std::uint8_t> colors;
    std::function<bool(const DataRevisionRef&)> visit;
    visit = [&graph, &colors, &visit](const DataRevisionRef& ref) {
        const auto color = colors.find(ref);
        if (color != colors.end()) {
            if (color->second == 1) return false;
            if (color->second == 2) return true;
        }
        colors[ref] = 1;
        const auto node = graph.find(ref);
        if (node == graph.end() || !node->second) return false;
        for (const auto& input : node->second->inputs) {
            if (!visit(input.source)) return false;
        }
        colors[ref] = 2;
        return true;
    };

    for (const auto& output : outputs) {
        if (!output || !visit(output->self)) return false;
    }
    return true;
}

} // namespace

class DataGraphStore::Impl final {
public:
    struct ObserverEntry final {
        DataChangeCallback callback;
    };

    Impl()
    {
        auto initial = std::make_shared<GraphState>();
        for (auto descriptor : GetBuiltInDataTypes()) {
            initial->types.emplace(descriptor.id, std::move(descriptor));
        }
        m_state = std::move(initial);

        const auto clockValue = static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch().count());
        std::random_device random;
        const auto randomValue =
            (static_cast<std::uint64_t>(random()) << 32U)
            ^ static_cast<std::uint64_t>(random());
        m_entityPrefix = clockValue ^ randomValue;
        if (m_entityPrefix == 0) m_entityPrefix = 1;
    }

    class ChangeBatch final : public DataChangeBatch {
    public:
        explicit ChangeBatch(std::weak_ptr<Impl> owner) : m_owner(std::move(owner)) {}
        ~ChangeBatch() noexcept override
        {
            const auto owner = m_owner.lock();
            if (!owner) return;
            {
                const std::lock_guard<std::mutex> lock(owner->m_changeMutex);
                if (owner->m_batchDepth != 0) --owner->m_batchDepth;
            }
            if (owner->m_isAlive.load()) owner->DrainChanges();
        }
    private:
        std::weak_ptr<Impl> m_owner;
    };

    void DrainChanges()
    {
        {
            std::lock_guard<std::mutex> lock(m_changeMutex);
            if (m_isDraining || m_batchDepth != 0) return;
            m_isDraining = true;
        }

        for (;;) {
            DataChangeSet change;
            {
                std::lock_guard<std::mutex> lock(m_changeMutex);
                if (m_changes.empty() || m_batchDepth != 0) {
                    m_isDraining = false;
                    return;
                }
                change = std::move(m_changes.front());
                m_changes.pop_front();
            }

            std::vector<std::pair<DataObserverId,
                std::shared_ptr<ObserverEntry>>> callbacks;
            try {
                std::lock_guard<std::mutex> lock(m_observerMutex);
                callbacks.reserve(m_observers.size());
                for (const auto& observer : m_observers) {
                    callbacks.push_back(observer);
                }
            }
            catch (...) {
                continue;
            }
            for (const auto& callback : callbacks) {
                {
                    const std::lock_guard<std::mutex> lock(m_observerMutex);
                    const auto found = m_observers.find(callback.first);
                    if (found == m_observers.end()
                        || found->second != callback.second) {
                        continue;
                    }
                }
                try {
                    // 同批 A 可退订 B；复用原闭包也保留 mutable callback 的状态。
                    // 锁外调用不承诺等待另一线程中已经开始的 callback 退出。
                    if (callback.second->callback) callback.second->callback(change);
                }
                catch (...) {
                    // 通知发生在正式 commit 之后；单个 observer 失败不能回滚数据，
                    // 也不能阻止同一 change set 的其他 observer。
                }
            }
        }
    }

    mutable std::mutex m_stateMutex;
    std::shared_ptr<const GraphState> m_state;

    std::mutex m_entityMutex;
    std::uint64_t m_entityPrefix = 0;
    std::uint64_t m_nextEntity = 1;

    std::mutex m_observerMutex;
    std::map<DataObserverId, std::shared_ptr<ObserverEntry>> m_observers;
    DataObserverId m_nextObserver = 1;

    std::mutex m_changeMutex;
    std::deque<DataChangeSet> m_changes;
    bool m_isDraining = false;
    std::size_t m_batchDepth = 0;
    std::thread::id m_batchOwner;
    std::atomic<bool> m_isAlive{ true };
};

DataGraphStore::DataGraphStore()
    : m_impl(std::make_shared<Impl>())
{
}

DataGraphStore::~DataGraphStore()
{
    m_impl->m_isAlive.store(false);
}

std::unique_ptr<DataChangeBatch> DataGraphStore::StartDataChanges()
{
    const std::lock_guard<std::mutex> lock(m_impl->m_changeMutex);
    const auto owner = std::this_thread::get_id();
    if ((m_impl->m_batchDepth != 0 && m_impl->m_batchOwner != owner)
        || m_impl->m_batchDepth == std::numeric_limits<std::size_t>::max()) {
        return {};
    }
    auto batch = std::make_unique<Impl::ChangeBatch>(m_impl);
    m_impl->m_batchOwner = owner;
    ++m_impl->m_batchDepth;
    return batch;
}

DataLifetimeState DataGraphStore::GetDataLifetime(const DataEntityId& scopeId) const
{
    std::shared_ptr<DataLifetimeScope> scope;
    {
        const std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
        const auto found = m_impl->m_state->lifetimes.find(scopeId);
        if (found == m_impl->m_state->lifetimes.end()) return {};
        scope = found->second;
    }
    const std::lock_guard<std::mutex> lock(scope->mutex);
    return scope->GetState();
}

DataLifetimeState DataGraphStore::SetDataRelease(const DataEntityId& scopeId)
{
    DataLifetimeState result;
    {
        const std::lock_guard<std::mutex> stateLock(m_impl->m_stateMutex);
        const auto found = m_impl->m_state->lifetimes.find(scopeId);
        if (found == m_impl->m_state->lifetimes.end()) return {};
        const auto scope = found->second;
        const std::lock_guard<std::mutex> scopeLock(scope->mutex);
        result = scope->GetState();
        if (result.status != DataLifetimeStatus::Releasing || !result.blockers.empty()) return result;
        DataChangeSet change;
        change.commitId = m_impl->m_state->commitId;
        change.releasedScopes.push_back(scopeId);
        const std::lock_guard<std::mutex> changeLock(m_impl->m_changeMutex);
        m_impl->m_changes.push_back(std::move(change));
        scope->status = result.status = DataLifetimeStatus::Released;
        scope->probes.clear();
        scope->uses.clear();
    }
    m_impl->DrainChanges();
    return result;
}

DataGraphSnapshot DataGraphStore::GetDataGraph() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
    return DataGraphSnapshot{ m_impl->m_state->commitId, m_impl->m_state };
}

DataSnapshot DataGraphStore::GetData(
    const DataGraphSnapshot& graph,
    const DataRevisionRef& ref) const
{
    return graph.view ? graph.view->GetData(ref) : DataSnapshot{};
}

DataQueryResult DataGraphStore::GetDataQuery(
    const DataGraphSnapshot& graph,
    const DataQuery& query) const
{
    return graph.view
        ? graph.view->GetDataQuery(query)
        : DataQueryResult{};
}

std::optional<DataBinding> DataGraphStore::GetDataBinding(
    const DataGraphSnapshot& graph,
    const std::string_view name) const
{
    return graph.view
        ? graph.view->GetDataBinding(name)
        : std::optional<DataBinding>{};
}

ProjectDataSnapshot DataGraphStore::GetProjectData() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
    ProjectDataSnapshot result;
    result.commitId = m_impl->m_state->commitId;
    result.bindings.reserve(m_impl->m_state->bindings.size());
    for (const auto& binding : m_impl->m_state->bindings) {
        result.bindings.push_back(binding.second);
    }
    return result;
}

DataEntityId DataGraphStore::CreateDataEntityId()
{
    std::lock_guard<std::mutex> lock(m_impl->m_entityMutex);
    if (m_impl->m_nextEntity == 0) return {};
    const auto sequence = m_impl->m_nextEntity++;
    DataEntityId result;
    for (std::size_t index = 0; index < 8; ++index) {
        result.bytes[index] = static_cast<std::uint8_t>(
            (m_impl->m_entityPrefix >> (index * 8U)) & 0xffU);
        result.bytes[index + 8] = static_cast<std::uint8_t>(
            (sequence >> (index * 8U)) & 0xffU);
    }
    return result;
}

bool DataGraphStore::SetDataType(DataTypeDescriptor descriptor)
{
    if (!GetDataTypeIdValid(descriptor.id) || !descriptor.validate) {
        return false;
    }
    std::set<DataFacetId> facets;
    for (const auto& facet : descriptor.facets) {
        if (facet.name.empty() || !facets.insert(facet).second) return false;
    }

    try {
        std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
        if (m_impl->m_state->types.find(descriptor.id)
            != m_impl->m_state->types.end()) {
            return false;
        }
        auto next = std::make_shared<GraphState>(*m_impl->m_state);
        next->types.emplace(descriptor.id, std::move(descriptor));
        m_impl->m_state = std::move(next);
        return true;
    }
    catch (...) {
        return false;
    }
}

DataCommitResult DataGraphStore::SetDataCommit(
    DataTransaction transaction)
{
    if (transaction.outputs.empty() && transaction.bindings.empty()
        && transaction.retireScopes.empty()) {
        return GetRejected(
            DataCommitFailure::InvalidTransaction,
            "Data transaction has no output or binding update.");
    }

    struct FrozenDraft final {
        DataEntityId entityId;
        DataGeneration expectedGeneration = 0;
        DataTypeId type;
        std::vector<DataInputRef> inputs;
        std::shared_ptr<const IDataPayload> payload;
        std::optional<DataProvenance> provenance;
        std::optional<DataEntityId> lifetimeScope;
        std::vector<std::shared_ptr<const void>> resources;
        std::vector<DataPreparedResource> preparedResources;
    };

    std::shared_ptr<const GraphState> validationState;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_stateMutex);
        validationState = m_impl->m_state;
    }

    std::vector<FrozenDraft> drafts;
    try {
        drafts.reserve(transaction.outputs.size());
        for (auto& output : transaction.outputs) {
            if (!GetDataEntityIdValid(output.entityId)
                || !GetDataTypeIdValid(output.type)
                || !output.payload) {
                return GetRejected(
                    DataCommitFailure::InvalidTransaction,
                    "Data output identity, type, or payload is invalid.");
            }
            const auto descriptor = validationState->types.find(output.type);
            if (descriptor == validationState->types.end()) {
                return GetRejected(
                    DataCommitFailure::TypeNotRegistered,
                    "Data output type is not registered.");
            }
            auto payload = output.payload->CreateSnapshot();
            if (!payload || payload->GetDataType() != output.type) {
                return GetRejected(
                    DataCommitFailure::PayloadInvalid,
                    "Payload snapshot type does not match the output type.");
            }
            std::string message;
            if (!descriptor->second.validate(*payload, message)) {
                return GetRejected(
                    DataCommitFailure::PayloadInvalid,
                    message.empty() ? "Payload validation failed." : message);
            }
            auto resources = payload->GetDataResources();
            if (output.lifetimeScope && !GetDataEntityIdValid(*output.lifetimeScope)) {
                return GetRejected(DataCommitFailure::InvalidTransaction, "Invalid lifetime scope.");
            }
            for (const auto& resource : output.preparedResources) {
                if (!resource.lease || resource.owner.empty()
                    || (resource.kind != DataResourceKind::Reader && resource.kind != DataResourceKind::RenderObject))
                    return GetRejected(DataCommitFailure::InvalidTransaction, "Invalid prepared resource.");
            }
            drafts.push_back(FrozenDraft{
                output.entityId,
                output.expectedGeneration,
                std::move(output.type),
                std::move(output.inputs),
                std::move(payload),
                std::move(output.provenance),
                output.lifetimeScope,
                std::move(resources), std::move(output.preparedResources) });
        }
    }
    catch (...) {
        return GetRejected(
            DataCommitFailure::OutOfMemory,
            "Payload snapshot allocation failed.");
    }

    DataCommitResult result;
    try {
        std::unique_lock<std::mutex> stateLock(m_impl->m_stateMutex);
        const auto current = m_impl->m_state;
        if (current->commitId == std::numeric_limits<DataCommitId>::max()) {
            return GetRejected(
                DataCommitFailure::Overflow,
                "Data commit id is exhausted.");
        }

        bool isActivated = true;
        for (const auto& expectation : transaction.expectations) {
            if (GetExpectationMatched(*current, expectation)) continue;
            if (expectation.use == DataExpectationUse::Required
                || transaction.policy
                    == DataPublishPolicy::RequireCurrentInputs) {
                return GetRejected(
                    DataCommitFailure::ExpectationFailed,
                    "Data transaction expectation failed.");
            }
            isActivated = false;
        }

        std::set<std::string> bindingNames;
        for (const auto& update : transaction.bindings) {
            if (update.binding.empty()
                || !bindingNames.insert(update.binding).second) {
                return GetRejected(
                    DataCommitFailure::InvalidTransaction,
                    "Data binding update is invalid or duplicated.");
            }
            if (GetBindingMatched(*current, update)) continue;
            if (transaction.policy
                == DataPublishPolicy::RequireCurrentInputs) {
                return GetRejected(
                    DataCommitFailure::ExpectationFailed,
                    "Data binding compare-and-set failed.");
            }
            isActivated = false;
        }
        if (!isActivated && drafts.empty()) {
            return GetRejected(
                DataCommitFailure::ExpectationFailed,
                "Historical binding-only transaction has no publishable output.");
        }

        std::map<DataEntityId, DataRevisionRef> entities;
        std::map<DataRevisionRef, std::size_t> provisional;
        for (std::size_t index = 0; index < drafts.size(); ++index) {
            const auto& draft = drafts[index];
            if (!entities.emplace(draft.entityId, DataRevisionRef{}).second) {
                return GetRejected(
                    DataCommitFailure::InvalidTransaction,
                    "A transaction cannot publish two generations of one entity.");
            }
            const auto head = current->heads.find(draft.entityId);
            const auto currentGeneration = head == current->heads.end()
                ? DataGeneration{ 0 } : head->second;
            if (currentGeneration != draft.expectedGeneration) {
                return GetRejected(
                    DataCommitFailure::ExpectationFailed,
                    "Data entity head compare-and-set failed.");
            }
            if (currentGeneration == std::numeric_limits<DataGeneration>::max()) {
                return GetRejected(
                    DataCommitFailure::Overflow,
                    "Data generation is exhausted.");
            }
            const DataRevisionRef ref{
                draft.entityId, currentGeneration + 1 };
            entities[draft.entityId] = ref;
            if (!provisional.emplace(ref, index).second) {
                return GetRejected(
                    DataCommitFailure::InvalidTransaction,
                    "Data output revision is duplicated.");
            }
        }

        for (const auto& draft : drafts) {
            std::set<std::string> roles;
            for (const auto& input : draft.inputs) {
                const auto existing = current->revisions.find(input.source);
                if (existing != current->revisions.end() && existing->second.scope
                    && !existing->second.scope->GetIsPublished()) {
                    return GetRejected(DataCommitFailure::ResultRetired, "Input revision has retired.");
                }
                if (input.role.empty()
                    || !roles.insert(input.role).second
                    || !GetDataRevisionRefValid(input.source)
                    || !GetReferenceExists(
                        *current, provisional, input.source)) {
                    return GetRejected(
                        DataCommitFailure::MissingInput,
                        "Data output input is invalid, duplicated, or missing.");
                }
                // 内置标签的 source-volume 契约必须引用同一网格；与节点和绑定一起原子验证。
                const auto labels = std::dynamic_pointer_cast<const LabelMap3DPayload>(draft.payload);
                if (labels && input.role == "source-volume") {
                    const auto stagedSource = provisional.find(input.source);
                    const auto sourcePayload = stagedSource != provisional.end()
                        ? drafts[stagedSource->second].payload
                        : current->GetData(input.source)->payload;
                    const auto image = std::dynamic_pointer_cast<const ImageGrid3DPayload>(sourcePayload);
                    if (!image) return GetRejected(DataCommitFailure::PayloadInvalid,
                        "Label source-volume is not an image grid.");
                    const auto& sourceGrid = image->GetGeometry();
                    const auto& labelGrid = labels->GetGeometry();
                    if (sourceGrid.extent != labelGrid.extent
                        || sourceGrid.dimensions != labelGrid.dimensions
                        || sourceGrid.spacing != labelGrid.spacing
                        || sourceGrid.origin != labelGrid.origin
                        || sourceGrid.direction != labelGrid.direction
                        || sourceGrid.coordinateFrame != labelGrid.coordinateFrame) {
                        return GetRejected(DataCommitFailure::PayloadInvalid,
                            "Label geometry differs from its source-volume.");
                    }
                }
            }
        }
        if (isActivated) {
            for (const auto& update : transaction.bindings) {
                const auto existing = update.target ? current->revisions.find(*update.target)
                    : current->revisions.end();
                if (existing != current->revisions.end() && existing->second.scope
                    && !existing->second.scope->GetIsPublished()) {
                    return GetRejected(DataCommitFailure::ResultRetired, "Binding target has retired.");
                }
                if (update.target
                    && (!GetDataRevisionRefValid(*update.target)
                        || !GetReferenceExists(
                            *current, provisional, *update.target))) {
                    return GetRejected(
                        DataCommitFailure::MissingInput,
                        "Data binding target does not exist.");
                }
                const auto binding = current->bindings.find(update.binding);
                const auto revision = binding == current->bindings.end()
                    ? DataBindingRevision{ 0 }
                    : binding->second.revision;
                if (revision == std::numeric_limits<DataBindingRevision>::max()) {
                    return GetRejected(
                        DataCommitFailure::Overflow,
                        "Data binding revision is exhausted.");
                }
            }
        }

        // 1. 冻结退役集合并锁定所有作用域，阻止校验与提交间取得新资源租约。
        std::map<DataEntityId, std::shared_ptr<DataLifetimeScope>> retiring;
        std::set<DataRevisionRef> retiredRefs;
        std::vector<std::unique_lock<std::mutex>> scopeLocks;
        if (!transaction.retireScopes.empty() && !isActivated) {
            return GetRejected(DataCommitFailure::ExpectationFailed, "Retirement requires current bindings.");
        }
        for (const auto& retirement : transaction.retireScopes) {
            const auto found = current->lifetimes.find(retirement.scopeId);
            if (found == current->lifetimes.end()
                || !retiring.emplace(retirement.scopeId, found->second).second) {
                return GetRejected(DataCommitFailure::InvalidTransaction, "Unknown or duplicate retirement scope.");
            }
        }
        scopeLocks.reserve(retiring.size());
        for (const auto& entry : retiring) scopeLocks.emplace_back(entry.second->mutex);
        for (const auto& retirement : transaction.retireScopes) {
            const auto scope = retiring.at(retirement.scopeId);
            if (scope->status != DataLifetimeStatus::Published) {
                return GetRejected(DataCommitFailure::ResultRetired, "Scope has already retired.");
            }
            auto expected = retirement.expectedRevisions;
            std::sort(expected.begin(), expected.end());
            auto actual = scope->revisions;
            std::sort(actual.begin(), actual.end());
            if (retirement.expectedStatus != scope->status || expected != actual) {
                return GetRejected(DataCommitFailure::ExpectationFailed, "Retirement scope expectation failed.");
            }
            retiredRefs.insert(actual.begin(), actual.end());
            for (const auto& use : scope->uses) {
                if (!use.resource.expired() && (!retirement.isResourceTransition || use.kind == DataResourceKind::Reader)) {
                    result.blockers.push_back({ use.revision, use.owner });
                }
            }
        }
        // 2. 依赖、绑定与本事务输入共同检查；集合内相互依赖不阻塞自己的退役。
        for (const auto& entry : current->revisions) {
            if (retiredRefs.count(entry.first)) continue;
            if (entry.second.scope && !entry.second.scope->GetIsPublished()) continue;
            for (const auto& input : entry.second.metadata->inputs) {
                if (retiredRefs.count(input.source)) result.blockers.push_back({entry.first, "DerivedRevision"});
            }
        }
        for (const auto& binding : current->bindings) {
            auto target = binding.second.target;
            const auto update = std::find_if(transaction.bindings.begin(), transaction.bindings.end(),
                [&binding](const DataBindingUpdate& item) { return item.binding == binding.first; });
            if (update != transaction.bindings.end()) target = update->target;
            if (target && retiredRefs.count(*target)) result.blockers.push_back({ *target, binding.first });
        }
        for (const auto& draft : drafts) {
            if (draft.lifetimeScope && current->lifetimes.count(*draft.lifetimeScope)) {
                return GetRejected(DataCommitFailure::ResultRetired, "A lifetime scope cannot be reused or extended.");
            }
            for (const auto& input : draft.inputs) {
                if (retiredRefs.count(input.source)) return GetRejected(DataCommitFailure::ResultRetired,
                    "A new output cannot depend on a retiring revision.");
            }
        }
        for (const auto& binding : transaction.bindings) {
            if (binding.target && retiredRefs.count(*binding.target)) {
                result.blockers.push_back({ *binding.target, binding.binding });
            }
        }
        if (!result.blockers.empty()) {
            result.failureReason = DataCommitFailure::ResultInUse;
            result.message = "Retirement is blocked by live consumers.";
            return result;
        }

        auto next = std::make_shared<GraphState>(*current);
        result.published.reserve(drafts.size());
        for (const auto& draft : drafts) {
            const auto ref = entities.at(draft.entityId);
            std::shared_ptr<DataLifetimeScope> scope;
            if (draft.lifetimeScope) {
                auto& slot = next->lifetimes[*draft.lifetimeScope];
                if (!slot) {
                    slot = std::make_shared<DataLifetimeScope>();
                    slot->id = *draft.lifetimeScope;
                }
                scope = slot;
            }
            auto snapshot = std::make_shared<const DataRevision>(DataRevision{
                ref, draft.type, draft.inputs, draft.payload, draft.provenance,
                scope ? scope->id : DataEntityId{}, scope });
            RevisionEntry entry{ snapshot, {}, scope, {} };
            for (const auto& resource : draft.resources) entry.resources.push_back(resource);
            if (scope) {
                auto metadata = std::make_shared<DataRevision>(*snapshot);
                metadata->payload.reset();
                entry.metadata = std::move(metadata);
                entry.value = snapshot;
                scope->revisions.push_back(ref);
                scope->owned.push_back(snapshot);
                for (const auto& resource : draft.preparedResources)
                    scope->uses.push_back({ref, resource.owner, resource.lease, resource.kind});
                scope->probes.push_back({ ref, "LegacyOwner:revision", snapshot });
                scope->probes.push_back({ ref, "LegacyOwner:payload", draft.payload });
                std::vector<std::weak_ptr<const void>> sharedInputs;
                for (const auto& input : draft.inputs) {
                    const auto staged = provisional.find(input.source);
                    if (staged != provisional.end()) {
                        for (const auto& resource : drafts[staged->second].resources) sharedInputs.push_back(resource);
                    }
                    else {
                        const auto& resources = current->revisions.at(input.source).resources;
                        sharedInputs.insert(sharedInputs.end(), resources.begin(), resources.end());
                    }
                }
                for (const auto& resource : draft.resources) {
                    if (!resource) continue;
                    const auto shared = std::any_of(sharedInputs.begin(), sharedInputs.end(),
                        [&resource](const auto& input) {
                            return !resource.owner_before(input) && !input.owner_before(resource);
                        });
                    if (!shared) scope->probes.push_back({ ref, "LegacyOwner:shared-bytes", resource });
                }
            }
            next->revisions.emplace(ref, std::move(entry));
            next->heads[ref.entityId] = ref.generation;
            for (const auto& input : snapshot->inputs) {
                next->derived[input.source].push_back(ref);
            }
            result.published.push_back(std::move(snapshot));
        }
        if (!GetGraphAcyclic(*current, result.published)) {
            return GetRejected(
                DataCommitFailure::CycleDetected,
                "Data transaction would create a cycle.");
        }

        DataChangeSet change;
        change.commitId = current->commitId + 1;
        for (const auto& snapshot : result.published) {
            change.published.push_back(snapshot->self);
        }
        if (isActivated) {
            result.bindings.reserve(transaction.bindings.size());
            change.bindings.reserve(transaction.bindings.size());
            for (const auto& update : transaction.bindings) {
                const auto previous = current->bindings.find(update.binding);
                const auto previousTarget = previous == current->bindings.end()
                    ? std::optional<DataRevisionRef>{}
                    : previous->second.target;
                const auto revision = previous == current->bindings.end()
                    ? DataBindingRevision{ 1 }
                    : previous->second.revision + 1;
                DataBinding binding{
                    update.binding, update.target, revision };
                next->bindings[update.binding] = binding;
                result.bindings.push_back(binding);
                change.bindings.push_back(DataBindingChange{
                    update.binding, previousTarget, binding });
            }
        }
        for (const auto& retirement : retiring) change.retiredScopes.push_back(retirement.first);
        std::vector<std::vector<DataSnapshot>> retiredOwners(retiring.size());
        next->commitId = change.commitId;
        result.graph = { next->commitId, next };

        // 通知先在 state lock 内按 CommitId 排队，同时持有 change lock；
        // state swap 完成前 drain 无法取走该项，回调始终观察到正式新状态。
        {
            std::lock_guard<std::mutex> changeLock(m_impl->m_changeMutex);
            m_impl->m_changes.push_back(change);
            std::size_t index = 0;
            for (const auto& retirement : retiring) {
                retirement.second->status = DataLifetimeStatus::Releasing;
                retiredOwners[index++].swap(retirement.second->owned);
            }
            m_impl->m_state = std::move(next);
        }
        result.status = isActivated
            ? DataCommitStatus::Succeeded
            : DataCommitStatus::SucceededHistorical;
        result.failureReason = DataCommitFailure::None;
        result.commitId = change.commitId;
        result.isActivated = isActivated;
        scopeLocks.clear();
        stateLock.unlock();
        // 最后一个 payload owner 的析构可能重入，必须在全部内部锁外释放。
        retiredOwners.clear();
        m_impl->DrainChanges();
        return result;
    }
    catch (...) {
        return GetRejected(
            DataCommitFailure::OutOfMemory,
            "Data graph state allocation failed.");
    }
}

DataObserverId DataGraphStore::AttachDataChange(
    DataChangeCallback callback)
{
    if (!callback) return 0;
    try {
        const auto entry = std::make_shared<Impl::ObserverEntry>();
        entry->callback = std::move(callback);
        std::lock_guard<std::mutex> lock(m_impl->m_observerMutex);
        if (m_impl->m_nextObserver == 0) return 0;
        const auto observerId = m_impl->m_nextObserver++;
        m_impl->m_observers.emplace(observerId, entry);
        return observerId;
    }
    catch (...) {
        return 0;
    }
}

bool DataGraphStore::DetachDataChange(const DataObserverId observerId)
{
    if (observerId == 0) return false;
    std::shared_ptr<Impl::ObserverEntry> removed;
    {
        const std::lock_guard<std::mutex> lock(m_impl->m_observerMutex);
        const auto found = m_impl->m_observers.find(observerId);
        if (found == m_impl->m_observers.end()) return false;
        removed = std::move(found->second);
        m_impl->m_observers.erase(found);
    }
    // 用户闭包的最后一个 owner 析构也可能重入 registry。
    return true;
}
