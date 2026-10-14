#include "Render/CropSphereVisual.h"
#include <vtkActor.h>
#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkSphereSource.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
using P = CropVectorDouble3Array;
constexpr int circleSegments = 96;
constexpr double tau = 6.2831853071795864769;
double Dot(const P& a, const P& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
P Cross(const P& a, const P& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
P Unit(P v) { const double length=std::hypot(v[0],v[1],v[2]); if(length>0)for(auto& x:v)x/=length; return v; }
P Transform(vtkMatrix4x4& matrix, const P& p, double w) {
    const double input[4]{p[0],p[1],p[2],w}; double result[4]{}; matrix.MultiplyPoint(input,result);
    return {result[0],result[1],result[2]};
}
P Perpendicular(const P& n) {
    P reference{}; int axis=0;
    for(int i=1;i<3;++i)if(std::abs(n[i])<std::abs(n[axis]))axis=i;
    reference[axis]=1; return Unit(Cross(n,reference));
}
void AppendCircle(vtkPoints& points, vtkCellArray& lines, const P& center,
    double radius, const P& a, const P& b) {
    std::array<vtkIdType,circleSegments+1> ids{};
    for(int i=0;i<circleSegments;++i) {
        P point{};
        for(int j=0;j<3;++j)point[j]=center[j]+radius*(a[j]*std::cos(tau*i/circleSegments)+b[j]*std::sin(tau*i/circleSegments));
        ids[i]=points.InsertNextPoint(point.data());
    }
    ids.back()=ids.front(); lines.InsertNextCell(static_cast<vtkIdType>(ids.size()),ids.data());
}
void SetCircle(vtkPolyData& output, const P& center, double radius, const P& normal) {
    vtkNew<vtkPoints> points; points->SetDataTypeToDouble(); vtkNew<vtkCellArray> lines;
    const auto a=Perpendicular(normal); AppendCircle(*points,*lines,center,radius,a,Cross(normal,a));
    output.Initialize(); output.SetPoints(points); output.SetLines(lines); output.Modified();
}
}

CropSphereVisual::CropSphereVisual() {
    m_modelToWorld=vtkSmartPointer<vtkMatrix4x4>::New(); m_worldToModel=vtkSmartPointer<vtkMatrix4x4>::New();
    m_rings=vtkSmartPointer<vtkPolyData>::New(); m_outline=vtkSmartPointer<vtkPolyData>::New();
    m_ringMapper=vtkSmartPointer<vtkPolyDataMapper>::New(); m_outlineMapper=vtkSmartPointer<vtkPolyDataMapper>::New();
    m_shellMapper=vtkSmartPointer<vtkPolyDataMapper>::New();
    m_ringMapper->SetInputData(m_rings); m_outlineMapper->SetInputData(m_outline);
    m_shell=vtkSmartPointer<vtkSphereSource>::New(); m_shell->SetOutputPointsPrecision(vtkAlgorithm::DOUBLE_PRECISION);
    m_shell->SetThetaResolution(40); m_shell->SetPhiResolution(24); m_shellMapper->SetInputConnection(m_shell->GetOutputPort());
    m_ringActor=vtkSmartPointer<vtkActor>::New(); m_ringActor->SetMapper(m_ringMapper);
    m_outlineActor=vtkSmartPointer<vtkActor>::New(); m_outlineActor->SetMapper(m_outlineMapper);
    m_shellActor=vtkSmartPointer<vtkActor>::New(); m_shellActor->SetMapper(m_shellMapper);
    for(auto* actor:{m_ringActor.GetPointer(),m_outlineActor.GetPointer(),m_shellActor.GetPointer()}) {
        actor->PickableOff(); actor->GetProperty()->LightingOff(); actor->SetUserMatrix(m_modelToWorld);
    }
    m_shellActor->GetProperty()->SetRepresentationToSurface(); m_shellActor->GetProperty()->SetSpecular(0);
    m_renderObserver=vtkSmartPointer<vtkCallbackCommand>::New(); m_renderObserver->SetClientData(this);
    m_renderObserver->SetCallback([](vtkObject*,unsigned long,void* data,void*) { static_cast<CropSphereVisual*>(data)->UpdateView(); });
    ApplyStyle();
}
CropSphereVisual::~CropSphereVisual() { DetachRenderer(); m_renderObserver->SetClientData(nullptr); }
void CropSphereVisual::AttachRenderer(vtkRenderer* renderer) {
    if(m_renderer==renderer)return;
    DetachRenderer(); m_renderer=renderer;
    if(!renderer)return;
    renderer->AddActor(m_ringActor); renderer->AddActor(m_outlineActor); renderer->AddActor(m_shellActor);
    m_renderTag=renderer->AddObserver(vtkCommand::StartEvent,m_renderObserver,1.0f);
    m_viewDirty=true; UpdateView();
}
void CropSphereVisual::DetachRenderer() {
    if(m_renderer) {
        m_renderer->RemoveObserver(m_renderTag);
        m_renderer->RemoveActor(m_ringActor); m_renderer->RemoveActor(m_outlineActor); m_renderer->RemoveActor(m_shellActor);
    }
    m_renderTag=0; m_renderer=nullptr; m_camera=nullptr; m_viewDirty=true;
}
void CropSphereVisual::SetGeometry(const P& center, double radius, vtkMatrix4x4* modelToWorld) {
    bool same=m_rings->GetNumberOfPoints()>0&&center==m_center&&radius==m_radius;
    for(int row=0;row<4&&same;++row)for(int col=0;col<4;++col)
        same=same&&modelToWorld->GetElement(row,col)==m_modelToWorld->GetElement(row,col);
    if(same){UpdateView();return;}
    m_center=center; m_radius=radius; m_modelToWorld->DeepCopy(modelToWorld);
    vtkMatrix4x4::Invert(m_modelToWorld,m_worldToModel);
    vtkNew<vtkPoints> points; points->SetDataTypeToDouble(); vtkNew<vtkCellArray> lines;
    AppendCircle(*points,*lines,center,radius,P{1,0,0},P{0,1,0});
    AppendCircle(*points,*lines,center,radius,P{1,0,0},P{0,0,1});
    AppendCircle(*points,*lines,center,radius,P{0,1,0},P{0,0,1});
    m_rings->SetPoints(points); m_rings->SetLines(lines); m_rings->Modified();
    m_shell->SetCenter(center.data()); m_shell->SetRadius(radius);
    m_radiusHandle={center[0]+radius,center[1],center[2]}; m_viewDirty=true; UpdateView();
}
void CropSphereVisual::SetVisible(bool visible) { m_visible=visible; ApplyStyle(); }
void CropSphereVisual::SetState(State state) { if(m_state==state)return; m_state=state; ApplyStyle(); }
void CropSphereVisual::ApplyStyle() {
    const bool passive=m_state==State::Passive, hover=m_state==State::Hover, dragging=m_state==State::Dragging;
    m_ringActor->GetProperty()->SetColor(passive?.65:.95,passive?.34:.38,passive?.26:.20);
    m_ringActor->GetProperty()->SetOpacity(passive?.45:(hover||dragging?.9:.72));
    m_ringActor->GetProperty()->SetLineWidth(hover||dragging?1.6f:1.3f);
    m_outlineActor->GetProperty()->SetColor(1,.72,.42);
    m_outlineActor->GetProperty()->SetOpacity(passive?.55:1.0);
    m_outlineActor->GetProperty()->SetLineWidth(hover||dragging?2.4f:2.1f);
    m_shellActor->GetProperty()->SetColor(.95,.38,.2); m_shellActor->GetProperty()->SetOpacity(dragging?.08:.04);
    m_ringActor->SetVisibility(m_visible); m_outlineActor->SetVisibility(m_visible&&m_outline->GetNumberOfPoints()>0);
    m_shellActor->SetVisibility(m_visible&&(hover||dragging));
}
void CropSphereVisual::UpdateView() {
    if(!m_renderer)return;
    auto* camera=m_renderer->GetActiveCamera();
    if(!m_viewDirty&&m_camera==camera&&m_cameraTime==camera->GetMTime())return;
    m_camera=camera; m_cameraTime=camera->GetMTime(); m_viewDirty=false;
    P normal{}, circleCenter=m_center; double circleRadius=m_radius;
    if(camera->GetParallelProjection()) {
        P direction{}; camera->GetDirectionOfProjection(direction.data());
        normal=Unit(Transform(*m_worldToModel,direction,0)); for(auto& x:normal)x=-x;
    } else {
        P eye{}; camera->GetPosition(eye.data()); eye=Transform(*m_worldToModel,eye,1);
        for(int i=0;i<3;++i)normal[i]=eye[i]-m_center[i];
        const double distance=std::hypot(normal[0],normal[1],normal[2]);
        if(distance<=m_radius) { m_outline->Initialize(); ApplyStyle(); return; }
        normal=Unit(normal); const double ratio=m_radius/distance;
        // 透视轮廓是切触圆，避免把过球心的大圆误当近距离/偏轴视角的真实边界。
        for(int i=0;i<3;++i)circleCenter[i]+=normal[i]*m_radius*ratio;
        circleRadius=m_radius*std::sqrt((1-ratio)*(1+ratio));
    }
    SetCircle(*m_outline,circleCenter,circleRadius,normal);
    P direction{}, up{}; camera->GetDirectionOfProjection(direction.data()); camera->GetViewUp(up.data());
    const auto right=Unit(Cross(direction,up)); const auto screenUp=Unit(Cross(right,direction));
    P diagonal{}; for(int i=0;i<3;++i)diagonal[i]=right[i]+screenUp[i];
    diagonal=Transform(*m_worldToModel,diagonal,0); const double along=Dot(diagonal,normal);
    for(int i=0;i<3;++i)diagonal[i]-=along*normal[i];
    diagonal=Unit(diagonal); if(Dot(diagonal,diagonal)==0)diagonal=Perpendicular(normal);
    for(int i=0;i<3;++i)m_radiusHandle[i]=circleCenter[i]+circleRadius*diagonal[i];
    ApplyStyle();
}
P CropSphereVisual::GetRadiusHandle() const { return m_radiusHandle; }
bool CropSphereVisual::GetNearOutline(int x,int y) const {
    if(!m_visible||!m_renderer||!m_outlineActor->GetVisibility())return false;
    const auto project=[this](vtkIdType id) {
        P p{}; m_outline->GetPoint(id,p.data()); p=Transform(*m_modelToWorld,p,1);
        m_renderer->SetWorldPoint(p[0],p[1],p[2],1); m_renderer->WorldToDisplay();
        const auto* display=m_renderer->GetDisplayPoint(); return P{display[0],display[1],display[2]};
    };
    for(vtkIdType i=0;i<m_outline->GetNumberOfPoints();++i) {
        const auto a=project(i), b=project((i+1)%m_outline->GetNumberOfPoints());
        if(a[2]<0||a[2]>1||b[2]<0||b[2]>1)continue;
        const double dx=b[0]-a[0],dy=b[1]-a[1],length=dx*dx+dy*dy;
        const double t=length>0?std::clamp(((x-a[0])*dx+(y-a[1])*dy)/length,0.0,1.0):0;
        if(std::hypot(x-a[0]-t*dx,y-a[1]-t*dy)<=8)return true;
    }
    return false;
}
void CropSphereVisual::BuildSection(vtkPolyData& output,const P& center,double radius,vtkPlane& plane) {
    P normal{}; plane.GetNormal(normal.data()); normal=Unit(normal);
    const auto* origin=plane.GetOrigin(); P delta{}; for(int i=0;i<3;++i)delta[i]=center[i]-origin[i];
    const double distance=Dot(delta,normal); output.Initialize();
    if(std::abs(distance)>radius)return;
    P sectionCenter{}; for(int i=0;i<3;++i)sectionCenter[i]=center[i]-distance*normal[i];
    const double ratio=std::abs(distance)/radius;
    const double sectionRadius=radius*std::sqrt((1-ratio)*(1+ratio));
    if(sectionRadius>0)SetCircle(output,sectionCenter,sectionRadius,normal);
    else {
        vtkNew<vtkPoints> points; points->SetDataTypeToDouble(); const vtkIdType id=points->InsertNextPoint(sectionCenter.data());
        vtkNew<vtkCellArray> vertices; vertices->InsertNextCell(1,&id); output.SetPoints(points); output.SetVerts(vertices);
    }
    output.Modified();
}
