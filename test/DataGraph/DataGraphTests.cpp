#include "Data/DataGraphStore.h"
#include "Data/DataPayloads.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class MutablePayload final : public IDataPayload {
public:
    MutablePayload(DataTypeId type, int value)
        : m_type(std::move(type)), m_value(value)
    {
    }

    DataTypeId GetDataType() const override { return m_type; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        return std::make_shared<const MutablePayload>(m_type, m_value);
    }
    int GetValue() const noexcept { return m_value; }
    void SetValue(const int value) noexcept { m_value = value; }

private:
    DataTypeId m_type;
    int m_value = 0;
};

std::shared_ptr<const ImageGrid3DPayload> BuildImage(const float value)
{
    GridGeometry3D geometry;
    geometry.extent = { 0, 0, 0, 0, 0, 0 };
    geometry.dimensions = { 1, 1, 1 };
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(sizeof(value));
    std::memcpy(bytes->data(), &value, sizeof(value));
    return std::make_shared<const ImageGrid3DPayload>(
        geometry,
        ImageValueType::Float32,
        1,
        bytes,
        DataBytes{},
        std::array<double, 2>{ value, value });
}

DataRevisionDraft BuildImageDraft(
    const DataEntityId& entityId,
    const DataGeneration expectedGeneration,
    const float value,
    std::vector<DataInputRef> inputs = {})
{
    return DataRevisionDraft{
        entityId,
        expectedGeneration,
        DataTypes::imageGrid3D,
        std::move(inputs),
        BuildImage(value),
        DataProvenance{ "test", "build-image", "1", "{}" } };
}

DataBindingUpdate BuildBinding(
    std::string binding,
    const DataBindingRevision expectedRevision,
    std::optional<DataRevisionRef> target,
    std::optional<DataRevisionRef> expectedTarget = {})
{
    return DataBindingUpdate{
        std::move(binding),
        expectedRevision,
        true,
        expectedTarget,
        target };
}

bool Check(const bool condition, const char* message)
{
    if (condition) return true;
    std::cerr << "[DataGraph] " << message << '\n';
    return false;
}

bool GetBasicCommitValid()
{
    DataGraphStore store;
    const auto entity = store.CreateDataEntityId();
    const DataRevisionRef ref{ entity, 1 };
    DataTransaction transaction;
    transaction.outputs.push_back(BuildImageDraft(entity, 0, 1.0f));
    transaction.bindings.push_back(BuildBinding(
        std::string(primaryVolumeBinding), 0, ref));
    const auto result = store.SetDataCommit(std::move(transaction));
    const auto graph = store.GetDataGraph();
    const auto binding = store.GetDataBinding(graph, primaryVolumeBinding);
    return Check(
        result.status == DataCommitStatus::Succeeded
            && result.commitId == 1
            && result.published.size() == 1
            && result.published.front()->self == ref
            && binding && binding->target == ref
            && binding->revision == 1,
        "basic output and binding commit failed");
}

bool GetGenerationValid()
{
    DataGraphStore store;
    const auto entity = store.CreateDataEntityId();
    DataTransaction first;
    first.outputs.push_back(BuildImageDraft(entity, 0, 1.0f));
    const auto firstResult = store.SetDataCommit(std::move(first));
    DataTransaction second;
    second.outputs.push_back(BuildImageDraft(entity, 1, 2.0f));
    const auto secondResult = store.SetDataCommit(std::move(second));
    DataTransaction stale;
    stale.outputs.push_back(BuildImageDraft(entity, 1, 3.0f));
    const auto staleResult = store.SetDataCommit(std::move(stale));
    return Check(
        firstResult.published.front()->self.generation == 1
            && secondResult.published.front()->self.generation == 2
            && firstResult.published.front()->self
                != secondResult.published.front()->self
            && staleResult.status == DataCommitStatus::Rejected
            && staleResult.failureReason
                == DataCommitFailure::ExpectationFailed,
        "entity generation was reused or stale head won");
}

