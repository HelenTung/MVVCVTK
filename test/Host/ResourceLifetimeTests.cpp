#include "Data/DataManager.h"
#include "Data/DataPayloads.h"
#include "Data/VtkDataBridge.h"
#include "App/Tasks/AppDataExportTaskService.h"
#include "App/Services/AppServiceFactory.h"
#include "Host/Internal/HostImageReadRuntime.h"
#include "Render/Internal/VolumeLodProductBuilder.h"
#include "Render/Internal/IsoSurfaceProductBuilder.h"

#include <vtkImageData.h>
#include <vtkDataArray.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace {
bool Check(bool value,const char* text)
{
    if (!value) std::cerr<<"Resource lifetime: "<<text<<'\n';
    return value;
}

struct Source final { DataRevisionRef ref; DataEntityId scope; };
Source Publish(const std::shared_ptr<BaseDataManager>& data,bool scoped,float offset=0)
{
    auto image=vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4,4,4);image->AllocateScalars(VTK_FLOAT,1);
    auto* values=static_cast<float*>(image->GetScalarPointer());
    for(int z=0;z<4;++z)for(int y=0;y<4;++y)for(int x=0;x<4;++x)
        values[(z*4+y)*4+x]=offset+x+y+z;
    VtkDataBridge bridge;
    Source source{{data->CreateDataEntityId(),1},scoped?data->CreateDataEntityId():DataEntityId{}};
    DataTransaction create;
    create.outputs.push_back({source.ref.entityId,0,DataTypes::imageGrid3D,{},bridge.CreateImagePayload(image),{},
        scoped?std::optional<DataEntityId>{source.scope}:std::nullopt});
    if(data->SetDataCommit(std::move(create)).status!=DataCommitStatus::Succeeded)return {};
    return source;
}
bool SetPrimary(const std::shared_ptr<BaseDataManager>& data,const Source& source)
{
    const auto current=data->GetDataBinding(data->GetDataGraph(),primaryVolumeBinding);
    DataTransaction transaction;
    transaction.bindings.push_back({std::string(primaryVolumeBinding),current?current->revision:0,true,
        current?current->target:std::nullopt,source.ref});
    return data->SetDataCommit(std::move(transaction)).status==DataCommitStatus::Succeeded;
}
DataCommitResult Retire(const std::shared_ptr<BaseDataManager>& data,const Source& source)
{
    DataTransaction transaction;
    transaction.retireScopes.push_back({source.scope,DataLifetimeStatus::Published,{source.ref},true});
    return data->SetDataCommit(std::move(transaction));
}

class ExportProbe final : public RawVolumeDataManager {
public:
    DataRevisionRef exported,sliced;
    bool ExportData(const VtkImageGridSnapshot& image,const std::string&,
        const DataExportParams&,const TaskStopToken&) override { exported=image->data->self;return true; }
    bool ExportSlices(const VtkImageGridSnapshot& image,const std::string&,Orientation,
        const WindowLevelParams&,const std::array<double,16>&,const TaskStopToken&) override
        { sliced=image->data->self;return true; }
};

bool GetQueuedExportsProtected()
{
    auto data=std::make_shared<ExportProbe>();
    const auto result=Publish(data,true),next=Publish(data,false,100);
    if(!SetPrimary(data,result))return false;
    auto state=std::make_shared<SharedInteractionState>();
    auto view=std::make_shared<ViewPresentationState>();
    AppDataExportTaskService service(data,state,view);
    auto exportTask=service.BuildDataTask("unused",".raw");
    auto slicesTask=service.BuildSlicesTask("unused",{},VizMode::SliceTop_down);
    if(!Check(exportTask&&slicesTask&&SetPrimary(data,next),"export task admission"))return false;
    const auto blocked=Retire(data,result);
    if(!Check(blocked.failureReason==DataCommitFailure::ResultInUse
        &&blocked.blockers.size()==2,"queued exports did not block retirement"))return false;
    auto exportDone=exportTask->get_future(),slicesDone=slicesTask->get_future();
    (*exportTask)(TaskStopToken{});(*slicesTask)(TaskStopToken{});
    if(!Check(exportDone.get()&&slicesDone.get()&&data->exported==result.ref&&data->sliced==result.ref,
        "queued exports followed a later primary input"))return false;
    // callable 与 future 仍在作用域内，工作结束必须已经归还源占用。
    if(!Check(Retire(data,result).status==DataCommitStatus::Succeeded,"completed exports retained Reader leases"))return false;
    return Check(data->SetDataRelease(result.scope).status==DataLifetimeStatus::Released,
        "completed export callable retained source resources");
}

bool GetFrozenReadsProtected()
{
    auto data=std::make_shared<RawVolumeDataManager>();
    const auto source=Publish(data,true,20),next=Publish(data,false,100);
    if(!SetPrimary(data,source))return false;
    auto snapshot=data->GetPrimaryImage();
    if(!SetPrimary(data,next))return false;
    const auto copied=data->GetImageReadResult(snapshot,ImageReadRequest{},TaskStopToken{});
    if(!Check(copied.error==ImageReadError::None&&copied.state&&copied.state->dataRevision==source.ref,
        "frozen synchronous read changed source"))return false;
    if(!Check(Retire(data,source).status==DataCommitStatus::Succeeded,"finished synchronous read retained lease"))return false;
    if(!Check(data->GetImageReadResult(snapshot,ImageReadRequest{},TaskStopToken{}).error==ImageReadError::ResultRetired,
        "old snapshot reopened a retired source"))return false;
    if(!Check(data->SetDataRelease(source.scope).status==DataLifetimeStatus::Releasing,
        "owning VTK snapshot was not tracked"))return false;
    snapshot.reset();
    return Check(data->SetDataRelease(source.scope).status==DataLifetimeStatus::Released,
        "copied image read bytes retained source allocation");
}

