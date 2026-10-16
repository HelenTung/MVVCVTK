#include "../DataGraph/RoiArchiveTestReader.h"
#include "../ControlledTaskGate.h"
#include "Host/RoiEditingHostFeature.h"
#include "Host/VtkAppHostSession.h"
#include "TestDataPort.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include <vtkCamera.h>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkCompositePolyDataMapper.h>
#include <vtkMultiBlockDataSet.h>
#include <vtkCellArray.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkWeakPointer.h>
#include <vtkCallbackCommand.h>
#include <future>
#include <cstdlib>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <atomic>

namespace {
void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
bool DetachEditor(RoiEditingHostFeature& editor)
{
    const auto deadline=std::chrono::steady_clock::now()+TaskInterleaving::synchronizationTimeout;
    do {
        if(editor.DetachHost())return true;
        std::this_thread::yield();
    }while(std::chrono::steady_clock::now()<deadline);
    return false;
}
void PumpEditor(RoiEditingHostFeature& editor,const std::function<bool()>& done)
{
    const auto deadline=std::chrono::steady_clock::now()+TaskInterleaving::synchronizationTimeout;
    while(!done() && std::chrono::steady_clock::now()<deadline){(void)editor.OnHostTick();std::this_thread::yield();}
    Require(done(),"editor work did not converge on owner ticks");
}
DataRevisionRef SetScopedImage(TestDataPort& data,vtkImageData* image,const DataEntityId& scope)
{
    VtkDataBridge bridge;const DataRevisionRef source{data.CreateDataEntityId(),1};
    DataTransaction transaction;
    transaction.outputs.push_back({source.entityId,0,DataTypes::imageGrid3D,{},bridge.CreateImagePayload(image),{},scope});
    transaction.bindings.push_back({std::string(primaryVolumeBinding),0,true,{},source});
    Require(data.SetDataCommit(std::move(transaction)).status==DataCommitStatus::Succeeded,"scoped source publication failed");
    return source;
}
bool GetReaderHeld(const TestDataPort& data,const DataEntityId& scope,const char* owner)
{
    const auto lifetime=data.GetDataLifetime(scope);
    return std::any_of(lifetime.blockers.begin(),lifetime.blockers.end(),[&](const auto& blocker){return blocker.owner==owner;});
}
class View final : public FeatureViewService, public OverlayService {
public:
    View(std::string name,HostRenderViewRole role):view{std::move(name),role}
    {
        window->SetOffScreenRendering(1); window->SetSize(128,128); window->AddRenderer(renderer);
        interactor->SetRenderWindow(window);
    }
    ~View() override {ClearOverlays();}
    bool SetInteracting(const InteractionSource&,bool value) override {isInteracting=value; return true;}
    std::optional<std::array<double,16>> GetModelToWorld() const override {return matrix;}
    std::optional<std::array<double,3>> GetWorldPosition(const std::array<double,3>& p) const override {return p;}
    std::optional<RenderInputStamp> GetRenderInputStamp() const override {return RenderInputStamp{source};}
    bool AttachRenderEffect(std::shared_ptr<RenderEffect>) override {return false;}
    bool DetachRenderEffect(const RenderEffect*) override {return false;}
    bool SetRenderNeeded() override {++renders;return true;}
    bool AttachOverlay(std::shared_ptr<FeatureOverlay> overlay) override
    {
        overlay->AttachRenderer(renderer); overlays.push_back(overlay);
        FeatureOverlayState state; state.modelToWorld=matrix;state.cursor=cursor; overlay->SetOverlayState(state);
        return !isRejected; // 故意模拟已部分挂接后才返回失败的端口。
    }
    void RemoveOverlay(std::shared_ptr<FeatureOverlay> overlay) noexcept override
    {
        overlay->DetachRenderer(renderer); overlays.erase(std::remove(overlays.begin(),overlays.end(),overlay),overlays.end());
    }
    void ClearOverlays() noexcept override {while(!overlays.empty()) RemoveOverlay(overlays.back());}
    HostFeatureView view;
    DataRevisionRef source;
    std::array<double,16> matrix=roiIdentityMatrix;
    std::array<double,3> cursor{};
    bool isRejected=false,isInteracting=false;
    int renders=0;
    std::vector<std::shared_ptr<FeatureOverlay>> overlays;
    vtkSmartPointer<vtkRenderer> renderer=vtkSmartPointer<vtkRenderer>::New();
    vtkSmartPointer<vtkRenderWindow> window=vtkSmartPointer<vtkRenderWindow>::New();
    vtkSmartPointer<vtkRenderWindowInteractor> interactor=vtkSmartPointer<vtkRenderWindowInteractor>::New();
    std::shared_ptr<FeatureViewLease> lease=std::make_shared<FeatureViewLease>(std::this_thread::get_id());
};
class Views final : public FeatureViewDirectory {
public:
    std::map<std::string,std::shared_ptr<View>> views;
    std::vector<HostFeatureView> GetViews(const HostViewTargets& targets) const override
    {
        std::vector<HostFeatureView> result;
        for (const auto& id:targets.viewIds) if (views.count(id)) result.push_back(views.at(id)->view);
        return result;
    }
    std::shared_ptr<FeatureViewService> GetFeaturePort(const std::string& id) const override {return views.count(id)?views.at(id):nullptr;}
    std::shared_ptr<OverlayService> GetOverlayPort(const std::string& id) const override {return views.count(id)?views.at(id):nullptr;}
    std::optional<HostInputView> GetInputView(const HostViewTarget& target) const override
    {
        const auto found=views.find(target.viewId); if (found==views.end()) return {};
        const auto& v=*found->second; return HostInputView{v.view,v.renderer,v.interactor,v.lease};
    }
};
void TestEditor()
{
    auto data=std::make_shared<TestDataPort>();
    vtkNew<vtkImageData> image; image->SetDimensions(5,5,5); image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto source=data->SetPrimaryImage(image)->data->self;
    auto views=std::make_shared<Views>();
    auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);
    auto slice=std::make_shared<View>("slice",HostRenderViewRole::TopDownSlice);
    main->source=slice->source=source; views->views={{"main",main},{"slice",slice}};
    RoiEditingHostFeature editor({{"main"},{{"main","slice"},{}}});
    Require(editor.SendRequest({RoiEditingAction::Commit}).error==RoiError::Unavailable,"detached request accepted");
    HostFeatureContext context; context.data=data; context.views=views;
    Require(editor.AttachHost(context) && !editor.AttachHost(context),"attach gate failed");
    RoiRequest draft; draft.metadata.name="interactive region"; draft.definition.source=source;
    RoiNode box; box.primitive.localToSource={1,0,0,2,0,1,0,2,0,0,1,2,0,0,0,1}; draft.definition.nodes={box};
    const auto before=data->GetDataGraph().commitId;
    slice->isRejected=true;
    Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::Unavailable
        && main->overlays.empty() && slice->overlays.empty() && !editor.GetState().hasDraft,"partial attach did not roll back");
    slice->isRejected=false;
    Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"box draft failed");
    Require(main->overlays.size()==1 && slice->overlays.size()==1 && data->GetDataGraph().commitId==before,"draft committed or overlays absent");
    const auto geometry=editor.GetState().draft->nodes[0].primitive.localToSource;
    main->matrix[3]=7;
    Require(editor.OnHostTick() && editor.GetState().draft->nodes[0].primitive.localToSource==geometry
        && data->GetDataGraph().commitId==before,"display transform changed source geometry");
    const auto renders=main->renders;
    Require(editor.OnHostTick() && main->renders==renders,"idle editor creates an infinite redraw loop");
    RoiEditingRequest hidden; hidden.action=RoiEditingAction::SetVisible; hidden.isVisible=false;
    Require(editor.SendRequest(hidden).error==RoiError::None && data->GetDataGraph().commitId==before,"visibility changed formal geometry");
    RoiError wrongThread=RoiError::None;
    std::thread other([&]{wrongThread=editor.SendRequest({RoiEditingAction::Cancel}).error;}); other.join();
    Require(wrongThread==RoiError::WrongThread,"wrong thread changed editor");
    Require(editor.SendRequest({RoiEditingAction::Cancel}).error==RoiError::None && main->overlays.empty()
        && data->GetDataGraph().commitId==before,"cancel wrote data or leaked overlays");
    Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"second draft failed");
    RoiEditingRequest update;update.action=RoiEditingAction::SetDraft;update.primitive=RoiPrimitive{};update.primitive->localToSource=geometry;update.draft=draft;update.draft->metadata.name="";
    Require(editor.SendRequest(update).error==RoiError::InvalidRequest && editor.GetState().hasDraft
        && editor.GetState().draft->nodes[0].primitive.localToSource==geometry,"invalid metadata destroyed the existing draft");
    update.draft->metadata.name="renamed draft";update.primitive->localToSource[3]+=1;
    Require(editor.SendRequest(update).error==RoiError::None && editor.GetState().hasDraft
        && editor.GetState().draft->nodes[0].primitive.localToSource==update.primitive->localToSource,"geometry and metadata update did not retain the draft");
    const auto saved=editor.SendRequest({RoiEditingAction::Commit});
    Require(saved.roi && saved.roi->metadata.name=="renamed draft" && saved.roi->definition.nodes[0].primitive.localToSource==update.primitive->localToSource,"commit lost the applied draft name or geometry");
    Require(saved.error==RoiError::None && saved.roi && !editor.GetState().hasDraft
        && main->overlays.size()==1 && slice->overlays.size()==1,"formal ROI projection was not retained");
    RoiEditingRequest shown;shown.action=RoiEditingAction::SetVisible;shown.isVisible=true;
    Require(editor.SendRequest(shown).error==RoiError::None,"committed ROI visibility failed");
    const auto committedData=data->GetDataGraph().commitId;
    Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None
        && main->overlays.size()==2,"draft and committed ROI ownership are not separate");
    Require(editor.SendRequest({RoiEditingAction::Cancel}).error==RoiError::None
        && main->overlays.size()==1 && slice->overlays.size()==1
        && data->GetDataGraph().commitId==committedData,"cancel did not restore the committed projection without a data write");
    auto otherSource=source; ++otherSource.generation;
    slice->source=otherSource;
    Require(editor.OnHostTick() && main->overlays.size()==1 && slice->overlays.empty()
        && data->GetData(data->GetDataGraph(),saved.roi->revision),
        "ROI projection was attached to a wrong source view or deleted formal ROI");
    slice->source=source;Require(editor.OnHostTick() && main->overlays.size()==1 && slice->overlays.size()==1,"ROI projection did not return when its source view returned");
    Require(DetachEditor(editor) && editor.DetachHost() && main->overlays.empty() && slice->overlays.empty()
        && data->GetData(data->GetDataGraph(),saved.roi->revision),"detach deleted formal ROI or left a projection");
    Require(editor.AttachHost(context),"reattach failed");
    RoiRequest edit; edit.action=RoiAction::SetGeometry; edit.definition=saved.roi->definition;
    edit.expectedRoi=saved.roi->revision; edit.expectedCatalogRevision=saved.roi->catalogRevision;
    Require(editor.SendRequest({RoiEditingAction::Begin,edit}).error==RoiError::None,"existing ROI draft failed");
    data->SetPrimaryImage(image);
    Require(editor.SendRequest({RoiEditingAction::Commit}).error==RoiError::RevisionConflict,"changed source binding accepted");
    Require(editor.OnHostTick() && !editor.GetState().hasDraft,"stale draft not retired on owner tick");
    Require(DetachEditor(editor),"final detach failed");
}
void TestCurves()
{
    auto data=std::make_shared<TestDataPort>();
    vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto source=data->SetPrimaryImage(image)->data->self;
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);
    main->source=source;views->views={{"main",main}};
    RoiEditingHostFeature editor({{"main"},{{"main"},{}}});HostFeatureContext context;context.data=data;context.views=views;
    Require(editor.AttachHost(context),"curve editor attach failed");
    DataBindingRevision catalogRevision=0;
    for(auto shape:{RoiShape::Sphere,RoiShape::Cylinder}) {
        RoiRequest draft;draft.definition.source=source;draft.metadata.name="曲面区域";
        draft.expectedCatalogRevision=catalogRevision;
        RoiNode node;node.primitive.shape=shape;node.primitive.origin={2,2,2};node.primitive.radius=1;
        node.primitive.height=shape==RoiShape::Cylinder?2:0;draft.definition.nodes={node};
        Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"curve draft rejected");
        const auto commit=data->GetDataGraph().commitId;
        RoiEditingRequest change;change.action=RoiEditingAction::SetDraft;change.primitive=node.primitive;
        change.primitive->radius=-1;
        Require(editor.SendRequest(change).error==RoiError::InvalidGeometry
            && editor.GetState().draft->nodes.front().primitive.radius==1,"invalid curve changed draft");
        change.primitive->radius=.75;
        Require(editor.SendRequest(change).error==RoiError::None && data->GetDataGraph().commitId==commit,
            "curve adjustment published data");
        const auto saved=editor.SendRequest({RoiEditingAction::Commit});
        Require(saved.error==RoiError::None && saved.roi,"curve commit failed");
        catalogRevision=saved.roi->catalogRevision;
        const auto read=data->GetRoi(data->GetDataGraph(),saved.roi->revision,source);
        Require(read.error==RoiError::None && read.roi->GetContains({2,2,2})
            && !read.roi->GetContains({3,2,2}) && read.roi->GetDefinition().nodes.front().primitive.radius==.75,
            "saved curve selection differs from its analytic radius");
    }
    Require(DetachEditor(editor) && main->overlays.empty(),"curve projection leaked on detach");
}
class Controller final:public FeatureHostControl {
public:
    HostInputBinding binding;std::shared_ptr<std::atomic<unsigned>> signals=std::make_shared<std::atomic<unsigned>>(0);
    bool AttachInput(HostInputBinding value)override{binding=std::move(value);return true;}
    unsigned detachFailures=0,detachCalls=0;
    bool DetachInput(std::string_view)override{++detachCalls;if(detachFailures){--detachFailures;return false;}binding={};return true;}
    bool SetActiveViews(const std::vector<std::string>&)override{return true;}
    bool SetViewStatus(const std::vector<std::string>&,const std::string&)override{return true;}
    bool SendSceneDelta(FeatureSceneDelta)override{return true;}
    bool SendOwnerComplete(std::function<void()> complete)override{complete();return true;}
    bool throwOnWork=false;
    std::shared_ptr<TaskInterleaving::Gate> workGate;
    const std::thread::id owner=std::this_thread::get_id();
    std::atomic<unsigned> workerSignals{0};
    bool SendWorkAvailable()override {
        ++*signals;
        if(std::this_thread::get_id()!=owner) {
            ++workerSignals;
            if(workGate)workGate->Hold();
            if(throwOnWork)throw std::runtime_error("injected notification failure");
        }
        return true;
    }
};
void TestWorkerDestruction(bool throwOnWork,bool active=false)
{
    struct Deletions {std::thread::id owner=std::this_thread::get_id();unsigned total=0,foreign=0;} deletions;
    class OperationPort final:public TestDataPort {
    public:
        bool waitForCancellation=false;
        mutable std::promise<void> entered;
        mutable std::atomic<bool> observedCancellation{false};
        RoiOperationResult GetRoiOperation(const DataGraphSnapshot& graph,const RoiOperationRequest& request) const override
        {
            if(waitForCancellation) {
                entered.set_value();
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
                while(!request.getCancelled()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
                observedCancellation.store(request.getCancelled());
            }
            return TestDataPort::GetRoiOperation(graph,request);
        }
    };
    auto data=std::make_shared<OperationPort>();data->waitForCancellation=active;
    vtkNew<vtkImageData> image;
    image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto source=data->SetPrimaryImage(image)->data->self;
    auto views=std::make_shared<Views>();
    auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);
    main->source=source;views->views={{"main",main}};
    auto host=std::make_shared<Controller>();host->throwOnWork=throwOnWork;
    auto editor=std::make_shared<RoiEditingHostFeature>(RoiEditingConfig{{"main"},{{"main"},{}}});
    HostFeatureContext context;context.data=data;context.views=views;context.host=host;
    Require(editor->AttachHost(context),"worker destruction attach failed");
    RoiRequest draft;draft.definition.source=source;
    // 球体不创建交互 widget；overlay 由 owner 的 View 保活并在 owner 上清理。
    RoiNode node;node.primitive.shape=throwOnWork?RoiShape::Box:RoiShape::Sphere;
    if(throwOnWork)node.primitive.localToSource={1,0,0,2,0,1,0,2,0,0,1,2,0,0,0,1};
    else {node.primitive.origin={2,2,2};node.primitive.radius=1;}
    draft.definition.nodes={node};
    Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"worker destruction draft failed");
    vtkNew<vtkCallbackCommand> onDelete;onDelete->SetClientData(&deletions);
    onDelete->SetCallback([](vtkObject*,unsigned long,void* value,void*) {
        auto& observed=*static_cast<Deletions*>(value);++observed.total;
        if(std::this_thread::get_id()!=observed.owner)++observed.foreign;
    });
    auto* actors=main->renderer->GetActors();actors->InitTraversal();
    while(auto* actor=actors->GetNextActor())actor->AddObserver(vtkCommand::DeleteEvent,onDelete);
    RoiEditingRequest start;start.action=RoiEditingAction::BeginOperation;start.operation=RoiOperationRequest{};
    start.operation->params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels};
    Require(editor->SendRequest(start).error==RoiError::Pending,"worker destruction operation was not admitted");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    if(active)Require(data->entered.get_future().wait_until(deadline)==std::future_status::ready,"worker did not enter the operation");
    else {
        while(!host->signals->load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        Require(host->signals->load()!=0,"worker did not reach completion notification");
    }
    if(throwOnWork) {
        // 通知失败不能使已完成计算失败；owner tick 仍能消费结果并正常 Detach。
        PumpEditor(*editor,[&]{return !editor->GetState().isComputing;});
        Require(editor->GetState().operation.error==RoiError::None,"notification exception changed the successful operation outcome");
        Require(DetachEditor(*editor)&&main->overlays.empty(),"owner detach did not release VTK resources");
        // vtkRenderer::GetActors 的枚举缓存也持有 actor；刷新后才能观察最后一次释放。
        (void)main->renderer->GetActors();
        Require(deletions.total!=0&&deletions.foreign==0,"normal owner detach released overlay props on another thread");
    }
    const std::weak_ptr<RoiEditingHostFeature> released=editor;
    std::thread destroy([value=std::move(editor)]()mutable{value.reset();});
    destroy.join();
    Require(released.expired(),"foreign release retained the feature");
    if(active)Require(data->observedCancellation.load(),"destruction did not cancel the active operation");
    // 非 owner 析构只覆盖 worker 最后防线；正常资源释放仍要求 owner Detach。
    main->ClearOverlays();
}
class GatedOperationPort final : public TestDataPort {
public:
    explicit GatedOperationPort(std::shared_ptr<TaskInterleaving::Gate> value):gate(std::move(value)){}
    RoiOperationResult GetRoiOperation(const DataGraphSnapshot& graph,const RoiOperationRequest& request) const override
    {
        gate->Hold();
        return TestDataPort::GetRoiOperation(graph,request);
    }
    std::shared_ptr<TaskInterleaving::Gate> gate;
};

