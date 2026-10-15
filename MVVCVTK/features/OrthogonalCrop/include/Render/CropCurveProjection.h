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
inline void AppendCircle(vtkRenderer* renderer,vtkMatrix4x4* matrix,vtkPolyData& output,const P& center,double radius,const P& a,const P& b,double start=0,double end=tau) {
    const auto project=[&](const P& p){return Project(renderer,matrix,p);};

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
    auto* points=output.GetPoints();auto* lines=output.GetLines();
    for(std::size_t interval=1;interval<breaks.size();++interval) {
        const double low=breaks[interval-1],high=breaks[interval],middle=(low+high)*.5;
        if(!std::all_of(planes.begin(),planes.end(),[&](const auto& p){return p[0]+p[1]*std::cos(middle)+p[2]*std::sin(middle)>=0;}))continue;
        std::vector<P> samples{point(low)};
        const auto append=[&](auto&& self,double from,double to,const P& p,const P& q,int depth)->void {
            const double angle=(from+to)*.5;const auto mid=project(point(angle));
            const double dx=q[0]-p[0],dy=q[1]-p[1],length=dx*dx+dy*dy;
            const double t=length>0?std::clamp(((mid[0]-p[0])*dx+(mid[1]-p[1])*dy)/length,0.0,1.0):0;
            const double error=std::hypot(mid[0]-p[0]-t*dx,mid[1]-p[1]-t*dy);
            if(depth<16&&std::isfinite(error)&&error>pixelError) {
                self(self,from,angle,p,mid,depth+1);self(self,angle,to,mid,q,depth+1);
            } else samples.push_back(point(to));
        };
        const int segments=std::max(1,static_cast<int>(std::ceil((high-low)*32/tau)));
        for(int i=0;i<segments;++i) {
            const double from=low+(high-low)*i/segments,to=low+(high-low)*(i+1)/segments;
            append(append,from,to,project(point(from)),project(point(to)),0);
        }
        const bool closed=low==0&&high==tau;
        const auto first=points->GetNumberOfPoints();lines->InsertNextCell(static_cast<int>(samples.size()));
        for(std::size_t i=0;i<samples.size();++i)
            lines->InsertCellPoint(closed&&i+1==samples.size()?first:points->InsertNextPoint(samples[i].data()));
    }
    points->Modified();lines->Modified();output.Modified();

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
}
