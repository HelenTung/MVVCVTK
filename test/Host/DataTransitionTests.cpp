#include "Host/LoadCommitCoordinator.h"
#include "App/Services/AppPorts.h"
#include "App/Services/AppServiceFactory.h"
#include "App/AppState.h"
#include "App/AppStateEvents.h"
#include <vtkPlaneSource.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
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

bool GetRealMeshTransitions()
{
    auto data=std::make_shared<RawVolumeDataManager>();auto image=BuildInput(data);if(!image)return false;
    auto plane=vtkSmartPointer<vtkPlaneSource>::New();plane->SetOrigin(-1,-1,-1);plane->SetPoint1(1,-1,0);plane->SetPoint2(-1,1,1);plane->Update();
    const auto entity=data->CreateDataEntityId();const DataRevisionRef meshRef{entity,1};
    DataTransaction create;create.outputs.push_back({entity,0,DataTypes::surfaceMesh,{},VtkPreparedDataView::BuildMeshPayload(plane->GetOutput()),{}});
    if(data->SetDataCommit(std::move(create)).status!=DataCommitStatus::Succeeded)return false;
    auto events=std::make_shared<SharedStateBroadcaster>();auto state=std::make_shared<SharedInteractionState>(events);
    std::vector<AppFactoryResult> views;std::vector<vtkSmartPointer<vtkRenderWindow>> windows;
    for(int i=0;i<2;++i) {
        AppServiceArgs args;args.dataManager=data;args.interactionState=state;args.eventSource=events;
        auto ports=CreateAppPorts(std::move(args));auto renderer=vtkSmartPointer<vtkRenderer>::New();
        auto window=vtkSmartPointer<vtkRenderWindow>::New();window->SetOffScreenRendering(1);window->SetSize(80,80);
        if(!ports.renderBind->SetRenderTarget(window,renderer))return false;
        AppViewUpdate update;update.mode=VizMode::SliceTop_down;
        if(!ports.app.view->SendViewUpdate(update)||!ports.interaction.update->SendUpdates())return false;
        views.push_back(std::move(ports));windows.push_back(std::move(window));
    }
    const auto graph=data->GetDataGraph();
    const auto mesh=VtkRenderInputView::FromMesh(graph,
        DataBinding{std::string(primaryVolumeBinding),meshRef,image->binding->revision+1},data->GetSurfaceMesh(graph,meshRef));
    if(!mesh||!mesh->mesh||mesh->image||mesh->imageView)return false;
    LoadCommitCoordinator coordinator(data);
    auto inconsistent=std::make_shared<VtkRenderInputView>(*mesh);inconsistent->data=image->data;
    if(!Check(!inconsistent->GetValid()&&views.front().dataStage->StartRenderInputStage(inconsistent,79)==DataStageStatus::Failed,
        "mesh stage accepted a different typed-view payload identity"))return false;
    // Existing image-only adapters must explicitly reject mesh candidates.
    LoadCommitRequest unsupported;unsupported.ownerId=5;unsupported.transactionRevision=80;unsupported.sourceRevision=meshRef;
    unsupported.renderInput=mesh;unsupported.stages={std::make_shared<StageProbe>()};unsupported.onPublish=[] {return true;};
    if(!Check(coordinator.SetLoadCommit(unsupported).status==LoadCommitStatus::Failed,"image-only stage accepted a mesh input"))return false;
    for(int failure=1;failure>=0;--failure) {
        LoadCommitRequest request;request.ownerId=5;request.transactionRevision=81+failure;request.sourceRevision=meshRef;
        request.renderInput=mesh;for(const auto& view:views)request.stages.push_back(view.dataStage);
        request.onPublish=[&] {
            for(const auto& view:views)if(view.featureView->GetRenderInputStamp()->dataRevision!=meshRef)return false;
            if(failure)return false;
            DataTransaction tx;tx.bindings.push_back({std::string(primaryVolumeBinding),image->binding->revision,true,image->data->self,meshRef});
            return data->SetDataCommit(std::move(tx)).status==DataCommitStatus::Succeeded;
        };
        if(coordinator.SetLoadCommit(request).status!=LoadCommitStatus::Preparing)return false;
        auto result=coordinator.SetLoadCommit(request);
        if(!Check(result.status==(failure?LoadCommitStatus::Failed:LoadCommitStatus::Succeeded),"real mesh candidate completion"))return false;
        for(const auto& view:views)if(!Check(view.featureView->GetRenderInputStamp()->dataRevision==(failure?image->data->self:meshRef),
            "real mesh view switch/rollback input identity"))return false;
    }
    for(const auto& window:windows)window->Render();
    for(auto& view:views) {
        AppViewUpdate mode;mode.mode=VizMode::Volume;
        if(!view.app.view->SendViewUpdate(mode)||!view.interaction.update->SendUpdates()
            ||view.featureView->GetRenderInputStamp()->dataRevision!=meshRef)return false;
    }
    auto restored=std::make_shared<VtkImageGridView>(*image);restored->binding->revision=mesh->binding->revision+1;
    LoadCommitRequest restore;restore.ownerId=5;restore.transactionRevision=84;restore.sourceRevision=image->data->self;
    restore.renderInput=VtkRenderInputView::FromImage(restored);for(const auto& view:views)restore.stages.push_back(view.dataStage);
    restore.onPublish=[&] {DataTransaction tx;tx.bindings.push_back({std::string(primaryVolumeBinding),mesh->binding->revision,true,meshRef,image->data->self});
        return data->SetDataCommit(std::move(tx)).status==DataCommitStatus::Succeeded;};
    if(coordinator.SetLoadCommit(restore).status!=LoadCommitStatus::Preparing)return false;
    auto result=coordinator.SetLoadCommit(restore);
    for(int tick=0;tick<1000&&result.status==LoadCommitStatus::Preparing;++tick) {
        for(auto& view:views)view.interaction.update->SendUpdates();
        result=coordinator.SetLoadCommit(restore);
    }
    const bool passed=result.status==LoadCommitStatus::Succeeded&&data->GetPrimaryImage()!=nullptr;
    for(auto& view:views)view.taskControl->StopTasks(std::chrono::steady_clock::now()+std::chrono::seconds(3));
    for(const auto& window:windows)window->Finalize();
    return Check(passed,"mesh-to-image restoration did not complete the real view transaction");
}

