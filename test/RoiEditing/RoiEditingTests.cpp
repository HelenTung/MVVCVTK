#include "Host/RoiEditingHostFeature.h"
#include "TestDataPort.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include <vtkCamera.h>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkCellArray.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
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
        FeatureOverlayState state; state.modelToWorld=matrix; overlay->SetOverlayState(state);
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
    Require(editor.DetachHost() && editor.DetachHost() && main->overlays.empty() && slice->overlays.empty()
        && data->GetData(data->GetDataGraph(),saved.roi->revision),"detach deleted formal ROI or left a projection");
    Require(editor.AttachHost(context),"reattach failed");
    RoiRequest edit; edit.action=RoiAction::SetGeometry; edit.definition=saved.roi->definition;
    edit.expectedRoi=saved.roi->revision; edit.expectedCatalogRevision=saved.roi->catalogRevision;
    Require(editor.SendRequest({RoiEditingAction::Begin,edit}).error==RoiError::None,"existing ROI draft failed");
    data->SetPrimaryImage(image);
    Require(editor.SendRequest({RoiEditingAction::Commit}).error==RoiError::RevisionConflict,"changed source binding accepted");
    Require(editor.OnHostTick() && !editor.GetState().hasDraft,"stale draft not retired on owner tick");
    Require(editor.DetachHost(),"final detach failed");
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
    Require(editor.DetachHost() && main->overlays.empty(),"curve projection leaked on detach");
}
class Controller final:public FeatureHostControl {
public:
    HostInputBinding binding;std::atomic<unsigned> signals{0};
    bool AttachInput(HostInputBinding value)override{binding=std::move(value);return true;}
    bool DetachInput(std::string_view)override{binding={};return true;}
    bool SetActiveViews(const std::vector<std::string>&)override{return true;}
    bool SetViewStatus(const std::vector<std::string>&,const std::string&)override{return true;}
    bool SendSceneDelta(FeatureSceneDelta)override{return true;}
    bool SendOwnerComplete(std::function<void()> complete)override{complete();return true;}
    bool SendWorkAvailable()override{++signals;return true;}
};
void TestDrawingAndLatestOperation()
{
    auto data=std::make_shared<TestDataPort>();vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);
    const auto source=data->SetPrimaryImage(image)->data->self;auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D),slice=std::make_shared<View>("slice",HostRenderViewRole::TopDownSlice);
    main->source=slice->source=source;views->views={{"main",main},{"slice",slice}};auto host=std::make_shared<Controller>();
    auto editor=std::make_shared<RoiEditingHostFeature>(RoiEditingConfig{{"main"},{{"main","slice"},{}}});HostFeatureContext context;context.data=data;context.views=views;context.host=host;Require(editor->AttachHost(context) && bool(host->binding.onInput),"drawing input was not registered");
    RoiRequest draft;draft.metadata.name="old";draft.definition.source=source;RoiNode node;node.primitive.localToSource={1,0,0,2,0,1,0,2,0,0,1,2,0,0,0,1};draft.definition.nodes={node};Require(editor->SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"drawing draft failed");
    FeatureOverlayState overlay;overlay.cursor={0,0,2};for(auto& item:slice->overlays)item->SetOverlayState(overlay);
    slice->renderer->GetActiveCamera()->SetPosition(2,2,10);slice->renderer->GetActiveCamera()->SetFocalPoint(2,2,2);slice->renderer->GetActiveCamera()->ParallelProjectionOn();slice->renderer->GetActiveCamera()->SetParallelScale(3);
    const auto display=[&](double x,double y){slice->renderer->SetWorldPoint(x,y,2,1);slice->renderer->WorldToDisplay();const auto* p=slice->renderer->GetDisplayPoint();return std::array<int,2>{int(std::lround(p[0])),int(std::lround(p[1]))};};
    RoiEditingRequest drawing;drawing.action=RoiEditingAction::BeginDrawing;drawing.drawing=RoiDrawingMode::SliceRectangle;Require(editor->SendRequest(drawing).error==RoiError::None,"rectangle tool failed");
    const auto first=display(1.2,1.2),last=display(2.8,2.8);InteractionEvent event;event.viewId="slice";event.x=first[0];event.y=first[1];event.eventKind=InteractionEventKind::PrimaryPress;Require(host->binding.onInput(event).isHandled,"rectangle press not captured");
    event.x=last[0];event.y=last[1];event.eventKind=InteractionEventKind::PointerMove;Require(host->binding.onInput(event).isSucceeded,"rectangle update failed");event.eventKind=InteractionEventKind::PrimaryRelease;Require(host->binding.onInput(event).isSucceeded,"rectangle release failed");
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
    const auto control=display(2.5,1.5),moved=display(2.8,1.5);event.x=control[0];event.y=control[1];event.eventKind=InteractionEventKind::PrimaryPress;Require(host->binding.onInput(event).isHandled,"control point was not hit");event.x=moved[0];event.y=moved[1];event.eventKind=InteractionEventKind::PointerMove;Require(host->binding.onInput(event).isSucceeded,"control point move failed");event.eventKind=InteractionEventKind::PrimaryRelease;Require(host->binding.onInput(event).isSucceeded,"control point release failed");
    Require(editor->GetState().draft->nodes[0].primitive.contours[0][1][0]>.7,"control point movement changed another selection");Require(editor->SendRequest({RoiEditingAction::Undo}).error==RoiError::None && editor->GetState().tool->contours[0][1][0]==.5,"control point undo left stale handles");
    Require(editor->DetachHost() && !host->binding.onInput && main->overlays.empty() && slice->overlays.empty(),"drawing/worker detach leaked resources");
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
    Require(editor.DetachHost() && main->overlays.empty(),"identity editor leaked display resources");
}
void TestAppliedSplit(){
    auto data=std::make_shared<TestDataPort>();vtkNew<vtkImageData> image;image->SetDimensions(5,5,5);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),125,1);const auto source=data->SetPrimaryImage(image)->data->self;
    RoiRequest input;input.metadata.name="split";RoiPrimitive primitive;primitive.shape=RoiShape::VoxelSet;primitive.voxels=std::make_shared<const RoiVoxelSet>(std::vector<RoiVoxel>{{{1,1,1},1},{{3,3,3},1}});input.definition={source,{{RoiNodeKind::Primitive,primitive}}};const auto original=data->SetRoi(input);Require(original.roi.has_value(),"split editor fixture failed");
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D);main->source=source;views->views={{"main",main}};RoiEditingHostFeature editor({{"main"},{{"main"},{}}});HostFeatureContext context;context.data=data;context.views=views;Require(editor.AttachHost(context),"split editor attach failed");
    input.action=RoiAction::SetGeometry;input.expectedRoi=original.roi->revision;Require(editor.SendRequest({RoiEditingAction::Begin,input}).error==RoiError::None,"split editor begin failed");RoiEditingRequest request;request.action=RoiEditingAction::BeginOperation;RoiOperationRequest operation;RoiComponentParams params;params.operation=RoiComponentOperation::Split;operation.params=params;request.operation=operation;Require(editor.SendRequest(request).error==RoiError::Pending,"split preview failed");WaitOperation(editor);
    Require(editor.SendRequest({RoiEditingAction::ApplyOperation}).error==RoiError::None && editor.GetState().operation.rois.size()==2,"apply discarded the split output batch");const auto before=data->GetDataGraph().commitId;
    Require(editor.SendRequest({RoiEditingAction::Commit}).error==RoiError::None && data->GetDataGraph().commitId==before+1 && RoiService::GetDescriptors(data->GetDataGraph()).size()==3,"applied split was saved as one union");Require(editor.DetachHost(),"split editor detach failed");
}

