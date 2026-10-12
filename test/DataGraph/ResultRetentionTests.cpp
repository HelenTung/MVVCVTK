// 测试用途：验证重复发布的真实 allocation 释放、显式保留和跨业务依赖保护。
#include "Data/DataGraphStore.h"
#include "Data/DataPayloads.h"
#include "../../MVVCVTK/features/common/FeatureInputReads.h"
#include <iostream>
#include <stdexcept>

namespace {
bool Check(bool value, const char* message)
{
    if (!value) std::cerr << "Retention: " << message << '\n';
    return value;
}
struct Output {
    DataRevisionRef ref;
    DataEntityId scope;
    std::weak_ptr<const ImageScalarStorage> allocation;
};
Output Publish(DataGraphStore& store, DataRevisionRef input = {}, const char* operation = "correct")
{
    GridGeometry3D geometry; geometry.dimensions = {2,1,1}; geometry.extent = {0,1,0,0,0,0};
    auto values = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{10,20});
    const auto image=std::make_shared<const ImageGrid3DPayload>(geometry,ImageValueType::UInt8,1,values);
    Output output{{store.CreateDataEntityId(),1},store.CreateDataEntityId(),image->GetValues()};
    DataRevisionDraft draft{output.ref.entityId,0,DataTypes::imageGrid3D,{},
        image,
        DataProvenance{"retention-test",operation,"1","{}"}};
    if (GetDataRevisionRefValid(input)) draft.inputs.push_back({"source",input});
    draft.lifetimeScope = output.scope; draft.lifetimePolicy = DataLifetimePolicy::ExplicitRetirement;
    DataTransaction transaction; transaction.outputs.push_back(std::move(draft));
    if (store.SetDataCommit(std::move(transaction)).status != DataCommitStatus::Succeeded)
        throw std::runtime_error("publish failed");
    return output;
}
bool Bind(DataGraphStore& store, const std::string& name, std::optional<DataRevisionRef> target)
{
    const auto previous = store.GetDataBinding(store.GetDataGraph(),name);
    DataTransaction transaction;
    transaction.bindings.push_back({name,previous ? previous->revision : 0,true,
        previous ? previous->target : std::optional<DataRevisionRef>{},target});
    return store.SetDataCommit(std::move(transaction)).status == DataCommitStatus::Succeeded;
}
bool GetRepeatedAllocationsReleased()
{
    DataGraphStore store;
    const auto source = Publish(store);
    auto current = Publish(store,source.ref);
    const auto graph = store.GetDataGraph();
    for (int iteration=0; iteration<32; ++iteration) {
        const auto previous = current;
        current = Publish(store,source.ref);
        if (!Check(previous.allocation.expired() && !store.GetData(graph,previous.ref),
            "replaced output remains owned by graph or old graph snapshot")) return false;
    }
    return Check(!source.allocation.expired() && !current.allocation.expired()
        && store.GetDataQuery(store.GetDataGraph(),{}).data.size()==2,
        "only original input and current result should remain readable");
}
bool GetKeepAndDependenciesProtected()
{
    DataGraphStore store; const auto source=Publish(store);
    const auto first=Publish(store,source.ref);
    if (!Bind(store,"retained",first.ref)) return false;
    const auto second=Publish(store,source.ref);
    if (!Check(!first.allocation.expired() && store.GetData(store.GetDataGraph(),first.ref),"explicit keep lost")) return false;
    const auto consumer=Publish(store,first.ref,"measure");
    if (!Bind(store,"retained",{})) return false;
    if (!Check(!first.allocation.expired(),"current downstream result lost its source")) return false;
    const auto nextConsumer=Publish(store,second.ref,"measure");
    return Check(first.allocation.expired() && consumer.allocation.expired()
        && !second.allocation.expired() && !nextConsumer.allocation.expired(),
        "replacing consumer did not release obsolete dependency chain");
}
bool GetReadersAndSnapshotsProtected()
{
    DataGraphStore store; const auto source=Publish(store); const auto first=Publish(store,source.ref);
    auto snapshot=store.GetData(store.GetDataGraph(),first.ref);
    auto readers=FeatureInternal::StartInputReads(store.GetDataGraph(),std::vector<DataRevisionRef>{first.ref},"reader");
    if (!Check(readers.has_value(),"input Reader lease acquired")) return false;
    const auto second=Publish(store,source.ref);
    if (!Check(store.GetData(store.GetDataGraph(),first.ref)!=nullptr,"active reader was retired")) return false;
    readers.reset();
    if (!Bind(store,"current",second.ref)) return false;
    if (!Check(!store.GetData(store.GetDataGraph(),first.ref) && snapshot->payload
        && !first.allocation.expired(),"legacy snapshot was invalidated or retired revision revived")) return false;
    snapshot.reset();
    return Check(first.allocation.expired() && store.SetDataRelease(first.scope).status==DataLifetimeStatus::Released,
        "legacy allocation was not released after last owner");
}
bool GetFailedPublicationAtomic()
{
    DataGraphStore store; const auto source=Publish(store); const auto first=Publish(store,source.ref);
    const auto snapshot=store.GetData(store.GetDataGraph(),first.ref); const auto before=store.GetDataGraph().commitId;
    DataTransaction transaction;
    DataRevisionDraft bad{store.CreateDataEntityId(),1,snapshot->type,snapshot->inputs,snapshot->payload,snapshot->provenance};
    bad.lifetimeScope=store.CreateDataEntityId(); bad.lifetimePolicy=DataLifetimePolicy::ExplicitRetirement;
    transaction.outputs.push_back(std::move(bad));
    return Check(store.SetDataCommit(std::move(transaction)).status==DataCommitStatus::Rejected
        && store.GetDataGraph().commitId==before && store.GetData(store.GetDataGraph(),first.ref)
        && !first.allocation.expired(),"failed publication retired current output");
}
bool GetBatchAndFinalReaderReleased()
{
    DataGraphStore store; const auto source=Publish(store);
    const auto sourceImage=std::dynamic_pointer_cast<const ImageGrid3DPayload>(store.GetData(store.GetDataGraph(),source.ref)->payload);
    Output outputs[2]; DataTransaction batch;
    for (auto& output : outputs) {
        auto image=sourceImage->CreateFloatSnapshot(std::make_unique<std::vector<float>>(2,3.0f),{3,3},{});
        output={{store.CreateDataEntityId(),1},store.CreateDataEntityId(),image->GetValues()};
        DataRevisionDraft draft{output.ref.entityId,0,DataTypes::imageGrid3D,{{"source",source.ref}},image,
            DataProvenance{"retention-test","correct","1","{}"}};
        draft.lifetimeScope=output.scope; draft.lifetimePolicy=DataLifetimePolicy::ExplicitRetirement;
        batch.outputs.push_back(std::move(draft));
    }
    if (!Check(store.SetDataCommit(std::move(batch)).status==DataCommitStatus::Succeeded
        && Bind(store,"source",source.ref),"batch results published")) return false;
    for (const auto& output : outputs)
        if (!Check(!output.allocation.expired(),"one member of a current batch was retired")) return false;
    auto snapshot=store.GetData(store.GetDataGraph(),outputs[0].ref);
    auto reader=snapshot->lifetime.lock()->StartResourceUse(outputs[0].ref,"last-reader");
    const auto current=Publish(store,source.ref);
    if (!Check(outputs[1].allocation.expired() && !outputs[0].allocation.expired(),"batch replacement ignored reader protection")) return false;
    DataTransaction unload;
    unload.retireScopes.push_back({current.scope,DataLifetimeStatus::Published,{current.ref},true});
    if (!Check(store.SetDataCommit(std::move(unload)).status==DataCommitStatus::Succeeded,"unload last current result")) return false;
    reader.reset(); snapshot.reset();
    return Check(Bind(store,"source",source.ref) && outputs[0].allocation.expired(),
        "last released reader leaves an obsolete allocation after current result unload");
}
}
bool GetResultRetentionTests()
{
    return GetRepeatedAllocationsReleased() && GetKeepAndDependenciesProtected()
        && GetReadersAndSnapshotsProtected() && GetFailedPublicationAtomic() && GetBatchAndFinalReaderReleased();
}