bool GetTypeValidationValid()
{
    DataGraphStore store;
    const DataTypeId unknownType{ "test.unknown", 1 };
    const auto unknownEntity = store.CreateDataEntityId();
    DataTransaction unknown;
    unknown.outputs.push_back(DataRevisionDraft{
        unknownEntity, 0, unknownType, {},
        std::make_shared<const MutablePayload>(unknownType, 1),
        std::nullopt });
    const auto unknownResult = store.SetDataCommit(std::move(unknown));

    const auto mismatchedEntity = store.CreateDataEntityId();
    DataTransaction mismatched;
    mismatched.outputs.push_back(DataRevisionDraft{
        mismatchedEntity, 0, DataTypes::imageGrid3D, {},
        std::make_shared<const MutablePayload>(unknownType, 2),
        std::nullopt });
    const auto mismatchedResult = store.SetDataCommit(
        std::move(mismatched));

    const auto invalidEntity = store.CreateDataEntityId();
    DataTransaction invalid;
    invalid.outputs.push_back(DataRevisionDraft{
        invalidEntity, 0, DataTypes::imageGrid3D, {},
        std::make_shared<const MutablePayload>(
            DataTypes::imageGrid3D, 3),
        std::nullopt });
    const auto invalidResult = store.SetDataCommit(std::move(invalid));
    return Check(
        unknownResult.failureReason
                == DataCommitFailure::TypeNotRegistered
            && mismatchedResult.failureReason
                == DataCommitFailure::PayloadInvalid
            && invalidResult.failureReason
                == DataCommitFailure::PayloadInvalid
            && store.GetDataGraph().commitId == 0,
        "unknown, mismatched, or invalid payload type was accepted");
}

bool GetOutputReferencesValid()
{
    DataGraphStore store;
    const auto resultSet = store.CreateDataEntityId();
    const auto left = store.CreateDataEntityId();
    const auto right = store.CreateDataEntityId();
    const auto table = store.CreateDataEntityId();
    const auto statistics = store.CreateDataEntityId();
    const DataRevisionRef resultSetRef{ resultSet, 1 };
    const DataRevisionRef leftRef{ left, 1 };
    const DataRevisionRef rightRef{ right, 1 };
    const DataRevisionRef tableRef{ table, 1 };
    const DataRevisionRef statisticsRef{ statistics, 1 };

    DataTransaction transaction;
    transaction.outputs.push_back(BuildImageDraft(resultSet, 0, 1.0f, {
        DataInputRef{ "left", leftRef },
        DataInputRef{ "right", rightRef },
        DataInputRef{ "table", tableRef },
        DataInputRef{ "statistics", statisticsRef } }));
    transaction.outputs.push_back(BuildImageDraft(left, 0, 2.0f));
    transaction.outputs.push_back(BuildImageDraft(right, 0, 3.0f));
    transaction.outputs.push_back(BuildImageDraft(table, 0, 4.0f, {
        DataInputRef{ "labels", leftRef } }));
    transaction.outputs.push_back(BuildImageDraft(statistics, 0, 5.0f, {
        DataInputRef{ "records", tableRef } }));
    const auto result = store.SetDataCommit(std::move(transaction));
    const auto graph = store.GetDataGraph();
    const auto leftChildren = graph.view->GetDerivedData(leftRef);
    const auto rightChildren = graph.view->GetDerivedData(rightRef);
    return Check(
        result.status == DataCommitStatus::Succeeded
            && result.commitId == 1
            && result.published.size() == 5
            && graph.view->GetData(resultSetRef)
            && graph.view->GetData(leftRef)
            && graph.view->GetData(rightRef)
            && graph.view->GetData(tableRef)
            && graph.view->GetData(statisticsRef)
            && leftChildren
                == std::vector<DataRevisionRef>{ resultSetRef, tableRef }
            && rightChildren
                == std::vector<DataRevisionRef>{ resultSetRef },
        "five-output transaction-local references or multi-parent index failed");
}

bool GetCycleRejected()
{
    DataGraphStore store;
    const auto first = store.CreateDataEntityId();
    const auto second = store.CreateDataEntityId();
    const auto third = store.CreateDataEntityId();
    const DataRevisionRef firstRef{ first, 1 };
    const DataRevisionRef secondRef{ second, 1 };
    const DataRevisionRef thirdRef{ third, 1 };
    DataTransaction selfCycle;
    selfCycle.outputs.push_back(BuildImageDraft(first, 0, 1.0f, {
        DataInputRef{ "self", firstRef } }));
    const auto selfResult = store.SetDataCommit(std::move(selfCycle));

    DataTransaction transaction;
    transaction.outputs.push_back(BuildImageDraft(first, 0, 1.0f, {
        DataInputRef{ "second", secondRef } }));
    transaction.outputs.push_back(BuildImageDraft(second, 0, 2.0f, {
        DataInputRef{ "third", thirdRef } }));
    transaction.outputs.push_back(BuildImageDraft(third, 0, 3.0f, {
        DataInputRef{ "first", firstRef } }));
    const auto before = store.GetDataGraph();
    const auto result = store.SetDataCommit(std::move(transaction));
    const auto after = store.GetDataGraph();
    return Check(
        selfResult.failureReason == DataCommitFailure::CycleDetected
            && result.failureReason == DataCommitFailure::CycleDetected
            && before.commitId == after.commitId
            && !after.view->GetData(firstRef)
            && !after.view->GetData(secondRef)
            && !after.view->GetData(thirdRef),
        "self or indirect cyclic outputs were partially published");
}

