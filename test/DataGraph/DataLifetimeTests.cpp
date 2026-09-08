#include "Data/DataGraphStore.h"
#include "Data/DataPayloads.h"

#include <iostream>
#include <thread>

namespace {
bool Check(bool value, const char* message)
{
    if (!value) std::cerr << "Lifetime: " << message << '\n';
    return value;
}

std::shared_ptr<const ImageGrid3DPayload> BuildImage()
{
    GridGeometry3D geometry;
    geometry.extent = {0, 1, 0, 0, 0, 0};
    geometry.dimensions = {2, 1, 1};
    return std::make_shared<const ImageGrid3DPayload>(geometry, ImageValueType::UInt8,
        1, std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{10, 20}));
}

DataTransaction BuildRetirement(DataEntityId scope, DataRevisionRef ref,
    DataBindingRevision bindingRevision = 1)
{
    DataTransaction transaction;
    transaction.retireScopes.push_back({scope, DataLifetimeStatus::Published, {ref}});
    transaction.bindings.push_back({"result", bindingRevision, true, ref, {}});
    return transaction;
}

bool GetAllocationReleased()
{
    DataGraphStore store;
    const auto sourceId=store.CreateDataEntityId();
    const DataRevisionRef sourceRef{sourceId,1};
    DataTransaction source;
    source.outputs.push_back({sourceId,0,DataTypes::imageGrid3D,{},BuildImage(),{}});
    auto sourceResult=store.SetDataCommit(std::move(source));
    const auto original=std::dynamic_pointer_cast<const ImageGrid3DPayload>(sourceResult.published.at(0)->payload);
    auto candidate=original->CreateMaskSnapshot(std::vector<std::uint8_t>{1,0});
    DataBytes heldMask=candidate->GetValidityMask();
    const auto scope=store.CreateDataEntityId();
    const auto outputId=store.CreateDataEntityId();
    const DataRevisionRef outputRef{outputId,1};
    DataTransaction output;
    output.outputs.push_back({outputId,0,DataTypes::imageGrid3D,{{"source",sourceRef}},candidate,{},scope});
    output.bindings.push_back({"result",0,false,{},outputRef});
    auto published=store.SetDataCommit(std::move(output));
    if (!Check(published.status==DataCommitStatus::Succeeded,"publish scoped image")) return false;
    auto oldGraph=published.graph;
    auto oldSnapshot=oldGraph.view->GetData(outputRef);
    candidate.reset();
    published={};
    if (!Check(store.SetDataCommit(BuildRetirement(scope,outputRef)).status==DataCommitStatus::Succeeded,
        "retire with legacy owning snapshot")) return false;
    if (!Check(!oldGraph.view->GetData(outputRef),"old graph regained retired payload")) return false;
    if (!Check(oldSnapshot && oldSnapshot->payload,"issued snapshot was mutated")) return false;
    if (!Check(store.SetDataRelease(scope).status==DataLifetimeStatus::Releasing,"live snapshot falsely released")) return false;
    oldSnapshot.reset();
    if (!Check(store.SetDataRelease(scope).status==DataLifetimeStatus::Releasing,"nested shared bytes falsely released")) return false;
    heldMask.reset();
    if (!Check(store.SetDataRelease(scope).status==DataLifetimeStatus::Released,"old metadata or shared Root scalar retained result")) return false;
    if (!Check(oldGraph.view->GetData(sourceRef)!=nullptr,"retirement damaged persistent Root")) return false;
    DataTransaction reactivate;
    reactivate.policy=DataPublishPolicy::AllowHistoricalResult;
    reactivate.bindings.push_back({"other",0,false,{},outputRef});
    if (!Check(store.SetDataCommit(std::move(reactivate)).failureReason==DataCommitFailure::ResultRetired,
        "retired binding activation not rejected")) return false;
    DataTransaction derive;
    derive.outputs.push_back({store.CreateDataEntityId(),0,DataTypes::imageGrid3D,{{"old",outputRef}},BuildImage(),{}});
    if (!Check(store.SetDataCommit(std::move(derive)).failureReason==DataCommitFailure::ResultRetired,
        "retired derived input not rejected")) return false;
    const auto staleBinding=store.SetDataCommit(BuildRetirement(scope,outputRef,2));
    if (!Check(staleBinding.failureReason==DataCommitFailure::ExpectationFailed,
        "stale binding expectation was ignored")) return false;
    DataTransaction replay;
    replay.retireScopes.push_back({scope,DataLifetimeStatus::Published,{outputRef}});
    return Check(store.SetDataCommit(std::move(replay)).failureReason==DataCommitFailure::ResultRetired,
        "retirement replay revived result");
}

