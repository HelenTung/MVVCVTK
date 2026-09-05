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
#include <utility>

namespace {

class GraphState final : public DataGraphView {
public:
    DataSnapshot GetData(const DataRevisionRef& ref) const override
    {
        const auto found = revisions.find(ref);
        return found == revisions.end() ? DataSnapshot{} : found->second;
    }

    std::optional<DataBinding> GetDataBinding(
        const std::string_view name) const override
    {
        const auto found = bindings.find(std::string(name));
        return found == bindings.end()
            ? std::optional<DataBinding>{}
            : std::optional<DataBinding>{ found->second };
    }

    DataQueryResult GetDataQuery(const DataQuery& query) const override
    {
        DataQueryResult result;
        result.commitId = commitId;
        for (const auto& entry : revisions) {
            const auto& snapshot = entry.second;
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
    std::map<DataRevisionRef, DataSnapshot> revisions;
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
    return state.revisions.find(ref) != state.revisions.end()
        || provisional.find(ref) != provisional.end();
}

bool GetGraphAcyclic(
    const GraphState& state,
    const std::vector<DataSnapshot>& outputs)
{
    std::map<DataRevisionRef, const DataRevision*> graph;
    for (const auto& entry : state.revisions) {
        if (entry.second) graph.emplace(entry.first, entry.second.get());
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

    void DrainChanges()
    {
        {
            std::lock_guard<std::mutex> lock(m_changeMutex);
            if (m_isDraining) return;
            m_isDraining = true;
        }

        for (;;) {
            DataChangeSet change;
            {
                std::lock_guard<std::mutex> lock(m_changeMutex);
                if (m_changes.empty()) {
                    m_isDraining = false;
                    return;
                }
                change = std::move(m_changes.front());
                m_changes.pop_front();
            }

            std::vector<DataChangeCallback> callbacks;
            try {
                std::lock_guard<std::mutex> lock(m_observerMutex);
                callbacks.reserve(m_observers.size());
                for (const auto& observer : m_observers) {
                    callbacks.push_back(observer.second);
                }
            }
            catch (...) {
                continue;
            }
            for (const auto& callback : callbacks) {
                try {
                    if (callback) callback(change);
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
    std::map<DataObserverId, DataChangeCallback> m_observers;
    DataObserverId m_nextObserver = 1;

    std::mutex m_changeMutex;
    std::deque<DataChangeSet> m_changes;
    bool m_isDraining = false;
};

DataGraphStore::DataGraphStore()
    : m_impl(std::make_unique<Impl>())
{
}

DataGraphStore::~DataGraphStore() = default;

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
    if (transaction.outputs.empty() && transaction.bindings.empty()) {
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
            drafts.push_back(FrozenDraft{
                output.entityId,
                output.expectedGeneration,
                std::move(output.type),
                std::move(output.inputs),
                std::move(payload),
                std::move(output.provenance) });
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
                if (input.role.empty()
                    || !roles.insert(input.role).second
                    || !GetDataRevisionRefValid(input.source)
                    || !GetReferenceExists(
                        *current, provisional, input.source)) {
                    return GetRejected(
                        DataCommitFailure::MissingInput,
                        "Data output input is invalid, duplicated, or missing.");
                }
            }
        }
        if (isActivated) {
            for (const auto& update : transaction.bindings) {
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

        auto next = std::make_shared<GraphState>(*current);
        result.published.reserve(drafts.size());
        for (const auto& draft : drafts) {
            const auto ref = entities.at(draft.entityId);
            auto snapshot = std::make_shared<const DataRevision>(DataRevision{
                ref,
                draft.type,
                draft.inputs,
                draft.payload,
                draft.provenance });
            next->revisions.emplace(ref, snapshot);
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
        next->commitId = change.commitId;

        // 通知先在 state lock 内按 CommitId 排队，同时持有 change lock；
        // state swap 完成前 drain 无法取走该项，回调始终观察到正式新状态。
        {
            std::lock_guard<std::mutex> changeLock(m_impl->m_changeMutex);
            m_impl->m_changes.push_back(change);
            m_impl->m_state = std::move(next);
        }
        result.status = isActivated
            ? DataCommitStatus::Succeeded
            : DataCommitStatus::SucceededHistorical;
        result.failureReason = DataCommitFailure::None;
        result.commitId = change.commitId;
        result.isActivated = isActivated;
        stateLock.unlock();
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
        std::lock_guard<std::mutex> lock(m_impl->m_observerMutex);
        if (m_impl->m_nextObserver == 0) return 0;
        const auto observerId = m_impl->m_nextObserver++;
        m_impl->m_observers.emplace(observerId, std::move(callback));
        return observerId;
    }
    catch (...) {
        return 0;
    }
}

bool DataGraphStore::DetachDataChange(const DataObserverId observerId)
{
    if (observerId == 0) return false;
    std::lock_guard<std::mutex> lock(m_impl->m_observerMutex);
    return m_impl->m_observers.erase(observerId) == 1;
}
