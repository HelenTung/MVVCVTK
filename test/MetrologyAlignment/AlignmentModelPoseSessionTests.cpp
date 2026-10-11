#include "Host/MetrologyAlignmentHostFeature.h"
#include "Host/FeatureModelTransformPort.h"
#include "Host/VtkAppHostSession.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Contracts/FeatureOverlay.h"
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkObjectFactory.h>
#include <vtkProp3D.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#define CHECK(v) do { if(!(v))throw std::runtime_error(#v); } while(false)
class PoseInteractor final:public vtkRenderWindowInteractor {
public: static PoseInteractor *New();vtkTypeMacro(PoseInteractor,vtkRenderWindowInteractor);
    void Initialize() override { this->Initialized=1;this->Enabled=1; }void Start() override {} int timerId=0;
protected:int InternalCreateTimer(int id,int,unsigned long) override {timerId=id;return id;}int InternalDestroyTimer(int) override{return 1;}
};
vtkStandardNewMacro(PoseInteractor);
class PoseProbe final:public HostFeature {
public:std::string_view GetFeatureId() const noexcept override{return "metrology.pose.test";}
    bool AttachHost(const HostFeatureContext &v) override{context=v;return true;}bool DetachHost() override{return true;}bool OnHostTick() override{return true;}
    HostFeatureContext context;
};
class FrameFailure final:public FeatureOverlay {
public:void SetInputData(vtkSmartPointer<vtkDataObject>) override{}void AttachRenderer(vtkSmartPointer<vtkRenderer>) override{}
    void DetachRenderer(vtkSmartPointer<vtkRenderer>) override{}
    void SetOverlayState(const FeatureOverlayState &) override{if(failures){--failures;throw std::runtime_error("pose frame failure");}}
    int failures=0;
};
void Run() {
    HostSessionConfig config;std::vector<vtkSmartPointer<PoseInteractor>> interactors;
    const std::array<HostRenderMode,4> modes{HostRenderMode::Volume,HostRenderMode::SliceTopDown,HostRenderMode::SliceFrontBack,HostRenderMode::SliceLeftRight};
    const std::array<HostRenderViewRole,4> roles{HostRenderViewRole::Primary3D,HostRenderViewRole::TopDownSlice,HostRenderViewRole::FrontBackSlice,HostRenderViewRole::LeftRightSlice};
    for(int i=0;i<4;++i) {
        auto window=vtkSmartPointer<vtkRenderWindow>::New();window->SetOffScreenRendering(1);
        auto interactor=vtkSmartPointer<PoseInteractor>::New();window->SetInteractor(interactor);interactor->SetRenderWindow(window);interactors.push_back(interactor);
        HostRenderViewConfig view;view.id="pose-"+std::to_string(i);view.role=roles[i];view.window.width=128;view.window.height=128;
        view.window.viewInit.viewMode=modes[i];view.renderWindow=window;view.isEventLoopEnabled=i==0;view.inputMode=HostInputMode::HostInjected;config.renderViews.push_back(view);
    }
    VtkAppHostSession session(std::move(config));CHECK(session.BuildSession());
    auto probe=std::make_shared<PoseProbe>();auto feature=std::make_shared<MetrologyAlignmentHostFeature>();
    CHECK(session.AttachFeature(probe));CHECK(session.AttachFeature(feature));CHECK(session.Start());
    const auto tick=[&]{int id=interactors[0]->timerId;CHECK(id);interactors[0]->InvokeEvent(vtkCommand::TimerEvent,&id);};
    const auto wait=[&](const auto &ready){const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        do{tick();if(ready())return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<limit);throw std::runtime_error("pose session timeout");};
    HostReloadRequest reload;reload.geometry.dimensions={16,16,16};reload.geometry.spacing={.5,.75,1};reload.geometry.origin={2,3,4};
    reload.geometry.direction={0,-1,0,1,0,0,0,0,1};reload.metadata.identity.datasetId="pose-session";
    reload.metadata.source.kind=ImageSourceKind::Memory;reload.metadata.source.uri="memory://pose-session";reload.voxels.resize(4096,0);
    for(int z=4;z<12;++z)for(int y=4;y<12;++y)for(int x=4;x<12;++x)reload.voxels[x+16*(y+16*z)]=100;
    bool loaded=false;CHECK(session.SendRequest(std::move(reload),[&](HostResult result){loaded=result.isSucceeded;}).status==HostAdmissionStatus::Accepted);wait([&]{return loaded;});for(int i=0;i<3;++i)tick();
    const auto primary=probe->context.data->GetPrimaryImage();CHECK(primary && primary->data);
    const auto source=std::dynamic_pointer_cast<const ImageGrid3DPayload>(primary->data->payload);CHECK(source);
    const auto *bytes=source->GetValues().get();const auto imageTime=primary->image->GetMTime();const auto descriptor=session.GetImageDescriptor();
    const DataRevisionRef meshRef{probe->context.data->CreateDataEntityId(),1},nominal{probe->context.data->CreateDataEntityId(),1};
    const std::vector<double> points{0,0,0,2,0,0,0,3,0,0,0,4};
    auto mesh=std::make_shared<const SurfaceMeshPayload>(points,std::vector<std::uint64_t>{},std::vector<MeshAttribute>{},source->GetGeometry().coordinateFrame);
    auto table=std::make_shared<const RecordTablePayload>(DataTypes::recordTable,"nominal",std::vector<RecordColumn>{{"v",std::vector<double>{1}}});
    DataTransaction tx;tx.outputs.push_back({meshRef.entityId,0,DataTypes::surfaceMesh,{{"source",primary->data->self}},mesh,{}});
    tx.outputs.push_back({nominal.entityId,0,DataTypes::recordTable,{},table,{}});CHECK(probe->context.data->SetDataCommit(std::move(tx)).status==DataCommitStatus::Succeeded);
    AlignmentInput input;input.source=primary->data->self;input.mesh=meshRef;input.scopeData=input.source;input.sourceFrameId="scan";
    input.coordinateFrame=source->GetGeometry().coordinateFrame;input.scope="whole-current-model";input.unit=AlignmentUnit::Millimeter;
    AlignmentRecipe recipe;recipe.id="pose";recipe.method=AlignmentMethod::Rps;recipe.datumCount=0;recipe.nominalData=nominal;recipe.targetFrameId="inspection";recipe.unit=input.unit;
    const double c=std::cos(.4),s=std::sin(.4);AlignmentMatrix expected{c,-s,0,.25,s,c,0,.5,0,0,1,-.75,0,0,0,1};
    for(std::size_t i=0;i<4;++i){AlignmentGeometrySpec g;g.id=std::to_string(i);g.kind=AlignmentGeometryKind::Point;g.region.pinnedMesh=meshRef;g.region.vertexIds={i};recipe.geometries.push_back(g);
        for(std::size_t j=0;j<3;++j){AlignmentConstraint constraint;constraint.geometryIndex=i;constraint.targetDirection={0,0,0};constraint.targetDirection[j]=1;
            constraint.nominalPoint={c*points[3*i]-s*points[3*i+1]+.25,s*points[3*i]+c*points[3*i+1]+.5,points[3*i+2]-.75};recipe.constraints.push_back(constraint);}}
    AlignmentRequest save;save.action=AlignmentAction::SaveRecipe;save.recipe=recipe;DataRevisionRef recipeRef;
    CHECK(feature->SendRequest(save,[&](AlignmentResult r){recipeRef=r.recipe;}).status==AlignmentAdmissionStatus::Accepted);
    AlignmentRequest solve;solve.input=input;solve.recipeRef=recipeRef;AlignmentResult solved;bool done=false;
    CHECK(feature->SendRequest(solve,[&](AlignmentResult r){solved=std::move(r);done=true;}).status==AlignmentAdmissionStatus::Accepted);wait([&]{return done;});CHECK(solved.status==AlignmentStatus::FullyDetermined);
    const auto transform=std::dynamic_pointer_cast<FeatureModelTransformPort>(probe->context.host);CHECK(transform);
    const auto before=*transform->GetTransformState();const auto graphCommit=probe->context.data->GetDataGraph().commitId;
    auto failure=std::make_shared<FrameFailure>();const auto overlay=probe->context.views->GetOverlayPort("pose-0");CHECK(overlay->AttachOverlay(failure));tick();failure->failures=1;
    AlignmentRequest apply;apply.action=AlignmentAction::ApplyModelPose;apply.resultRef=solved.result;AlignmentResult applied;done=false;
    CHECK(feature->SendRequest(apply,[&](AlignmentResult r){applied=std::move(r);done=true;}).status==AlignmentAdmissionStatus::Accepted);
    CHECK(!done && feature->GetState().modelPose.isPending);tick();CHECK(!done && transform->GetTransformState()->modelToWorld==before.modelToWorld);
    wait([&]{return done;});CHECK(applied.status==AlignmentStatus::FullyDetermined && applied.isModelPoseApplied && feature->GetState().modelPose.isCurrent);
    const auto actual=transform->GetTransformState()->modelToWorld;for(std::size_t i=0;i<16;++i)CHECK(std::abs(actual[i]-expected[i])<1e-7);
    for(const auto &endpoint:session.GetRenderViewEndpoints()) {
        CHECK(probe->context.views->GetFeaturePort(endpoint.id)->GetModelToWorld()==actual);
        bool matching=false;auto *props=endpoint.renderer->GetViewProps();props->InitTraversal();
        while(auto *item=props->GetNextProp())if(auto *prop=vtkProp3D::SafeDownCast(item))if(auto *matrix=prop->GetUserMatrix()){
            bool same=true;for(int i=0;i<16;++i)same=same && std::abs(matrix->GetData()[i]-actual[i])<1e-8;matching=matching||same;}
        CHECK(matching);
    }
    CHECK(probe->context.data->GetDataGraph().commitId==graphCommit);CHECK(source->GetValues().get()==bytes);CHECK(primary->image->GetMTime()==imageTime);
    const auto after=session.GetImageDescriptor();CHECK(descriptor && after && descriptor->spacing==after->spacing && descriptor->origin==after->origin && descriptor->direction==after->direction);
    overlay->RemoveOverlay(failure);CHECK(session.DetachFeature(*feature));CHECK(session.DetachFeature(*probe));CHECK(session.Stop());
    std::cout<<"PoseSession: volume + 3 MPR views, frame failure recovery, native storage unchanged\n";
}
int main(){try{Run();return 0;}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