void TestOwnerOperationWait(bool detach)
{
    auto gate=std::make_shared<TaskInterleaving::Gate>();gate->Arm();
    auto data=std::make_shared<GatedOperationPort>(gate);
    vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto scope=data->CreateDataEntityId();const auto source=SetScopedImage(*data,image,scope);
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);
    main->source=source;views->views={{"main",main}};
    auto editor=std::make_shared<RoiEditingHostFeature>(RoiEditingConfig{{"main"},{{"main"},{}}});
    TaskInterleaving::ReleaseOnExit release(gate);
    auto host=std::make_shared<Controller>();
    HostFeatureContext context;context.data=data;context.views=views;context.host=host;
    Require(editor->AttachHost(context),"operation wait attach failed");
    RoiRequest draft;draft.definition.source=source;RoiNode node;node.primitive.shape=RoiShape::Sphere;
    node.primitive.origin={2,2,2};node.primitive.radius=1;draft.definition.nodes={node};
    Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"operation wait draft failed");
    RoiEditingRequest start;start.action=RoiEditingAction::BeginOperation;start.operation=RoiOperationRequest{};
    start.operation->params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels};
    Require(editor->SendRequest(start).error==RoiError::Pending,"operation wait task was not admitted");
    Require(gate->WaitEntered(),"operation worker did not enter the gate");
    const auto before=data->GetDataGraph().commitId;
    Require(editor->SendRequest({RoiEditingAction::Commit}).error==RoiError::Pending,"blocked operation save was not deferred");
    auto replacement=draft;replacement.definition.nodes.front().primitive.origin={1,1,1};
    replacement.definition.nodes.front().primitive.radius=.25;
    const auto owner=std::this_thread::get_id();RoiError error=RoiError::Unavailable;bool detached=false;
    const auto observation=TaskInterleaving::ObserveOwnerCall(gate,[&]{
        Require(std::this_thread::get_id()==owner,"operation owner call changed threads");
        if(detach){
            detached=editor->DetachHost();
            Require(!detached && editor->GetState().isAttached && !editor->GetState().hasDraft,"blocked detach falsely completed or retained the draft");
            Require(!editor->DetachHost() && host->detachCalls==0,"blocked detach retry dropped input ownership");
            Require(editor->SendRequest({RoiEditingAction::Begin,replacement}).error==RoiError::Unavailable,"detaching editor admitted a new draft");
        }else{
            error=editor->SendRequest({RoiEditingAction::Cancel}).error;
            const auto cancelled=editor->GetState();
            Require(!cancelled.hasDraft && !cancelled.preview && !cancelled.isComputing && !cancelled.isSaving && main->overlays.empty(),"cancel did not immediately invalidate logical draft state");
            Require(editor->SendRequest({RoiEditingAction::Begin,replacement}).error==RoiError::None,"cancelled operation blocked immediate editing");
            RoiEditingRequest change;change.action=RoiEditingAction::SetDraft;change.primitive=replacement.definition.nodes.front().primitive;change.primitive->radius=.5;
            Require(editor->SendRequest(change).error==RoiError::None,"replacement draft could not be edited while old work was blocked");
        }
        Require(GetReaderHeld(*data,scope,"roi-operation-reader"),"cancel or detach released the blocked operation input lease");
        Require(data->GetDataGraph().commitId==before,"cancel or detach published a deferred save");
    });
    if(detach){
        // Stop 已停止普通帧推进；显式重试本身必须能够回收任务。
        Require(DetachEditor(*editor),"operation detach retry failed without ticks");
    }else{
        PumpEditor(*editor,[&]{return !GetReaderHeld(*data,scope,"roi-operation-reader");});
        Require(editor->GetState().draft->nodes.front().primitive.origin==std::array<double,3>{1,1,1}
            && editor->GetState().draft->nodes.front().primitive.radius==.5 && data->GetDataGraph().commitId==before,"late cancelled operation replaced the new edit or committed its old save");
        Require(editor->SendRequest({RoiEditingAction::Cancel}).error==RoiError::None,"replacement cancel failed");
        Require(DetachEditor(*editor),"operation cancel cleanup detach failed");
    }
    Require(!GetReaderHeld(*data,scope,"roi-operation-reader") && !GetReaderHeld(*data,scope,"roi-draft-reader"),"finished operation retained an input lease");
    std::cout<<(detach?"detach-operation-wait":"cancel-operation-wait")<<" elapsed_ms="<<observation.elapsedMs
        <<" returned_while_held="<<observation.returnedWhileHeld<<'\n';
    Require(detach?!detached:error==RoiError::None,"operation owner call violated retry/cancel semantics");
    Require(observation.returnedWhileHeld,detach
        ?"DetachHost waited for a blocked operation on the owner thread"
        :"Cancel waited for a blocked operation on the owner thread");
}

