// 测试用途：普通消费者无 autoinit 创建/绘制，卸载历史卷及当前卷，再次加载。
#include "Host/VtkAppHostSession.h"
#include "Host/HostFeature.h"
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

class UnloadProbe final : public HostFeature {
public:
    std::shared_ptr<TrustedDataPort> data;
    std::string_view GetFeatureId() const noexcept override { return "test.unload.probe"; }
    bool AttachHost(const HostFeatureContext& context) override { data=context.data;return bool(data); }
    bool DetachHost() override {data.reset();return true;}
    bool OnHostTick() override {return true;}
};

int main()
{
    const auto check=[](bool ok,const char* message) { if(!ok)throw std::runtime_error(message); std::cout<<"PASS "<<message<<'\n'; };
    try {
        HostSessionConfig config; config.driveMode=HostDriveMode::HostDriven;
        HostRenderViewConfig view; view.id="unload"; view.role=HostRenderViewRole::Primary3D;
        config.renderViews.push_back(view);
        VtkAppHostSession host(std::move(config)); check(host.BuildSession(),"build consumer session");
        const auto* endpoint=host.GetRenderViewEndpoint(view.id);
        check(endpoint && endpoint->renderer->IsA("vtkOpenGLRenderer")
            && endpoint->renderWindow->IsA("vtkWin32OpenGLRenderWindow"),"factories work without consumer autoinit");
        endpoint->renderWindow->SetOffScreenRendering(1);
        const auto load=[&](const char* id) {
            HostReloadRequest request; request.geometry.dimensions={2,2,2};
            request.geometry.spacing={.153712345678,.2,.3}; request.geometry.origin={100000.003,0,0};
            request.voxels={0,100,100,0,100,0,0,100}; request.metadata.identity.datasetId=id;
            request.metadata.source.kind=ImageSourceKind::Memory; request.metadata.source.uri=std::string("memory://")+id;
            bool done=false,success=false;
            check(host.SendRequest(std::move(request),[&](HostResult result){done=true;success=result.isSucceeded;}).status==HostAdmissionStatus::Accepted,"load accepted");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
            while(!done && std::chrono::steady_clock::now()<deadline) { host.SendUpdates(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            check(done&&success,"load completes");
            const auto image=host.GetImageDescriptor(); check(bool(image),"loaded descriptor exists");
            check(image->spacing[0]==.153712345678 && image->origin[0]==-100000.003-.153712345678,"layout and VTK preserve double geometry");
            return image->dataRevision;
        };
        const auto first=load("first"), second=load("second");
        check(host.SendRender({{view.id}}).status==HostRenderStatus::Rendered,"first render without autoinit");
        const auto unload=[&](DataRevisionRef ref,bool expected) {
            HostDataUnloadRequest request; request.dataRevision=ref; bool done=false,success=false;
            const auto admission=host.SendRequest(std::move(request),[&](HostResult result){done=true;success=result.isSucceeded;});
            check(admission.status==HostAdmissionStatus::Accepted,"unload completion admitted");
            for(int i=0;i<3&&!done;++i)host.SendUpdates();
            check(done&&success==expected,"unload completion matches transaction");
        };
        unload({first.entityId,first.generation+1},false);
        check(bool(host.GetImageDescriptor(first)),"stale head unload retains data");
        auto probe=std::make_shared<UnloadProbe>(); check(host.AttachFeature(probe),"attach dependency probe");
        auto source=probe->data->GetData(probe->data->GetDataGraph(),first);
        DataTransaction derived;
        DataRevisionDraft draft{probe->data->CreateDataEntityId(),0,source->type,{{"source",first}},source->payload};
        draft.lifetimeScope=probe->data->CreateDataEntityId(); draft.lifetimePolicy=DataLifetimePolicy::ExplicitRetirement;
        derived.outputs.push_back(draft);
        const DataRevisionRef output{draft.entityId,1};
        derived.bindings.push_back({"test.unload.output",0,true,{},output});
        check(probe->data->SetDataCommit(derived).status==DataCommitStatus::Succeeded,"publish persistent derived result");
        const auto createRoi=[&](DataRevisionRef ref,DataBindingRevision expected,const char* name) {
            RoiRequest request;request.definition.source=ref;request.definition.nodes.push_back(RoiNode{});
            request.metadata.name=name;request.expectedCatalogRevision=expected;
            const auto result=host.SetRoi(request);check(result.error==RoiError::None && result.roi.has_value(),"create dataset ROI");
            return *result.roi;
        };
        const auto roiA=createRoi(first,0,"first-roi");
        const auto roiB=createRoi(second,roiA.catalogRevision,"second-roi");
        auto reader=source->lifetime.lock()->StartResourceUse(first,"test-unload-reader");
        unload(first,false);
        check(host.GetImageDescriptor()->dataRevision==second && host.GetImageDescriptor(first),"blocked unload preserves selected and historical data");
        DataTransaction unbind; unbind.bindings.push_back({"test.unload.output",1,true,output,{}});
        check(probe->data->SetDataCommit(unbind).status==DataCommitStatus::Succeeded,"close derived result binding");
        unload(first,false); // Reader 仍阻塞，即使显示 binding 已解除。
        reader.reset(); source.reset(); draft.payload.reset(); derived.outputs.clear();
        unload(first,true);
        check(!probe->data->GetData(probe->data->GetDataGraph(),output),"unbound explicit derived results retire with dataset");
        const auto remainingRois=host.GetRoiDescriptors();
        check(remainingRois.size()==1 && remainingRois.front().revision==roiB.revision
            && !host.GetRoiDescriptor(roiA.revision),"ROI catalog keeps other datasets during unload");
        check(host.DetachFeature(*probe),"detach dependency probe");
        check(!host.GetImageDescriptor(first) && host.GetImageDescriptor()->dataRevision==second,"unload historical dataset preserves current");
        HostSessionSetRequest spacing; spacing.spacing=std::array<double,3>{.4,.5,.6};
        bool spacingDone=false,spacingSucceeded=false;
        check(host.SendRequest(std::move(spacing),[&](HostResult result){spacingDone=true;spacingSucceeded=result.isSucceeded;}).status==HostAdmissionStatus::Accepted,"spacing creates another generation");
        const auto spacingDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        while(!spacingDone && std::chrono::steady_clock::now()<spacingDeadline){host.SendUpdates();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        check(spacingDone&&spacingSucceeded,"spacing display preparation completes");
        host.SendUpdates(); const auto changed=host.GetImageDescriptor(); check(bool(changed),"updated generation exists");
        unload(changed->dataRevision,true);
        check(!host.GetImageDescriptor(second) && !host.GetImageDescriptor(changed->dataRevision)
            && !host.GetImageDescriptor() && host.GetRoiDescriptors().empty()
            && host.GetDataCatalog()->data.empty(),"all dataset generations and their ROI retire without orphan catalog scopes");
        host.SendUpdates();
        const auto third=load("third");
        check(bool(host.GetImageDescriptor(third)) && host.SendRender({{view.id}}).status==HostRenderStatus::Rendered,"session renders after unload and reload");
        check(host.Stop(),"stop"); return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