bool GetDependenciesProtected()
{
    DataGraphStore store;
    const auto scope=store.CreateDataEntityId();
    const auto id=store.CreateDataEntityId();
    const DataRevisionRef ref{id,1};
    DataTransaction create;
    create.outputs.push_back({id,0,DataTypes::imageGrid3D,{},BuildImage(),{},scope});
    create.bindings.push_back({"result",0,false,{},ref});
    auto result=store.SetDataCommit(std::move(create));
    auto snapshot=result.published.at(0);
    auto lease=snapshot->lifetime.lock()->StartResourceUse(ref,"ExportTask");
    const auto before=store.GetDataGraph().commitId;
    const auto rejected=store.SetDataCommit(BuildRetirement(scope,ref));
    if (!Check(rejected.failureReason==DataCommitFailure::ResultInUse && rejected.blockers.size()==1
        && rejected.blockers[0].owner=="ExportTask" && store.GetDataGraph().commitId==before,
        "lease retirement rejection not atomic")) return false;
    lease.reset();
    const auto dependencyId=store.CreateDataEntityId();
    DataTransaction derive;
    derive.outputs.push_back({dependencyId,0,DataTypes::imageGrid3D,{{"input",ref}},BuildImage(),{}});
    if (!Check(store.SetDataCommit(std::move(derive)).status==DataCommitStatus::Succeeded,"publish dependency")) return false;
    const auto blocked=store.SetDataCommit(BuildRetirement(scope,ref));
    return Check(blocked.failureReason==DataCommitFailure::ResultInUse && !blocked.blockers.empty()
        && store.GetDataLifetime(scope).status==DataLifetimeStatus::Published,
        "downstream dependency was silently broken");
}

bool GetScopeClosureValid()
{
    DataGraphStore store;
    const auto scope=store.CreateDataEntityId();
    const auto first=store.CreateDataEntityId(), second=store.CreateDataEntityId();
    const DataRevisionRef a{first,1},b{second,1};
    DataTransaction transaction;
    transaction.outputs.push_back({first,0,DataTypes::imageGrid3D,{},BuildImage(),{},scope});
    transaction.outputs.push_back({second,0,DataTypes::imageGrid3D,{{"input",a}},BuildImage(),{},scope});
    auto commit=store.SetDataCommit(std::move(transaction));
    if (!Check(commit.status==DataCommitStatus::Succeeded,"multi-output scope")) return false;
    DataTransaction incomplete;
    incomplete.retireScopes.push_back({scope,DataLifetimeStatus::Published,{a}});
    if (!Check(store.SetDataCommit(std::move(incomplete)).failureReason==DataCommitFailure::ExpectationFailed,
        "partial scope expectation accepted")) return false;
    commit={};
    // 另一个 scope 不在退役锁集合中，必须保持可读且不能被误当成同一把锁。
    const auto otherScope=store.CreateDataEntityId();
    const auto otherId=store.CreateDataEntityId();
    DataTransaction other;
    other.outputs.push_back({otherId,0,DataTypes::imageGrid3D,{},BuildImage(),{},otherScope});
    if (!Check(store.SetDataCommit(std::move(other)).status==DataCommitStatus::Succeeded,"second scope")) return false;
    DataTransaction complete;
    complete.retireScopes.push_back({scope,DataLifetimeStatus::Published,{b,a}});
    if (!Check(store.SetDataCommit(std::move(complete)).status==DataCommitStatus::Succeeded,"internal scope edges blocked retirement")) return false;
    return Check(store.SetDataRelease(scope).status==DataLifetimeStatus::Released,"scope closure did not release");
}

bool GetBatchNotificationsValid()
{
    DataGraphStore store;
    int notifications=0;
    bool featureReady=false, observedReady=true;
    store.AttachDataChange([&](const DataChangeSet&) {
        ++notifications;
        observedReady=observedReady && featureReady;
        (void)store.GetDataGraph();
    });
    auto outer=store.StartDataChanges();
    auto inner=store.StartDataChanges();
    bool foreignRejected=false;
    std::thread other([&] { foreignRejected=!store.StartDataChanges(); });
    other.join();
    DataTransaction create;
    create.outputs.push_back({store.CreateDataEntityId(),0,DataTypes::imageGrid3D,{},BuildImage(),{}});
    const auto commit=store.SetDataCommit(std::move(create));
    inner.reset();
    if (!Check(commit.status==DataCommitStatus::Succeeded && notifications==0 && foreignRejected,
        "batch notification escaped before Feature state commit")) return false;
    featureReady=true;
    outer.reset();
    return Check(notifications==1 && observedReady,"batch did not deliver committed state exactly once");
}
}

bool GetDataLifetimeTests()
{
    return GetAllocationReleased() && GetDependenciesProtected()
        && GetScopeClosureValid() && GetBatchNotificationsValid();
}