void TestChunkedDisplay(){
    auto data=std::make_shared<TestDataPort>();vtkNew<vtkImageData> image;image->SetDimensions(34,4,4);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);std::fill_n(static_cast<unsigned char*>(image->GetScalarPointer()),34*4*4,1);const auto source=data->SetPrimaryImage(image)->data->self;
    auto views=std::make_shared<Views>();auto main=std::make_shared<View>("main",HostRenderViewRole::Primary3D),slice=std::make_shared<View>("slice",HostRenderViewRole::TopDownSlice);main->source=slice->source=source;views->views={{"main",main},{"slice",slice}};
    RoiEditingHostFeature editor({{"main"},{{"main","slice"},{}}});HostFeatureContext context;context.data=data;context.views=views;Require(editor.AttachHost(context),"block display attach failed");
    std::vector<RoiVoxel> voxels;for(int x=1;x<=32;++x)for(int y=1;y<=2;++y)for(int z=1;z<=2;++z)voxels.push_back({{x,y,z},1});
    RoiRequest draft;draft.metadata.name="块边界";RoiPrimitive primitive;primitive.shape=RoiShape::VoxelSet;primitive.voxels=std::make_shared<const RoiVoxelSet>(std::move(voxels));draft.definition={source,{{RoiNodeKind::Primitive,primitive}}};
    Require(editor.SendRequest({RoiEditingAction::Begin,draft}).error==RoiError::None,"block display begin failed");FeatureOverlayState state;state.cursor={0,0,1};for(const auto& overlay:slice->overlays)overlay->SetOverlayState(state);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(editor.GetState().displayStatus==RoiDisplayStatus::Updating && std::chrono::steady_clock::now()<deadline){editor.OnHostTick();std::this_thread::yield();}
    // 仅读取本 fixture 拥有的 actor，核验块面没有内部接缝且复用了顶点。
    const auto blocks=[&]{std::vector<vtkActor*> actors;auto* collection=main->renderer->GetActors();collection->InitTraversal();while(auto* actor=collection->GetNextActor())if(actor->GetVisibility() && actor->GetMapper() && vtkPolyData::SafeDownCast(actor->GetMapper()->GetInput()) && vtkPolyData::SafeDownCast(actor->GetMapper()->GetInput())->GetNumberOfPolys())actors.push_back(actor);return actors;};
    const auto initial=blocks();std::size_t faces=0,points=0;for(auto* actor:initial){auto* mesh=vtkPolyData::SafeDownCast(actor->GetMapper()->GetInput());faces+=mesh->GetNumberOfPolys();points+=mesh->GetNumberOfPoints();}
    Require(editor.GetState().displayStatus==RoiDisplayStatus::Displayed && initial.size()==2 && faces==2*(32*2+32*2+2*2) && points<faces*4,"block display created seams or repeated every face vertex");
    const auto sliceLines=[&]{vtkPolyData* result=nullptr;auto* collection=slice->renderer->GetActors();collection->InitTraversal();while(auto* actor=collection->GetNextActor())if(actor->GetVisibility() && actor->GetMapper()){auto* mesh=vtkPolyData::SafeDownCast(actor->GetMapper()->GetInput());if(mesh && mesh->GetNumberOfLines())result=mesh;}return result;};
    auto* contour=sliceLines();Require(contour,"direct slice contour did not finish");double length=0;vtkIdType count=0;const vtkIdType* ids=nullptr;auto* lines=contour->GetLines();lines->InitTraversal();while(lines->GetNextCell(count,ids)){Require(count==2,"slice emitted a non-segment");double a[3],b[3];contour->GetPoint(ids[0],a);contour->GetPoint(ids[1],b);Require(a[2]==1 && b[2]==1,"slice contour belongs to an old plane");length+=std::hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]);}
    Require(std::abs(length-2*(32+2))<1e-12,"slice contour perimeter differs from its voxel rectangle");
    state.cursor={0,0,3};for(const auto& overlay:slice->overlays)overlay->SetOverlayState(state);
    bool completed=false;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(std::chrono::steady_clock::now()<until){editor.OnHostTick();if(editor.GetState().displayStatus!=RoiDisplayStatus::Updating && !sliceLines()){completed=true;break;}std::this_thread::yield();}
    Require(completed && blocks()==initial && editor.GetState().preview->GetContains({10,1,1}),"slice movement rebuilt three-dimensional actors or changed formal membership");
    main->lease->StopLease();Require(editor.OnHostTick() && !editor.GetState().hasDraft && data->GetDataGraph().view->GetData(source),"stopped view lease accepted a late display or deleted source data");
    Require(editor.DetachHost() && main->overlays.empty() && slice->overlays.empty(),"block display resources leaked");
}

}
int main()
{
    try {TestEditor();TestCurves();TestDrawingAndLatestOperation();TestOutputIdentityAndDisplay();TestAppliedSplit();TestChunkedDisplay(); return 0;} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
