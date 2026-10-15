#include "Render/CropSphereVisual.h"
#include <vtkActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkUnsignedCharArray.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
using P = CropVectorDouble3Array;
constexpr int circleSegments = 96;
constexpr double tau = 6.2831853071795864769;
// 显示层弦差目标来自球控件方案，单位为 display pixel，不参与裁切精度。
constexpr double pixelError = .5;
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
    output.GetPoints()->Reset(); output.GetLines()->Reset();
    const auto a=Perpendicular(normal); AppendCircle(*output.GetPoints(),*output.GetLines(),center,radius,a,Cross(normal,a));
    output.GetPoints()->Modified(); output.GetLines()->Modified(); output.Modified();
}
}

CropSphereVisual::CropSphereVisual() {
    m_modelToWorld=vtkSmartPointer<vtkMatrix4x4>::New(); m_worldToModel=vtkSmartPointer<vtkMatrix4x4>::New();
    m_rings=vtkSmartPointer<vtkPolyData>::New(); m_outline=vtkSmartPointer<vtkPolyData>::New();
    m_ringMapper=vtkSmartPointer<vtkPolyDataMapper>::New(); m_outlineMapper=vtkSmartPointer<vtkPolyDataMapper>::New();
    m_ringMapper->SetInputData(m_rings); m_outlineMapper->SetInputData(m_outline);
    for(auto* data:{m_rings.GetPointer(),m_outline.GetPointer()}) {
        vtkNew<vtkPoints> points; points->SetDataTypeToDouble(); data->SetPoints(points);
        vtkNew<vtkCellArray> lines; data->SetLines(lines);
    }
    m_ringColors=vtkSmartPointer<vtkUnsignedCharArray>::New(); m_ringColors->SetNumberOfComponents(3);
    m_rings->GetPointData()->SetScalars(m_ringColors);
    m_ringMapper->SetColorModeToDirectScalars(); m_ringMapper->SetScalarModeToUsePointData();
    m_ringActor=vtkSmartPointer<vtkActor>::New(); m_ringActor->SetMapper(m_ringMapper);
    m_outlineActor=vtkSmartPointer<vtkActor>::New(); m_outlineActor->SetMapper(m_outlineMapper);
    for(auto* actor:{m_ringActor.GetPointer(),m_outlineActor.GetPointer()}) {
        actor->PickableOff(); actor->UseBoundsOff(); actor->GetProperty()->LightingOff(); actor->SetUserMatrix(m_modelToWorld);
    }
    m_renderObserver=vtkSmartPointer<vtkCallbackCommand>::New(); m_renderObserver->SetClientData(this);
    m_renderObserver->SetCallback([](vtkObject*,unsigned long,void* data,void*) { static_cast<CropSphereVisual*>(data)->UpdateView(); });
    ApplyStyle();
}
CropSphereVisual::~CropSphereVisual() { DetachRenderer(); m_renderObserver->SetClientData(nullptr); }
void CropSphereVisual::AttachRenderer(vtkRenderer* renderer) {
    if(m_renderer==renderer)return;
    DetachRenderer(); m_renderer=renderer;
    if(!renderer)return;
    renderer->AddActor(m_ringActor); renderer->AddActor(m_outlineActor);
    m_renderTag=renderer->AddObserver(vtkCommand::StartEvent,m_renderObserver,1.0f);
    m_viewDirty=true; UpdateView();
}
void CropSphereVisual::DetachRenderer() {
    if(m_renderer) {
        m_renderer->RemoveObserver(m_renderTag);
        m_renderer->RemoveActor(m_ringActor); m_renderer->RemoveActor(m_outlineActor);
    }
    m_renderTag=0; m_renderer=nullptr; m_camera=nullptr; m_viewDirty=true;
}
void CropSphereVisual::SetGeometry(const P& center, double radius, vtkMatrix4x4* modelToWorld) {
    bool same=center==m_center&&radius==m_radius;
    for(int row=0;row<4&&same;++row)for(int col=0;col<4;++col)
        same=same&&modelToWorld->GetElement(row,col)==m_modelToWorld->GetElement(row,col);
    if(same){UpdateView();return;}
    m_center=center; m_radius=radius; m_modelToWorld->DeepCopy(modelToWorld);
    vtkMatrix4x4::Invert(m_modelToWorld,m_worldToModel);
    const auto direction=m_hasHandleDirection?m_handleDirection:P{1,0,0};
    for(int i=0;i<3;++i)m_radiusHandle[i]=center[i]+radius*direction[i];
    m_viewDirty=true; UpdateView();
}
void CropSphereVisual::SetVisible(bool visible) { m_visible=visible; ApplyStyle(); }
void CropSphereVisual::SetState(State state) { if(m_state==state)return; m_state=state; ApplyStyle(); }
void CropSphereVisual::SetRadiusHandleDirection(const P& direction) {
    if(!std::isfinite(Dot(direction,direction))||Dot(direction,direction)==0)return;
    m_handleDirection=Unit(direction); m_hasHandleDirection=true;
    for(int i=0;i<3;++i)m_radiusHandle[i]=m_center[i]+m_radius*m_handleDirection[i];
}
void CropSphereVisual::ApplyStyle() {
    const bool passive=m_state==State::Passive, hover=m_state==State::Hover, dragging=m_state==State::Dragging;
    m_ringColors->SetNumberOfTuples(m_rings->GetNumberOfPoints());
    for(vtkIdType i=0;i<m_rings->GetNumberOfPoints();++i) {
        P point{};m_rings->GetPoint(i,point.data());
        for(int j=0;j<3;++j)point[j]-=m_center[j];
        const bool front=Dot(point,m_viewNormal)>=m_tangentOffset;
        const double brightness=(front?1.0:.42)*(passive?.65:(hover||dragging?1.0:.85));
        const unsigned char color[]{static_cast<unsigned char>(242*brightness),
            static_cast<unsigned char>(97*brightness),static_cast<unsigned char>(51*brightness)};
        m_ringColors->SetTypedTuple(i,color);
    }
    m_ringColors->Modified(); m_ringActor->GetProperty()->SetOpacity(1);
    m_ringActor->GetProperty()->SetLineWidth(hover||dragging?1.6f:1.3f);
    m_outlineActor->GetProperty()->SetColor(passive?.65:1,passive?.47:.72,passive?.27:.42);
    m_outlineActor->GetProperty()->SetOpacity(1);
    m_outlineActor->GetProperty()->SetLineWidth(hover||dragging?2.4f:2.1f);
    m_ringActor->SetVisibility(m_visible); m_outlineActor->SetVisibility(m_visible&&m_outline->GetNumberOfPoints()>0);
}
P CropSphereVisual::Project(const P& point) const {
    const auto world=Transform(*m_modelToWorld,point,1);
    m_renderer->SetWorldPoint(world[0],world[1],world[2],1); m_renderer->WorldToDisplay();
    const auto* display=m_renderer->GetDisplayPoint(); return {display[0],display[1],display[2]};
}
void CropSphereVisual::AppendCircle(vtkPolyData& output,const P& center,double radius,const P& a,const P& b) {
    const auto point=[&](double angle) { P p{}; for(int j=0;j<3;++j)p[j]=center[j]+radius*(a[j]*std::cos(angle)+b[j]*std::sin(angle));return p; };
    int segments=32;
    // 全圆按投影弦差细分，避免视口边缘的短弧被漏判；极端近裁面投影保持有界。
    for(;segments<65536;segments*=2) {
        bool refine=false;
        for(int i=0;i<segments&&!refine;++i) {
            const auto p=Project(point(tau*i/segments)),q=Project(point(tau*(i+1)/segments));
            const auto mid=Project(point(tau*(i+.5)/segments));
            const double dx=q[0]-p[0],dy=q[1]-p[1],length=dx*dx+dy*dy;
            const double t=length>0?std::clamp(((mid[0]-p[0])*dx+(mid[1]-p[1])*dy)/length,0.0,1.0):0;
            refine=std::hypot(mid[0]-p[0]-t*dx,mid[1]-p[1]-t*dy)>pixelError;
        }
        if(!refine)break;
    }
    auto* points=output.GetPoints();auto* lines=output.GetLines();const auto first=points->GetNumberOfPoints();
    lines->InsertNextCell(segments+1);
    for(int i=0;i<segments;++i) { const auto p=point(tau*i/segments);lines->InsertCellPoint(points->InsertNextPoint(p.data())); }
    lines->InsertCellPoint(first); points->Modified();lines->Modified();output.Modified();
}
bool CropSphereVisual::GetHandleUsable() const {
    const auto center=Project(m_center),handle=Project(m_radiusHandle);
    // 两个 8 px 手柄的命中区应分离；悬停/拖动时由 UpdateView 禁止自动摆位。
    return handle[2]>=0&&handle[2]<=1&&handle[0]>=m_viewportPixels[0]+8&&handle[0]<=m_viewportPixels[2]-8
        &&handle[1]>=m_viewportPixels[1]+8&&handle[1]<=m_viewportPixels[3]-8
        &&std::hypot(center[0]-handle[0],center[1]-handle[1])>=16;
}
void CropSphereVisual::UpdateView() {
    if(!m_renderer)return;
    // 1. 相机身份/版本、视口像素尺寸和模型几何共同决定本次显示投影。
    auto* camera=m_renderer->GetActiveCamera();
    const auto* origin=m_renderer->GetOrigin(); const auto* size=m_renderer->GetSize();
    const std::array<int,4> viewport{origin[0],origin[1],origin[0]+size[0],origin[1]+size[1]};
    if(!m_viewDirty&&m_camera==camera&&m_cameraTime==camera->GetMTime()&&m_viewportPixels==viewport)return;
    m_viewportPixels=viewport;
    m_camera=camera; m_cameraTime=camera->GetMTime(); m_viewDirty=false;
    P normal{}, circleCenter=m_center; double circleRadius=m_radius;m_tangentOffset=0;
    m_rings->DeleteCells();m_rings->GetPoints()->Reset();m_rings->GetLines()->Reset();
    m_outline->DeleteCells();m_outline->GetPoints()->Reset();m_outline->GetLines()->Reset();
    bool hasOutline=true;
    if(camera->GetParallelProjection()) {
        P direction{}; camera->GetDirectionOfProjection(direction.data());
        normal=Unit(Transform(*m_worldToModel,direction,0)); for(auto& x:normal)x=-x;
    } else {
        P eye{}; camera->GetPosition(eye.data()); eye=Transform(*m_worldToModel,eye,1);
        for(int i=0;i<3;++i)normal[i]=eye[i]-m_center[i];
        const double distance=std::hypot(normal[0],normal[1],normal[2]);
        hasOutline=distance>m_radius;
        normal=Unit(normal);
        // 透视轮廓是切触圆，避免把过球心的大圆误当近距离/偏轴视角的真实边界。
        if(hasOutline) {
            const double ratio=m_radius/distance;
            m_tangentOffset=m_radius*ratio;
            for(int i=0;i<3;++i)circleCenter[i]+=normal[i]*m_tangentOffset;
            circleRadius=m_radius*std::sqrt((1-ratio)*(1+ratio));
        }
    }
    m_viewNormal=normal;
    // 2. 更新解析轮廓与三圆环，仅省略与轮廓重合的正视圆环，复用已有容器。
    if(hasOutline) { const auto a=Perpendicular(normal);AppendCircle(*m_outline,circleCenter,circleRadius,a,Cross(normal,a)); }
    const std::array<P,3> axes{P{1,0,0},P{0,1,0},P{0,0,1}};
    for(int ring=0;ring<3;++ring) {
        const auto& a=axes[ring==2?1:0];const auto& b=axes[ring==0?1:2];
        const auto facing=Dot(Cross(a,b),normal);
        bool duplicate=hasOutline&&facing*facing>.5;
        // 比较同一径向的屏幕距离；正视或投影差不足半像素时只保留较强的外轮廓。
        for(int i=0;i<32&&duplicate;++i) {
            P radial{};for(int j=0;j<3;++j)radial[j]=a[j]*std::cos(tau*i/32)+b[j]*std::sin(tau*i/32);
            P tangent=radial;const auto along=Dot(radial,normal);
            for(int j=0;j<3;++j)tangent[j]-=along*normal[j];
            if(Dot(tangent,tangent)<1e-12){duplicate=false;break;}
            tangent=Unit(tangent);P p{},q{};
            for(int j=0;j<3;++j){p[j]=m_center[j]+m_radius*radial[j];q[j]=circleCenter[j]+circleRadius*tangent[j];}
            const auto x=Project(p),y=Project(q);
            duplicate=std::hypot(x[0]-y[0],x[1]-y[1])<=pixelError;
        }
        if(!duplicate)AppendCircle(*m_rings,m_center,m_radius,a,b);
    }
    m_rings->GetPoints()->Modified();m_rings->GetLines()->Modified();m_rings->Modified();
    m_outline->GetPoints()->Modified();m_outline->GetLines()->Modified();m_outline->Modified();
    // 3. 方向缓存属于显示层；悬停/拖动时冻结，空闲且确实不可操作时才重新摆位。
    if(m_hasHandleDirection&&(m_state==State::Hover||m_state==State::Dragging||GetHandleUsable())) { ApplyStyle();return; }
    if(!hasOutline) { ApplyStyle();return; }
    P direction{}, up{}; camera->GetDirectionOfProjection(direction.data()); camera->GetViewUp(up.data());
    const auto right=Unit(Cross(direction,up)); const auto screenUp=Unit(Cross(right,direction));
    P diagonal{}; for(int i=0;i<3;++i)diagonal[i]=right[i]+screenUp[i];
    diagonal=Transform(*m_worldToModel,diagonal,0); const double along=Dot(diagonal,normal);
    for(int i=0;i<3;++i)diagonal[i]-=along*normal[i];
    diagonal=Unit(diagonal); if(Dot(diagonal,diagonal)==0)diagonal=Perpendicular(normal);
    P handleDirection{};
    for(int i=0;i<3;++i)handleDirection[i]=(circleCenter[i]-m_center[i]+circleRadius*diagonal[i])/m_radius;
    SetRadiusHandleDirection(handleDirection);
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
    // 本输出由交圆管线独占。一次分配全部容器，避免修改 GetLines/GetVerts 返回的共享空数组。
    if(!output.GetPoints()||output.GetNumberOfPolys()>0||output.GetNumberOfStrips()>0) {
        output.Initialize();
        vtkNew<vtkPoints> points;points->SetDataTypeToDouble();output.SetPoints(points);
        vtkNew<vtkCellArray> lines;output.SetLines(lines);
        vtkNew<vtkCellArray> vertices;output.SetVerts(vertices);
    }
    P normal{}; plane.GetNormal(normal.data()); normal=Unit(normal);
    const auto* origin=plane.GetOrigin(); P delta{}; for(int i=0;i<3;++i)delta[i]=center[i]-origin[i];
    const double distance=Dot(delta,normal);
    output.DeleteCells();
    if(output.GetPoints())output.GetPoints()->Reset();
    if(output.GetLines())output.GetLines()->Reset();
    if(output.GetVerts())output.GetVerts()->Reset();
    output.Modified();
    if(std::abs(distance)>radius)return;
    P sectionCenter{}; for(int i=0;i<3;++i)sectionCenter[i]=center[i]-distance*normal[i];
    const double ratio=std::abs(distance)/radius;
    const double sectionRadius=radius*std::sqrt((1-ratio)*(1+ratio));
    if(sectionRadius>0)SetCircle(output,sectionCenter,sectionRadius,normal);
    else {
        const vtkIdType id=output.GetPoints()->InsertNextPoint(sectionCenter.data());output.GetVerts()->InsertNextCell(1,&id);
    }
    output.Modified();
}
