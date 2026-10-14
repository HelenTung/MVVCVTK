#include "Interaction/CropCurveWidget.h"
#include "Render/CropGeometryOverlay.h"
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkHandleRepresentation.h>
#include <vtkHandleWidget.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPlaneSource.h>
#include <vtkPointHandleRepresentation3D.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkWindowToImageFilter.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

namespace {
using P=CropVectorDouble3Array;
// 解析几何断言使用 double 舍入量级，沿用现有 widget 回归的绝对容差。
constexpr double tolerance=1e-11;
double Dot(const P& a,const P& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
P Transform(vtkMatrix4x4& matrix,const P& p){double v[4]{p[0],p[1],p[2],1},r[4]{};matrix.MultiplyPoint(v,r);return {r[0],r[1],r[2]};}
struct Actors {
    vtkActor* rings=nullptr;vtkActor* outline=nullptr;vtkActor* shell=nullptr;
};
Actors GetActors(vtkRenderer& renderer) {
    Actors result;auto* actors=renderer.GetActors();actors->InitTraversal();
    while(auto* actor=actors->GetNextActor()) {
        auto* mapper=vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        if(actor->GetPickable()||!mapper)continue;
        mapper->Update();auto* data=mapper->GetInput();if(!data)continue;
        if(data->GetNumberOfLines()==3)result.rings=actor;
        else if(data->GetNumberOfLines()==1)result.outline=actor;
        else if(data->GetNumberOfPolys()>0)result.shell=actor;
    }
    return result;
}
vtkPolyData* GetData(vtkActor* actor){return actor?vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput():nullptr;}
}

int GetCropSphereVisualFailures() {
    int failures=0;const auto check=[&](bool pass,const char* message){if(!pass){++failures;std::cerr<<"Sphere visual: "<<message<<'\n';}};
    auto renderer=vtkSmartPointer<vtkRenderer>::New();auto window=vtkSmartPointer<vtkRenderWindow>::New();
    auto interactor=vtkSmartPointer<vtkRenderWindowInteractor>::New();
    window->SetOffScreenRendering(1);window->SetSize(400,400);window->AddRenderer(renderer);interactor->SetRenderWindow(window);
    CropCurveWidget widget;widget.SetContext(interactor,renderer);
    const CropMatrixDouble16Array affine{-2,.3,0,5, 0,3,.2,-2, 0,0,.5,1, 0,0,0,1};
    vtkNew<vtkMatrix4x4> matrix;matrix->DeepCopy(affine.data());
    CropOpItem op;op.geometryType=CropShape::Sphere;op.centerInInputModel={1,2,3};op.radius=2;
    check(widget.SetGeometry(op,affine)&&widget.SetEnabled(true),"sphere enabled");
    auto* camera=renderer->GetActiveCamera();camera->ParallelProjectionOn();renderer->ResetCamera();window->Render();
    auto actors=GetActors(*renderer);auto* rings=GetData(actors.rings);auto* outline=GetData(actors.outline);
    check(rings&&outline&&actors.shell,"independent ring, outline and shell actors exist");
    if(rings&&outline&&actors.shell) {
        check(rings->GetNumberOfPoints()==288&&rings->GetNumberOfPolys()==0&&rings->GetNumberOfLines()==3,
            "exactly three continuous closed rings without triangle wireframe");
        for(int ring=0;ring<3;++ring) {
            P min{1e9,1e9,1e9},max{-1e9,-1e9,-1e9};
            for(int i=0;i<96;++i){P p{};rings->GetPoint(ring*96+i,p.data());for(int j=0;j<3;++j){p[j]-=op.centerInInputModel[j];min[j]=std::min(min[j],p[j]);max[j]=std::max(max[j],p[j]);}
                check(std::abs(std::hypot(p[0],p[1],p[2])-op.radius)<tolerance,"ring points lie on model sphere");}
            const int fixedAxis=ring==0?2:ring==1?1:0;
            check(std::abs(min[fixedAxis])<tolerance&&std::abs(max[fixedAxis])<tolerance,"rings occupy XY, XZ and YZ planes");
            vtkIdType count=0;const vtkIdType* ids=nullptr;rings->GetLines()->GetCellAtId(ring,count,ids);
            check(count==97&&ids[0]==ids[count-1],"ring polyline closes at its first point");
        }
        check(actors.outline->GetProperty()->GetLineWidth()>actors.rings->GetProperty()->GetLineWidth()
            &&!actors.shell->GetVisibility(),"idle outline dominates rings and has no shell");
        const auto verifyOutline=[&] {
            P eye{},direction{};camera->GetPosition(eye.data());camera->GetDirectionOfProjection(direction.data());
            for(vtkIdType i=0;i<outline->GetNumberOfPoints();++i) {
                P p{};outline->GetPoint(i,p.data());P offset{};for(int j=0;j<3;++j)offset[j]=p[j]-op.centerInInputModel[j];
                check(std::abs(std::hypot(offset[0],offset[1],offset[2])-op.radius)<tolerance,"outline is on actual sphere");
                auto world=Transform(*matrix,p);P ray=direction;
                if(!camera->GetParallelProjection())for(int j=0;j<3;++j)ray[j]=world[j]-eye[j];
                // 世界射线乘逆仿射的线性部分；与模型球面法线垂直即为切触点。
                vtkNew<vtkMatrix4x4> inverse;vtkMatrix4x4::Invert(matrix,inverse);P modelRay{};
                for(int row=0;row<3;++row)for(int col=0;col<3;++col)modelRay[row]+=inverse->GetElement(row,col)*ray[col];
                check(std::abs(Dot(offset,modelRay))<tolerance*std::max(1.0,std::hypot(modelRay[0],modelRay[1],modelRay[2])),
                    "affine silhouette is tangent to parallel or perspective rays");
            }
        };
        verifyOutline();const auto idleTime=outline->GetMTime();window->Render();
        check(outline->GetMTime()==idleTime,"idle frame does not rebuild camera geometry");
        camera->Azimuth(47);camera->Elevation(26);window->Render();verifyOutline();
        camera->ParallelProjectionOff();window->Render();verifyOutline();
        auto replacement=vtkSmartPointer<vtkCamera>::New();replacement->DeepCopy(camera);renderer->SetActiveCamera(replacement);camera=replacement;
        camera->Azimuth(18);window->Render();verifyOutline();
        P center{},radius{};widget.GetHandle(0)->GetHandleRepresentation()->GetWorldPosition(center.data());
        widget.GetHandle(1)->GetHandleRepresentation()->GetWorldPosition(radius.data());
        const auto project=[&](const P& p){renderer->SetWorldPoint(p[0],p[1],p[2],1);renderer->WorldToDisplay();const auto* d=renderer->GetDisplayPoint();return P{d[0],d[1],d[2]};};
        const auto c=project(center),r=project(radius);
        check(std::hypot(r[0]-c[0],r[1]-c[1])>20,"camera-facing radius handle remains separate from center");
        int redraws=0;widget.SetRenderCallback([&]{++redraws;});
        interactor->SetEventPosition(int(c[0]),int(c[1]));interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
        check(actors.shell->GetVisibility()&&std::abs(actors.shell->GetProperty()->GetOpacity()-.04)<tolerance&&redraws>0,"hover shows faint shell through redraw request");
        interactor->InvokeEvent(vtkCommand::LeaveEvent);check(!actors.shell->GetVisibility(),"leaving view clears hover shell");
        auto* handle=widget.GetHandle(0);handle->InvokeEvent(vtkCommand::StartInteractionEvent);
        P moved=op.centerInInputModel;moved[0]+=.5;auto world=Transform(*matrix,moved);handle->GetHandleRepresentation()->SetWorldPosition(world.data());
        handle->InvokeEvent(vtkCommand::InteractionEvent);
        check(actors.shell->GetVisibility()&&std::abs(actors.shell->GetProperty()->GetOpacity()-.08)<tolerance&&widget.GetGeometry().radius==op.radius,"center drag uses stronger shell and keeps radius");
        handle->InvokeEvent(vtkCommand::EndInteractionEvent);check(!actors.shell->GetVisibility(),"release restores idle shell state");
    }
    widget.SetEnabled(false);check(renderer->GetViewProps()->GetNumberOfItems()==0,"disable removes owned props");
    camera->Azimuth(10);window->Render();
    const auto verifySection=[&](HostRenderViewRole role,int worldAxis) {
        CropGeometryOverlay overlay(role);overlay.SetOperation(op,{-5,5,-5,5,-5,5});overlay.AttachRenderer(renderer);
        FeatureOverlayState state;state.modelToWorld=affine;state.cursor=Transform(*matrix,op.centerInInputModel);
        overlay.SetOverlayState(state);
        // 灰色实面与交圆共面，复现实际图像切片遮住交圆的深度冲突。
        vtkNew<vtkPlaneSource> surface;P origin=state.cursor,a=state.cursor,b=state.cursor;
        const int axisA=(worldAxis+1)%3,axisB=(worldAxis+2)%3;
        origin[axisA]-=10;origin[axisB]-=10;a=origin;b=origin;a[axisA]+=20;b[axisB]+=20;
        surface->SetOrigin(origin.data());surface->SetPoint1(a.data());surface->SetPoint2(b.data());
        vtkNew<vtkPolyDataMapper> surfaceMapper;surfaceMapper->SetInputConnection(surface->GetOutputPort());
        vtkNew<vtkActor> surfaceActor;surfaceActor->SetMapper(surfaceMapper);surfaceActor->GetProperty()->SetColor(.25,.25,.25);surfaceActor->GetProperty()->LightingOff();renderer->AddActor(surfaceActor);
        P eye=state.cursor,up{};eye[worldAxis]+=30;up[axisB]=1;
        camera->ParallelProjectionOn();camera->SetPosition(eye.data());camera->SetFocalPoint(state.cursor.data());camera->SetViewUp(up.data());renderer->ResetCamera();
        window->Render();auto section=GetActors(*renderer);auto* data=GetData(section.outline);
        check(data&&data->GetNumberOfPoints()==96&&data->GetNumberOfPolys()==0,"slice draws one analytic intersection circle");
        if(data)for(vtkIdType i=0;i<data->GetNumberOfPoints();++i){P p{};data->GetPoint(i,p.data());const auto w=Transform(*matrix,p);
            check(std::abs(w[worldAxis]-state.cursor[worldAxis])<tolerance,"section points lie on current world slice");
            for(int j=0;j<3;++j)p[j]-=op.centerInInputModel[j];check(std::abs(std::hypot(p[0],p[1],p[2])-op.radius)<tolerance,"section points lie on model sphere");}
        const auto redPixels=[&] {
            vtkNew<vtkWindowToImageFilter> capture;capture->SetInput(window);capture->ReadFrontBufferOff();capture->Update();
            auto* image=capture->GetOutput();const auto* dimensions=image->GetDimensions();int red=0;
            for(int y=0;y<dimensions[1];++y)for(int x=0;x<dimensions[0];++x) {
                const auto r=image->GetScalarComponentAsDouble(x,y,0,0),g=image->GetScalarComponentAsDouble(x,y,0,1),b=image->GetScalarComponentAsDouble(x,y,0,2);
                if(r>2*g&&r>2*b)++red;
            }
            return red;
        };
        check(redPixels()>0,"intersection remains visible over coplanar slice surface");
        state.cursor[worldAxis]+=20;overlay.SetOverlayState(state);window->Render();
        check(data&&data->GetNumberOfPoints()==0,"disjoint slice clears previous contour");
        check(redPixels()==0,"disjoint slice has no stale red contour pixels");
        state.cursor=Transform(*matrix,op.centerInInputModel);overlay.SetOverlayState(state);
        check(data&&data->GetNumberOfPoints()==96,"returning slice restores intersection");
        overlay.DetachRenderer(renderer);renderer->RemoveActor(surfaceActor);
    };
    verifySection(HostRenderViewRole::TopDownSlice,2);verifySection(HostRenderViewRole::FrontBackSlice,1);verifySection(HostRenderViewRole::LeftRightSlice,0);
    vtkNew<vtkPolyData> tangent;vtkNew<vtkPlane> plane;plane->SetNormal(0,0,1);plane->SetOrigin(0,0,2);
    CropSphereVisual::BuildSection(*tangent,{0,0,0},2,*plane);
    check(tangent->GetNumberOfPoints()==1&&tangent->GetNumberOfVerts()==1,"tangent plane displays a single point");
    {
        CropGeometryOverlay passive(HostRenderViewRole::Primary3D);passive.SetOperation(op,{-5,5,-5,5,-5,5});passive.AttachRenderer(renderer);
        renderer->ResetCamera();window->Render();const auto passiveActors=GetActors(*renderer);
        check(passiveActors.rings&&passiveActors.outline&&passiveActors.shell&&!passiveActors.shell->GetVisibility()
            &&passiveActors.rings->GetProperty()->GetOpacity()<.5,"passive sphere has dim rings and outline without shell or handles");
        passive.SetSphereEditing(true);check(passiveActors.rings&&!passiveActors.rings->GetVisibility(),"editing sphere suppresses duplicate passive volume geometry");
        passive.DetachRenderer(renderer);
    }
    check(renderer->GetViewProps()->GetNumberOfItems()==0,"overlay detach removes all owned resources");
    widget.SetContext(nullptr,nullptr);window->Finalize();return failures;
}