bool GetAtomicFailureValid()
{
    DataGraphStore store;
    const auto first = store.CreateDataEntityId();
    const auto second = store.CreateDataEntityId();
    const auto missing = store.CreateDataEntityId();
    DataTransaction transaction;
    transaction.outputs.push_back(BuildImageDraft(first, 0, 1.0f));
    transaction.outputs.push_back(BuildImageDraft(second, 0, 2.0f, {
        DataInputRef{ "missing", DataRevisionRef{ missing, 1 } } }));
    const auto result = store.SetDataCommit(std::move(transaction));
    const DataRevisionRef secondRef{ second, 1 };
    DataTransaction duplicateRole;
    duplicateRole.outputs.push_back(BuildImageDraft(first, 0, 3.0f, {
        DataInputRef{ "source", secondRef },
        DataInputRef{ "source", secondRef } }));
    duplicateRole.outputs.push_back(BuildImageDraft(second, 0, 4.0f));
    const auto duplicateResult = store.SetDataCommit(
        std::move(duplicateRole));
    const auto graph = store.GetDataGraph();
    return Check(
        result.failureReason == DataCommitFailure::MissingInput
            && duplicateResult.failureReason
                == DataCommitFailure::MissingInput
            && graph.commitId == 0
            && !graph.view->GetData(DataRevisionRef{ first, 1 })
            && !graph.view->GetData(DataRevisionRef{ second, 1 }),
        "missing input exposed a partial transaction");
}

bool GetBindingAbaValid()
{
    DataGraphStore store;
    const auto first = store.CreateDataEntityId();
    const auto second = store.CreateDataEntityId();
    const DataRevisionRef firstRef{ first, 1 };
    const DataRevisionRef secondRef{ second, 1 };
    DataTransaction initial;
    initial.outputs.push_back(BuildImageDraft(first, 0, 1.0f));
    initial.outputs.push_back(BuildImageDraft(second, 0, 2.0f));
    initial.bindings.push_back(BuildBinding("active", 0, firstRef));
    if (store.SetDataCommit(std::move(initial)).status
        != DataCommitStatus::Succeeded) {
        return Check(false, "ABA setup failed");
    }
    DataTransaction toSecond;
    toSecond.bindings.push_back(BuildBinding("active", 1, secondRef, firstRef));
    DataTransaction toFirst;
    toFirst.bindings.push_back(BuildBinding("active", 2, firstRef, secondRef));
    const auto secondResult = store.SetDataCommit(std::move(toSecond));
    const auto firstResult = store.SetDataCommit(std::move(toFirst));
    DataTransaction stale;
    stale.bindings.push_back(BuildBinding("active", 1, secondRef, firstRef));
    const auto staleResult = store.SetDataCommit(std::move(stale));
    const auto binding = store.GetDataBinding(store.GetDataGraph(), "active");
    return Check(
        secondResult.status == DataCommitStatus::Succeeded
            && firstResult.status == DataCommitStatus::Succeeded
            && staleResult.failureReason
                == DataCommitFailure::ExpectationFailed
            && binding && binding->target == firstRef
            && binding->revision == 3,
        "binding ABA was not detected by revision");
}