class GatedQueryRead final : public RoiReadView {
public:
    GatedQueryRead(RoiReadSnapshot read,std::shared_ptr<TaskInterleaving::Gate> gate,std::thread::id owner)
        :m_read(std::move(read)),m_gate(std::move(gate)),m_owner(owner){}
    DataRevisionRef GetRevision()const noexcept override{return m_read->GetRevision();}
    DataRevisionRef GetSource()const noexcept override{return m_read->GetSource();}
    const RoiDefinition& GetDefinition()const noexcept override{return m_read->GetDefinition();}
    const std::vector<DataRevisionRef>& GetDependencies()const noexcept override{return m_read->GetDependencies();}
    std::array<double,6> GetBounds()const noexcept override{return m_read->GetBounds();}
    RoiQueryResult GetQuery()const override{if(std::this_thread::get_id()!=m_owner)m_gate->Hold();return m_read->GetQuery();}
    bool GetContains(const std::array<double,3>& p)const override{return m_read->GetContains(p);}
    RoiPointsResult GetPoints(const std::vector<std::array<double,3>>& p)const override{return m_read->GetPoints(p);}
    RoiPlanesResult GetClipPlanes()const override{return m_read->GetClipPlanes();}
    RoiTriangleSelection GetTriangleSelection(const RoiTriangle& t,double tolerance,std::function<bool()> cancelled)const override{return m_read->GetTriangleSelection(t,tolerance,std::move(cancelled));}
    RoiTriangleSelection GetTriangleChunks(const RoiTriangle& t,std::size_t count,const RoiTriangleCallback& consume,double tolerance,std::function<bool()> cancelled)const override{return m_read->GetTriangleChunks(t,count,consume,tolerance,std::move(cancelled));}
    RoiPolygonResult GetClippedTriangle(const RoiTriangle& t)const override{return m_read->GetClippedTriangle(t);}
    double GetBoundaryDistance(const std::array<double,3>& p)const noexcept override{return m_read->GetBoundaryDistance(p);}
    RoiMaskResult GetMaskChunk(const RoiMaskRequest& request)const override{return m_read->GetMaskChunk(request);}
private:
    RoiReadSnapshot m_read;std::shared_ptr<TaskInterleaving::Gate> m_gate;std::thread::id m_owner;
};

class GatedDisplayPort final : public TestDataPort {
public:
    GatedDisplayPort(std::shared_ptr<TaskInterleaving::Gate> value,std::thread::id owner):gate(std::move(value)),ownerThread(owner){}
    RoiReadResult GetRoiPreview(const DataGraphSnapshot& graph,const RoiDefinition& definition)const override
    {
        auto result=TestDataPort::GetRoiPreview(graph,definition);
        if(result.roi)result.roi=std::make_shared<const GatedQueryRead>(result.roi,gate,ownerThread);
        return result;
    }
    std::shared_ptr<TaskInterleaving::Gate> gate;std::thread::id ownerThread;
};

void TestOwnerDisplayWait(bool detach)
{
    auto gate=std::make_shared<TaskInterleaving::Gate>();gate->Arm();
    auto data=std::make_shared<GatedDisplayPort>(gate,std::this_thread::get_id());
    vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto scope=data->CreateDataEntityId();const auto source=SetScopedImage(*data,image,scope);
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);
    main->source=source;views->views={{"main",main}};
    auto editor=std::make_shared<RoiEditingHostFeature>(RoiEditingConfig{{"main"},{{"main"},{}}});
    TaskInterleaving::ReleaseOnExit release(gate);
    auto host=std::make_shared<Controller>();
    HostFeatureContext context;context.data=data;context.views=views;context.host=host;
    Require(editor->AttachHost(context),"display wait attach failed");
    RoiRequest draft;draft.definition={source,{{RoiNodeKind::SourceDomain}}};
    Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"display wait draft failed");
    Require(gate->WaitEntered(),"display worker did not enter the query gate");
    const auto owner=std::this_thread::get_id();RoiError error=RoiError::Unavailable;bool detached=false;
    const auto observation=TaskInterleaving::ObserveOwnerCall(gate,[&]{
        Require(std::this_thread::get_id()==owner,"display owner call changed threads");
        if(detach){
            detached=editor->DetachHost();
            Require(!detached && editor->GetState().isAttached && !editor->GetState().hasDraft,"blocked display detach falsely completed");
            Require(!editor->DetachHost() && host->detachCalls==0,"display detach retry released the input binding early");
            Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::Unavailable,"detaching display admitted a new request");
        }else{
            error=editor->SendRequest({RoiEditingAction::Cancel}).error;
            Require(!editor->GetState().hasDraft && !editor->GetState().preview && main->overlays.empty(),"display cancel retained its visible draft");
        }
        Require(GetReaderHeld(*data,scope,"roi-display-reader"),"blocked display lost its source reader lease");
    });
    if(!detach){
        Require(error==RoiError::None,"display cancel failed");
        // 没有草稿时也必须回收旧显示，后续编辑不能被旧 future 永久占住。
        PumpEditor(*editor,[&]{return !GetReaderHeld(*data,scope,"roi-display-reader");});
        Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"display cancellation prevented a later draft");
        PumpEditor(*editor,[&]{return editor->GetState().displayStatus!=RoiDisplayStatus::Updating;});
        Require(editor->GetState().displayStatus==RoiDisplayStatus::Displayed,"replacement display did not finish");
        Require(editor->SendRequest({RoiEditingAction::Cancel}).error==RoiError::None,"replacement display cancel failed");
    }else{
        Require(!detached,"display detach reported success while the worker was blocked");
        PumpEditor(*editor,[&]{return !GetReaderHeld(*data,scope,"roi-display-reader");});
        host->detachFailures=1;
        Require(!editor->DetachHost() && editor->GetState().isAttached && bool(host->binding.onAction),"failed input detach was reported as completed");
        Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::Unavailable,"input-detach retry admitted requests");
    }
    Require(DetachEditor(*editor) && editor->DetachHost() && !host->binding.onAction,"display detach retry failed");
    Require(!GetReaderHeld(*data,scope,"roi-display-reader") && !GetReaderHeld(*data,scope,"roi-draft-reader"),"display completion retained its input leases");
    std::cout<<(detach?"detach-display-wait":"cancel-display-wait")<<" elapsed_ms="<<observation.elapsedMs
        <<" returned_while_held="<<observation.returnedWhileHeld<<'\n';
    Require(observation.returnedWhileHeld,detach
        ?"DetachHost waited for a blocked display on the owner thread"
        :"Cancel waited for a blocked display on the owner thread");
}

