#include "Host/LoadCommitCoordinator.h"
#include "App/Services/AppPorts.h"
#include "Data/DataManager.h"
#include "Data/VtkDataBridge.h"

#include <vtkImageData.h>
#include <iostream>
#include <stdexcept>

namespace {
class StageProbe final : public AppDataStagePort {
public:
    DataStageStatus StartDataStage(const VtkImageGridSnapshot&, std::uint64_t) override
    {
        ++starts;
        if (onStart) onStart();
        if (throwStart) throw std::runtime_error("start failure");
        return failStart ? DataStageStatus::Failed : DataStageStatus::Preparing;
    }
    DataStageStatus SetDataStageReady(const VtkImageGridSnapshot&, std::uint64_t) override
    {
        if (throwReady) throw std::runtime_error("ready failure");
        return failReady ? DataStageStatus::Failed : DataStageStatus::Ready;
    }
    DataStageStatus GetDataStageStatus(std::uint64_t) const override { return DataStageStatus::Ready; }
    bool SetViewStage(const VtkImageGridSnapshot&, std::uint64_t) override
    {
        if (throwCommit) { committed=true; throw std::runtime_error("commit failure after switch"); }
        if (failCommit) return false;
        committed=true; return true;
    }
    bool ResetViewStage(std::uint64_t) override { committed=false; ++resets; return true; }
    bool ClearDataStage(std::uint64_t) override { ++clears; return true; }
    void SetDataStageComplete(std::uint64_t) noexcept override { ++completes; }
    bool failStart=false,failReady=false,failCommit=false,committed=false;
    int starts=0,resets=0,clears=0,completes=0;
    bool throwStart=false,throwReady=false,throwCommit=false;
    std::function<void()> onStart;
};

bool Check(bool condition,const char* name)
{
    if (!condition) std::cerr<<"Data transition: "<<name<<'\n';
    return condition;
}

VtkImageGridSnapshot BuildInput(const std::shared_ptr<BaseDataManager>& data)
{
    auto image=vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2,1,1);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    auto* values=static_cast<unsigned char*>(image->GetScalarPointer()); values[0]=3;values[1]=9;
    VtkDataBridge bridge;
    const auto id=data->CreateDataEntityId();
    const DataRevisionRef ref{id,1};
    DataTransaction transaction;
    transaction.outputs.push_back({id,0,DataTypes::imageGrid3D,{},bridge.CreateImagePayload(image),{}});
    transaction.bindings.push_back({std::string(primaryVolumeBinding),0,true,{},ref});
    if(data->SetDataCommit(std::move(transaction)).status!=DataCommitStatus::Succeeded)return {};
    return data->GetPrimaryImage();
}

bool GetLoadedSnapshotIdentity()
{
    auto data=std::make_shared<RawVolumeDataManager>();
    auto image=vtkSmartPointer<vtkImageData>::New();image->SetDimensions(2,1,1);image->AllocateScalars(VTK_FLOAT,1);
    auto* values=static_cast<float*>(image->GetScalarPointer());values[0]=1;values[1]=3;
    ImageMetadata metadata;
    metadata.identity.datasetId="loaded-identity";
    metadata.source.kind=ImageSourceKind::Memory;
    metadata.source.uri="memory://loaded-identity";
    if(!Check(data->SetImageSnapshot(image,metadata),"loaded image stage"))return false;
    const auto stage=data->GetLoadStage();VtkImageGridSnapshot published;
    if(!data->SetLoadCommit(stage,published)||!published)return false;
    const auto graph=data->GetDataGraph();const auto canonical=data->GetData(graph,published->data->self);
    const auto read=data->GetPrimaryImage(),byRef=data->GetImageGrid(graph,published->data->self);
    return Check(canonical&&read&&byRef&&read->data.get()==canonical.get()&&byRef->data.get()==canonical.get()
        &&read->image==published->image&&byRef->image==published->image,
        "loaded VTK cache leaked the pre-publication data identity or rebuilt the volume");
}