bool GetHistoricalPolicyValid()
{
    DataGraphStore store;
    const auto first = store.CreateDataEntityId();
    const auto second = store.CreateDataEntityId();
    const auto resultEntity = store.CreateDataEntityId();
    const DataRevisionRef firstRef{ first, 1 };
    const DataRevisionRef secondRef{ second, 1 };
    const DataRevisionRef resultRef{ resultEntity, 1 };

    DataTransaction setup;
    setup.outputs.push_back(BuildImageDraft(first, 0, 1.0f));
    setup.outputs.push_back(BuildImageDraft(second, 0, 2.0f));
    setup.bindings.push_back(BuildBinding(
        std::string(primaryVolumeBinding), 0, firstRef));
    store.SetDataCommit(std::move(setup));
    DataTransaction switchInput;
    switchInput.bindings.push_back(BuildBinding(
        std::string(primaryVolumeBinding), 1, secondRef, firstRef));
    store.SetDataCommit(std::move(switchInput));

    DataExpectation activation;
    activation.kind = DataExpectationKind::Binding;
    activation.use = DataExpectationUse::Activation;
    activation.binding = std::string(primaryVolumeBinding);
    activation.expectedBindingRevision = 1;
    activation.isTargetChecked = true;
    activation.expectedTarget = firstRef;
    DataTransaction historical;
    historical.policy = DataPublishPolicy::AllowHistoricalResult;
    historical.expectations.push_back(activation);
    historical.outputs.push_back(BuildImageDraft(resultEntity, 0, 3.0f, {
        DataInputRef{ "source-volume", firstRef } }));
    historical.bindings.push_back(BuildBinding("analysis.active", 0, resultRef));
    const auto result = store.SetDataCommit(std::move(historical));
    const auto graph = store.GetDataGraph();

    const auto rejectedEntity = store.CreateDataEntityId();
    DataExpectation required = activation;
    required.use = DataExpectationUse::Required;
    DataTransaction rejected;
    rejected.policy = DataPublishPolicy::AllowHistoricalResult;
    rejected.expectations = { activation, required };
    rejected.outputs.push_back(BuildImageDraft(
        rejectedEntity, 0, 4.0f, {
            DataInputRef{ "source-volume", firstRef } }));
    const auto rejectedResult = store.SetDataCommit(std::move(rejected));
    return Check(
        result.status == DataCommitStatus::SucceededHistorical
            && !result.isActivated
            && graph.view->GetData(resultRef)
            && !graph.view->GetDataBinding("analysis.active"),
        "historical result was rejected or partially activated")
        && Check(
            rejectedResult.failureReason
                    == DataCommitFailure::ExpectationFailed
                && store.GetDataGraph().commitId == graph.commitId,
            "failed required expectation published a historical result");
}

bool GetQueryAndProjectSnapshotValid()
{
    DataGraphStore store;
    const auto source = store.CreateDataEntityId();
    const auto derived = store.CreateDataEntityId();
    const DataRevisionRef sourceRef{ source, 1 };
    const DataRevisionRef derivedRef{ derived, 1 };
    DataTransaction transaction;
    transaction.outputs.push_back(BuildImageDraft(source, 0, 1.0f));
    transaction.outputs.push_back(BuildImageDraft(derived, 0, 2.0f, {
        DataInputRef{ "source-volume", sourceRef } }));
    transaction.bindings.push_back(BuildBinding(
        std::string(primaryVolumeBinding), 0, sourceRef));
    const auto commit = store.SetDataCommit(std::move(transaction));
    const auto graph = store.GetDataGraph();
    DataQuery query;
    query.facet = DataFacets::scalarGrid3D;
    query.input = sourceRef;
    query.inputRole = "source-volume";
    query.producerId = "test";
    const auto matches = store.GetDataQuery(graph, query);
    const auto project = store.GetProjectData();
    return Check(
        commit.status == DataCommitStatus::Succeeded
            && matches.commitId == commit.commitId
            && matches.data.size() == 1
            && matches.data.front()->self == derivedRef
            && project.commitId == commit.commitId
            && project.bindings.size() == 1
            && project.bindings.front().name == primaryVolumeBinding
            && project.bindings.front().target == sourceRef,
        "facet/input query or project binding snapshot was inconsistent");
}

bool GetSnapshotStable()
{
    DataGraphStore store;
    const auto first = store.CreateDataEntityId();
    const auto second = store.CreateDataEntityId();
    DataTransaction initial;
    initial.outputs.push_back(BuildImageDraft(first, 0, 1.0f));
    store.SetDataCommit(std::move(initial));
    const auto oldGraph = store.GetDataGraph();
    DataTransaction next;
    next.outputs.push_back(BuildImageDraft(second, 0, 2.0f));
    store.SetDataCommit(std::move(next));
    return Check(
        oldGraph.commitId == 1
            && oldGraph.view->GetData(DataRevisionRef{ first, 1 })
            && !oldGraph.view->GetData(DataRevisionRef{ second, 1 })
            && store.GetDataGraph().commitId == 2,
        "old graph snapshot changed after a later commit");
}

