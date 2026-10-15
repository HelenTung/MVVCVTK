#include "Render/CropCylinderVisual.h"
#include "Render/CropCurveProjection.h"
#include <vtkActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
namespace {
using P=CropVectorDouble3Array;
double Dot(const P& a,const P& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
P Add(P a,const P& b,double f=1){for(int j=0;j<3;++j)a[j]+=f*b[j];return a;}
P Cross(const P& a,const P& b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
P Unit(P a){double n=std::sqrt(Dot(a,a));if(n>0)for(auto& x:a)x/=n;return a;}
P Transform(vtkMatrix4x4* m,const double* p,double w){double in[4]{p[0],p[1],p[2],w},out[4];m->MultiplyPoint(in,out);return {out[0],out[1],out[2]};}
}
CropCylinderVisual::CropCylinderVisual(){
    m_matrix=vtkSmartPointer<vtkMatrix4x4>::New();m_inverse=vtkSmartPointer<vtkMatrix4x4>::New();
    m_outline=vtkSmartPointer<vtkPolyData>::New();m_auxiliary=vtkSmartPointer<vtkPolyData>::New();
    m_actor=vtkSmartPointer<vtkActor>::New();m_auxActor=vtkSmartPointer<vtkActor>::New();
    for(int i=0;i<2;++i){auto* data=i?m_auxiliary.GetPointer():m_outline.GetPointer();auto* actor=i?m_auxActor.GetPointer():m_actor.GetPointer();
        vtkNew<vtkPoints> points;points->SetDataTypeToDouble();data->SetPoints(points);vtkNew<vtkCellArray> lines;data->SetLines(lines);
        vtkNew<vtkPolyDataMapper> mapper;mapper->SetInputData(data);actor->SetMapper(mapper);actor->SetUserMatrix(m_matrix);
        actor->PickableOff();actor->UseBoundsOff();actor->GetProperty()->LightingOff();actor->GetProperty()->SetColor(i?.48:1,i?.30:.72,i?.20:.42);actor->GetProperty()->SetLineWidth(i?1.f:2.1f);actor->VisibilityOff();}
    m_observer=vtkSmartPointer<vtkCallbackCommand>::New();m_observer->SetClientData(this);
    m_observer->SetCallback([](vtkObject*,unsigned long,void* self,void*){static_cast<CropCylinderVisual*>(self)->UpdateView();});
}
CropCylinderVisual::~CropCylinderVisual(){DetachRenderer();m_observer->SetClientData(nullptr);}
void CropCylinderVisual::AttachRenderer(vtkRenderer* r){if(m_renderer==r)return;DetachRenderer();m_renderer=r;if(r){r->AddActor(m_actor);r->AddActor(m_auxActor);m_tag=r->AddObserver(vtkCommand::StartEvent,m_observer,1.f);m_dirty=true;UpdateView();}}
void CropCylinderVisual::DetachRenderer(){if(m_renderer){m_renderer->RemoveObserver(m_tag);m_renderer->RemoveActor(m_actor);m_renderer->RemoveActor(m_auxActor);}m_renderer=nullptr;m_camera=nullptr;m_tag=0;}
void CropCylinderVisual::SetVisible(bool v){m_visible=v;m_actor->SetVisibility(v);m_auxActor->SetVisibility(v);}
void CropCylinderVisual::SetRadiusActive(bool v){if(m_radiusActive==v)return;m_radiusActive=v;m_dirty=true;UpdateView();}
void CropCylinderVisual::SetGeometry(const CropOpItem& op,vtkMatrix4x4* matrix){
    bool same=op.centerInInputModel==m_op.centerInInputModel&&op.axisInInputModel==m_op.axisInInputModel&&op.radius==m_op.radius&&op.height==m_op.height;
    for(int j=0;j<4;++j)for(int k=0;k<4;++k)same=same&&matrix->GetElement(j,k)==m_matrix->GetElement(j,k);
    if(!same){m_op=op;m_matrix->DeepCopy(matrix);vtkMatrix4x4::Invert(m_matrix,m_inverse);m_dirty=true;}UpdateView();
}
void CropCylinderVisual::UpdateView(){
    if(!m_renderer)return;auto* camera=m_renderer->GetActiveCamera();const auto* size=m_renderer->GetSize();const auto* viewport=m_renderer->GetViewport();
    const std::array<int,2> dimensions{size[0],size[1]};const std::array<double,4> area{viewport[0],viewport[1],viewport[2],viewport[3]};
    if(!m_dirty&&m_camera==camera&&m_cameraTime==camera->GetMTime()&&m_size==dimensions&&m_viewport==area)return;
    m_dirty=false;m_camera=camera;m_cameraTime=camera->GetMTime();m_size=dimensions;m_viewport=area;
    for(auto* d:{m_outline.GetPointer(),m_auxiliary.GetPointer()}){d->GetPoints()->Reset();d->GetLines()->Reset();}
    const P a=Unit(m_op.axisInInputModel),c=m_op.centerInInputModel;const double radius=m_op.radius,h=m_op.height/2;
    const P eye=Transform(m_inverse,camera->GetPosition(),1),view=Unit(Transform(m_inverse,camera->GetDirectionOfProjection(),0));
    P u=Add(eye,c,-1);if(camera->GetParallelProjection())u=Add(P{},view,-1);u=Add(u,a,-Dot(u,a));
    const double distance=std::sqrt(Dot(u,u));
    if(distance<1e-12){P reference=std::abs(a[0])<.8?P{1,0,0}:P{0,1,0};u=Unit(Cross(a,reference));}else u=Unit(u);
    const P v=Cross(a,u);const bool tangent=camera->GetParallelProjection()?distance>1e-12:distance>radius;
    const double angle=tangent?std::acos(camera->GetParallelProjection()?0:std::clamp(radius/distance,0.,1.)):0;
    std::vector<double> breaks{0,CropCurveProjection::tau};if(tangent){breaks.push_back(angle);breaks.push_back(CropCurveProjection::tau-angle);std::sort(breaks.begin(),breaks.end());}
    for(int cap:{-1,1}){
        const auto center=Add(c,a,cap*h);const bool capFront=cap*Dot(a,camera->GetParallelProjection()?Add(P{},view,-1):Add(eye,center,-1))>0;
        for(std::size_t i=1;i<breaks.size();++i){const double mid=(breaks[i-1]+breaks[i])*.5;const P n=Add(P{},Add(P{u[0]*std::cos(mid),u[1]*std::cos(mid),u[2]*std::cos(mid)},v,std::sin(mid)));
            const bool sideFront=camera->GetParallelProjection()?Dot(n,view)<0:Dot(n,Add(eye,c,-1))>radius;
            if(!capFront&&!sideFront)continue;
            auto& out=capFront!=sideFront?*m_outline:*m_auxiliary;
            CropCurveProjection::AppendCircle(m_renderer,m_matrix,out,center,radius,u,v,breaks[i-1],breaks[i]);}
    }
    if(tangent)for(double t:{angle,-angle}){P n=Add(P{u[0]*std::cos(t),u[1]*std::cos(t),u[2]*std::cos(t)},v,std::sin(t));const P p=Add(c,n,radius);
        CropCurveProjection::AppendLine(m_renderer,m_matrix,*m_outline,Add(p,a,-h),Add(p,a,h));}
    CropCurveProjection::AppendLine(m_renderer,m_matrix,*m_auxiliary,Add(c,a,-h),Add(c,a,h));
    if(m_radiusActive||distance>1e-12)CropCurveProjection::AppendCircle(m_renderer,m_matrix,*m_auxiliary,c,radius,u,v);
    m_auxActor->GetProperty()->SetColor(m_radiusActive?.85:.48,m_radiusActive?.48:.30,m_radiusActive?.20:.20);
    for(auto* d:{m_outline.GetPointer(),m_auxiliary.GetPointer()}){d->GetPoints()->Modified();d->GetLines()->Modified();d->Modified();}
}