bool GetTransitionCase(int failure)
{
    auto data=std::make_shared<RawVolumeDataManager>();
    auto input=BuildInput(data);if(!input)return false;
    LoadCommitCoordinator coordinator(data);
    auto first=std::make_shared<StageProbe>(),second=std::make_shared<StageProbe>();
    second->failStart=failure==1;second->failReady=failure==2;second->failCommit=failure==3;
    second->throwStart=failure==7;second->throwReady=failure==8;second->throwCommit=failure==9;
    int publications=0,notifications=0;bool featureCommitted=false,observedComplete=true;
    data->AttachDataChange([&](const DataChangeSet&) {
        ++notifications;
        observedComplete=observedComplete&&featureCommitted&&first->completes==1&&second->completes==1;
    });
    LoadCommitRequest request;
    request.ownerId=8;request.transactionRevision=1;request.sourceRevision=input->data->self;
    request.pending=input;request.stages={first,second};
    request.onPublish=[&] {
        ++publications;
        if (failure==10) throw std::runtime_error("publish failure");
        if (!Check(coordinator.SetLoadCommit(request).status == LoadCommitStatus::Failed
            && coordinator.SetLoadCancelled(1,LoadCommitFailure::Cancelled,8).status == LoadCommitStatus::Failed,
            "publication allowed recursive advancement/cancellation")) return false;
        if (!first->committed||!second->committed||failure==4) return false;
        DataTransaction transaction;
        transaction.bindings.push_back({"transition.test",0,true,{},input->data->self});
        if(data->SetDataCommit(std::move(transaction)).status!=DataCommitStatus::Succeeded)return false;
        featureCommitted=true;
        return true;
    };
    bool reentryRejected=true;
    first->onStart=[&] {
        reentryRejected=coordinator.SetLoadCommit(request).status==LoadCommitStatus::Failed
            &&coordinator.SetLoadCancelled(1,LoadCommitFailure::Cancelled,8).status==LoadCommitStatus::Failed;
    };
    auto result=coordinator.SetLoadCommit(request);
    if(!Check(reentryRejected,"stage allowed reentrant progress"))return false;
    if(failure==7)return Check(result.status==LoadCommitStatus::Failed&&!coordinator.GetIsPending()
        &&first->clears==1&&second->clears==1,"throwing start leaked candidates");
    if(failure==1)return Check(result.status==LoadCommitStatus::Failed&&publications==0&&first->clears==1,"start failure leaked earlier View stage");
    if(!Check(result.status==LoadCommitStatus::Preparing&&publications==0,"Start published before readiness"))return false;
    if(failure==5) {
        result=coordinator.SetLoadCancelled(1,LoadCommitFailure::Cancelled,8);
        return Check(result.status==LoadCommitStatus::Cancelled&&!coordinator.GetIsPending()
            &&first->clears==1&&second->clears==1&&publications==0,"cancel stage cleanup");
    }
    if(failure==6) {
        auto foreign=request;foreign.ownerId=9;
        const auto rejected=coordinator.SetLoadCommit(foreign);
        if(!Check(rejected.status==LoadCommitStatus::Failed&&coordinator.GetIsPending(),"another Feature stole active transition"))return false;
    }
    {
        auto batch=data->StartDataChanges();
        result=coordinator.SetLoadCommit(request);
        if(!Check(notifications==0,"graph observers escaped the participant/View commit boundary"))return false;
    }
    if(failure>=8)return Check(result.status==LoadCommitStatus::Failed&&!coordinator.GetIsPending()
        &&!first->committed&&!second->committed&&first->clears==1&&second->clears==1&&notifications==0,
        "throwing stage/publication did not rollback completely");
    if(failure==2)return Check(result.status==LoadCommitStatus::Failed&&publications==0
        &&first->clears==1&&second->clears==1,"ready failure changed graph");
    if(failure==3)return Check(result.status==LoadCommitStatus::Failed&&publications==0
        &&first->resets==1&&!first->committed,"partial View commit did not rollback");
    if(failure==4)return Check(result.status==LoadCommitStatus::Failed&&publications==1
        &&first->resets==1&&second->resets==1&&notifications==0&&!featureCommitted,"publish failure did not rollback Views");
    return Check(result.status==LoadCommitStatus::Succeeded&&publications==1&&notifications==1
        &&observedComplete&&!coordinator.GetIsPending(),"commit/notification order");
}
}

bool GetDataTransitionTests()
{
    bool passed=GetLoadedSnapshotIdentity();
    for(int failure=0;failure<=10;++failure)passed=GetTransitionCase(failure)&&passed;
    return passed;
}