bool GetMaskedPickCoordinates()
{
    auto image=vtkSmartPointer<vtkImageData>::New();image->SetExtent(4,5,-2,-2,1,1);
    image->SetSpacing(2,3,4);image->SetOrigin(10,20,30);
    const double direction[9]={0,-1,0,1,0,0,0,0,1};image->SetDirectionMatrix(direction);
    image->AllocateScalars(VTK_UNSIGNED_CHAR,1);image->GetPointData()->GetScalars()->FillComponent(0,1);
    auto mask=vtkSmartPointer<vtkImageData>::New();mask->CopyStructure(image);mask->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    auto* bytes=static_cast<unsigned char*>(mask->GetScalarPointer());bytes[0]=0;bytes[1]=255;
    auto data=std::make_shared<RawVolumeDataManager>();const auto entity=data->CreateDataEntityId();const DataRevisionRef ref{entity,1};
    DataTransaction tx;tx.outputs.push_back({entity,0,DataTypes::imageGrid3D,{},VtkDataBridge{}.CreateImagePayload(image,mask),{}});
    tx.bindings.push_back({std::string(primaryVolumeBinding),0,true,{},ref});
    if(data->SetDataCommit(std::move(tx)).status!=DataCommitStatus::Succeeded)return false;
    auto events=std::make_shared<SharedStateBroadcaster>();auto state=std::make_shared<SharedInteractionState>(events);
    state->SetImageDataReady({ref,1,{1,1},{2,3,4},{16,30,34}});
    AppServiceArgs args;args.dataManager=data;args.interactionState=state;args.eventSource=events;
    auto ports=CreateAppPorts(std::move(args));auto renderer=vtkSmartPointer<vtkRenderer>::New();auto window=vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1);window->SetSize(64,64);
    AppViewUpdate view;view.mode=VizMode::SliceTop_down;
    if(!ports.renderBind->SetRenderTarget(window,renderer)||!ports.app.view->SendViewUpdate(view)||!ports.interaction.update->SendUpdates())return false;
    const auto& model=ports.interaction.model;
    if(!Check(!model->GetPointVisible({16,28,34})&&model->GetPointVisible({16,30,34})&&!model->GetPointVisible({16,100,34}),
        "picking did not honor canonical mask, direction, spacing and nonzero extent"))return false;
    const std::array<double,16> transform{-2,0.5,0,100,0,3,0,-40,0,0,1.5,7,0,0,0,1};
    if(!model->SetModelMatrix(transform))return false;
    if(!Check(model->GetPointVisible({16,30,34}),"pending transform changed picking before visual state applied"))return false;
    if(!ports.interaction.update->SendUpdates())return false;
    const bool passed=!model->GetPointVisible({82,44,58})&&model->GetPointVisible({83,50,58});
    ports.taskControl->StopTasks(std::chrono::steady_clock::now()+std::chrono::seconds(3));window->Finalize();
    return Check(passed,"reflected/sheared display picking did not invert the applied model transform");
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
    passed=GetRealMeshTransitions()&&passed;
    passed=GetMaskedPickCoordinates()&&passed;
    for(int failure=0;failure<=10;++failure)passed=GetTransitionCase(failure)&&passed;
    return passed;
}