void TestOwnerNotificationWait()
{
    auto gate=std::make_shared<TaskInterleaving::Gate>();gate->Arm();
    auto data=std::make_shared<TestDataPort>();
    vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto scope=data->CreateDataEntityId();const auto source=SetScopedImage(*data,image,scope);
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);
    main->source=source;views->views={{"main",main}};
    auto host=std::make_shared<Controller>();host->workGate=gate;
    auto editor=std::make_shared<RoiEditingHostFeature>(RoiEditingConfig{{"main"},{{"main"},{}}});
    TaskInterleaving::ReleaseOnExit release(gate);
    HostFeatureContext context;context.data=data;context.views=views;context.host=host;
    Require(editor->AttachHost(context),"notification wait attach failed");
    RoiRequest draft;draft.definition={source,{{RoiNodeKind::SourceDomain}}};
    // 先使用解析球体，避免显示任务先进入通知门闩。
    draft.definition.nodes.front().kind=RoiNodeKind::Primitive;
    draft.definition.nodes.front().primitive.shape=RoiShape::Sphere;
    draft.definition.nodes.front().primitive.origin={2,2,2};draft.definition.nodes.front().primitive.radius=1;
    Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"notification wait draft failed");
    RoiEditingRequest start;start.action=RoiEditingAction::BeginOperation;start.operation=RoiOperationRequest{};
    start.operation->params=RoiInvertParams{};
    Require(editor->SendRequest(start).error==RoiError::Pending && gate->WaitEntered(),"operation did not reach the terminal notification");
    const auto observation=TaskInterleaving::ObserveOwnerCall(gate,[&]{
        Require(!editor->DetachHost() && editor->GetState().isAttached,"notification-in-flight detach falsely completed");
        (void)editor->OnHostTick();
        Require(GetReaderHeld(*data,scope,"roi-operation-reader"),"notification-in-flight task released its reader early");
    });
    Require(observation.returnedWhileHeld,"owner joined a thread still sending its completion notification");
    Require(DetachEditor(*editor) && !GetReaderHeld(*data,scope,"roi-operation-reader"),"notification completion could not be reaped on retry");
}

void TestOperationNotificationLifetime()
{
    auto data=std::make_shared<TestDataPort>();
    vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto source=data->SetPrimaryImage(image)->data->self;
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);
    main->source=source;views->views={{"main",main}};
    auto editor=std::make_shared<RoiEditingHostFeature>(RoiEditingConfig{{"main"},{{"main"},{}}});
    auto host=std::make_shared<Controller>();const auto signals=host->signals;const std::weak_ptr<Controller> weakHost=host;
    {
        HostFeatureContext context;context.data=data;context.views=views;context.host=host;
        Require(editor->AttachHost(context),"notification lifetime attach failed");
    }
    host.reset(); // 挂载参数只是临时上下文，不能承担异步任务的通知端口寿命。
    RoiRequest draft;draft.metadata.name="通知生命周期";draft.definition.source=source;
    RoiNode node;node.primitive.localToSource={1,0,0,2,0,1,0,2,0,0,1,2,0,0,0,1};draft.definition.nodes={node};
    Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"notification draft failed");
    RoiEditingRequest start;start.action=RoiEditingAction::BeginOperation;start.operation=RoiOperationRequest{};
    start.operation->params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels};
    Require(editor->SendRequest(start).error==RoiError::Pending,"notification operation was not deferred");
    unsigned consumed=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(editor->GetState().isComputing && std::chrono::steady_clock::now()<deadline) {
        const auto available=signals->load();
        if(available!=consumed){consumed=available;Require(editor->OnHostTick(),"notified owner update failed");}
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(consumed && !editor->GetState().isComputing && editor->GetState().error==RoiError::None,
        "operation lost its work notification after the attachment context was released");
    Require(DetachEditor(*editor) && weakHost.expired(),"notification port survived worker detach");
}
void TestDrawingAndLatestOperation()
{
    auto data=std::make_shared<TestDataPort>();vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto source=data->SetPrimaryImage(image)->data->self;auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D),slice=std::make_shared<View>("slice",HostRenderViewRole::TopDownSlice);
    main->source=slice->source=source;views->views={{"main",main},{"slice",slice}};auto host=std::make_shared<Controller>();
    auto editor=std::make_shared<RoiEditingHostFeature>(RoiEditingConfig{{"main"},{{"main","slice"},{}}});HostFeatureContext context;context.data=data;context.views=views;context.host=host;Require(editor->AttachHost(context) && bool(host->binding.onAction),"drawing input was not registered");
    RoiRequest draft;draft.metadata.name="old";draft.definition.source=source;RoiNode node;node.primitive.localToSource={1,0,0,2,0,1,0,2,0,0,1,2,0,0,0,1};draft.definition.nodes={node};Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"drawing draft failed");
    FeatureOverlayState overlay;overlay.cursor={0,0,2};for(auto& item:slice->overlays)item->SetOverlayState(overlay);
    slice->renderer->GetActiveCamera()->SetPosition(2,2,10);slice->renderer->GetActiveCamera()->SetFocalPoint(2,2,2);slice->renderer->GetActiveCamera()->ParallelProjectionOn();slice->renderer->GetActiveCamera()->SetParallelScale(3);
    const auto display=[&](double x,double y){slice->renderer->SetWorldPoint(x,y,2,1);slice->renderer->WorldToDisplay();const auto* p=slice->renderer->GetDisplayPoint();return std::array<int,2>{int(std::lround(p[0])),int(std::lround(p[1]))};};
    RoiEditingRequest drawing;drawing.action=RoiEditingAction::BeginDrawing;drawing.drawing=RoiDrawingMode::SliceRectangle;Require(editor->SendRequest(drawing).error==RoiError::None,"rectangle tool failed");
    const auto first=display(1.2,1.2),last=display(2.8,2.8);InteractionEvent event;event.viewId="slice";event.x=first[0];event.y=first[1];event.eventKind=InteractionEventKind::PrimaryPress;Require(host->binding.onAction("roi.draw",event,{}).isHandled,"rectangle press not captured");
    event.x=last[0];event.y=last[1];event.eventKind=InteractionEventKind::PointerMove;Require(host->binding.onAction("roi.draw",event,{}).isSucceeded,"rectangle update failed");event.eventKind=InteractionEventKind::PrimaryRelease;Require(host->binding.onAction("roi.draw",event,{}).isSucceeded,"rectangle release failed");
    Require(editor->GetState().preview->GetContains({2,2,2}) && !editor->GetState().preview->GetContains({3,3,2}) && editor->GetState().undoCount==1,"slice rectangle ignored its plane or applied baseline");
    Require(editor->SendRequest({RoiEditingAction::Undo}).error==RoiError::None && editor->GetState().preview->GetContains({2.9,2.9,2}),"gesture undo did not restore the prior region");
    Require(editor->SendRequest({RoiEditingAction::Redo}).error==RoiError::None && !editor->GetState().preview->GetContains({2.9,2.9,2}),"gesture redo failed");
    RoiOperationRequest operation;operation.params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels};RoiEditingRequest start;start.action=RoiEditingAction::BeginOperation;start.operation=operation;Require(editor->SendRequest(start).error==RoiError::Pending,"operation was not deferred");
    operation.params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,0,RoiRadiusUnit::Voxels};start.action=RoiEditingAction::SetOperation;start.operation=operation;Require(editor->SendRequest(start).error==RoiError::Pending,"latest operation was not queued");
    RoiEditingRequest metadata;metadata.action=RoiEditingAction::SetMetadata;metadata.draft=draft;metadata.draft->metadata.name="计算期间修改的名称";Require(editor->SendRequest(metadata).error==RoiError::None,"metadata update during calculation failed");
    const auto before=data->GetDataGraph().commitId;Require(editor->SendRequest({RoiEditingAction::Commit}).error==RoiError::Pending,"direct save did not wait for the latest candidate");
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(editor->GetState().hasDraft && std::chrono::steady_clock::now()<until){editor->OnHostTick();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    const auto descriptors=RoiService::GetDescriptors(data->GetDataGraph());Require(!editor->GetState().hasDraft && descriptors.size()==1 && descriptors.front().metadata.name=="计算期间修改的名称" && data->GetDataGraph().commitId==before+1,"save used a stale candidate/name or published more than once");
    const auto read=data->GetRoi(data->GetDataGraph(),descriptors.front().revision,source);Require(read.roi->GetContains({2,2,2}) && !read.roi->GetContains({1,2,2}),"late dilation overwrote the latest zero-radius candidate");
    Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"failed-operation draft did not start");operation.params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,-1,RoiRadiusUnit::Voxels};start.action=RoiEditingAction::BeginOperation;start.operation=operation;Require(editor->SendRequest(start).error==RoiError::Pending,"invalid-parameter operation not admitted");Require(editor->SendRequest({RoiEditingAction::Commit}).error==RoiError::Pending,"failed-operation save wait did not start");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(editor->GetState().isComputing && std::chrono::steady_clock::now()<deadline){editor->OnHostTick();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    Require(!editor->GetState().isSaving && editor->GetState().hasDraft && editor->GetState().error==RoiError::InvalidRequest && data->GetDataGraph().commitId==before+1,"invalid geometry left save waiting or changed formal data");
    Require(editor->SendRequest({RoiEditingAction::Cancel}).error==RoiError::None,"failed draft cancellation failed");
    RoiRequest contour;contour.metadata.name="curve";contour.definition.source=source;RoiNode curve;curve.primitive.shape=RoiShape::Contour;curve.primitive.localToSource[3]=2;curve.primitive.localToSource[7]=2;curve.primitive.localToSource[11]=2;curve.primitive.contours={{{-.5,-.5},{.5,-.5},{0,.5}}};contour.definition.nodes={curve};
    Require(editor->SendRequest({RoiEditingAction::Begin,contour}).error==RoiError::None,"control point draft failed");drawing.drawing=RoiDrawingMode::EditControlPoint;Require(editor->SendRequest(drawing).error==RoiError::None,"control point editing was not enabled");
    const auto control=display(2.5,1.5),moved=display(2.8,1.5);event.x=control[0];event.y=control[1];event.eventKind=InteractionEventKind::PrimaryPress;Require(host->binding.onAction("roi.draw",event,{}).isHandled,"control point was not hit");event.x=moved[0];event.y=moved[1];event.eventKind=InteractionEventKind::PointerMove;Require(host->binding.onAction("roi.draw",event,{}).isSucceeded,"control point move failed");event.eventKind=InteractionEventKind::PrimaryRelease;Require(host->binding.onAction("roi.draw",event,{}).isSucceeded,"control point release failed");
    Require(editor->GetState().draft->nodes[0].primitive.contours[0][1][0]>.7,"control point movement changed another selection");Require(editor->SendRequest({RoiEditingAction::Undo}).error==RoiError::None && editor->GetState().tool->contours[0][1][0]==.5,"control point undo left stale handles");
    Require(DetachEditor(*editor) && !host->binding.onAction && main->overlays.empty() && slice->overlays.empty(),"drawing/worker detach leaked resources");
}

