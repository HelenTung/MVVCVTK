#include "Render/CropCylinderVisual.h"
#include "Render/CropCurveProjection.h"
#include <vtkActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkPlane.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
namespace {
using P=CropVectorDouble3Array;
double Dot(const P& a,const P& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
P Add(P a,const P& b,double f=1){for(int j=0;j<3;++j)a[j]+=f*b[j];return a;}
P Cross(const P& a,const P& b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
P Unit(P a){double n=std::hypot(a[0],a[1],a[2]);if(n>0)for(auto& x:a)x/=n;return a;}
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
void CropCylinderVisual::SetVisible(bool v){if(m_visible==v)return;m_visible=v;if(v){m_dirty=true;UpdateView();}m_actor->SetVisibility(v);m_auxActor->SetVisibility(v);}
void CropCylinderVisual::SetRadiusActive(bool v){if(m_radiusActive==v)return;m_radiusActive=v;m_dirty=true;UpdateView();}
void CropCylinderVisual::SetGeometry(const CropOpItem& op,vtkMatrix4x4* matrix){
    bool same=op.centerInInputModel==m_op.centerInInputModel&&op.axisInInputModel==m_op.axisInInputModel&&op.radius==m_op.radius&&op.height==m_op.height;
    for(int j=0;j<4;++j)for(int k=0;k<4;++k)same=same&&matrix->GetElement(j,k)==m_matrix->GetElement(j,k);
    if(!same){m_op=op;m_matrix->DeepCopy(matrix);vtkMatrix4x4::Invert(m_matrix,m_inverse);m_dirty=true;}UpdateView();
}
void CropCylinderVisual::UpdateView(){
    if(!m_visible||!m_renderer)return;auto* camera=m_renderer->GetActiveCamera();const auto* size=m_renderer->GetSize();const auto* viewport=m_renderer->GetViewport();
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
void CropCylinderVisual::BuildSection(vtkPolyData& output,const CropOpItem& op,vtkPlane& plane,vtkRenderer* renderer,vtkMatrix4x4* matrix) {
    // 独占交线容器，切换自盒/平面网格时不修改 VTK 的共享空数组。
    if(!output.GetPoints()||output.GetNumberOfPolys()>0||output.GetNumberOfStrips()>0) {
        output.Initialize();vtkNew<vtkPoints> points;points->SetDataTypeToDouble();output.SetPoints(points);
        vtkNew<vtkCellArray> lines;output.SetLines(lines);vtkNew<vtkCellArray> vertices;output.SetVerts(vertices);
    }
    output.DeleteCells();output.GetPoints()->Reset();output.GetLines()->Reset();output.GetVerts()->Reset();output.Modified();
    if(!renderer||!matrix||!(op.radius>0)||!(op.height>0)||!std::isfinite(op.radius)||!std::isfinite(op.height))return;
    const P axis=Unit(op.axisInInputModel),normal=Unit(P{plane.GetNormal()[0],plane.GetNormal()[1],plane.GetNormal()[2]});
    if(!(Dot(axis,axis)>0)||!(Dot(normal,normal)>0))return;
    const P origin{plane.GetOrigin()[0],plane.GetOrigin()[1],plane.GetOrigin()[2]},center=op.centerInInputModel;
    const double distance=Dot(normal,Add(center,origin,-1)),q=Dot(normal,axis),radius=op.radius,half=op.height/2;
    P radial=Add(normal,axis,-q);const double s=std::hypot(radial[0],radial[1],radial[2]);
    const double tolerance=64*std::numeric_limits<double>::epsilon()*std::max({radius,half,std::abs(distance)});
    if(!std::isfinite(distance)||std::abs(distance)>std::abs(q)*half+s*radius+tolerance)return;
    if(s>0)radial=Unit(radial);else radial=Unit(Cross(axis,std::abs(axis[0])<.8?P{1,0,0}:P{0,1,0}));
    const P tangent=Cross(axis,radial);
    if(std::abs(q)>=s) {
        // 1. 正截/常规斜截为椭圆；先解析拆出有限高度内的弧段，再复用屏幕弦差与视锥裁剪。
        const double z0=-distance/q,amplitude=-radius*s/q;const P ellipseCenter=Add(center,axis,z0),a=Add(radial,axis,-s/q);
        std::vector<double> breaks{0,CropCurveProjection::tau};
        if(amplitude!=0)for(double cap:{-half,half}) {
            const double value=(cap-z0)/amplitude;if(std::abs(value)>1)continue;
            const double angle=std::acos(std::clamp(value,-1.,1.));breaks.push_back(angle);breaks.push_back(CropCurveProjection::tau-angle);
        }
        std::sort(breaks.begin(),breaks.end());breaks.erase(std::unique(breaks.begin(),breaks.end()),breaks.end());
        for(std::size_t i=1;i<breaks.size();++i)if(std::abs(z0+amplitude*std::cos((breaks[i-1]+breaks[i])*.5))<=half+tolerance)
            CropCurveProjection::AppendCircle(renderer,matrix,output,ellipseCenter,radius,a,tangent,breaks[i-1],breaks[i]);
    } else {
        // 2. 近轴向截面改用高度参数，避免除以接近零的 n·axis 产生巨大的椭圆再相消。
        const double intercept=-distance/s,derivative=-q/s;double low=-half,high=half;
        if(derivative!=0){const double a=(-radius-intercept)/derivative,b=(radius-intercept)/derivative;low=std::max(low,std::min(a,b));high=std::min(high,std::max(a,b));}
        else if(std::abs(intercept)>radius+tolerance)return;
        if(high>low) {
            const double middle=low+(high-low)*.5,extent=(high-low)*.5;
            const double x=std::clamp(std::fma(derivative,middle,intercept),-radius,radius),slope=derivative*extent/radius;
            const P base=Add(Add(center,axis,middle),radial,x),linear=Add(P{axis[0]*extent,axis[1]*extent,axis[2]*extent},radial,derivative*extent);
            for(int sign:{-1,1}) {
                if(sign==1&&derivative==0&&std::abs(x)==radius)continue;
                CropCurveProjection::AppendConic(renderer,matrix,output,base,linear,P{sign*radius*tangent[0],sign*radius*tangent[1],sign*radius*tangent[2]},x/radius,slope);
            }
        }
    }
    // 3. 与两个有限端盘相交的弦封闭截面；端盘共面时圆周已由侧面分支生成。
    if(s>0)for(double cap:{-half,half}) {
        const double x=-std::fma(q,cap,distance)/s;if(std::abs(x)>radius+tolerance)continue;
        const double bounded=std::clamp(x,-radius,radius),ratio=bounded/radius,width=radius*std::sqrt(std::max(0.,(1-ratio)*(1+ratio)));
        const P base=Add(Add(center,axis,cap),radial,bounded);
        if(width>0)CropCurveProjection::AppendLine(renderer,matrix,output,Add(base,tangent,-width),Add(base,tangent,width));
        else if(output.GetNumberOfLines()==0)CropCurveProjection::AppendPoint(renderer,matrix,output,base);
    }
    output.GetPoints()->Modified();output.GetLines()->Modified();output.GetVerts()->Modified();output.Modified();
}
P CropCylinderVisual::GetRadiusHandleDirection(const P& previous) const {
    if(!m_renderer)return previous;
    const auto* origin=m_renderer->GetOrigin();const auto* size=m_renderer->GetSize();
    const auto center=CropCurveProjection::Project(m_renderer,m_matrix,m_op.centerInInputModel);
    const auto usable=[&](const P& p){
        return std::all_of(p.begin(),p.end(),[](double x){return std::isfinite(x);})&&p[2]>=0&&p[2]<=1
            &&p[0]>=origin[0]+8&&p[0]<=origin[0]+size[0]-8&&p[1]>=origin[1]+8&&p[1]<=origin[1]+size[1]-8
            &&(!(center[2]>=0&&center[2]<=1)||std::hypot(p[0]-center[0],p[1]-center[1])>=16);
    };
    if(usable(CropCurveProjection::Project(m_renderer,m_matrix,Add(m_op.centerInInputModel,previous,m_op.radius))))return previous;
    P result=previous;double best=-1;
    const auto consider=[&](const P& p){
        auto radial=Add(p,m_op.centerInInputModel,-1);radial=Add(radial,m_op.axisInInputModel,-Dot(radial,m_op.axisInInputModel));
        const double length=std::hypot(radial[0],radial[1],radial[2]);if(!std::isfinite(length)||length==0)return;
        for(auto& x:radial)x/=length;
        const auto display=CropCurveProjection::Project(m_renderer,m_matrix,Add(m_op.centerInInputModel,radial,m_op.radius));
        if(!usable(display))return;
        const double margin=std::min({display[0]-origin[0],origin[0]+size[0]-display[0],display[1]-origin[1],origin[1]+size[1]-display[1]});
        const double score=std::isfinite(center[0])&&std::isfinite(center[1])?std::min(margin,std::hypot(display[0]-center[0],display[1]-center[1])):margin;
        if(score>best){best=score;result=radial;}
    };
    // 从已裁剪的可见弧段产生候选；投回中间截面后再验证，不让屏幕位置改变裁切体。
    for(auto* data:{m_outline.GetPointer(),m_auxiliary.GetPointer()}){
        for(vtkIdType i=0;i<data->GetNumberOfPoints();++i){P p{};data->GetPoint(i,p.data());consider(p);}
        auto* lines=data->GetLines();lines->InitTraversal();vtkIdType count=0;const vtkIdType* ids=nullptr;
        while(lines->GetNextCell(count,ids))for(vtkIdType i=1;i<count;++i){P a{},b{};data->GetPoint(ids[i-1],a.data());data->GetPoint(ids[i],b.data());
            for(double t:{.25,.5,.75})consider(Add(P{a[0]*(1-t),a[1]*(1-t),a[2]*(1-t)},b,t));}
    }
    return result;
}
