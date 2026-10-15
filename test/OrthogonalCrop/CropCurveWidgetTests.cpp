#include "Interaction/CropCurveWidget.h"
#include "Algorithms/CropGeometry.h"
#include "Render/CropCylinderVisual.h"
#include <vtkCellArray.h>
#include <vtkWindowToImageFilter.h>
#include <vtkPNGWriter.h>
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
#include <limits>
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
    P expectedCenter{};double projection=0;for(int i=0;i<3;++i)projection+=(P{2,1,3}[i]+initial.axisInInputModel[i])*initial.axisInInputModel[i];
    for(int i=0;i<3;++i)expectedCenter[i]=initial.axisInInputModel[i]*(-1+projection/2);
    check(near(position(2),lower)&&near(changed.centerInInputModel,expectedCenter)
        &&std::abs(changed.height-projection)<1e-11&&near(changed.axisInInputModel,initial.axisInInputModel)
        &&changed.radius==1&&widget.GetHandle(4)->GetEnabled(),"height drag constrains to original axis and preserves opposite cap");
    // 半径柄含轴向偏移的输入必须在交互帧内回到中间截面，而非松手后修正。
    widget.SetGeometry(initial,matrix);auto* radiusHandle=widget.GetHandle(1);radiusHandle->InvokeEvent(vtkCommand::StartInteractionEvent);
    const P rawRadius{3,-1,2};auto radiusWorld=world(rawRadius);radiusHandle->GetHandleRepresentation()->SetWorldPosition(radiusWorld.data());
    radiusHandle->InvokeEvent(vtkCommand::InteractionEvent);
    P projected=rawRadius;double axial=0;for(int i=0;i<3;++i)axial+=rawRadius[i]*initial.axisInInputModel[i];
    for(int i=0;i<3;++i)projected[i]-=axial*initial.axisInInputModel[i];
    check(near(model(position(1)),projected)&&widget.GetGeometry().height==initial.height
        &&near(widget.GetGeometry().axisInInputModel,initial.axisInInputModel),"radius drag stays on center section without changing height or axis");
    const auto duringRadius=position(1);radiusHandle->InvokeEvent(vtkCommand::EndInteractionEvent);
    check(near(position(1),duringRadius),"released cylinder radius handle retains its chosen direction");
    const auto radiusBeforeInvalid=widget.GetGeometry().radius;
    move(1,P{std::numeric_limits<double>::infinity(),0,0});const auto stableHandle=position(1);
    check(widget.GetGeometry().radius==radiusBeforeInvalid&&near(stableHandle,duringRadius)
        &&std::all_of(stableHandle.begin(),stableHandle.end(),[](double v){return std::isfinite(v);}),"invalid native drag cannot corrupt the retained radial frame");
    widget.StartGesture();const auto beforeRotation=widget.GetGeometry();const auto savedRadius=position(1);
    move(4,P{4,2,-3});const auto rotated=widget.GetGeometry();
    check(!near(rotated.axisInInputModel,beforeRotation.axisInInputModel)&&rotated.height==beforeRotation.height
        &&rotated.radius==beforeRotation.radius&&near(rotated.centerInInputModel,beforeRotation.centerInInputModel),"dedicated rotation changes only axis and transported radial frame");
    widget.CancelGesture();
    check(near(widget.GetGeometry().axisInInputModel,beforeRotation.axisInInputModel)&&near(position(1),savedRadius),"cancel restores cylinder parameters and radial handle frame");
    const auto beforeView=widget.GetGeometry();const auto predicate=CropGeometry::Build(beforeView);
    renderer->GetActiveCamera()->SetPosition(8,5,20);renderer->GetActiveCamera()->SetFocalPoint(0,0,0);window->Render();
    check(near(widget.GetGeometry().axisInInputModel,beforeView.axisInInputModel)&&widget.GetGeometry().radius==beforeView.radius
        &&widget.GetGeometry().height==beforeView.height,"camera updates leave finite crop parameters unchanged");
    const auto afterPredicate=CropGeometry::Build(widget.GetGeometry());bool sameMembership=true;
    for(int z=-3;z<=3;++z)for(int y=-3;y<=3;++y)for(int x=-3;x<=3;++x)
        sameMembership&=predicate->GetInside({double(x),double(y),double(z)})==afterPredicate->GetInside({double(x),double(y),double(z)});
    check(sameMembership,"camera-only redraw preserves independent finite-cylinder membership samples");
    const auto old=widget.GetGeometry();auto invalid=matrix;invalid[0]=invalid[1]=invalid[2]=0;
    check(!widget.SetGeometry(operation,invalid)&&near(widget.GetGeometry().centerInInputModel,old.centerInInputModel),"singular transform leaves previous geometry intact");
    const auto calls=phases.size();widget.SetEnabled(false);move(0,P{9,9,9});
    check(!widget.GetEnabled()&&phases.size()==calls&&near(widget.GetGeometry().centerInInputModel,old.centerInInputModel),"disabled or detached handles cannot publish interaction");
    {
        vtkNew<vtkRenderer> view;vtkNew<vtkRenderWindow> frame;frame->SetOffScreenRendering(1);frame->SetSize(400,400);frame->SetMultiSamples(0);frame->AddRenderer(view);
        vtkNew<vtkMatrix4x4> identity;CropOpItem cylinder;cylinder.geometryType=CropShape::Cylinder;cylinder.axisInInputModel={0,1,0};cylinder.radius=2;cylinder.height=4;
        CropCylinderVisual visual;visual.SetGeometry(cylinder,identity);visual.AttachRenderer(view);visual.SetVisible(true);
        view->GetActiveCamera()->SetPosition(0,0,20);view->GetActiveCamera()->SetFocalPoint(0,0,0);view->GetActiveCamera()->SetClippingRange(.1,100);frame->Render();
        auto* actors=view->GetActors();actors->InitTraversal();auto* contour=actors->GetNextActor();auto* data=vtkPolyDataMapper::SafeDownCast(contour->GetMapper())->GetInput();
        int generators=0;bool tangent=true;vtkIdType count=0;const vtkIdType* ids=nullptr;data->GetLines()->InitTraversal();
        while(data->GetLines()->GetNextCell(count,ids))if(count==2){P a{},b{};data->GetPoint(ids[0],a.data());data->GetPoint(ids[1],b.data());
            if(std::abs(a[1]-b[1])>3.9){++generators;tangent&=std::abs(20*a[2]-4)<1e-10&&std::abs(a[0]*a[0]+a[2]*a[2]-4)<1e-10;}}
        check(generators==2&&tangent&&!contour->GetUseBounds(),"perspective cylinder side lines satisfy the analytic tangent condition and stay outside framing bounds");
        vtkNew<vtkWindowToImageFilter> pixels;pixels->SetInput(frame);pixels->ReadFrontBufferOff();pixels->Update();vtkNew<vtkPNGWriter> writer;writer->SetFileName("cylinder-perspective.png");writer->SetInputConnection(pixels->GetOutputPort());writer->Write();
        cylinder.axisInInputModel={0,0,1};visual.SetGeometry(cylinder,identity);frame->Render();
        bool endOnly=data->GetNumberOfPoints()>16;
        for(vtkIdType i=0;i<data->GetNumberOfPoints();++i){P p{};data->GetPoint(i,p.data());endOnly&=std::abs(p[2]-2)<1e-10;}
        check(endOnly,"axial view naturally shows the front circular silhouette without fabricated long sides");
        cylinder.axisInInputModel={0,1,0};visual.SetGeometry(cylinder,identity);view->GetActiveCamera()->SetPosition(0,0,1);frame->Render();
        vtkIdType sampled=0;actors->InitTraversal();while(auto* actor=actors->GetNextActor())sampled+=vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput()->GetNumberOfPoints();
        check(sampled<1000,"camera inside cylinder does not refine invisible or projection-discontinuous arcs to a global maximum");
        visual.DetachRenderer();frame->Finalize();
    }
    {
        vtkNew<vtkRenderer> view;vtkNew<vtkRenderWindow> frame;vtkNew<vtkRenderWindowInteractor> input;
        frame->SetOffScreenRendering(1);frame->SetSize(400,400);frame->AddRenderer(view);input->SetRenderWindow(frame);
        auto* camera=view->GetActiveCamera();camera->ParallelProjectionOn();camera->SetParallelScale(4);camera->SetPosition(0,0,20);camera->SetFocalPoint(0,0,0);camera->SetClippingRange(.1,100);
        CropCurveWidget side;side.SetContext(input,view);vtkNew<vtkMatrix4x4> identity;
        CropOpItem cylinder;cylinder.geometryType=CropShape::Cylinder;cylinder.axisInInputModel={0,1,0};cylinder.radius=2;cylinder.height=4;
        check(side.SetGeometry(cylinder,CropMatrixDouble16Array{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1})&&side.SetEnabled(true),"side-view cylinder accepts identity placement");frame->Render();
        const auto getPosition=[&]{P p{};side.GetHandle(1)->GetHandleRepresentation()->GetWorldPosition(p.data());return p;};
        const auto project=[&](const P& p){view->SetWorldPoint(p[0],p[1],p[2],1);view->WorldToDisplay();const auto* d=view->GetDisplayPoint();return P{d[0],d[1],d[2]};};
        auto center=project({0,0,0}),radius=project(getPosition());
        check(std::hypot(radius[0]-center[0],radius[1]-center[1])>=16,"side view restores a radius handle that would overlap the center");
        auto* handle=side.GetHandle(1);handle->InvokeEvent(vtkCommand::StartInteractionEvent);P dragged{-3,1,0};handle->GetHandleRepresentation()->SetWorldPosition(dragged.data());handle->InvokeEvent(vtkCommand::InteractionEvent);handle->InvokeEvent(vtkCommand::EndInteractionEvent);frame->Render();
        check(near(getPosition(),P{-3,0,0})&&side.GetGeometry().radius==3,"usable radius direction persists after constrained release");
        radius=project(getPosition());input->SetEventPosition(int(std::lround(radius[0])),int(std::lround(radius[1])));input->InvokeEvent(vtkCommand::MouseMoveEvent);
        const auto held=getPosition();camera->SetPosition(-20,0,0);frame->Render();
        check(near(getPosition(),held),"cylinder hover freezes radius placement while camera moves");
        input->InvokeEvent(vtkCommand::LeaveEvent);frame->Render();center=project({0,0,0});radius=project(getPosition());
        check(std::hypot(radius[0]-center[0],radius[1]-center[1])>=16&&side.GetGeometry().radius==3
            &&side.GetGeometry().height==4&&side.GetGeometry().axisInInputModel==P{0,1,0},"leaving hover restores the cylinder handle without another camera change or parameter mutation");
        side.SetEnabled(false);side.SetContext(nullptr,nullptr);frame->Finalize();
    }
    widget.SetContext(nullptr,nullptr);check(!widget.SetEnabled(true),"missing view context rejects enable");window->Finalize();
    return failures;
}