void WaitOperation(RoiEditingHostFeature& editor){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(editor.GetState().isComputing && std::chrono::steady_clock::now()<deadline){Require(editor.OnHostTick(),"operation tick failed");std::this_thread::yield();}
    Require(!editor.GetState().isComputing,"operation did not finish");
}
void TestOutputIdentityAndDisplay(){
    auto data=std::make_shared<TestDataPort>();vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto source=data->SetPrimaryImage(image)->data->self;auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);main->source=source;views->views={{"main",main}};
    RoiEditingHostFeature editor({{"main"},{{"main"},{}}});HostFeatureContext context;context.data=data;context.views=views;Require(editor.AttachHost(context),"identity editor attach failed");
    const auto make=[&]{RoiRequest request;request.metadata.name="A";RoiPrimitive sphere;sphere.shape=RoiShape::Sphere;sphere.origin={2,2,2};sphere.radius=.4;request.definition={source,{{RoiNodeKind::Primitive,sphere}}};const auto result=data->SetRoi(request);Require(result.roi.has_value(),"formal identity fixture failed");return *result.roi;};
    const auto edit=[&](const RoiDescriptor& roi){RoiRequest request;request.action=RoiAction::SetGeometry;request.definition=roi.definition;request.expectedRoi=roi.revision;request.metadata=roi.metadata;Require(editor.SendRequest({RoiEditingAction::Begin,request}).error==RoiError::None,"identity draft failed");};
    const auto start=[&](RoiOutputMode output){RoiEditingRequest operation;operation.action=RoiEditingAction::BeginOperation;RoiOperationRequest request;request.output=output;request.params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels};operation.operation=request;Require(editor.SendRequest(operation).error==RoiError::Pending,"identity operation not admitted");WaitOperation(editor);Require(editor.GetState().error==RoiError::None,"identity operation failed");};
    for(const auto output:{RoiOutputMode::CreateNew,RoiOutputMode::UpdateTarget})for(bool applied:{false,true}){
        const auto original=make();edit(original);start(output);const auto before=data->GetDataGraph().commitId;
        if(applied)Require(editor.SendRequest({RoiEditingAction::ApplyOperation}).error==RoiError::None && data->GetDataGraph().commitId==before,"apply published formal data");
        const auto saved=editor.SendRequest({RoiEditingAction::Commit});Require(saved.error==RoiError::None && saved.roi,"identity save failed");
        Require((saved.roi->revision.entityId==original.revision.entityId)==(output==RoiOutputMode::UpdateTarget),"apply changed new/update output identity");
        const auto read=data->GetRoi(data->GetDataGraph(),saved.roi->revision,source);Require(read.roi && read.roi->GetContains({3,2,2}) && !read.roi->GetContains({4,2,2}),"save did not publish the dilation candidate");
        const auto old=data->GetRoi(data->GetDataGraph(),original.revision,source);Require(old.roi && !old.roi->GetContains({3,2,2}),"formal input revision was mutated");
    }
    for(const auto output:{RoiOutputMode::CreateNew,RoiOutputMode::UpdateTarget}){
        const auto original=make();edit(original);start(output);RoiEditingRequest save;save.action=RoiEditingAction::Commit;save.output=output==RoiOutputMode::CreateNew?RoiOutputMode::UpdateTarget:RoiOutputMode::CreateNew;
        const auto result=editor.SendRequest(save);Require(result.roi && (result.roi->revision.entityId==original.revision.entityId)==(*save.output==RoiOutputMode::UpdateTarget),"commit-time output mode was ignored");
    }
    {
        const auto original=make();edit(original);start(RoiOutputMode::UpdateTarget);
        RoiEditingRequest invalid;invalid.action=RoiEditingAction::SetDraft;invalid.primitive=original.definition.nodes.front().primitive;invalid.primitive->radius=-1;invalid.selection=RoiSelectionMode::Replace;invalid.output=RoiOutputMode::CreateNew;
        Require(editor.SendRequest(invalid).error==RoiError::InvalidGeometry && editor.GetState().output==RoiOutputMode::UpdateTarget,"failed draft changed output identity");
        invalid.action=RoiEditingAction::Commit;invalid.primitive.reset();invalid.selection.reset();RoiRequest metadata;invalid.draft=metadata;
        Require(editor.SendRequest(invalid).error==RoiError::InvalidRequest && editor.GetState().output==RoiOutputMode::UpdateTarget,"invalid commit metadata changed output identity");
        const auto saved=editor.SendRequest({RoiEditingAction::Commit});Require(saved.roi && saved.roi->revision.entityId==original.revision.entityId,"failed request contaminated the retry target");
    }
    const auto original=make();edit(original);start(RoiOutputMode::CreateNew);Require(editor.SendRequest({RoiEditingAction::ApplyOperation}).error==RoiError::None,"new-mode apply failed");
    RoiEditingRequest subtract;subtract.action=RoiEditingAction::SetDraft;subtract.primitive=original.definition.nodes.front().primitive;subtract.selection=RoiSelectionMode::Subtract;
    subtract.selection=RoiSelectionMode::Intersect;Require(editor.SendRequest(subtract).error==RoiError::None && editor.GetState().preview->GetContains({2,2,2}) && !editor.GetState().preview->GetContains({3,2,2}),"selection-only intersection used stale geometry");
    subtract.selection=RoiSelectionMode::Add;Require(editor.SendRequest(subtract).error==RoiError::None && editor.GetState().preview->GetContains({3,2,2}),"selection-only addition used stale geometry");
    subtract.selection=RoiSelectionMode::Subtract;
    Require(editor.SendRequest(subtract).error==RoiError::None && editor.SendRequest({RoiEditingAction::ApplySelection}).error==RoiError::None,"edit after apply failed");
    Require(editor.SendRequest({RoiEditingAction::Undo}).error==RoiError::None && editor.GetState().output==RoiOutputMode::CreateNew && editor.GetState().preview->GetContains({2,2,2}),"undo lost the complete accepted edit state");
    Require(editor.SendRequest({RoiEditingAction::CancelOperation}).error==RoiError::InvalidRequest && editor.GetState().preview->GetContains({2,2,2}),"undo revived an already applied operation");
    Require(editor.SendRequest({RoiEditingAction::Redo}).error==RoiError::None && !editor.GetState().preview->GetContains({2,2,2}),"redo lost the subtraction");
    Require(editor.SendRequest({RoiEditingAction::CancelOperation}).error==RoiError::InvalidRequest && !editor.GetState().preview->GetContains({2,2,2}),"cancel of an applied operation restored an obsolete edit state");
    const auto edited=editor.SendRequest({RoiEditingAction::Commit});Require(edited.roi && edited.roi->revision.entityId!=original.revision.entityId && edited.roi->definition.nodes.back().kind==RoiNodeKind::Difference,"continued edit returned to the original update target");
    const auto formal=data->GetData(data->GetDataGraph(),edited.roi->revision);Require(formal && formal->provenance && formal->provenance->operationId=="morphology" && std::any_of(formal->inputs.begin(),formal->inputs.end(),[&](const auto& input){return input.source==original.revision;}),"accepted operation provenance was dropped by editing");
    edit(make());main->matrix[15]=0;RoiEditingRequest change;change.action=RoiEditingAction::SetDraft;change.primitive=original.definition.nodes.front().primitive;change.primitive->radius=.3;
    Require(editor.SendRequest(change).error==RoiError::None && editor.GetState().displayStatus==RoiDisplayStatus::Failed && editor.GetState().preview->GetContains({2,2,2}),"display failure invalidated a valid draft");
    Require(editor.OnHostTick() && editor.GetState().hasDraft && editor.GetState().displayStatus==RoiDisplayStatus::Failed,"host tick discarded a draft after projection failure");
    const auto saved=editor.SendRequest({RoiEditingAction::Commit});Require(saved.error==RoiError::None && saved.roi && saved.roi->definition.nodes.front().primitive.radius==.3,"display projection failure blocked valid saving");main->matrix=roiIdentityMatrix;
    edit(make());main->matrix[15]=0;Require(editor.OnHostTick() && editor.GetState().hasDraft && editor.GetState().displayStatus==RoiDisplayStatus::Failed,"late projection failure retained a misleading display status");
    main->matrix=roiIdentityMatrix;Require(editor.OnHostTick() && editor.GetState().displayStatus==RoiDisplayStatus::Displayed,"valid projection did not restore the current display");Require(editor.SendRequest({RoiEditingAction::Cancel}).error==RoiError::None,"projection recovery cancel failed");
    RoiRequest tiny;tiny.metadata.name="subvoxel analytic region";RoiPrimitive small;small.shape=RoiShape::Sphere;small.origin={1.25,1.25,1.25};small.radius=.05;
    tiny.definition={source,{{RoiNodeKind::Primitive,small},{RoiNodeKind::Primitive,small},{RoiNodeKind::Union,{},0,1}}};
    Require(editor.SendRequest({RoiEditingAction::Begin,tiny}).error==RoiError::None,"subvoxel analytic draft failed");const auto tinyDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(editor.GetState().displayStatus==RoiDisplayStatus::Updating && std::chrono::steady_clock::now()<tinyDeadline){editor.OnHostTick();std::this_thread::yield();}
    Require(editor.GetState().displayStatus==RoiDisplayStatus::Failed && editor.GetState().preview->GetContains(small.origin),"unsampled analytic geometry was reported as a true empty region");
    Require(editor.SendRequest({RoiEditingAction::Commit}).error==RoiError::None,"unsampled analytic display prevented valid saving");
    RoiRequest empty;empty.metadata.name="empty";empty.definition={source,{{RoiNodeKind::Empty}}};Require(editor.SendRequest({RoiEditingAction::Begin,empty}).error==RoiError::None,"empty draft failed");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(editor.GetState().displayStatus==RoiDisplayStatus::Updating && std::chrono::steady_clock::now()<deadline){editor.OnHostTick();std::this_thread::yield();}
    Require(editor.GetState().displayStatus==RoiDisplayStatus::Empty && editor.GetState().displayError==RoiError::None,"real empty region was confused with a display failure");Require(editor.SendRequest({RoiEditingAction::Cancel}).error==RoiError::None,"empty draft cancel failed");
    for(const auto shape:{RoiShape::HalfSpace,RoiShape::Brush}){
        RoiRequest draft;draft.metadata.name="参数同步";RoiPrimitive primitive;primitive.shape=shape;
        if(shape==RoiShape::HalfSpace){primitive.origin={2,2,2};primitive.normal={1,0,0};}
        else{primitive.path={{2,2,2}};primitive.radius=1.1;primitive.localToSource[11]=2;primitive.depth={-.2,.2};}
        draft.definition={source,{{RoiNodeKind::Primitive,primitive}}};Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"parameter draft failed");
        RoiEditingRequest edit;edit.action=RoiEditingAction::SetDraft;edit.primitive=primitive;
        if(shape==RoiShape::HalfSpace)edit.primitive->normal={-1,0,0};else edit.primitive->height=1;
        Require(editor.SendRequest(edit).error==RoiError::None,"single-field parameter update failed");
        const auto point=shape==RoiShape::HalfSpace?std::array<double,3>{3,2,2}:std::array<double,3>{2,2,3};Require(!editor.GetState().preview->GetContains(point) && editor.GetState().preview->GetContains({2,2,2}),"normal/brush mode update did not reach preview");
        const auto saved=editor.SendRequest({RoiEditingAction::Commit});const auto read=saved.roi?data->GetRoi(data->GetDataGraph(),saved.roi->revision,source):RoiReadResult{};
        Require(read.roi && !read.roi->GetContains(point) && read.roi->GetContains({2,2,2}),"normal/brush mode save differs from preview");
    }
    edit(make());start(RoiOutputMode::CreateNew);RoiEditingRequest next;next.action=RoiEditingAction::SetOperation;RoiOperationRequest operation;operation.output=RoiOutputMode::CreateNew;operation.params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,2,RoiRadiusUnit::Voxels};next.operation=operation;
    Require(editor.SendRequest(next).error==RoiError::Pending && editor.SendRequest({RoiEditingAction::Commit}).error==RoiError::Pending,"versioned pending save failed");
    const auto before=data->GetDataGraph().commitId;operation.params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,0,RoiRadiusUnit::Voxels};next.operation=operation;Require(editor.SendRequest(next).error==RoiError::Pending,"superseding operation failed");WaitOperation(editor);
    Require(editor.GetState().hasDraft && !editor.GetState().isSaving && data->GetDataGraph().commitId==before,"old pending save committed newer parameters");
    Require(DetachEditor(editor) && main->overlays.empty(),"identity editor leaked display resources");
}
void TestAppliedSplit(){
    auto data=std::make_shared<TestDataPort>();vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);const auto source=data->SetPrimaryImage(image)->data->self;
    RoiRequest input;input.metadata.name="split";RoiPrimitive primitive;primitive.shape=RoiShape::VoxelSet;primitive.voxels=MakeRoiVoxelFixture(std::vector<RoiVoxel>{{{1,1,1},1},{{3,3,3},1}});input.definition={source,{{RoiNodeKind::Primitive,primitive}}};const auto original=data->SetRoi(input);Require(original.roi.has_value(),"split editor fixture failed");
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);main->source=source;views->views={{"main",main}};RoiEditingHostFeature editor({{"main"},{{"main"},{}}});HostFeatureContext context;context.data=data;context.views=views;Require(editor.AttachHost(context),"split editor attach failed");
    input.action=RoiAction::SetGeometry;input.expectedRoi=original.roi->revision;Require(editor.SendRequest({RoiEditingAction::Begin,input}).error==RoiError::None,"split editor begin failed");RoiEditingRequest request;request.action=RoiEditingAction::BeginOperation;RoiOperationRequest operation;RoiComponentParams params;params.operation=RoiComponentOperation::Split;operation.params=params;request.operation=operation;Require(editor.SendRequest(request).error==RoiError::Pending,"split preview failed");WaitOperation(editor);
    Require(editor.SendRequest({RoiEditingAction::ApplyOperation}).error==RoiError::None && editor.GetState().operation.rois.size()==2,"apply discarded the split output batch");
    Require(editor.SendRequest({RoiEditingAction::Undo}).error==RoiError::None && editor.SendRequest({RoiEditingAction::Redo}).error==RoiError::None && editor.GetState().operation.rois.size()==2,"split history lost the applied batch");
    Require(editor.SendRequest({RoiEditingAction::CancelOperation}).error==RoiError::InvalidRequest && editor.GetState().operation.rois.size()==2,"redo revived an applied split parameter preview");
    request.action=RoiEditingAction::SetOperation;Require(editor.SendRequest(request).error==RoiError::InvalidRequest,"applied split still accepted old preview parameter changes");const auto before=data->GetDataGraph().commitId;
    Require(editor.SendRequest({RoiEditingAction::Commit}).error==RoiError::None && data->GetDataGraph().commitId==before+1 && RoiService::GetDescriptors(data->GetDataGraph()).size()==3,"applied split was saved as one union");Require(DetachEditor(editor),"split editor detach failed");
}

