#include "Interaction/CropCurveWidget.h"
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkProperty.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPointData.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
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
    const auto initialCenter=world(operation.centerInInputModel);
    renderer->GetActiveCamera()->SetFocalPoint(initialCenter.data());
    renderer->GetActiveCamera()->SetPosition(initialCenter[0]+10,initialCenter[1]+10,initialCenter[2]+30);
    renderer->GetActiveCamera()->SetClippingRange(.1,1000);window->Render();
    vtkNew<vtkMatrix4x4> inverse;inverse->DeepCopy(matrix.data());vtkMatrix4x4::Invert(inverse,inverse);
    const auto model=[&](P p){double input[4]{p[0],p[1],p[2],1},output[4]{};inverse->MultiplyPoint(input,output);return P{output[0],output[1],output[2]};};
    const auto radiusPosition=model(position(1));P radial{};for(int i=0;i<3;++i)radial[i]=radiusPosition[i]-operation.centerInInputModel[i];
    check(near(position(0),world(operation.centerInInputModel))&&std::abs(std::hypot(radial[0],radial[1],radial[2])-operation.radius)<1e-11
        &&widget.GetHandle(0)->GetEnabled()&&widget.GetHandle(1)->GetEnabled()&&!widget.GetHandle(2)->GetEnabled(),"sphere handles project exact model center and radius");
    move(0,P{2,1,4});move(1,P{5,1,4});
    check(near(widget.GetGeometry().centerInInputModel,P{2,1,4})&&std::abs(widget.GetGeometry().radius-3)<1e-11
        &&phases.size()==6&&phases[0]==CropInteractionPhase::Hover&&phases[1]==CropInteractionPhase::Dragging
        &&phases[2]==CropInteractionPhase::Released,"sphere drag uses inverse affine without changing its model radius semantics");
    check(near(position(1),world(P{5,1,4})),"radius handle keeps the released direction under full affine");
    window->Render();check(near(position(1),world(P{5,1,4})),"render after release does not reset the radius handle");
    // 负方向是旧实现松手后跳回相机对角线的反例。
    move(1,P{-1,1,4});
    check(near(position(1),world(P{-1,1,4}))&&widget.GetGeometry().radius==3,"negative radius direction survives release");
    const auto released=position(1),centerAfterDrag=position(0);
    renderer->SetWorldPoint(released[0],released[1],released[2],1);renderer->WorldToDisplay();
    const auto* display=renderer->GetDisplayPoint();interactor->SetEventPosition(int(std::lround(display[0])),int(std::lround(display[1])));
    interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
    vtkNew<vtkCamera> savedCamera;savedCamera->DeepCopy(renderer->GetActiveCamera());
    renderer->GetActiveCamera()->SetPosition(centerAfterDrag[0]+10*(released[0]-centerAfterDrag[0]),
        centerAfterDrag[1]+10*(released[1]-centerAfterDrag[1]),centerAfterDrag[2]+10*(released[2]-centerAfterDrag[2]));
    renderer->GetActiveCamera()->SetFocalPoint(centerAfterDrag.data());window->Render();
    check(near(position(1),released),"native radius hover freezes placement when camera makes handle overlap center");
    interactor->InvokeEvent(vtkCommand::LeaveEvent);renderer->GetActiveCamera()->DeepCopy(savedCamera);window->Render();
    vtkActor* outline=nullptr;auto* actors=renderer->GetActors();actors->InitTraversal();
    while(auto* actor=actors->GetNextActor()) {
        auto* mapper=vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        if(!actor->GetPickable()&&mapper&&mapper->GetInput()&&mapper->GetInput()->GetPointData()->GetScalars())outline=actor;
    }
    P renderedCenter{};if(outline)std::copy_n(outline->GetCenter(),3,renderedCenter.data());
    check(outline&&near(renderedCenter,world(P{2,1,4})),"sphere outline and handles apply the display affine exactly once");
    bool allowEvents=false;widget.SetContextGate([&]{return allowEvents;});const auto gateBefore=widget.GetGeometry();
    move(0,P{8,8,8});check(near(widget.GetGeometry().centerInInputModel,gateBefore.centerInInputModel)
        &&near(position(0),world(gateBefore.centerInInputModel)),"context gate restores ignored native handle motion before geometry changes");
    allowEvents=true;
    operation.centerInInputModel={1e8,1e8,1e8};operation.radius=.1;
    check(widget.SetGeometry(operation,matrix),"large model center remains representable by the controller");
    const auto largeCenter=world(operation.centerInInputModel);
    renderer->GetActiveCamera()->SetFocalPoint(largeCenter.data());
    renderer->GetActiveCamera()->SetPosition(largeCenter[0]+10,largeCenter[1]+10,largeCenter[2]+30);
    renderer->GetActiveCamera()->SetClippingRange(.1,1000);window->Render();
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