bool GetQueuedReadProtected()
{
    auto data=std::make_shared<RawVolumeDataManager>();
    const auto source=Publish(data,true,20),next=Publish(data,false,100);
    if(!SetPrimary(data,source))return false;
    std::promise<void> gate;const auto go=gate.get_future().share();
    auto executor=CreateAppTaskExecutor([go](AppWorkerWork work) {
        return std::thread([go,work=std::move(work)] { go.wait();work(); });
    });
    HostImageReadRuntime reader;
    std::promise<ImageReadResult> complete;auto done=complete.get_future();
    const auto admission=reader.StartImageRead(data,executor,{},[&](ImageReadResult value) {complete.set_value(std::move(value));});
    const bool switched=SetPrimary(data,next);
    const auto blocked=Retire(data,source);
    gate.set_value();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(done.wait_for(std::chrono::milliseconds(1))!=std::future_status::ready
        &&std::chrono::steady_clock::now()<deadline)reader.SendComplete(false);
    if(!Check(admission==ImageReadAdmission::Accepted&&switched
        &&blocked.failureReason==DataCommitFailure::ResultInUse,"queued image reader not protected"))return false;
    if(!Check(done.wait_for(std::chrono::seconds(0))==std::future_status::ready,"image read completion missing"))return false;
    const auto value=done.get();
    if(!Check(value.error==ImageReadError::None&&value.state&&value.state->dataRevision==source.ref,
        "queued image read followed a later primary"))return false;
    if(!Check(Retire(data,source).status==DataCommitStatus::Succeeded,"finished image read retained lease"))return false;
    return Check(data->SetDataRelease(source.scope).status==DataLifetimeStatus::Released,"image reader source not released");
}

bool GetDerivedProductsProtected()
{
    auto data=std::make_shared<RawVolumeDataManager>();
    const auto source=Publish(data,true);
    RenderResourceCoordinator resources([](RenderLaneWork) {return false;},data);
    auto input=data->GetImageGrid(data->GetDataGraph(),source.ref);
    auto inputUse=resources.StartDataUse({source.ref});
    if(!Check(input&&inputUse,"scoped render input admission"))return false;
    VolumeLodBuildRequest volume;
    volume.inputUse=*inputUse;volume.input=input->image;volume.requestRevision=1;
    volume.key.inputStamp={source.ref};volume.key.inputIdentity=input->image;
    volume.key.outputDimensions={2,2,2};volume.requestedQuality=VolumeQuality::Low;
    IsoSurfaceBuildRequest iso;
    iso.inputUse=*inputUse;iso.input=input->image;iso.requestRevision=2;
    iso.key.inputStamp={source.ref};iso.key.inputIdentity=input->image;
    iso.key.outputDimensions={4,4,4};iso.key.isoValue=4.5;
    auto builtVolume=VolumeLodProductBuilder().BuildProduct(volume,RenderTaskToken{});
    auto builtIso=IsoSurfaceProductBuilder().BuildProduct(iso,RenderTaskToken{});
    if(!Check(builtVolume.product&&builtIso.product&&builtIso.product->surface->GetNumberOfPoints()>0,
        "derived resource builders"))return false;
    if(!Check(resources.SetVolumeProduct(volume.key,builtVolume.product)
        &&resources.SetIsoSurfaceProduct(iso.key,builtIso.product),"derived cache insertion"))return false;
    vtkSmartPointer<vtkDataArray> volumeArray=builtVolume.product->volume->GetPointData()->GetScalars();
    vtkSmartPointer<vtkDataArray> meshPoints=builtIso.product->surface->GetPoints()->GetData();
    const auto volumeKey=volume.key;const auto isoKey=iso.key;
    // 模拟批处理通知尚未派发：即使缓存还在，退役后也不能重新读出。
    {
        auto batch=data->StartDataChanges();
        if(!Check(Retire(data,source).status==DataCommitStatus::Succeeded,"prepared render transition retirement"))return false;
        if(!Check(!resources.GetVolumeProduct(volumeKey)&&!resources.GetIsoSurfaceProduct(isoKey)
            &&!resources.SetVolumeProduct(volumeKey,builtVolume.product)
            &&!resources.SetIsoSurfaceProduct(isoKey,builtIso.product)
            &&!resources.StartDataUse({source.ref}),"retired cache/late product reactivation"))return false;
    }
    if(!Check(resources.GetResourceState().cacheBytes==0,"retirement did not evict derived caches"))return false;
    if(!Check(VolumeLodProductBuilder().BuildProduct(volume,RenderTaskToken{}).failureReason==RenderProductFailure::StaleInput
        &&IsoSurfaceProductBuilder().BuildProduct(iso,RenderTaskToken{}).failureReason==RenderProductFailure::StaleInput,
        "queued retired source still built a product"))return false;
    input.reset();inputUse.reset();volume={};iso={};builtVolume={};builtIso={};
    if(!Check(data->SetDataRelease(source.scope).status==DataLifetimeStatus::Releasing,
        "standalone derived VTK arrays were not retained"))return false;
    volumeArray=nullptr;
    if(!Check(data->SetDataRelease(source.scope).status==DataLifetimeStatus::Releasing,
        "iso points escaped resource accounting"))return false;
    meshPoints=nullptr;
    return Check(data->SetDataRelease(source.scope).status==DataLifetimeStatus::Released,
        "last derived allocation release not acknowledged");
}
}

bool GetResourceLifetimeTests()
{
    bool passed=GetQueuedExportsProtected();
    passed=GetFrozenReadsProtected()&&passed;
    passed=GetQueuedReadProtected()&&passed;
    return GetDerivedProductsProtected()&&passed;
}