void TestChunkedDisplay(bool throwOnWork=false){
    auto data=std::make_shared<TestDataPort>();vtkNew<vtkImageData> image;image->SetDimensions(34,4,4);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),34*4*4,1);const auto source=data->SetPrimaryImage(image)->data->self;
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D),slice=std::make_shared<View>("slice",HostRenderViewRole::TopDownSlice);main->source=slice->source=source;views->views={{"main",main},{"slice",slice}};
    auto host=std::make_shared<Controller>();host->throwOnWork=throwOnWork;
    RoiEditingHostFeature editor({{"main"},{{"main","slice"},{}}});HostFeatureContext context;context.data=data;context.views=views;if(throwOnWork)context.host=host;Require(editor.AttachHost(context),"block display attach failed");
    std::vector<RoiVoxel> voxels;for(int x=1;x<=32;++x)for(int y=1;y<=2;++y)for(int z=1;z<=2;++z)voxels.push_back({{x,y,z},1});
    RoiRequest draft;draft.metadata.name="块边界";RoiPrimitive primitive;primitive.shape=RoiShape::VoxelSet;primitive.voxels=MakeRoiVoxelFixture(voxels);draft.definition={source,{{RoiNodeKind::Primitive,primitive}}};
    Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"block display begin failed");FeatureOverlayState state;state.cursor={0,0,1};for(const auto& overlay:slice->overlays)overlay->SetOverlayState(state);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(editor.GetState().displayStatus==RoiDisplayStatus::Updating && std::chrono::steady_clock::now()<deadline){editor.OnHostTick();std::this_thread::yield();}
    // 仅读取本 fixture 拥有的 actor，核验块面没有内部接缝且复用了顶点。
    const auto blocks=[&]{std::vector<vtkPolyData*> meshes;auto* collection=main->renderer->GetActors();collection->InitTraversal();while(auto* actor=collection->GetNextActor())if(actor->GetVisibility() && vtkCompositePolyDataMapper::SafeDownCast(actor->GetMapper())){
        auto* data=vtkMultiBlockDataSet::SafeDownCast(actor->GetMapper()->GetInputDataObject(0,0));if(data)for(unsigned index=0;index<data->GetNumberOfBlocks();++index)meshes.push_back(vtkPolyData::SafeDownCast(data->GetBlock(index)));}return meshes;};
    const auto initial=blocks();std::size_t faces=0,points=0;for(auto* mesh:initial){faces+=mesh->GetNumberOfPolys();points+=mesh->GetNumberOfPoints();}
    Require(editor.GetState().displayStatus==RoiDisplayStatus::Displayed && initial.size()==2 && faces==2*(32*2+32*2+2*2) && points<faces*4,"block display created seams or repeated every face vertex");
    if(throwOnWork)Require(host->workerSignals.load()>1,"display batch and completion notification exceptions were not exercised");
    const auto sliceLines=[&]{vtkPolyData* result=nullptr;auto* collection=slice->renderer->GetActors();collection->InitTraversal();while(auto* actor=collection->GetNextActor())if(actor->GetVisibility() && actor->GetMapper()){auto* mesh=vtkPolyData::SafeDownCast(actor->GetMapper()->GetInput());if(mesh && mesh->GetNumberOfLines())result=mesh;}return result;};
    auto* contour=sliceLines();Require(contour,"direct slice contour did not finish");double length=0;vtkIdType count=0;const vtkIdType* ids=nullptr;auto* lines=contour->GetLines();lines->InitTraversal();while(lines->GetNextCell(count,ids)){Require(count==2,"slice emitted a non-segment");double a[3],b[3];contour->GetPoint(ids[0],a);contour->GetPoint(ids[1],b);Require(a[2]==1 && b[2]==1,"slice contour belongs to an old plane");length+=std::hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]);}
    Require(std::abs(length-2*(32+2))<1e-12,"slice contour perimeter differs from its voxel rectangle");
    state.cursor={0,0,3};for(const auto& overlay:slice->overlays)overlay->SetOverlayState(state);
    bool completed=false;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(std::chrono::steady_clock::now()<until){editor.OnHostTick();if(editor.GetState().displayStatus!=RoiDisplayStatus::Updating && !sliceLines()){completed=true;break;}std::this_thread::yield();}
    Require(completed && blocks()==initial && editor.GetState().preview->GetContains({10,1,1}),"slice movement rebuilt three-dimensional actors or changed formal membership");
    main->lease->StopLease();Require(editor.OnHostTick() && !editor.GetState().hasDraft && data->GetDataGraph().view->GetData(source),"stopped view lease accepted a late display or deleted source data");
    Require(DetachEditor(editor) && main->overlays.empty() && slice->overlays.empty(),"block display resources leaked");
}

