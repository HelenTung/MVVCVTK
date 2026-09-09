#include "Interaction/CropCurveWidget.h"
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCommand.h>
#include <vtkProperty.h>
#include <vtkHandleRepresentation.h>
#include <vtkHandleWidget.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

int GetCropCurveWidgetFailures()
{
    using P=CropVectorDouble3Array;
    int failures=0;const auto check=[&](bool value,const char* message){if(!value){++failures;std::cerr<<"Curve widget: "<<message<<'\n';}};
    const CropMatrixDouble16Array matrix{-2,.3,0,5, 0,3,.2,-2, 0,0,.5,1, 0,0,0,1};
    const auto world=[&](const P& p){P result{};for(int i=0;i<3;++i)result[i]=matrix[i*4]*p[0]+matrix[i*4+1]*p[1]+matrix[i*4+2]*p[2]+matrix[i*4+3];return result;};
    const auto near=[](const P& a,const P& b){for(int i=0;i<3;++i)if(std::abs(a[i]-b[i])>1e-11)return false;return true;};
    auto renderer=vtkSmartPointer<vtkRenderer>::New();auto window=vtkSmartPointer<vtkRenderWindow>::New();
    auto interactor=vtkSmartPointer<vtkRenderWindowInteractor>::New();
    window->SetOffScreenRendering(1);window->SetSize(200,200);window->AddRenderer(renderer);interactor->SetRenderWindow(window);
    CropCurveWidget widget;widget.SetContext(interactor,renderer);
    std::vector<CropInteractionPhase> phases;widget.SetCallback([&](auto value){phases.push_back(value);});
    const auto position=[&](int index){P value{};widget.GetHandle(index)->GetHandleRepresentation()->GetWorldPosition(value.data());return value;};
    const auto move=[&](int index,const P& value) {
        auto* handle=widget.GetHandle(index);handle->InvokeEvent(vtkCommand::StartInteractionEvent);
        auto mapped=world(value);handle->GetHandleRepresentation()->SetWorldPosition(mapped.data());
        handle->InvokeEvent(vtkCommand::InteractionEvent);handle->InvokeEvent(vtkCommand::EndInteractionEvent);
    };
    CropOpItem operation;operation.geometryType=CropShape::Sphere;operation.centerInInputModel={1,2,3};operation.radius=2;
    check(widget.SetGeometry(operation,matrix)&&widget.SetEnabled(true),"sphere context and affine accepted");
    renderer->ResetCamera();window->Render();
    check(near(position(0),world(operation.centerInInputModel))&&near(position(1),world(P{3,2,3}))
        &&widget.GetHandle(0)->GetEnabled()&&widget.GetHandle(1)->GetEnabled()&&!widget.GetHandle(2)->GetEnabled(),"sphere handles project exact model center and radius");
    move(0,P{2,1,4});move(1,P{5,1,4});
    check(near(widget.GetGeometry().centerInInputModel,P{2,1,4})&&std::abs(widget.GetGeometry().radius-3)<1e-11
        &&phases.size()==6&&phases[0]==CropInteractionPhase::Hover&&phases[1]==CropInteractionPhase::Dragging
        &&phases[2]==CropInteractionPhase::Released,"sphere drag uses inverse affine without changing its model radius semantics");
    vtkActor* outline=nullptr;auto* actors=renderer->GetActors();actors->InitTraversal();
    while(auto* actor=actors->GetNextActor())if(!actor->GetPickable()&&actor->GetProperty()->GetRepresentation()==VTK_WIREFRAME)outline=actor;
    P renderedCenter{};if(outline)std::copy_n(outline->GetCenter(),3,renderedCenter.data());
    check(outline&&near(renderedCenter,world(P{2,1,4})),"sphere outline and handles apply the display affine exactly once");
    bool allowEvents=false;widget.SetContextGate([&]{return allowEvents;});const auto gateBefore=widget.GetGeometry();
    move(0,P{8,8,8});check(near(widget.GetGeometry().centerInInputModel,gateBefore.centerInInputModel)
        &&near(position(0),world(gateBefore.centerInInputModel)),"context gate restores ignored native handle motion before geometry changes");
    allowEvents=true;
    operation.centerInInputModel={1e8,1e8,1e8};operation.radius=.1;
    check(widget.SetGeometry(operation,matrix),"large model center remains representable by the controller");
    if(outline) {
        const auto* bounds=outline->GetBounds();
        check(bounds[1]>bounds[0]&&bounds[3]>bounds[2]&&bounds[5]>bounds[4],"double outline points preserve a small sphere at a large model center");
    }
    operation.geometryType=CropShape::Cylinder;operation.centerInInputModel={0,0,0};operation.axisInInputModel={1,1,1};operation.radius=1;operation.height=2;
    check(widget.SetEnabled(false)&&widget.SetGeometry(operation,matrix)&&widget.SetEnabled(true),"finite cylinder enabled");
    window->Render();const auto initial=widget.GetGeometry();const auto lower=position(2);
    move(3,P{2,1,3});const auto changed=widget.GetGeometry();
    P expectedCenter{},delta{};for(int i=0;i<3;++i){const double bottom=-initial.axisInInputModel[i];expectedCenter[i]=(P{2,1,3}[i]+bottom)*.5;delta[i]=P{2,1,3}[i]-bottom;}
    check(near(position(2),lower)&&near(changed.centerInInputModel,expectedCenter)
        &&std::abs(changed.height-std::hypot(delta[0],delta[1],delta[2]))<1e-11
        &&changed.radius==1&&widget.GetHandle(3)->GetEnabled(),"cap drag preserves opposite cap and derives arbitrary model axis and height");
    const auto old=changed;auto invalid=matrix;invalid[0]=invalid[1]=invalid[2]=0;
    check(!widget.SetGeometry(operation,invalid)&&near(widget.GetGeometry().centerInInputModel,old.centerInInputModel),"singular transform leaves previous geometry intact");
    const auto calls=phases.size();widget.SetEnabled(false);move(0,P{9,9,9});
    check(!widget.GetEnabled()&&phases.size()==calls&&near(widget.GetGeometry().centerInInputModel,old.centerInInputModel),"disabled or detached handles cannot publish interaction");
    widget.SetContext(nullptr,nullptr);check(!widget.SetEnabled(true),"missing view context rejects enable");window->Finalize();
    return failures;
}
