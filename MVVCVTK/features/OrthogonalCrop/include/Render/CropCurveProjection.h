#pragma once
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include "OrthogonalCropTypes.h"
// 球与圆柱复用视锥裁剪和局部屏幕弦差；不参与业务几何判定。
namespace CropCurveProjection {
using P=CropVectorDouble3Array;
constexpr double tau=6.2831853071795864769,pixelError=.5;
inline P Project(vtkRenderer* renderer,vtkMatrix4x4* matrix,const P& p) {
    const double in[4]{p[0],p[1],p[2],1};double w[4];matrix->MultiplyPoint(in,w);
    renderer->SetWorldPoint(w);renderer->WorldToDisplay();const auto* d=renderer->GetDisplayPoint();return {d[0],d[1],d[2]};
}
template<class Curve>
inline void AppendSpan(vtkRenderer* renderer,vtkMatrix4x4* matrix,vtkPolyData& output,
    const Curve& point,double low,double high,int segments,bool closed=false) {
    const auto project=[&](double t){return Project(renderer,matrix,point(t));};
    std::vector<P> samples{point(low)};
    const auto append=[&](auto&& self,double from,double to,const P& p,const P& q,int depth)->void {
        const double angle=(from+to)*.5;const auto mid=project(angle);
        const double dx=q[0]-p[0],dy=q[1]-p[1],length=dx*dx+dy*dy;
        const double t=length>0?std::clamp(((mid[0]-p[0])*dx+(mid[1]-p[1])*dy)/length,0.0,1.0):0;
        const double error=std::hypot(mid[0]-p[0]-t*dx,mid[1]-p[1]-t*dy);
        if(depth<16&&std::isfinite(error)&&error>pixelError) {
            self(self,from,angle,p,mid,depth+1);self(self,angle,to,mid,q,depth+1);
        } else samples.push_back(point(to));
    };
    for(int i=0;i<segments;++i) {
        const double from=low+(high-low)*i/segments,to=low+(high-low)*(i+1)/segments;
        append(append,from,to,project(from),project(to),0);
    }
    auto* points=output.GetPoints();auto* lines=output.GetLines();
    const auto first=points->GetNumberOfPoints();lines->InsertNextCell(static_cast<int>(samples.size()));
    for(std::size_t i=0;i<samples.size();++i)
        lines->InsertCellPoint(closed&&i+1==samples.size()?first:points->InsertNextPoint(samples[i].data()));
}
inline void AppendCircle(vtkRenderer* renderer,vtkMatrix4x4* matrix,vtkPolyData& output,const P& center,double radius,const P& a,const P& b,double start=0,double end=tau) {
    const auto point=[&](double angle) { P p{}; for(int j=0;j<3;++j)p[j]=center[j]+radius*(a[j]*std::cos(angle)+b[j]*std::sin(angle));return p; };
    // 1. 齐次裁剪平面在圆上均为 c + u*cos(t) + v*sin(t)。
    // 先解析求交，只有位于视锥内的连续弧段参与屏幕弦差；不跨越近裁面或相机平面。
    vtkNew<vtkMatrix4x4> clip;
    vtkMatrix4x4::Multiply4x4(renderer->GetActiveCamera()->GetCompositeProjectionTransformMatrix(renderer->GetTiledAspectRatio(),0,1),matrix,clip);
    std::array<double,4> c{},u{},v{};
    const double input[4]{center[0],center[1],center[2],1};
    const double axisA[4]{radius*a[0],radius*a[1],radius*a[2],0},axisB[4]{radius*b[0],radius*b[1],radius*b[2],0};
    clip->MultiplyPoint(input,c.data());clip->MultiplyPoint(axisA,u.data());clip->MultiplyPoint(axisB,v.data());
    std::array<std::array<double,3>,6> planes{};
    for(int k=0;k<6;++k) {
        const int axis=k/2;const double sign=k%2?-1:1;
        for(int j=0;j<3;++j) {
            const auto& value=j==0?c:j==1?u:v;
            planes[k][j]=(k==4?0:value[3])+sign*value[axis];
        }
    }
    std::vector<double> breaks{start,end};
    for(const auto& plane:planes) {
        const double amplitude=std::hypot(plane[1],plane[2]);
        if(amplitude==0||std::abs(plane[0])>amplitude)continue;
        const double phase=std::atan2(plane[2],plane[1]),delta=std::acos(std::clamp(-plane[0]/amplitude,-1.0,1.0));
        for(double angle:{phase-delta,phase+delta}){angle=std::fmod(angle+tau,tau);if(angle>start&&angle<end)breaks.push_back(angle);}
    }
    std::sort(breaks.begin(),breaks.end());breaks.erase(std::unique(breaks.begin(),breaks.end()),breaks.end());
    for(std::size_t interval=1;interval<breaks.size();++interval) {
        const double low=breaks[interval-1],high=breaks[interval],middle=(low+high)*.5;
        if(!std::all_of(planes.begin(),planes.end(),[&](const auto& p){return p[0]+p[1]*std::cos(middle)+p[2]*std::sin(middle)>=0;}))continue;
        const int segments=std::max(1,static_cast<int>(std::ceil((high-low)*32/tau)));
        AppendSpan(renderer,matrix,output,point,low,high,segments,low==0&&high==tau);
    }
    output.GetPoints()->Modified();output.GetLines()->Modified();output.Modified();

}
inline void AppendLine(vtkRenderer* renderer,vtkMatrix4x4* matrix,vtkPolyData& output,P a,P b) {
    vtkNew<vtkMatrix4x4> clip;vtkMatrix4x4::Multiply4x4(renderer->GetActiveCamera()->GetCompositeProjectionTransformMatrix(renderer->GetTiledAspectRatio(),0,1),matrix,clip);
    const double inA[4]{a[0],a[1],a[2],1},inB[4]{b[0],b[1],b[2],1};double c[4],d[4];clip->MultiplyPoint(inA,c);clip->MultiplyPoint(inB,d);
    double low=0,high=1;
    for(int k=0;k<6;++k){const int axis=k/2;const double sign=k%2?-1:1;
        const double x=(k==4?0:c[3])+sign*c[axis],y=(k==4?0:d[3])+sign*d[axis];
        if(x<0&&y<0)return;if(x<0)low=std::max(low,x/(x-y));else if(y<0)high=std::min(high,x/(x-y));}
    if(low>high)return;P p{},q{};for(int j=0;j<3;++j){p[j]=(1-low)*a[j]+low*b[j];q[j]=(1-high)*a[j]+high*b[j];}
    const vtkIdType ids[2]{output.GetPoints()->InsertNextPoint(p.data()),output.GetPoints()->InsertNextPoint(q.data())};output.GetLines()->InsertNextCell(2,ids);
}
// 近轴向截面的稳定参数式：p(t)=center+linear*t+bulge*sqrt(1-(x+s*t)^2)。
// 每个齐次裁剪平面形如 a+b*t+c*sqrt(Q(t))，平方后为二次式；额外根只分段，仍按原式判定可见区间。
inline void AppendConic(vtkRenderer* renderer,vtkMatrix4x4* matrix,vtkPolyData& output,
    const P& center,const P& linear,const P& bulge,double x,double slope) {
    const auto root=[&](double t){const double v=std::fma(slope,t,x);return std::sqrt(std::max(0.,(1-v)*(1+v)));};
    const auto point=[&](double t){P p{};const double s=root(t);for(int j=0;j<3;++j)p[j]=center[j]+linear[j]*t+bulge[j]*s;return p;};
    vtkNew<vtkMatrix4x4> clip;vtkMatrix4x4::Multiply4x4(renderer->GetActiveCamera()->GetCompositeProjectionTransformMatrix(renderer->GetTiledAspectRatio(),0,1),matrix,clip);
    const double a[4]{center[0],center[1],center[2],1},b[4]{linear[0],linear[1],linear[2],0},c[4]{bulge[0],bulge[1],bulge[2],0};
    double ca[4],cb[4],cc[4];clip->MultiplyPoint(a,ca);clip->MultiplyPoint(b,cb);clip->MultiplyPoint(c,cc);
    std::array<std::array<double,3>,6> planes{};std::vector<double> breaks{-1,1};
    const auto addRoot=[&](double t){if(std::isfinite(t)&&t>-1&&t<1)breaks.push_back(t);};
    for(int k=0;k<6;++k) {
        const int axis=k/2;const double sign=k%2?-1:1;
        auto& plane=planes[k];plane={(k==4?0:ca[3])+sign*ca[axis],(k==4?0:cb[3])+sign*cb[axis],(k==4?0:cc[3])+sign*cc[axis]};
        if(!std::all_of(plane.begin(),plane.end(),[](double v){return std::isfinite(v);}))return;
        const double scale=std::max({std::abs(plane[0]),std::abs(plane[1]),std::abs(plane[2])});if(scale==0)continue;
        for(auto& v:plane)v/=scale;
        const double p=plane[0],q=plane[1],r=plane[2];
        const double aa=q*q+r*r*slope*slope,bb=2*p*q+2*r*r*x*slope,cc0=p*p-r*r*(1-x*x);
        if(aa==0){if(bb!=0)addRoot(-cc0/bb);continue;}
        double discriminant=bb*bb-4*aa*cc0;
        if(discriminant<0&&discriminant>-32*std::numeric_limits<double>::epsilon()*(bb*bb+4*std::abs(aa*cc0)))discriminant=0;
        if(discriminant<0)continue;
        const double value=-.5*(bb+std::copysign(std::sqrt(discriminant),bb));
        if(value==0)addRoot(-bb/(2*aa));else{addRoot(value/aa);addRoot(cc0/value);}
    }
    std::sort(breaks.begin(),breaks.end());breaks.erase(std::unique(breaks.begin(),breaks.end()),breaks.end());
    for(std::size_t i=1;i<breaks.size();++i) {
        const double middle=(breaks[i-1]+breaks[i])*.5,s=root(middle);
        if(!std::all_of(planes.begin(),planes.end(),[&](const auto& p){return p[0]+p[1]*middle+p[2]*s>=-64*std::numeric_limits<double>::epsilon();}))continue;
        AppendSpan(renderer,matrix,output,point,breaks[i-1],breaks[i],4);
    }
}
inline void AppendPoint(vtkRenderer* renderer,vtkMatrix4x4* matrix,vtkPolyData& output,const P& point) {
    const auto d=Project(renderer,matrix,point);const auto* origin=renderer->GetOrigin();const auto* size=renderer->GetSize();
    if(!std::all_of(d.begin(),d.end(),[](double v){return std::isfinite(v);})||d[2]<0||d[2]>1
        ||d[0]<origin[0]||d[0]>origin[0]+size[0]||d[1]<origin[1]||d[1]>origin[1]+size[1])return;
    const auto id=output.GetPoints()->InsertNextPoint(point.data());output.GetVerts()->InsertNextCell(1,&id);
}
}