struct SliceQueries {std::atomic<std::size_t> total{0},outside{0};};
class SliceQuery final:public RoiQuery {
public:
    SliceQuery(std::unique_ptr<RoiQuery> query,std::shared_ptr<SliceQueries> observed):m_query(std::move(query)),m_observed(std::move(observed)){}
    bool GetContains(const std::array<double,3>& point)const noexcept override{
        ++m_observed->total;if(point[2]<31 || point[2]>33)++m_observed->outside;return m_query->GetContains(point);
    }
private:std::unique_ptr<RoiQuery> m_query;std::shared_ptr<SliceQueries> m_observed;
};
class SliceRead final:public RoiReadView {
public:
    SliceRead(RoiReadSnapshot read,std::shared_ptr<SliceQueries> observed):m_read(std::move(read)),m_observed(std::move(observed)){}
    DataRevisionRef GetRevision()const noexcept override{return m_read->GetRevision();}
    DataRevisionRef GetSource()const noexcept override{return m_read->GetSource();}
    const RoiDefinition& GetDefinition()const noexcept override{return m_read->GetDefinition();}
    const std::vector<DataRevisionRef>& GetDependencies()const noexcept override{return m_read->GetDependencies();}
    std::array<double,6> GetBounds()const noexcept override{return m_read->GetBounds();}
    RoiQueryResult GetQuery()const override{auto result=m_read->GetQuery();if(result.query)result.query=std::make_unique<SliceQuery>(std::move(result.query),m_observed);return result;}
    bool GetContains(const std::array<double,3>& point)const override{return m_read->GetContains(point);}
    RoiPointsResult GetPoints(const std::vector<std::array<double,3>>& points)const override{return m_read->GetPoints(points);}
    RoiPlanesResult GetClipPlanes()const override{return m_read->GetClipPlanes();}
    RoiTriangleSelection GetTriangleSelection(const RoiTriangle& triangle,double tolerance,std::function<bool()> cancelled)const override{return m_read->GetTriangleSelection(triangle,tolerance,std::move(cancelled));}
    RoiTriangleSelection GetTriangleChunks(const RoiTriangle& triangle,std::size_t count,const RoiTriangleCallback& consume,double tolerance,std::function<bool()> cancelled)const override{return m_read->GetTriangleChunks(triangle,count,consume,tolerance,std::move(cancelled));}
    RoiPolygonResult GetClippedTriangle(const RoiTriangle& triangle)const override{return m_read->GetClippedTriangle(triangle);}
    double GetBoundaryDistance(const std::array<double,3>& point)const noexcept override{return m_read->GetBoundaryDistance(point);}
    RoiMaskResult GetMaskChunk(const RoiMaskRequest& request)const override{return m_read->GetMaskChunk(request);}
private:RoiReadSnapshot m_read;std::shared_ptr<SliceQueries> m_observed;
};
class SliceData final:public TestDataPort {
public:
    RoiReadResult GetRoiPreview(const DataGraphSnapshot& graph,const RoiDefinition& definition)const override{
        auto result=TestDataPort::GetRoiPreview(graph,definition);if(result.roi)result.roi=std::make_shared<const SliceRead>(result.roi,queries);return result;
    }
    std::shared_ptr<SliceQueries> queries=std::make_shared<SliceQueries>();
};
void TestSliceOnlyDisplay(){
    auto data=std::make_shared<SliceData>();vtkNew<vtkImageData> image;image->SetDimensions(18,18,66);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),18*18*66,1);
    const auto source=data->SetPrimaryImage(image)->data->self;auto views=std::make_shared<Views>();auto slice=std::make_shared<View>("slice",HostRenderViewRole::TopDownSlice);slice->source=source;slice->cursor={8,8,32};views->views={{"slice",slice}};
    RoiEditingHostFeature editor({{"slice"},{{"slice"},{}}});HostFeatureContext context;context.data=data;context.views=views;Require(editor.AttachHost(context),"slice-only attach failed");
    RoiRequest draft;draft.metadata.name="仅切片";draft.definition={source,{{RoiNodeKind::SourceDomain}}};
    Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"slice-only begin failed");FeatureOverlayState plane;plane.cursor={8,8,32};for(const auto& overlay:slice->overlays)overlay->SetOverlayState(plane);
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(editor.GetState().displayStatus==RoiDisplayStatus::Updating && std::chrono::steady_clock::now()<until){editor.OnHostTick();std::this_thread::yield();}
    Require(editor.GetState().displayStatus==RoiDisplayStatus::Displayed && data->queries->total>0 && data->queries->outside==0,"slice-only display scanned unrelated three-dimensional voxel layers");
    Require(DetachEditor(editor) && slice->overlays.empty(),"slice-only resources leaked");
}
void TestProgressiveDisplay(){
    auto data=std::make_shared<TestDataPort>();vtkNew<vtkImageData> image;image->SetDimensions(258,18,18);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),258*18*18,1);
    const auto source=data->SetPrimaryImage(image)->data->self;auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);
    auto top=std::make_shared<View>("top",HostRenderViewRole::TopDownSlice),front=std::make_shared<View>("front",HostRenderViewRole::FrontBackSlice),side=std::make_shared<View>("side",HostRenderViewRole::LeftRightSlice);
    for(const auto& view:{main,top,front,side}){view->source=source;view->cursor={128,8,8};views->views.emplace(view->view.id,view);}
    RoiEditingHostFeature editor({{"main"},{{"main","top","front","side"},{}}});auto host=std::make_shared<Controller>();const auto signals=host->signals;std::weak_ptr<Controller> weakHost=host;
    {HostFeatureContext context;context.data=data;context.views=views;context.host=host;Require(editor.AttachHost(context),"progressive attach failed");}host.reset();
    RoiRequest draft;draft.metadata.name="渐进显示";draft.definition={source,{{RoiNodeKind::SourceDomain}}};
    const auto begin=[&]{Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"progressive begin failed");FeatureOverlayState plane;plane.cursor={128,8,8};for(const auto& view:{top,front,side})for(const auto& overlay:view->overlays)overlay->SetOverlayState(plane);};begin();
    const auto sliceMesh=[&](const std::shared_ptr<View>& view){vtkPolyData* result=nullptr;auto* actors=view->renderer->GetActors();actors->InitTraversal();while(auto* actor=actors->GetNextActor())if(actor->GetVisibility() && actor->GetMapper()){
        auto* mesh=vtkPolyData::SafeDownCast(actor->GetMapper()->GetInputDataObject(0,0));if(mesh && mesh->GetNumberOfLines())result=mesh;}return result;};
    const auto blockCount=[&]{unsigned count=0,actorsSeen=0;auto* actors=main->renderer->GetActors();actors->InitTraversal();while(auto* actor=actors->GetNextActor())if(actor->GetVisibility() && vtkCompositePolyDataMapper::SafeDownCast(actor->GetMapper())){
        ++actorsSeen;count=vtkMultiBlockDataSet::SafeDownCast(actor->GetMapper()->GetInputDataObject(0,0))->GetNumberOfBlocks();}Require(actorsSeen<=1,"progressive display created one actor per block");return count;};
    bool slicesEarly=false,movedEarly=false,requestedMove=false;unsigned previous=0,consumed=0;
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(editor.GetState().displayStatus==RoiDisplayStatus::Updating && std::chrono::steady_clock::now()<until){
        const auto current=signals->load();if(current==consumed){std::this_thread::yield();continue;}consumed=current;
        editor.OnHostTick();const auto count=blockCount();Require(count>=previous && count-previous<=4,"one tick consumed an unbounded set of display blocks");previous=count;
        if(sliceMesh(top) && sliceMesh(front) && sliceMesh(side) && editor.GetState().displayStatus==RoiDisplayStatus::Updating){slicesEarly=true;
            if(!requestedMove){FeatureOverlayState plane;plane.cursor={128,8,15};for(const auto& overlay:top->overlays)overlay->SetOverlayState(plane);requestedMove=true;}}
        if(requestedMove && sliceMesh(top)){double point[3];sliceMesh(top)->GetPoint(0,point);if(point[2]==15 && editor.GetState().displayStatus==RoiDisplayStatus::Updating)movedEarly=true;}
        std::this_thread::yield();
    }
    Require(editor.GetState().displayStatus==RoiDisplayStatus::Displayed && slicesEarly && movedEarly && previous==17*2*2,"slice updates waited for complete construction or dropped the final display blocks");
    Require(editor.SendRequest({RoiEditingAction::Cancel}).error==RoiError::None,"progressive cancel failed");begin();
    bool partial=false;const auto cancelUntil=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(std::chrono::steady_clock::now()<cancelUntil){editor.OnHostTick();if(blockCount()>0 && editor.GetState().displayStatus==RoiDisplayStatus::Updating){partial=true;break;}std::this_thread::yield();}
    Require(partial,"progressive cancellation fixture did not reach a partial display");
    vtkWeakPointer<vtkPolyData> stale;
    auto* actors=main->renderer->GetActors();actors->InitTraversal();while(auto* actor=actors->GetNextActor())if(actor->GetVisibility() && vtkCompositePolyDataMapper::SafeDownCast(actor->GetMapper()))
        stale=vtkPolyData::SafeDownCast(vtkMultiBlockDataSet::SafeDownCast(actor->GetMapper()->GetInputDataObject(0,0))->GetBlock(0));
    Require(stale!=nullptr,"old display block fixture is missing");
    RoiEditingRequest replacement;replacement.action=RoiEditingAction::SetDraft;replacement.selection=RoiSelectionMode::Replace;RoiPrimitive sphere;sphere.shape=RoiShape::Sphere;sphere.origin={10,8,8};sphere.radius=.1;replacement.primitive=sphere;
    Require(editor.SendRequest(replacement).error==RoiError::None,"replacement during partial display failed");
    const auto releaseUntil=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(stale && std::chrono::steady_clock::now()<releaseUntil){editor.OnHostTick();std::this_thread::yield();}
    Require(!stale && blockCount()==0 && editor.GetState().preview->GetContains(sphere.origin) && !editor.GetState().preview->GetContains({100,8,8}),"old display batches survived a new draft or retained hidden mesh storage");
    Require(editor.SendRequest({RoiEditingAction::Cancel}).error==RoiError::None,"replacement cancel failed");begin();
    partial=false;const auto sourceUntil=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(std::chrono::steady_clock::now()<sourceUntil){editor.OnHostTick();if(blockCount()>0 && editor.GetState().displayStatus==RoiDisplayStatus::Updating){partial=true;break;}std::this_thread::yield();}
    Require(partial,"source expiry fixture did not reach a partial display");main->lease->StopLease();
    Require(editor.OnHostTick() && !editor.GetState().hasDraft && blockCount()==0,"source lease expiry accepted queued display batches");
    Require(editor.SendRequest({RoiEditingAction::Cancel}).error==RoiError::None && DetachEditor(editor),"cancel/Detach blocked a bounded display producer");
    for(const auto& view:{main,top,front,side})Require(view->overlays.empty(),"cancelled progressive display retained an overlay");
    Require(weakHost.expired(),"detach retained the display notification port");
}

