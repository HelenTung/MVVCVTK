#include "Render/CropCylinderVisual.h"
#include "Render/CropGeometryOverlay.h"
#include <vtkActor.h>
#include <vtkActor2D.h>
#include <vtkActor2DCollection.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCylinderSource.h>
#include <vtkCutter.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkPropCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkVersion.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
namespace {
using P=CropVectorDouble3Array;
P Display(vtkRenderer* renderer,const P& point){renderer->SetWorldPoint(point[0],point[1],point[2],1);renderer->WorldToDisplay();const auto* d=renderer->GetDisplayPoint();return {d[0],d[1],d[2]};}
double CircleError(vtkPolyData& data,vtkRenderer* renderer,double radius) {
    double maximum=0;vtkIdType count=0;const vtkIdType* ids=nullptr;data.GetLines()->InitTraversal();
    while(data.GetLines()->GetNextCell(count,ids))for(vtkIdType i=1;i<count;++i) {
        P a{},b{};data.GetPoint(ids[i-1],a.data());data.GetPoint(ids[i],b.data());
        P middle{(a[0]+b[0])*.5,(a[1]+b[1])*.5,0};const double length=std::hypot(middle[0],middle[1]);if(length==0)continue;
        middle[0]*=radius/length;middle[1]*=radius/length;
        const auto p=Display(renderer,a),q=Display(renderer,b),r=Display(renderer,middle);const double dx=q[0]-p[0],dy=q[1]-p[1],squared=dx*dx+dy*dy;
        const double t=squared>0?std::clamp(((r[0]-p[0])*dx+(r[1]-p[1])*dy)/squared,0.,1.):0;
        maximum=std::max(maximum,std::hypot(r[0]-p[0]-t*dx,r[1]-p[1]-t*dy));
    }
    return maximum;
}
vtkPolyDataMapper2D* Contour(vtkRenderer* renderer) {
    auto* actors=renderer->GetActors2D();actors->InitTraversal();
    while(auto* actor=actors->GetNextActor2D())if(auto* mapper=vtkPolyDataMapper2D::SafeDownCast(actor->GetMapper()))return mapper;
    return nullptr;
}
}
int GetCropCylinderVisualFailures() {
    int failures=0;const auto check=[&](bool value,const char* message){if(!value){++failures;std::cerr<<"Cylinder visual: "<<message<<'\n';}};
    std::cout<<"Cylinder display regression, VTK "<<vtkVersion::GetVTKVersion()<<'\n';
    vtkNew<vtkRenderer> renderer;vtkNew<vtkRenderWindow> window;window->SetOffScreenRendering(1);window->SetMultiSamples(0);window->SetSize(400,400);window->AddRenderer(renderer);
    auto* camera=renderer->GetActiveCamera();camera->ParallelProjectionOn();camera->SetPosition(0,0,80);camera->SetFocalPoint(0,0,0);camera->SetParallelScale(20);camera->SetClippingRange(.1,300);
    vtkNew<vtkMatrix4x4> identity;vtkNew<vtkPlane> plane;vtkNew<vtkPolyData> section;
    CropOpItem op;op.geometryType=CropShape::Cylinder;op.axisInInputModel={0,0,1};op.radius=10;op.height=40;
    window->Render();
    // 独立重现旧显示网格的内缩；不把它解释为裁切 predicate 的误差。
    vtkNew<vtkCylinderSource> legacy;legacy->SetRadius(10);legacy->SetHeight(40);legacy->SetResolution(64);legacy->CappingOn();
    vtkNew<vtkPlane> legacyPlane;legacyPlane->SetNormal(0,1,0);vtkNew<vtkCutter> cutter;cutter->SetInputConnection(legacy->GetOutputPort());cutter->SetCutFunction(legacyPlane);cutter->Update();
    double sag=0;vtkIdType count=0;const vtkIdType* ids=nullptr;auto* old=cutter->GetOutput();old->GetLines()->InitTraversal();
    while(old->GetLines()->GetNextCell(count,ids))for(vtkIdType i=1;i<count;++i){P a{},b{};old->GetPoint(ids[i-1],a.data());old->GetPoint(ids[i],b.data());sag=std::max(sag,10-std::hypot((a[0]+b[0])*.5,(a[2]+b[2])*.5));}
    const double expected=10*(1-std::cos(3.14159265358979323846/64));check(std::abs(sag-expected)<2e-6,"64-facet reference agrees with independent circle sag formula");
    camera->SetParallelScale(2);camera->SetPosition(9.8,0,80);camera->SetFocalPoint(9.8,0,0);plane->SetNormal(0,0,1);plane->SetOrigin(0,0,0);
    CropCylinderVisual::BuildSection(*section,op,*plane,renderer,identity);
    const double pixelError=CircleError(*section,renderer,10);
    check(section->GetNumberOfPoints()>4&&pixelError<=.500001,"1000-pixel-radius visible circle arc meets adaptive screen chord error");
    std::cout<<"Legacy sag mm="<<sag<<", adaptive visible arc error px="<<pixelError<<'\n';
    const auto valid=[&](double limit) {
        if(section->GetNumberOfPoints()==0)return false;
        for(vtkIdType i=0;i<section->GetNumberOfPoints();++i){P p{};section->GetPoint(i,p.data());if(!std::all_of(p.begin(),p.end(),[](double x){return std::isfinite(x);})
            ||std::abs(plane->EvaluateFunction(p.data()))>1e-8||std::abs(p[2])>limit+1e-8||std::hypot(p[0],p[1])>10+1e-8)return false;}
        return true;
    };
    camera->SetParallelScale(15);camera->SetPosition(0,0,80);camera->SetFocalPoint(0,0,0);
    op.height=6;plane->SetNormal(1,0,1);plane->SetOrigin(8,0,0);CropCylinderVisual::BuildSection(*section,op,*plane,renderer,identity);
    bool cap=false;section->GetLines()->InitTraversal();
    while(section->GetLines()->GetNextCell(count,ids))if(count==2){P a{},b{};section->GetPoint(ids[0],a.data());section->GetPoint(ids[1],b.data());cap=cap||(std::abs(a[2]-3)<1e-9&&std::abs(b[2]-3)<1e-9&&std::abs(a[1]-b[1])>10);}
    check(valid(3)&&cap,"oblique finite cylinder section includes the analytic cap chord");
    camera->SetPosition(80,0,0);camera->SetViewUp(0,0,1);camera->SetParallelScale(25);op.height=40;
    for(double axial:{0.,1e-14,-1e-14,.2,-.2}) {
        plane->SetNormal(1,0,axial);plane->SetOrigin(3,0,0);CropCylinderVisual::BuildSection(*section,op,*plane,renderer,identity);
        check(valid(20),"parallel and nearly parallel sections remain on plane and finite cylinder");
    }
    plane->SetNormal(1,0,0);plane->SetOrigin(10,0,0);CropCylinderVisual::BuildSection(*section,op,*plane,renderer,identity);
    check(valid(20)&&section->GetNumberOfLines()==1,"tangent axial plane retains one finite generator");
    plane->SetOrigin(11,0,0);CropCylinderVisual::BuildSection(*section,op,*plane,renderer,identity);
    check(section->GetNumberOfPoints()==0,"disjoint plane clears previous section");
    plane->SetNormal(0,0,1);plane->SetOrigin(0,0,20);CropCylinderVisual::BuildSection(*section,op,*plane,renderer,identity);
    check(valid(20)&&section->GetNumberOfLines()>0,"coplanar cap shows its circular boundary");
    op.axisInInputModel={1/std::sqrt(3.),1/std::sqrt(3.),1/std::sqrt(3.)};plane->SetOrigin(0,0,0);
    camera->SetPosition(0,0,80);camera->SetViewUp(0,1,0);camera->SetParallelScale(20);CropCylinderVisual::BuildSection(*section,op,*plane,renderer,identity);
    bool tilted=section->GetNumberOfPoints()>4;
    for(vtkIdType i=0;i<section->GetNumberOfPoints();++i){P p{};section->GetPoint(i,p.data());const double z=(p[0]+p[1]+p[2])/std::sqrt(3.);
        tilted=tilted&&std::abs(p[2])<1e-9&&std::abs(z)<=20+1e-9&&std::abs(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]-z*z-100)<1e-8;}
    check(tilted,"tilted cylinder section satisfies independent plane and radial-distance equations");op.axisInInputModel={0,0,1};
    // 两种交线参数式均在透视/裁剪/非均匀显示仿射下检查可见区间。
    vtkNew<vtkMatrix4x4> matrix;const double affine[16]{2,.3,0,3,0,-1.5,.2,1,0,0,.5,2,0,0,0,1};matrix->DeepCopy(affine);
    camera->ParallelProjectionOff();camera->SetPosition(20,12,30);camera->SetFocalPoint(3,1,2);camera->SetViewUp(0,1,0);camera->SetClippingRange(12,70);
    renderer->SetViewport(.1,.15,.9,.85);
    for(P normal:{P{0,0,1},P{1,0,.2}}) {
        plane->SetNormal(normal.data());plane->SetOrigin(0,0,0);CropCylinderVisual::BuildSection(*section,op,*plane,renderer,matrix);
        bool inside=section->GetNumberOfPoints()>0;const auto* origin=renderer->GetOrigin();const auto* size=renderer->GetSize();
        for(vtkIdType i=0;i<section->GetNumberOfPoints();++i){P p{};section->GetPoint(i,p.data());const double in[4]{p[0],p[1],p[2],1};double world[4];matrix->MultiplyPoint(in,world);const auto d=Display(renderer,{world[0],world[1],world[2]});
            inside=inside&&std::all_of(d.begin(),d.end(),[](double x){return std::isfinite(x);})&&d[2]>=-1e-8&&d[2]<=1+1e-8&&d[0]>=origin[0]-1e-6&&d[0]<=origin[0]+size[0]+1e-6&&d[1]>=origin[1]-1e-6&&d[1]<=origin[1]+size[1]+1e-6;}
        check(inside&&valid(20),"ellipse and near-axial conic clip correctly under perspective and display affine");
    }
    renderer->SetViewport(0,0,1,1);camera->ParallelProjectionOn();camera->SetPosition(0,0,80);camera->SetFocalPoint(0,0,0);camera->SetViewUp(0,1,0);camera->SetParallelScale(40);camera->SetClippingRange(.1,300);
    {
        const auto props=renderer->GetViewProps()->GetNumberOfItems();
        auto overlay=std::make_unique<CropGeometryOverlay>(HostRenderViewRole::TopDownSlice);overlay->SetOperation(op,{-15,15,-15,15,-25,25});overlay->AttachRenderer(renderer.GetPointer());window->Render();
        auto* mapper=Contour(renderer);check(mapper&&mapper->GetInput()->GetNumberOfPoints()>0,"slice overlay consumes the analytic cylinder section");
        if(mapper){const auto coarse=mapper->GetInput()->GetNumberOfPoints();camera->SetParallelScale(12);window->Render();check(mapper->GetInput()->GetNumberOfPoints()>coarse,"camera-only zoom retessellates the slice contour");
            const auto time=mapper->GetInput()->GetMTime();window->Render();check(mapper->GetInput()->GetMTime()==time,"unchanged slice render reuses contour data");
            FeatureOverlayState state;state.cursor={0,0,5};overlay->SetOverlayState(state);window->Render();bool zPlane=true;auto* data=mapper->GetInput();for(vtkIdType i=0;i<data->GetNumberOfPoints();++i){P p{};data->GetPoint(i,p.data());zPlane=zPlane&&std::abs(p[2]-5)<1e-9;}check(zPlane,"cursor update moves the analytic section onto the new slice plane");
        }
        overlay->AttachRenderer(nullptr);overlay.reset();check(renderer->GetViewProps()->GetNumberOfItems()==props,"overlay destruction detaches its props and camera observer after an empty attach");window->Render();
    }
    {
        camera->SetParallelScale(20);CropCylinderVisual visual;visual.SetGeometry(op,identity);visual.AttachRenderer(renderer);visual.SetVisible(true);window->Render();
        auto* actors=renderer->GetActors();actors->InitTraversal();auto* actor=actors->GetNextActor();auto* data=actor?vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput():nullptr;
        check(data&&data->GetNumberOfPoints()>0,"passive cylinder has visible contour data");
        if(data){const auto time=data->GetMTime();visual.SetVisible(false);op.radius=11;visual.SetGeometry(op,identity);camera->Azimuth(15);window->Render();check(data->GetMTime()==time,"hidden passive cylinder does not rebuild after geometry/camera changes");visual.SetVisible(true);window->Render();check(data->GetMTime()>time&&data->GetNumberOfPoints()>0,"restoring visibility rebuilds the latest passive contour");}
        visual.DetachRenderer();check(renderer->GetActors()->GetNumberOfItems()==0,"passive detach removes owned actors");window->Render();
    }
    window->Finalize();return failures;
}
