#include "App/Services/AppServiceFactory.h"
#include "App/Services/AppPorts.h"
#include "App/AppState.h"
#include "App/AppStateEvents.h"
#include "Data/DataManager.h"
#include "Render/Support/BaseVisualStrategy.h"
#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkDataArray.h>
#include <vtkPointData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkPropCollection.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
class RulerData final : public RawVolumeDataManager {
public: using BaseDataManager::SetOwnedImage;
};
class RulerStrategy final : public BaseVisualStrategy {
public:
    bool isDelayed = false;
    vtkRenderer* rejectedRenderer = nullptr;
    std::array<double,16> appliedMatrix{};
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    bool SetVisualState(const RenderParams& params, UpdateFlags flags) override {
        if ((flags & UpdateFlags::Transform) != UpdateFlags::None) appliedMatrix=params.modelMatrix;
        return true;
    }
    RenderTransitionState GetTransitionState() const override {
        RenderTransitionState state;
        state.status=isDelayed?RenderProductStatus::Preparing:RenderProductStatus::Idle;
        return state;
    }
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) override {
        if(renderer==rejectedRenderer)throw std::runtime_error("ruler test rebind rejection");
        BaseVisualStrategy::AttachRenderer(renderer);
    }
};
int GetRulerCount(vtkRenderer* renderer) {
    int count=0;
    auto* props=renderer->GetViewProps(); props->InitTraversal();
    while(auto* prop=props->GetNextProp())count+=prop->IsA("RulerProp")?1:0;
    return count;
}
}

int GetRulerStateFailures() {
    int failures=0;
    const auto check=[&](bool value,const char* message){
        if(!value){++failures;std::cerr<<"FAIL: "<<message<<'\n';}return value;
    };
    auto data=std::make_shared<RulerData>();
    auto image=vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(3,3,3);image->SetSpacing(.02,.04,.06);
    image->AllocateScalars(VTK_FLOAT,1);image->GetPointData()->GetScalars()->FillComponent(0,1);
    if(!check(data->SetOwnedImage(image),"transaction seed"))return failures;
    auto events=std::make_shared<SharedStateBroadcaster>();
    auto shared=std::make_shared<SharedInteractionState>(events);
    std::shared_ptr<RulerStrategy> active,candidate;
    AppServiceArgs args;args.dataManager=data;args.interactionState=shared;args.eventSource=events;
    args.strategyCreate=[&](VizMode)->std::shared_ptr<AbstractVisualStrategy>{
        auto strategy=std::make_shared<RulerStrategy>();
        if(!active)active=strategy;else{candidate=strategy;strategy->isDelayed=true;}
        return strategy;
    };
    auto ports=CreateAppPorts(std::move(args));
    auto window=vtkSmartPointer<vtkRenderWindow>::New();window->SetOffScreenRendering(1);window->SetSize(400,300);
    auto renderer=vtkSmartPointer<vtkRenderer>::New();window->AddRenderer(renderer);
    AppViewUpdate update;update.mode=VizMode::SliceTop_down;
    if(!check(ports.renderBind->SetRenderTarget(window,renderer)
        && ports.app.view->SendViewUpdate(update) && ports.interaction.update->SendUpdates(),"initial transaction view"))return failures;
    renderer->GetActiveCamera()->SetParallelScale(25);window->Render();
    const auto old=ports.app.view->GetViewState();
    check(old.rulerState.status==RulerStatus::Visible,"old input actual draw");
    const auto oldSnapshot=data->GetPrimaryImage();
    constexpr std::uint64_t transaction=91;
    check(ports.dataStage->StartDataStage(oldSnapshot,transaction)==DataStageStatus::Preparing,"prepare candidate");
    std::array<double,16> scale{2,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    check(ports.interaction.model->SetModelMatrix(scale),"transform old image during preparation");
    ports.interaction.update->SendPendingUpdates();window->Render();
    const auto transformed=ports.app.view->GetViewState();
    check(active->appliedMatrix==scale && transformed.rulerState.status==RulerStatus::Visible
        && std::abs(transformed.rulerState.lengthMm/transformed.rulerState.lengthPixels-25.0/300)<1e-9,
        "old ruler uses actually applied transform while candidate waits");
    candidate->isDelayed=false;
    check(ports.dataStage->SetDataStageReady(oldSnapshot,transaction)==DataStageStatus::Ready
        && ports.dataStage->SetViewStage(oldSnapshot,transaction),"candidate commit");
    check(ports.dataStage->ResetViewStage(transaction),"candidate rollback");
    window->Render();
    const auto restored=ports.app.view->GetViewState();
    check(restored.rulerState.dataRevision==old.rulerState.dataRevision
        && restored.rulerState.status==RulerStatus::Visible
        && std::abs(restored.rulerState.lengthMm/restored.rulerState.lengthPixels-25.0/300)<1e-9,
        "rollback preserves transform applied after candidate preparation");
    check(ports.dataStage->ClearDataStage(transaction),"clear rolled back candidate");
    auto nextWindow=vtkSmartPointer<vtkRenderWindow>::New();nextWindow->SetOffScreenRendering(1);nextWindow->SetSize(400,300);
    auto nextRenderer=vtkSmartPointer<vtkRenderer>::New();nextWindow->AddRenderer(nextRenderer);
    active->rejectedRenderer=nextRenderer;
    check(!ports.renderBind->SetRenderTarget(nextWindow,nextRenderer),"renderer rebind failure");
    check(GetRulerCount(renderer)==1 && GetRulerCount(nextRenderer)==0,"failed rebind leaves ruler only on old renderer");
    window->Render();
    check(ports.app.view->GetViewState().rulerState.status==RulerStatus::Visible,"failed rebind retains renderable ruler");
    active->rejectedRenderer=nullptr;
    check(ports.renderBind->SetRenderTarget(nextWindow,nextRenderer),"successful rebind");
    check(GetRulerCount(renderer)==0 && GetRulerCount(nextRenderer)==1,"successful rebind moves only owned ruler");
    nextWindow->Render();
    check(ports.app.view->GetViewState().rulerState.status==RulerStatus::Visible,"rebound ruler visible");
    check(shared->StartLoad(LoadEventKind::File) && shared->SetFileLoadFailed(),"inject active file failure"); ports.interaction.update->SendPendingUpdates();
    const auto cleared=ports.app.view->GetViewState();
    check(cleared.rulerState.status==RulerStatus::NoData && cleared.rulerState.label.empty(),"load failure clears cached ruler immediately");
    std::cout<<"Ruler state failures="<<failures<<'\n';
    return failures;
}
