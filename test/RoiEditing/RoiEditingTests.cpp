#include "Host/RoiEditingHostFeature.h"
#include "TestDataPort.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

#include <algorithm>
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
        if (isRejected) return false;
        overlay->AttachRenderer(renderer); overlays.push_back(overlay);
        FeatureOverlayState state; state.modelToWorld=matrix; overlay->SetOverlayState(state);
        return true;
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
    const auto saved=editor.SendRequest({RoiEditingAction::Commit});
    Require(saved.error==RoiError::None && saved.roi && !editor.GetState().hasDraft && main->overlays.empty(),"formal commit failed");
    Require(editor.DetachHost() && editor.DetachHost() && data->GetData(data->GetDataGraph(),saved.roi->revision),"detach deleted formal ROI");
    Require(editor.AttachHost(context),"reattach failed");
    RoiRequest edit; edit.action=RoiAction::SetGeometry; edit.definition=saved.roi->definition;
    edit.expectedRoi=saved.roi->revision; edit.expectedCatalogRevision=saved.roi->catalogRevision;
    Require(editor.SendRequest({RoiEditingAction::Begin,edit}).error==RoiError::None,"existing ROI draft failed");
    data->SetPrimaryImage(image);
    Require(editor.SendRequest({RoiEditingAction::Commit}).error==RoiError::RevisionConflict,"changed source binding accepted");
    Require(editor.OnHostTick() && !editor.GetState().hasDraft,"stale draft not retired on owner tick");
    Require(editor.DetachHost(),"final detach failed");
}
}
int main()
{
    try {TestEditor(); return 0;} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