bool GetPayloadFrozen()
{
    DataGraphStore store;
    const DataTypeId type{ "test.mutable", 1 };
    if (!store.SetDataType(DataTypeDescriptor{
            type,
            { DataFacetId{ "test" } },
            [](const IDataPayload& payload, std::string&) {
                return dynamic_cast<const MutablePayload*>(&payload) != nullptr;
            } })) {
        return Check(false, "custom type registration failed");
    }
    const auto entity = store.CreateDataEntityId();
    auto mutablePayload = std::make_shared<MutablePayload>(type, 7);
    DataTransaction transaction;
    transaction.outputs.push_back(DataRevisionDraft{
        entity, 0, type, {}, mutablePayload, std::nullopt });
    const auto result = store.SetDataCommit(std::move(transaction));
    mutablePayload->SetValue(99);
    const auto* frozen = result.published.empty()
        ? nullptr
        : dynamic_cast<const MutablePayload*>(
            result.published.front()->payload.get());
    return Check(
        frozen && frozen->GetValue() == 7,
        "published payload shares caller-mutable state");
}

bool GetNotificationValid()
{
    DataGraphStore store;
    std::vector<DataCommitId> observed;
    const auto throwing = store.AttachDataChange(
        [](const DataChangeSet&) { throw 7; });
    DataObserverId self = 0;
    self = store.AttachDataChange(
        [&store, &observed, &self](const DataChangeSet& change) {
            observed.push_back(change.commitId);
            if (self != 0) {
                store.DetachDataChange(self);
                self = 0;
            }
            const auto graph = store.GetDataGraph();
            if (graph.commitId != change.commitId) observed.clear();
        });
    const auto later = store.AttachDataChange(
        [&observed](const DataChangeSet& change) {
            observed.push_back(change.commitId);
        });
    const auto first = store.CreateDataEntityId();
    DataTransaction transaction;
    transaction.outputs.push_back(BuildImageDraft(first, 0, 1.0f));
    const auto result = store.SetDataCommit(std::move(transaction));
    return Check(
        throwing != 0 && later != 0
            && result.status == DataCommitStatus::Succeeded
            && observed == std::vector<DataCommitId>{ 1, 1 },
        "observer exception, detach, or committed-state reentry failed");
}

bool GetConcurrentCasValid()
{
    DataGraphStore store;
    const auto base = store.CreateDataEntityId();
    const auto first = store.CreateDataEntityId();
    const auto second = store.CreateDataEntityId();
    const DataRevisionRef baseRef{ base, 1 };
    DataTransaction setup;
    setup.outputs.push_back(BuildImageDraft(base, 0, 1.0f));
    setup.bindings.push_back(BuildBinding("active", 0, baseRef));
    store.SetDataCommit(std::move(setup));

    std::atomic<int> winners{ 0 };
    const auto commit = [&store, &winners, baseRef](const DataEntityId entity) {
        const DataRevisionRef ref{ entity, 1 };
        DataTransaction transaction;
        transaction.outputs.push_back(BuildImageDraft(entity, 0, 2.0f, {
            DataInputRef{ "source", baseRef } }));
        transaction.bindings.push_back(BuildBinding("active", 1, ref, baseRef));
        if (store.SetDataCommit(std::move(transaction)).status
            == DataCommitStatus::Succeeded) {
            ++winners;
        }
    };
    std::thread left(commit, first);
    std::thread right(commit, second);
    left.join();
    right.join();
    const auto graph = store.GetDataGraph();
    const auto binding = graph.view->GetDataBinding("active");
    return Check(
        winners.load() == 1 && graph.commitId == 2
            && binding && binding->revision == 2,
        "concurrent binding CAS did not have exactly one winner");
}

} // namespace

int main()
{
    const bool isSucceeded =
        GetBasicCommitValid()
        && GetGenerationValid()
        && GetTypeValidationValid()
        && GetOutputReferencesValid()
        && GetCycleRejected()
        && GetAtomicFailureValid()
        && GetBindingAbaValid()
        && GetHistoricalPolicyValid()
        && GetQueryAndProjectSnapshotValid()
        && GetSnapshotStable()
        && GetPayloadFrozen()
        && GetNotificationValid()
        && GetConcurrentCasValid();
    return isSucceeded ? 0 : 1;
}
