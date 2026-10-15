#include "Interaction/CropCurveWidget.h"
#include "Render/CropGeometryOverlay.h"
#include <vtkActor.h>
#include <vtkActor2D.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkCubeSource.h>
#include <vtkDataArray.h>
#include <vtkHandleRepresentation.h>
#include <vtkHandleWidget.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPlaneSource.h>
#include <vtkPointData.h>
#include <vtkPointHandleRepresentation3D.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProperty.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkTextActor.h>
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
        if(data->GetPointData()->GetScalars())result.rings=actor;
        else if(data->GetNumberOfLines()>0)result.outline=actor;
        else if(data->GetNumberOfPolys()>0)result.shell=actor;
    }
    return result;
}
vtkPolyData* GetData(vtkActor* actor){return actor?vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput():nullptr;}
vtkActor2D* GetSectionActor(vtkRenderer& renderer) {
    auto* props=renderer.GetViewProps();props->InitTraversal();
    while(auto* prop=props->GetNextProp())if(auto* actor=vtkActor2D::SafeDownCast(prop);actor&&!vtkTextActor::SafeDownCast(prop))
        if(vtkPolyDataMapper2D::SafeDownCast(actor->GetMapper()))return actor;
    return nullptr;
}
}

int GetCropSphereVisualFailures() {
    std::cout<<"Sphere visual: widget geometry and states"<<std::endl;
    int failures=0;const auto check=[&](bool pass,const char* message){if(!pass){++failures;std::cerr<<"Sphere visual: "<<message<<'\n';}};
    auto renderer=vtkSmartPointer<vtkRenderer>::New();auto window=vtkSmartPointer<vtkRenderWindow>::New();
    auto interactor=vtkSmartPointer<vtkRenderWindowInteractor>::New();
    window->SetOffScreenRendering(1);window->SetSize(400,400);window->AddRenderer(renderer);interactor->SetRenderWindow(window);
    CropCurveWidget widget;widget.SetContext(interactor,renderer);
    const CropMatrixDouble16Array affine{-2,.3,0,5, 0,3,.2,-2, 0,0,.5,1, 0,0,0,1};
    vtkNew<vtkMatrix4x4> matrix;matrix->DeepCopy(affine.data());
    CropOpItem op;op.geometryType=CropShape::Sphere;op.centerInInputModel={1,2,3};op.radius=2;
    check(widget.SetGeometry(op,affine)&&widget.SetEnabled(true),"sphere enabled");
    auto* camera=renderer->GetActiveCamera();camera->ParallelProjectionOn();
    const auto initialCenter=Transform(*matrix,op.centerInInputModel);
    camera->SetFocalPoint(initialCenter.data());camera->SetPosition(initialCenter[0]+14,initialCenter[1]+10,initialCenter[2]+30);
    camera->SetParallelScale(8);camera->SetClippingRange(.1,1000);window->Render();
    auto actors=GetActors(*renderer);auto* rings=GetData(actors.rings);auto* outline=GetData(actors.outline);
    check(rings&&outline&&!actors.shell,"independent rings and outline exist without a sphere shell");
    if(rings&&outline) {
        check(rings->GetNumberOfPolys()==0&&rings->GetNumberOfLines()==3,
            "exactly three continuous closed rings without triangle wireframe");
        for(int ring=0;ring<3;++ring) {
            vtkIdType count=0;const vtkIdType* ids=nullptr;rings->GetLines()->GetCellAtId(ring,count,ids);
            P min{1e9,1e9,1e9},max{-1e9,-1e9,-1e9};
            for(vtkIdType i=0;i<count-1;++i){P p{};rings->GetPoint(ids[i],p.data());for(int j=0;j<3;++j){p[j]-=op.centerInInputModel[j];min[j]=std::min(min[j],p[j]);max[j]=std::max(max[j],p[j]);}
                check(std::abs(std::hypot(p[0],p[1],p[2])-op.radius)<tolerance,"ring points lie on model sphere");}
            const int fixedAxis=ring==0?2:ring==1?1:0;
            check(std::abs(min[fixedAxis])<tolerance&&std::abs(max[fixedAxis])<tolerance,"rings occupy XY, XZ and YZ planes");
            check(count>=33&&ids[0]==ids[count-1],"adaptively sampled ring closes at its first point");
        }
        check(actors.outline->GetProperty()->GetLineWidth()>actors.rings->GetProperty()->GetLineWidth()
            &&actors.rings->GetProperty()->GetOpacity()==1&&actors.outline->GetProperty()->GetOpacity()==1,"idle outline dominates opaque rings");
        auto* colors=rings->GetPointData()->GetScalars();double colorRange[2]{};colors->GetRange(colorRange,0);
        check(colors->GetNumberOfComponents()==3&&colorRange[0]<colorRange[1],"front and rear arcs use opaque RGB brightness");
        const auto verifyOutline=[&] {
            P eye{},direction{};camera->GetPosition(eye.data());camera->GetDirectionOfProjection(direction.data());
            vtkNew<vtkMatrix4x4> inverse;vtkMatrix4x4::Invert(matrix,inverse);
            double frontMinimum=255,rearMaximum=0;bool hasFront=false,hasRear=false;
            for(vtkIdType i=0;i<rings->GetNumberOfPoints();++i) {
                P point{},offset{},ray=direction,modelRay{};rings->GetPoint(i,point.data());const auto world=Transform(*matrix,point);
                for(int j=0;j<3;++j){offset[j]=point[j]-op.centerInInputModel[j];if(!camera->GetParallelProjection())ray[j]=world[j]-eye[j];}
                for(int row=0;row<3;++row)for(int col=0;col<3;++col)modelRay[row]+=inverse->GetElement(row,col)*ray[col];
                const auto facing=Dot(offset,modelRay),red=colors->GetComponent(i,0);
                if(facing<-tolerance){hasFront=true;frontMinimum=std::min(frontMinimum,red);}
                if(facing>tolerance){hasRear=true;rearMaximum=std::max(rearMaximum,red);}
            }
            check(hasFront&&hasRear&&frontMinimum>rearMaximum,"front arcs are brighter than rear arcs under affine parallel and perspective cameras");
            for(vtkIdType i=0;i<outline->GetNumberOfPoints();++i) {
                P p{};outline->GetPoint(i,p.data());P offset{};for(int j=0;j<3;++j)offset[j]=p[j]-op.centerInInputModel[j];
                check(std::abs(std::hypot(offset[0],offset[1],offset[2])-op.radius)<tolerance,"outline is on actual sphere");
                auto world=Transform(*matrix,p);P ray=direction;
                if(!camera->GetParallelProjection())for(int j=0;j<3;++j)ray[j]=world[j]-eye[j];
                // 世界射线乘逆仿射的线性部分；与模型球面法线垂直即为切触点。
                P modelRay{};
                for(int row=0;row<3;++row)for(int col=0;col<3;++col)modelRay[row]+=inverse->GetElement(row,col)*ray[col];
                check(std::abs(Dot(offset,modelRay))<tolerance*std::max(1.0,std::hypot(modelRay[0],modelRay[1],modelRay[2])),
                    "affine silhouette is tangent to parallel or perspective rays");
            }
        };
        verifyOutline();const auto idleTime=outline->GetMTime();window->Render();
        check(outline->GetMTime()==idleTime,"idle frame does not rebuild camera geometry");
        auto* outlinePoints=outline->GetPoints();auto* outlineLines=outline->GetLines();
        auto* ringPoints=rings->GetPoints();auto* ringLines=rings->GetLines();
        camera->Azimuth(47);camera->Elevation(26);window->Render();verifyOutline();
        check(outline->GetPoints()==outlinePoints&&outline->GetLines()==outlineLines&&rings->GetPoints()==ringPoints&&rings->GetLines()==ringLines,
            "camera updates reuse point and line containers");
        camera->ParallelProjectionOff();window->Render();verifyOutline();
        auto replacement=vtkSmartPointer<vtkCamera>::New();replacement->DeepCopy(camera);renderer->SetActiveCamera(replacement);camera=replacement;
        camera->Azimuth(18);window->Render();verifyOutline();
        P center{},radius{};widget.GetHandle(0)->GetHandleRepresentation()->GetWorldPosition(center.data());
        widget.GetHandle(1)->GetHandleRepresentation()->GetWorldPosition(radius.data());
        const auto project=[&](const P& p){renderer->SetWorldPoint(p[0],p[1],p[2],1);renderer->WorldToDisplay();const auto* d=renderer->GetDisplayPoint();return P{d[0],d[1],d[2]};};
        const auto c=project(center),r=project(radius);
        check(std::hypot(r[0]-c[0],r[1]-c[1])>=16,"two 8 px native handle hit regions remain separate");
        int redraws=0;widget.SetRenderCallback([&]{++redraws;});
        interactor->SetEventPosition(int(c[0]),int(c[1]));interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
        const auto hoverWidth=actors.outline->GetProperty()->GetLineWidth();
        check(hoverWidth>2.1f&&!GetActors(*renderer).shell&&redraws>0,"hover strengthens opaque curves without adding shell");
        interactor->InvokeEvent(vtkCommand::LeaveEvent);check(actors.outline->GetProperty()->GetLineWidth()<hoverWidth,"leaving view clears hover emphasis");
        auto* handle=widget.GetHandle(0);handle->InvokeEvent(vtkCommand::StartInteractionEvent);
        P moved=op.centerInInputModel;moved[0]+=.5;auto world=Transform(*matrix,moved);handle->GetHandleRepresentation()->SetWorldPosition(world.data());
        handle->InvokeEvent(vtkCommand::InteractionEvent);
        check(!GetActors(*renderer).shell&&actors.rings->GetProperty()->GetOpacity()==1&&widget.GetGeometry().radius==op.radius,"center drag keeps opaque curves and radius");
        handle->InvokeEvent(vtkCommand::EndInteractionEvent);check(!GetActors(*renderer).shell,"release never creates a shell");
    }
    widget.SetEnabled(false);check(renderer->GetViewProps()->GetNumberOfItems()==0,"disable removes owned props");
    camera->Azimuth(10);window->Render();
    const auto verifySection=[&](HostRenderViewRole role,int worldAxis) {
        std::cout<<"Sphere visual: slice axis="<<worldAxis<<" topology="<<vtkMapper::GetResolveCoincidentTopology()<<std::endl;
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
        window->Render();auto* section=GetSectionActor(*renderer);vtkPolyData* data=nullptr;
        if(section){auto* mapper=vtkPolyDataMapper2D::SafeDownCast(section->GetMapper());mapper->Update();data=mapper->GetInput();check(!section->GetUseBounds(),"slice auxiliary prop does not affect framing");}
        check(data&&data->GetNumberOfPoints()==96&&data->GetNumberOfPolys()==0,"slice draws one analytic intersection circle");
        vtkNew<vtkMatrix4x4> inverse;vtkMatrix4x4::Invert(matrix,inverse);
        if(data)for(vtkIdType i=0;i<data->GetNumberOfPoints();++i){P w{};data->GetPoint(i,w.data());auto p=Transform(*inverse,w);
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
        state.cursor=Transform(*matrix,op.centerInInputModel);overlay.SetOverlayState(state);window->Render();
        check(data&&data->GetNumberOfPoints()==96,"returning slice restores intersection");
        overlay.DetachRenderer(renderer);renderer->RemoveActor(surfaceActor);
    };
    const auto globalMode=vtkMapper::GetResolveCoincidentTopology();
    for(int mode:{0,1,2}) {
        vtkMapper::SetResolveCoincidentTopology(mode);
        verifySection(HostRenderViewRole::TopDownSlice,2);verifySection(HostRenderViewRole::FrontBackSlice,1);verifySection(HostRenderViewRole::LeftRightSlice,0);
        check(vtkMapper::GetResolveCoincidentTopology()==mode,"overlay construction, display and destruction preserve global topology mode");
    }
    vtkMapper::SetResolveCoincidentTopology(globalMode);
    {
        CropGeometryOverlay overlay(HostRenderViewRole::TopDownSlice);FeatureOverlayState state;
        CropOpItem shape;shape.geometryType=CropShape::Plane;shape.planeNormalInInputModel={0,0,1};
        overlay.SetOperation(shape,{-5,5,-5,5,-5,5});overlay.SetOverlayState(state);overlay.AttachRenderer(renderer);
        auto* actor=GetSectionActor(*renderer);auto* mapper=actor?vtkPolyDataMapper2D::SafeDownCast(actor->GetMapper()):nullptr;
        if(mapper)mapper->Update();auto* data=mapper?mapper->GetInput():nullptr;
        check(data&&data->GetNumberOfPolys()==0&&data->GetNumberOfLines()==4,"coplanar plane stays a boundary wireframe in foreground projection");
        for(auto type:{CropShape::Sphere,CropShape::Box,CropShape::Cylinder,CropShape::Sphere}) {
            shape.geometryType=type;shape.radius=2;shape.height=4;
            overlay.SetOperation(shape,{-5,5,-5,5,-5,5});overlay.SetOverlayState(state);if(mapper)mapper->Update();
            check(data&&data->GetNumberOfPolys()==0&&data->GetNumberOfLines()>0,"shape switches preserve the slice line pipeline");
            if(type==CropShape::Sphere)check(data&&data->GetNumberOfPoints()==96,"switching from polygon geometry restores an analytic sphere section");
        }
        overlay.DetachRenderer(renderer);
    }
    std::cout<<"Sphere visual: section transitions"<<std::endl;
    vtkNew<vtkPolyData> tangent;vtkNew<vtkPlane> plane;plane->SetNormal(0,0,1);plane->SetOrigin(0,0,2);
    CropSphereVisual::BuildSection(*tangent,{0,0,0},2,*plane);
    check(tangent->GetNumberOfPoints()==1&&tangent->GetNumberOfVerts()==1,"tangent plane displays a single point");
    auto* sectionPoints=tangent->GetPoints();plane->SetOrigin(0,0,0);
    CropSphereVisual::BuildSection(*tangent,{0,0,0},2,*plane);auto* sectionLines=tangent->GetLines();
    plane->SetOrigin(0,0,3);CropSphereVisual::BuildSection(*tangent,{0,0,0},2,*plane);
    plane->SetOrigin(0,0,0);CropSphereVisual::BuildSection(*tangent,{0,0,0},2,*plane);
    check(tangent->GetPoints()==sectionPoints&&tangent->GetLines()==sectionLines&&tangent->GetNumberOfVerts()==0,
        "section buffers survive empty and tangent transitions without stale vertices");
    {
        std::cout<<"Sphere visual: sampling and hover"<<std::endl;
        vtkNew<vtkMatrix4x4> identity;
        CropSphereVisual visual;visual.SetGeometry({0,0,0},2,identity);visual.SetVisible(true);
        camera->ParallelProjectionOn();camera->SetPosition(0,0,20);camera->SetFocalPoint(0,0,0);camera->SetViewUp(0,1,0);camera->SetParallelScale(3);
        camera->SetClippingRange(.1,1000);
        visual.AttachRenderer(renderer);window->Render();auto direct=GetActors(*renderer);
        check(direct.rings&&GetData(direct.rings)->GetNumberOfLines()==2,"face-on ring is suppressed where it duplicates the silhouette");
        visual.SetRadiusHandleDirection({0,0,1});visual.SetState(CropSphereVisual::State::Hover);
        camera->SetParallelScale(3.1);window->Render();
        check(visual.GetRadiusHandle()==P{0,0,2},"hover freezes automatic placement even when handle projects onto center");
        const auto hoverGeometryTime=GetData(direct.outline)->GetMTime();
        visual.SetState(CropSphereVisual::State::Idle);window->Render();
        check(visual.GetRadiusHandle()!=P{0,0,2},"unhovered unusable handle can move back to silhouette");
        check(GetData(direct.outline)->GetMTime()==hoverGeometryTime,"unhover restores handle without rebuilding cached curves");
        // 400 px 视口中的可见圆弧来自 2000 px 半径，超过旧 96 段的半像素弦差目标。
        camera->SetPosition(2,0,20);camera->SetFocalPoint(2,0,0);camera->SetParallelScale(.2);window->Render();
        auto* sampled=GetData(direct.outline);const auto count=sampled->GetNumberOfPoints();
        const auto project=[&](const P& p){renderer->SetWorldPoint(p[0],p[1],p[2],1);renderer->WorldToDisplay();const auto* d=renderer->GetDisplayPoint();return P{d[0],d[1],d[2]};};
        double maxError=0;
        vtkIdType cellSize=0;const vtkIdType* ids=nullptr;sampled->GetLines()->InitTraversal();
        while(sampled->GetLines()->GetNextCell(cellSize,ids))for(vtkIdType i=1;i<cellSize;++i) {
            P a{},b{},mid{};sampled->GetPoint(ids[i-1],a.data());sampled->GetPoint(ids[i],b.data());
            for(int j=0;j<3;++j)mid[j]=a[j]+b[j];const auto length=std::hypot(mid[0],mid[1],mid[2]);for(auto& value:mid)value*=2/length;
            const auto p=project(a),q=project(b),m=project(mid);
            maxError=std::max(maxError,std::hypot(m[0]-(p[0]+q[0])*.5,m[1]-(p[1]+q[1])*.5));
        }
        check(count>1&&maxError<=.5,"visible local arc meets the requested half-pixel chord error");
        const auto handle=project(visual.GetRadiusHandle());
        check(handle[0]>=8&&handle[0]<=392&&handle[1]>=8&&handle[1]<=392&&handle[2]>=0&&handle[2]<=1,
            "visible local boundary provides an operable fallback radius handle");
        camera->ParallelProjectionOff();camera->SetPosition(0,0,1);camera->SetFocalPoint(0,0,0);window->Render();
        auto* visible=GetData(direct.rings);
        check(visible->GetNumberOfPoints()>0,"inside-sphere perspective retains visible ring arcs");
        for(vtkIdType i=0;i<visible->GetNumberOfPoints();++i) {
            P point{};visible->GetPoint(i,point.data());const auto screen=project(point);
            check(std::all_of(screen.begin(),screen.end(),[](double value){return std::isfinite(value);})
                &&screen[2]>=-tolerance&&screen[2]<=1+tolerance&&screen[0]>=-tolerance&&screen[0]<=400+tolerance&&screen[1]>=-tolerance&&screen[1]<=400+tolerance,
                "perspective sampling excludes invisible and discontinuous projection intervals");
        }
        std::cout<<"Sphere visual: inside perspective ring points="<<visible->GetNumberOfPoints()<<'\n';
        camera->ParallelProjectionOn();camera->SetPosition(2,0,20);camera->SetFocalPoint(2,0,0);window->Render();
        const auto beforeResize=sampled->GetMTime();window->SetSize(600,600);window->Render();
        check(sampled->GetMTime()>beforeResize,"viewport resize invalidates display sampling");window->SetSize(400,400);
        visual.DetachRenderer();
    }
    {
        std::cout<<"Sphere visual: framing"<<std::endl;
        // 工件为解析立方体；移动/放大辅助球不应改变 renderer 按工件得到的相机。
        vtkNew<vtkCubeSource> piece;piece->SetBounds(-1,1,-1,1,-1,1);
        vtkNew<vtkPolyDataMapper> pieceMapper;pieceMapper->SetInputConnection(piece->GetOutputPort());
        vtkNew<vtkActor> pieceActor;pieceActor->SetMapper(pieceMapper);renderer->AddActor(pieceActor);
        camera->SetPosition(0,0,20);camera->SetFocalPoint(0,0,0);camera->SetParallelScale(3);renderer->ResetCamera();
        P expectedPosition{},expectedFocus{};camera->GetPosition(expectedPosition.data());camera->GetFocalPoint(expectedFocus.data());
        const auto expectedScale=camera->GetParallelScale();
        const CropMatrixDouble16Array identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        auto farSphere=op;farSphere.centerInInputModel={100,20,0};farSphere.radius=40;
        widget.SetGeometry(farSphere,identity);widget.SetEnabled(true);window->Render();renderer->ResetCamera();
        P actualPosition{},actualFocus{};camera->GetPosition(actualPosition.data());camera->GetFocalPoint(actualFocus.data());
        check(actualPosition==expectedPosition&&actualFocus==expectedFocus&&camera->GetParallelScale()==expectedScale,
            "large displaced sphere, outline and native handles do not change workpiece framing");
        widget.SetEnabled(false);renderer->RemoveActor(pieceActor);
    }
    {
        std::cout<<"Sphere visual: passive overlay"<<std::endl;
        camera->ParallelProjectionOn();camera->SetFocalPoint(op.centerInInputModel.data());
        camera->SetPosition(op.centerInInputModel[0]+14,op.centerInInputModel[1]+10,op.centerInInputModel[2]+30);
        camera->SetParallelScale(8);camera->SetClippingRange(.1,1000);
        CropGeometryOverlay passive(HostRenderViewRole::Primary3D);passive.SetOperation(op,{-5,5,-5,5,-5,5});passive.AttachRenderer(renderer);
        window->Render();const auto passiveActors=GetActors(*renderer);
        check(passiveActors.rings&&passiveActors.outline&&!passiveActors.shell
            &&passiveActors.rings->GetProperty()->GetOpacity()==1,"passive sphere uses dim opaque curves without shell or handles");
        passive.SetSphereEditing(true);check(passiveActors.rings&&!passiveActors.rings->GetVisibility(),"editing sphere suppresses duplicate passive volume geometry");
        passive.DetachRenderer(renderer);
    }
    check(renderer->GetViewProps()->GetNumberOfItems()==0,"overlay detach removes all owned resources");
    widget.SetContext(nullptr,nullptr);window->Finalize();return failures;
}