void TestCommittedComplexDisplayStatus()
{
    auto data=std::make_shared<TestDataPort>();vtkNew<vtkImageData> image;
    image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto source=data->SetPrimaryImage(image)->data->self;
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);
    main->source=source;views->views={{"main",main}};auto host=std::make_shared<Controller>();
    RoiEditingHostFeature editor({{"main"},{{"main"},{}}});HostFeatureContext context;
    context.data=data;context.views=views;context.host=host;Require(editor.AttachHost(context),"formal display attach failed");
    RoiRequest draft;draft.metadata.name="正式复杂区域";
    draft.definition={source,{{RoiNodeKind::SourceDomain}}};
    Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"formal display begin failed");
    Require(editor.GetState().displayStatus==RoiDisplayStatus::Updating,"complex draft did not schedule display");
    Require(editor.SendRequest({RoiEditingAction::Commit}).error==RoiError::None,"formal display commit failed");
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(editor.GetState().displayStatus==RoiDisplayStatus::Updating && std::chrono::steady_clock::now()<until){editor.OnHostTick();std::this_thread::yield();}
    Require(!editor.GetState().hasDraft && editor.GetState().displayStatus==RoiDisplayStatus::Displayed
        && editor.GetState().displayError==RoiError::None && editor.GetState().visibleRois.size()==1,
        "formal display retained the discarded draft status after rendering completed");
    Require(DetachEditor(editor),"formal display detach failed");
}
void TestHostEditing(const char* sample)
{
    const int dimension=sample?64:8;
    HostRenderViewConfig view;view.id="main";view.role=HostRenderViewRole::Primary3D;
    view.inputMode=HostInputMode::HostInjected;view.window.viewInit.viewMode=HostRenderMode::Volume;
    view.renderWindow=vtkSmartPointer<vtkRenderWindow>::New();view.renderWindow->SetOffScreenRendering(1);
    HostSessionConfig config;config.renderViews={view};config.driveMode=HostDriveMode::HostDriven;config.onWorkAvailable=[]{};
    VtkAppHostSession session(config);
    struct StopGuard {VtkAppHostSession& session;~StopGuard(){(void)session.Stop();}} stop{session};
    auto editor=std::make_shared<RoiEditingHostFeature>(RoiEditingConfig{{"main"},{{"main"},{}}});
    Require(session.BuildSession() && session.AttachFeature(editor),"actual Host/editor attach failed");
    // 60 秒沿用 RoiIntegrationTests 的实际 Host 加载/完成保护，非取消响应门限。
    const auto pump=[&](const std::function<bool()>& done){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        while(!done() && std::chrono::steady_clock::now()<deadline){(void)session.SendUpdates();std::this_thread::yield();}
        Require(done(),"actual Host work did not converge");
    };
    bool loaded=false;HostResult loadResult;
    const auto complete=[&](HostResult result){loadResult=std::move(result);loaded=true;};
    if(sample){
        HostLoadRequest load;load.filePath=sample;load.geometry.dimensions={dimension,dimension,dimension};
        // 现有 archived-roi/real-source.json：1536^3 CT 的 64^3 区域，物理单位 mm。
        load.geometry.spacing={.1537,.1537,.1537};load.geometry.origin={113.1232,87.3016,113.1232};
        load.metadata.source.kind=ImageSourceKind::RawFile;load.metadata.identity.datasetId="roi-editing-real-region";load.metadata.source.uri=sample;
        Require(session.SendRequest(std::move(load),complete).status==HostAdmissionStatus::Accepted,"real ROI input load rejected");
    }else{
        HostReloadRequest load;load.geometry.dimensions={dimension,dimension,dimension};load.geometry.spacing={.7,1.1,1.3};
        load.geometry.origin={12,-17,23};load.geometry.direction={0,-1,0,1,0,0,0,0,1};
        load.voxels.assign(std::size_t(dimension)*dimension*dimension,1);
        load.metadata.source.kind=ImageSourceKind::Memory;load.metadata.identity.datasetId="roi-editor-host-retry";load.metadata.source.uri="memory://roi-editor-host-retry";
        Require(session.SendRequest(std::move(load),complete).status==HostAdmissionStatus::Accepted,"Host retry input load rejected");
    }
    pump([&]{return loaded;});Require(loadResult.isSucceeded,"actual Host input load failed");
    const auto descriptor=session.GetImageDescriptor();Require(bool(descriptor),"Host input descriptor missing");
    const auto& geometry=*descriptor;RoiRequest draft;draft.metadata.name="实际宿主编辑";draft.definition.source=geometry.dataRevision;
    RoiNode box;const double center=(dimension-1)*.5,half=(dimension/2-1)*.5;
    for(int row=0;row<3;++row){
        box.primitive.localToSource[row*4+3]=geometry.origin[row];
        for(int axis=0;axis<3;++axis){
            const auto coefficient=geometry.direction[row*3+axis]*geometry.spacing[axis];
            box.primitive.localToSource[row*4+axis]=coefficient*half;
            box.primitive.localToSource[row*4+3]+=coefficient*(geometry.extent[axis*2]+center);
        }
    }
    draft.definition.nodes={box};
    Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"actual Host draft rejected");
    const auto checkMask=[&](bool inverted){
        const auto state=editor->GetState();Require(bool(state.preview),"actual Host preview missing");
        RoiMaskRequest request;
        request.region.size={std::size_t(dimension),std::size_t(dimension),std::size_t(dimension)};
        const auto mask=state.preview->GetMaskChunk(request);const auto count=std::size_t(dimension)*dimension*dimension;
        Require(mask.error==RoiError::None && mask.isComplete && mask.values.size()==count,"actual Host ROI mask incomplete");
        std::size_t selected=0;
        for(std::size_t index=0;index<count;++index){
            const int x=int(index%dimension),y=int(index/dimension%dimension),z=int(index/(dimension*dimension));
            const bool inside=x>=dimension/4 && x<3*dimension/4 && y>=dimension/4 && y<3*dimension/4 && z>=dimension/4 && z<3*dimension/4;
            const bool expected=inverted?!inside:inside;
            Require((mask.values[index]!=0)==expected,"public ROI editing mask differs from the independent integer-index reference");
            selected+=mask.values[index]!=0;
        }
        return selected;
    };
    const auto initialSelected=checkMask(false);
    RoiEditingRequest operation;operation.action=RoiEditingAction::BeginOperation;operation.operation=RoiOperationRequest{};operation.operation->params=RoiInvertParams{};
    Require(editor->SendRequest(operation).error==RoiError::Pending,"actual Host operation not admitted");
    pump([&]{return !editor->GetState().isComputing;});
    Require(editor->GetState().operation.error==RoiError::None,"actual Host operation failed");
    const auto invertedSelected=checkMask(true);
    const auto saved=editor->SendRequest({RoiEditingAction::Commit});
    Require(saved.error==RoiError::None && saved.roi && !editor->GetState().hasDraft,"actual Host ROI commit failed");
    const auto committed=session.GetRoiDescriptor(saved.roi->revision);
    Require(committed && committed->definition.source==geometry.dataRevision && committed->metadata.name==draft.metadata.name,"actual Host committed ROI lost source or metadata");
    for(bool stopSession:{false,true}){
        auto gate=std::make_shared<TaskInterleaving::Gate>();gate->Arm();TaskInterleaving::ReleaseOnExit release(gate);
        Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"Host retry draft failed");
        auto blocked=operation;const auto owner=std::this_thread::get_id();
        blocked.operation->getCancelled=[gate,owner]{if(std::this_thread::get_id()!=owner)gate->Hold();return false;};
        Require(editor->SendRequest(blocked).error==RoiError::Pending,"Host retry operation not admitted");
        pump([&]{return gate->GetIsHeld();});
        const auto observation=TaskInterleaving::ObserveOwnerCall(gate,[&]{
            const bool stopped=stopSession?session.Stop():session.DetachFeature(*editor);
            Require(!stopped && editor->GetState().isAttached && !editor->GetState().hasDraft,"Host falsely completed detach/Stop with blocked ROI work");
            Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::Unavailable,"Host-pending detach accepted editing");
        });
        Require(observation.returnedWhileHeld,"Host detach/Stop waited for a blocked ROI task");
        const auto deadline=std::chrono::steady_clock::now()+TaskInterleaving::synchronizationTimeout;bool stopped=false;
        while(!stopped && std::chrono::steady_clock::now()<deadline){stopped=stopSession?session.Stop():session.DetachFeature(*editor);std::this_thread::yield();}
        Require(stopped && !editor->GetState().isAttached,"Host detach/Stop retry did not finish without ticks");
        std::cout<<(stopSession?"host-stop-retry":"host-detach-retry")<<" elapsed_ms="<<observation.elapsedMs<<" returned_while_held="<<observation.returnedWhileHeld<<'\n';
        if(!stopSession)Require(session.GetRoiDescriptor(saved.roi->revision) && session.AttachFeature(editor),"Host detach removed the formal ROI or prevented reattach");
    }
    Require(session.Stop(),"actual Host final Stop was not idempotent");
    std::cout<<"roi-public-entry real="<<bool(sample)<<" voxels="<<std::size_t(dimension)*dimension*dimension
        <<" initial_selected="<<initialSelected<<" inverted_selected="<<invertedSelected<<'\n';
}
}
int main(int argc,char** argv)
{
#ifdef _MSC_VER
    // 生命周期负例失败时向 CTest 返回错误，不弹出阻塞自动化的 CRT 对话框。
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    try {
        if(argc==3&&std::string(argv[1])=="real-ct"){TestHostEditing(argv[2]);return 0;}
        if(argc==2&&std::string(argv[1])=="host-detach-retry"){TestHostEditing(nullptr);return 0;}
        if(argc==2&&std::string(argv[1])=="detach-notification-wait"){TestOwnerNotificationWait();return 0;}
        if(argc==2&&std::string(argv[1])=="cancel-operation-wait"){TestOwnerOperationWait(false);return 0;}
        if(argc==2&&std::string(argv[1])=="detach-operation-wait"){TestOwnerOperationWait(true);return 0;}
        if(argc==2&&std::string(argv[1])=="cancel-display-wait"){TestOwnerDisplayWait(false);return 0;}
        if(argc==2&&std::string(argv[1])=="detach-display-wait"){TestOwnerDisplayWait(true);return 0;}
        if(argc==2&&std::string(argv[1])=="worker-cancellation"){TestWorkerDestruction(false,true);return 0;}
        if(argc==2&&std::string(argv[1])=="worker-destruction"){TestWorkerDestruction(false);return 0;}
        if(argc==2&&std::string(argv[1])=="notification-exception"){TestWorkerDestruction(true);TestChunkedDisplay(true);return 0;}
        if(argc==2&&std::string(argv[1])=="display"){TestChunkedDisplay();TestSliceOnlyDisplay();TestProgressiveDisplay();TestCommittedComplexDisplayStatus();return 0;}
        TestEditor();TestCurves();TestDrawingAndLatestOperation();TestOperationNotificationLifetime();TestOutputIdentityAndDisplay();TestAppliedSplit();TestCommittedComplexDisplayStatus();
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
