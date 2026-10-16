#include "Interaction/CropCurveWidget.h"
#include "FeatureSupport/WidgetBindings.h"
#include "FeatureSupport/RoutedWidgets.h"
#include "Algorithms/CropGeometry.h"
#include "Render/CropSphereVisual.h"
#include "Render/CropCylinderVisual.h"
#include "Render/CropCurveProjection.h"
#include "Render/Support/RenderTextStyle.h"
#include <vtkActor.h>
#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkHandleWidget.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPointHandleRepresentation3D.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkWeakPointer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace {
using P=CropVectorDouble3Array;
P Add(const P& a,const P& b,double scale=1) {return {a[0]+scale*b[0],a[1]+scale*b[1],a[2]+scale*b[2]};}
double Dot(const P& a,const P& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
P Cross(const P& a,const P& b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
P Unit(const P& value) {const double length=std::hypot(value[0],value[1],value[2]);return {value[0]/length,value[1]/length,value[2]/length};}
P Radial(const P& axis) {
    P reference{};int index=0;for(int i=1;i<3;++i)if(std::abs(axis[i])<std::abs(axis[index]))index=i;
    reference[index]=1;return Unit(Cross(axis,reference));
}
P Transform(vtkMatrix4x4* matrix,const P& point) {
    const double input[4]={point[0],point[1],point[2],1};double output[4];matrix->MultiplyPoint(input,output);
    return {output[0],output[1],output[2]};
}
}

class CropCurveWidget::Impl final {
public:
    struct Handle final {
        Impl* owner=nullptr;std::size_t index=0;
        vtkSmartPointer<vtkHandleWidget> widget;
        vtkSmartPointer<vtkPointHandleRepresentation3D> representation;
        vtkSmartPointer<vtkCallbackCommand> observer;
        std::array<unsigned long,3> tags{};
    };
    Impl() {
        modelToWorld=vtkSmartPointer<vtkMatrix4x4>::New();worldToModel=vtkSmartPointer<vtkMatrix4x4>::New();
        text=vtkSmartPointer<vtkTextActor>::New();text->PickableOff();text->UseBoundsOff();text->SetPosition(12,36);
        text->GetTextProperty()->SetFontSize(12);text->GetTextProperty()->SetColor(1,.86,.62);
        RenderTextStyle::SetFont(*text->GetTextProperty());text->GetTextProperty()->ShadowOn();text->VisibilityOff();
        viewObserver=vtkSmartPointer<vtkCallbackCommand>::New();viewObserver->SetClientData(this);
        viewObserver->SetCallback([](vtkObject*,unsigned long,void* data,void*) {
            auto& self=*static_cast<Impl*>(data);self.SyncView();
        });
        mouseObserver=vtkSmartPointer<vtkCallbackCommand>::New();mouseObserver->SetClientData(this);
        mouseObserver->SetCallback([](vtkObject*,unsigned long event,void* data,void*) {
            auto& self=*static_cast<Impl*>(data);self.OnMouse(event);
        });
        for(std::size_t i=0;i<handles.size();++i) {
            auto& handle=handles[i];handle.owner=this;handle.index=i;
            handle.widget=vtkSmartPointer<FeatureSupport::RoutedHandleWidget>::New();
            handle.representation=vtkSmartPointer<vtkPointHandleRepresentation3D>::New();
            handle.representation->UseBoundsOff();
            handle.representation->AllOff();handle.representation->TranslationModeOn();
            handle.representation->GetProperty()->SetColor(i==1?0.2:0.9,i==1?0.8:0.6,0.2);
            handle.representation->GetSelectedProperty()->SetColor(1,0.68,0.16);
            handle.representation->SetHandleSize(8);handle.widget->AllowHandleResizeOff();
            handle.widget->EnableAxisConstraintOff();handle.widget->SetRepresentation(handle.representation);
            handle.observer=vtkSmartPointer<vtkCallbackCommand>::New();handle.observer->SetClientData(&handle);
            handle.observer->SetCallback([](vtkObject*,unsigned long event,void* data,void*) {
                if(!data)return;
                auto& handle=*static_cast<Handle*>(data);
                if(handle.owner)handle.owner->OnEvent(handle.index,event);
            });
            const std::array<unsigned long,3> events{vtkCommand::StartInteractionEvent,vtkCommand::InteractionEvent,vtkCommand::EndInteractionEvent};
            for(int event=0;event<3;++event)handle.tags[event]=handle.widget->AddObserver(events[event],handle.observer);
        }
    }
    ~Impl() {
        SetEnabled(false);
        for(auto& handle:handles) {
            handle.owner=nullptr;for(auto tag:handle.tags)handle.widget->RemoveObserver(tag);
            handle.observer->SetClientData(nullptr);handle.widget->SetInteractor(nullptr);
            handle.widget->SetDefaultRenderer(nullptr);handle.widget->SetCurrentRenderer(nullptr);
        }
    }
    CropOpItem inputGeometry,dragGeometry;
    P radial{1,0,0},inputRadial{1,0,0},dragRadial{1,0,0},dragLever{},rotationOffset{0,0,1},inputRotation{0,0,1};
    double rotationLength=0,inputRotationLength=0;
    bool hasRadial=false;
    // 最小旋转传递局部径向，轴连续变化时不重新选择参考坐标轴。
    static P Rotate(const P& p,const P& from,const P& to) {
        const auto cross=Cross(from,to);const double c=std::clamp(Dot(from,to),-1.,1.);
        if(c<-.999999999){const P n=Radial(from);return Add(Add(P{},p,-1),n,2*Dot(n,p));}
        return Add(Add(p,Cross(cross,p)),Cross(cross,Cross(cross,p)),1/(1+c));
    }
    P RotationLever() const {return Add(operation.centerInInputModel,rotationOffset,rotationLength>0?rotationLength:1.5*std::max(operation.radius,operation.height/2));}

    void SetContext(vtkRenderWindowInteractor* value,vtkRenderer* target) {
        if(interactor==value&&renderer==target)return;
        SetEnabled(false);interactor=value;renderer=target;
        for(auto& handle:handles) {
            handle.widget->SetInteractor(value);handle.widget->SetDefaultRenderer(target);handle.widget->SetCurrentRenderer(target);
        }
    }
    bool SetGeometry(CropOpItem value,const CropMatrixDouble16Array& transform) {
        if(value.geometryType!=CropShape::Sphere&&value.geometryType!=CropShape::Cylinder)return false;
        const auto valid=CropGeometry::Build(value);
        if(!valid||!std::all_of(transform.begin(),transform.end(),[](double x){return std::isfinite(x);})
            ||transform[12]!=0||transform[13]!=0||transform[14]!=0||transform[15]!=1)return false;
        vtkNew<vtkMatrix4x4> candidate;candidate->DeepCopy(transform.data());
        const auto determinant=candidate->Determinant();if(!std::isfinite(determinant)||determinant==0)return false;
        vtkNew<vtkMatrix4x4> inverse;vtkMatrix4x4::Invert(candidate,inverse);
        for(int row=0;row<4;++row)for(int col=0;col<4;++col)if(!std::isfinite(inverse->GetElement(row,col)))return false;
        const auto next=valid->GetOperation();
        if(next.geometryType==CropShape::Cylinder){
            if(hasRadial&&operation.geometryType==CropShape::Cylinder){
                radial=Rotate(radial,operation.axisInInputModel,next.axisInInputModel);
                rotationOffset=Rotate(rotationOffset,operation.axisInInputModel,next.axisInInputModel);
            }else{radial=Radial(next.axisInInputModel);rotationOffset=Unit(Add(next.axisInInputModel,radial));rotationLength=0;}
            radial=Unit(Add(radial,next.axisInInputModel,-Dot(radial,next.axisInInputModel)));hasRadial=true;
        }
        operation=next;modelToWorld->DeepCopy(candidate);worldToModel->DeepCopy(inverse);ready=true;
        Sync();return true;
    }
    bool SetEnabled(bool value) {
        if(value&&(!ready||!interactor||!renderer))return false;
        enabled=false;active=-1;hovered=-1;hover=false;
        for(auto& handle:handles)handle.widget->Off();
        sphereVisual.DetachRenderer();sphereVisual.SetVisible(false);cylinderVisual.DetachRenderer();cylinderVisual.SetVisible(false);
        if(renderer){renderer->RemoveObserver(viewTag);renderer->RemoveViewProp(text);}
        if(interactor){interactor->RemoveObserver(mouseTag);interactor->RemoveObserver(leaveTag);}
        viewTag=mouseTag=leaveTag=0;
        if(!value)return true;
        Sync();enabled=true;
        if(operation.geometryType==CropShape::Sphere) {sphereVisual.AttachRenderer(renderer);sphereVisual.SetVisible(true);}
        else {cylinderVisual.AttachRenderer(renderer);cylinderVisual.SetVisible(true);}
        renderer->AddViewProp(text);viewTag=renderer->AddObserver(vtkCommand::StartEvent,viewObserver,.5f);
        mouseTag=interactor->AddObserver(vtkCommand::MouseMoveEvent,mouseObserver,.6f);
        leaveTag=interactor->AddObserver(vtkCommand::LeaveEvent,mouseObserver,.6f);
        const auto count=operation.geometryType==CropShape::Sphere?2:5;
        for(int i=0;i<count;++i)handles[i].widget->On();
        SyncView();
        return true;
    }
    void Sync(int skip=-1) {
        if(!ready)return;
        const bool isSphere=operation.geometryType==CropShape::Sphere;
        const auto& center=operation.centerInInputModel;const auto& axis=operation.axisInInputModel;
        if(isSphere)sphereVisual.SetGeometry(center,operation.radius,modelToWorld);
        const P direction=isSphere?P{1,0,0}:radial;
        const std::array<P,5> positions{center,Add(center,direction,operation.radius),
            Add(center,axis,-operation.height/2),Add(center,axis,operation.height/2),RotationLever()};
        for(int i=0;i<(isSphere?2:5);++i)if(i!=skip) {
            auto world=Transform(modelToWorld,isSphere&&i==1?sphereVisual.GetRadiusHandle():positions[i]);
            handles[i].representation->SetWorldPosition(world.data());
        }
        if(isSphere) {
            ApplySphereStyle();
        } else {
            cylinderVisual.SetGeometry(operation,modelToWorld);
            ApplyCylinderStyle();

        }
    }
    void SyncView() {
        if(!enabled)return;
        if(operation.geometryType==CropShape::Cylinder){
            cylinderVisual.UpdateView();
            // 轴向正视时不让端点与中心争夺同一命中区域；旋转柄仍可用于转出正视。
            if(active<0){
                const auto direction=hover?radial:cylinderVisual.GetRadiusHandleDirection(radial);
                if(direction!=radial){
                    radial=direction;
                    auto radius=Transform(modelToWorld,Add(operation.centerInInputModel,radial,operation.radius));
                    auto rotation=Transform(modelToWorld,RotationLever());
                    handles[1].representation->SetWorldPosition(radius.data());handles[4].representation->SetWorldPosition(rotation.data());
                }
                std::vector<P> display;
                for(int i:{0,1,2,3}){
                    P world{};handles[i].representation->GetWorldPosition(world.data());
                    renderer->SetWorldPoint(world[0],world[1],world[2],1);renderer->WorldToDisplay();const auto* d=renderer->GetDisplayPoint();
                    const bool separate=i<2||std::all_of(display.begin(),display.end(),[&](const P& p){return std::hypot(d[0]-p[0],d[1]-p[1])>=16;});
                    handles[i].representation->SetVisibility(separate);handles[i].widget->SetProcessEvents(separate);
                    if(separate)display.push_back({d[0],d[1],d[2]});
                }
                const bool rotationAvailable=UpdateRotationHandle();
                handles[4].representation->SetVisibility(rotationAvailable);handles[4].widget->SetProcessEvents(rotationAvailable);
            }
            return;
        }
        sphereVisual.UpdateView();
        if(active!=1) {
            auto world=Transform(modelToWorld,sphereVisual.GetRadiusHandle());
            handles[1].representation->SetWorldPosition(world.data());
        }
    }
    std::array<double,4> GetHandleEnvelope() const {
        // VTK 9.4.2 的 NearbyEvent 将实际拾取限制在中心两侧 10 个显示像素内，
        // CellPicker 的 0.01 容差仍受此前置范围约束；另覆盖 10 像素高亮光标的完整投影。
        std::array<double,4> half{10.5,10.5,0,0};double handleSize=10,lineWidth=0;
        for(const auto& h:handles){handleSize=std::max(handleSize,h.representation->GetHandleSize());lineWidth=std::max({lineWidth,double(h.representation->GetProperty()->GetLineWidth()),double(h.representation->GetSelectedProperty()->GetLineWidth())});}
        const auto* focal=renderer->GetActiveCamera()->GetFocalPoint();
        renderer->SetWorldPoint(focal[0],focal[1],focal[2],1);renderer->WorldToDisplay();
        P center{};std::copy_n(renderer->GetDisplayPoint(),3,center.data());P corners[2];
        for(int i=0;i<2;++i){const double sign=i?1:-1;renderer->SetDisplayPoint(center[0]+sign*handleSize/2,center[1]+sign*handleSize/2,center[2]);renderer->DisplayToWorld();const auto* p=renderer->GetWorldPoint();for(int j=0;j<3;++j)corners[i][j]=p[j]/p[3];}
        // 与 SizeHandlesInPixels 相同的世界半径；在平行和透视相机下屏幕包络均不依赖深度。
        const auto diagonal=Add(corners[1],corners[0],-1);const double radius=std::hypot(diagonal[0],diagonal[1],diagonal[2])/2;
        auto* projection=renderer->GetActiveCamera()->GetCompositeProjectionTransformMatrix(renderer->GetTiledAspectRatio(),0,1);
        const double world[4]{focal[0],focal[1],focal[2],1};double clip[4];projection->MultiplyPoint(world,clip);
        if(!(clip[3]>0)||!std::isfinite(radius))return {std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity(),0,0};
        // 半径随透视 w 同比变化。两裁剪平面的支持半径也转成 w 的系数，保留完整光标深度。
        for(int axis=0;axis<3;++axis){half[2]+=radius/clip[3]*std::abs(projection->GetElement(2,axis));half[3]+=radius/clip[3]*std::abs(projection->GetElement(3,axis)-projection->GetElement(2,axis));}
        for(int i=0;i<8;++i){renderer->SetWorldPoint(focal[0]+(i&1?radius:-radius),focal[1]+(i&2?radius:-radius),focal[2]+(i&4?radius:-radius),1);renderer->WorldToDisplay();const auto* p=renderer->GetDisplayPoint();
            for(int axis=0;axis<2;++axis){if(!std::isfinite(p[axis]))return {std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity(),0,0};half[axis]=std::max(half[axis],std::abs(p[axis]-center[axis])+lineWidth/2+.5);}}
        return half;
    }
    bool UpdateRotationHandle() {
        const auto* origin=renderer->GetOrigin();const auto* size=renderer->GetSize();const auto half=GetHandleEnvelope();
        if(size[0]<=2*half[0]||size[1]<=2*half[1])return false;
        std::vector<P> occupied;
        for(int i=0;i<4;++i)if(GetHandleAvailable(handles[i])){P p{};handles[i].representation->GetWorldPosition(p.data());renderer->SetWorldPoint(p[0],p[1],p[2],1);renderer->WorldToDisplay();std::copy_n(renderer->GetDisplayPoint(),3,p.data());if(p[2]>=0&&p[2]<=1)occupied.push_back(p);}
        vtkNew<vtkMatrix4x4> clip;vtkMatrix4x4::Multiply4x4(renderer->GetActiveCamera()->GetCompositeProjectionTransformMatrix(renderer->GetTiledAspectRatio(),0,1),modelToWorld,clip);
        const auto& center=operation.centerInInputModel;const double base[4]{center[0],center[1],center[2],1};double c[4];clip->MultiplyPoint(base,c);
        if(!std::all_of(c,c+4,[](double x){return std::isfinite(x);}))return false;
        using Interval=std::array<double,2>;
        const auto intersect=[](Interval& range,double a,double b){
            if(b>0)range[0]=std::max(range[0],-a/b);else if(b<0)range[1]=std::min(range[1],-a/b);else if(a<0)return false;
            return range[0]<=range[1];
        };
        const double wanted=rotationLength>0?rotationLength:1.5*std::max(operation.radius,operation.height/2);
        const auto chooseLength=[&](const P& direction,double& chosen){
            const double vector[4]{direction[0],direction[1],direction[2],0};double d[4];clip->MultiplyPoint(vector,d);
            if(!std::all_of(d,d+4,[](double x){return std::isfinite(x);}))return false;
            Interval visible{0,std::numeric_limits<double>::infinity()};
            // 1. 在相机前方求带完整手柄边距的视锥区间，不以实体默认长度作为上限。
            if(!intersect(visible,c[3],d[3]))return false;
            for(int axis=0;axis<3;++axis){const double inset=axis<2?1-2*half[axis]/size[axis]:1-half[3];
                const double lower=axis<2?inset:-half[2];
                if(!intersect(visible,lower*c[3]+c[axis],lower*d[3]+d[axis])
                    ||!intersect(visible,inset*c[3]-c[axis],inset*d[3]-d[axis]))return false;}
            std::vector<Interval> available{visible};
            // 2. u(L)、v(L) 是一次式之比；冲突矩形四条边直接给出要剔除的长度区间。
            for(const auto& other:occupied){Interval blocked=visible;bool overlaps=true;
                for(int axis=0;axis<2&&overlaps;++axis){const double margin=2*half[axis]+2; // 两份包络及操作间隙，均为最终显示像素。
                    const double low=2*(other[axis]-margin-origin[axis])/size[axis]-1,high=2*(other[axis]+margin-origin[axis])/size[axis]-1;
                    overlaps=intersect(blocked,c[axis]-low*c[3],d[axis]-low*d[3])&&intersect(blocked,high*c[3]-c[axis],high*d[3]-d[axis]);}
                if(!overlaps)continue;
                std::vector<Interval> remaining;
                for(const auto& range:available){
                    if(blocked[1]<range[0]||blocked[0]>range[1])remaining.push_back(range);
                    else {if(blocked[0]>range[0])remaining.push_back({range[0],std::nextafter(blocked[0],-std::numeric_limits<double>::infinity())});if(blocked[1]<range[1])remaining.push_back({std::nextafter(blocked[1],std::numeric_limits<double>::infinity()),range[1]});}}
                available=std::move(remaining);
            }
            // 3. 选离现有长度最近的可用点；当前位置合法时精确保留，避免松手和重绘跳动。
            double distance=std::numeric_limits<double>::infinity();
            for(const auto& range:available){const double length=std::clamp(wanted,range[0],range[1]);
                if(length>0&&std::isfinite(length)&&c[3]+length*d[3]>0&&std::abs(length-wanted)<distance){distance=std::abs(length-wanted);chosen=length;}}
            return std::isfinite(distance);
        };
        P chosen=rotationOffset;double chosenLength=0;
        if(!chooseLength(rotationOffset,chosenLength)){
            const auto tangent=Cross(operation.axisInInputModel,radial);double best=std::numeric_limits<double>::infinity();
            for(int sign:{-1,1})for(int i=0;i<32;++i){const double angle=6.283185307179586*i/32;const auto& a=operation.axisInInputModel;
                const auto direction=Unit(Add(Add(P{sign*a[0],sign*a[1],sign*a[2]},radial,std::cos(angle)),tangent,std::sin(angle)));double length;
                if(!chooseLength(direction,length))continue;
                const auto delta=Add(Add(P{},direction,length),rotationOffset,-wanted);const double distance=Dot(delta,delta);
                if(distance<best){best=distance;chosen=direction;chosenLength=length;}}
            if(!std::isfinite(best))return false;
        }
        rotationOffset=chosen;rotationLength=chosenLength;auto p=Transform(modelToWorld,RotationLever());handles[4].representation->SetWorldPosition(p.data());return true;
    }
    bool GetHandleAvailable(const Handle& handle) const {
        return handle.widget->GetEnabled()&&handle.widget->GetProcessEvents()&&handle.representation->GetVisibility();
    }
    void ApplySphereStyle() {
        const auto state=active>=0?visualState:(hover?CropSphereVisual::State::Hover:CropSphereVisual::State::Idle);
        sphereVisual.SetState(state);
        for(int i=0;i<2;++i) {
            const bool selected=active==i||hovered==i;
            handles[i].representation->GetProperty()->SetColor(1,i==0?.88:.64,i==0?.58:.26);
            handles[i].representation->GetProperty()->SetOpacity(selected?1:.65);
            handles[i].representation->SetHandleSize(selected?10:8);
        }
        text->SetVisibility(hover||active>=0);
        std::ostringstream label;label<<std::fixed<<std::setprecision(2);
        if(active==0&&visualState==CropSphereVisual::State::Dragging) {
            const auto& center=operation.centerInInputModel;label<<"Center = ("<<center[0]<<", "<<center[1]<<", "<<center[2]<<") mm";
        } else label<<"R = "<<operation.radius<<" mm";
        text->SetInput(label.str().c_str());
    }
    void ApplyCylinderStyle() {
        cylinderVisual.SetRadiusActive(active==1||hovered==1);
        for(int i=0;i<5;++i){const bool selected=active==i||hovered==i;
            handles[i].representation->GetProperty()->SetColor(i==1?.2:i==4?.45:.95,i==1?.8:i==4?.65:.65,i==4?1:.2);
            handles[i].representation->GetProperty()->SetOpacity(selected?1:.8);handles[i].representation->SetHandleSize(selected?10:8);}
        text->SetVisibility(hover||active>=0);std::ostringstream label;label<<std::fixed<<std::setprecision(2);
        const int handle=active>=0?active:hovered;
        if(handle==0)label<<u8"移动中心";
        else if(handle==1)label<<u8"调整半径  R = "<<operation.radius<<" mm";
        else if(handle==2||handle==3)label<<u8"调整高度  H = "<<operation.height<<" mm";
        else if(handle==4)label<<u8"旋转圆柱";
        text->SetInput(label.str().c_str());
    }
    void OnMouse(unsigned long event) {
        if(!enabled||active>=0)return;
        bool allowed=true;try{if(gate)allowed=gate();}catch(...){allowed=false;}
        const auto* position=interactor->GetEventPosition();int nextHandle=-1;bool nextHover=false;
        if(allowed&&event!=vtkCommand::LeaveEvent&&renderer->IsInViewport(position[0],position[1])) {
            SyncView();
            for(int i=0;i<(operation.geometryType==CropShape::Sphere?2:5);++i)if(GetHandleAvailable(handles[i])&&handles[i].representation->ComputeInteractionState(position[0],position[1])!=vtkHandleRepresentation::Outside){nextHandle=i;break;}
            nextHover=nextHandle>=0||(operation.geometryType==CropShape::Sphere&&sphereVisual.GetNearOutline(position[0],position[1]));
        }
        if(nextHover==hover&&nextHandle==hovered)return;
        hover=nextHover;hovered=nextHandle;if(operation.geometryType==CropShape::Sphere)ApplySphereStyle();else ApplyCylinderStyle();if(renderCallback)renderCallback();
    }
    void OnEvent(std::size_t index,unsigned long event) {
        if(!enabled||index>=(operation.geometryType==CropShape::Sphere?2U:5U))return;
        try {if(gate&&!gate()){active=-1;hover=false;hovered=-1;Sync();if(renderCallback)renderCallback();return;}}catch(...){active=-1;hover=false;hovered=-1;Sync();return;}
        if(event==vtkCommand::StartInteractionEvent) {
            static_cast<FeatureSupport::RoutedHandleWidget*>(handles[index].widget.GetPointer())->ReleaseInputFocus();
            dragGeometry=operation;dragRadial=radial;dragLever=Unit(Add(RotationLever(),operation.centerInInputModel,-1));
            active=static_cast<int>(index);visualState=CropSphereVisual::State::Hover;
            if(operation.geometryType==CropShape::Sphere)ApplySphereStyle();else ApplyCylinderStyle();
            if(renderCallback)renderCallback();if(callback)callback(CropInteractionPhase::Hover);return;
        }
        if(active!=static_cast<int>(index)){Sync();return;}
        if(event==vtkCommand::InteractionEvent) {
            P position;handles[index].representation->GetWorldPosition(position.data());position=Transform(worldToModel,position);
            if(!std::all_of(position.begin(),position.end(),[](double v){return std::isfinite(v);})){Sync();return;}
            auto next=operation;auto nextRadial=radial,nextRotation=rotationOffset;
            if(index==0)next.centerInInputModel=position;
            else if(index==1) {
                auto delta=Add(position,operation.centerInInputModel,-1);
                if(operation.geometryType==CropShape::Cylinder)delta=Add(delta,operation.axisInInputModel,-Dot(delta,operation.axisInInputModel));
                next.radius=std::hypot(delta[0],delta[1],delta[2]);
                if(operation.geometryType==CropShape::Cylinder&&next.radius>0&&std::isfinite(next.radius))nextRadial=Unit(delta);
            } else if(index==4) {
                const auto delta=Add(position,dragGeometry.centerInInputModel,-1);
                const double length=std::hypot(delta[0],delta[1],delta[2]);
                if(!std::isfinite(length)||length<1e-10){Sync();return;}
                const auto direction=Unit(delta);
                next.axisInInputModel=Rotate(dragGeometry.axisInInputModel,dragLever,direction);
                nextRadial=Rotate(dragRadial,dragLever,direction);nextRotation=direction;
            } else {
                const auto& axis=operation.axisInInputModel;
                const auto opposite=Add(operation.centerInInputModel,axis,(index==2?1:-1)*operation.height/2);
                const double length=(index==2?-1:1)*Dot(Add(position,opposite,-1),axis);
                if(length<=0){Sync();return;}
                next.height=length;next.centerInInputModel=Add(opposite,axis,(index==2?-1:1)*length/2);
            }
            const auto valid=CropGeometry::Build(next);
            if(!valid){Sync();return;}
            if(operation.geometryType==CropShape::Sphere&&index==1)
                sphereVisual.SetRadiusHandleDirection(Add(position,operation.centerInInputModel,-1));
            radial=nextRadial;rotationOffset=nextRotation;operation=valid->GetOperation();visualState=CropSphereVisual::State::Dragging;Sync(operation.geometryType==CropShape::Sphere?static_cast<int>(index):-1);
            if(callback)callback(CropInteractionPhase::Dragging);
        } else if(event==vtkCommand::EndInteractionEvent) {
            active=-1;if(operation.geometryType==CropShape::Cylinder){hover=false;hovered=-1;}Sync();SyncView();if(callback)callback(CropInteractionPhase::Released);
        }
    }
    CropOpItem operation;bool ready=false,enabled=false;int active=-1;
    vtkRenderWindowInteractor* interactor=nullptr;vtkWeakPointer<vtkRenderer> renderer;
    vtkSmartPointer<vtkMatrix4x4> modelToWorld,worldToModel;
    CropCylinderVisual cylinderVisual;
    CropSphereVisual sphereVisual;
    CropSphereVisual::State visualState=CropSphereVisual::State::Idle;
    bool hover=false;int hovered=-1;
    unsigned long viewTag=0,mouseTag=0,leaveTag=0;
    vtkSmartPointer<vtkCallbackCommand> viewObserver,mouseObserver;
    vtkSmartPointer<vtkTextActor> text;
    std::array<Handle,5> handles;
    std::function<void(CropInteractionPhase)> callback;
    std::function<bool()> gate;
    std::function<void()> renderCallback;
};

CropCurveWidget::CropCurveWidget():m_impl(std::make_unique<Impl>()){}
CropCurveWidget::~CropCurveWidget()=default;
void CropCurveWidget::SetContext(vtkRenderWindowInteractor* interactor,vtkRenderer* renderer){m_impl->SetContext(interactor,renderer);}
bool CropCurveWidget::SetGeometry(CropOpItem operation,const CropMatrixDouble16Array& matrix){return m_impl->SetGeometry(std::move(operation),matrix);}
CropOpItem CropCurveWidget::GetGeometry() const{return m_impl->operation;}
void CropCurveWidget::SetCallback(std::function<void(CropInteractionPhase)> callback){m_impl->callback=std::move(callback);}
void CropCurveWidget::SetRenderCallback(std::function<void()> callback){m_impl->renderCallback=std::move(callback);}
bool CropCurveWidget::SetEnabled(bool enabled){return m_impl->SetEnabled(enabled);}
bool CropCurveWidget::GetEnabled() const{return m_impl->enabled;}
vtkHandleWidget* CropCurveWidget::GetHandle(std::size_t index) const{return index<m_impl->handles.size()?m_impl->handles[index].widget.GetPointer():nullptr;}

bool CropCurveWidget::GetTransformSame(const CropMatrixDouble16Array& matrix) const {
    if(!m_impl->ready)return false;
    for(int row=0;row<4;++row)for(int column=0;column<4;++column)
        if(matrix[row*4+column]!=m_impl->modelToWorld->GetElement(row,column))return false;
    return true;
}
void CropCurveWidget::SetContextGate(std::function<bool()> gate){m_impl->gate=std::move(gate);}

bool CropCurveWidget::GetInputAccepted(int x,int y) const {
    if(!m_impl->enabled)return false;
    m_impl->SyncView();
    for(const auto& h:m_impl->handles)if(m_impl->GetHandleAvailable(h)&&h.representation->ComputeInteractionState(x,y)!=vtkHandleRepresentation::Outside)return true;
    return false;
}
void CropCurveWidget::SetInputBindings(const std::vector<EffectiveBinding>& values){for(auto& h:m_impl->handles)FeatureSupport::SetWidgetBindings(h.widget,values,{{"crop.widget.select.drag",vtkWidgetEvent::Select}});}
void CropCurveWidget::StartGesture(){m_impl->SyncView();m_impl->inputGeometry=m_impl->operation;m_impl->inputRadial=m_impl->radial;m_impl->inputRotation=m_impl->rotationOffset;m_impl->inputRotationLength=m_impl->rotationLength;}
void CropCurveWidget::CancelGesture(){m_impl->operation=m_impl->inputGeometry;m_impl->radial=m_impl->inputRadial;m_impl->rotationOffset=m_impl->inputRotation;m_impl->rotationLength=m_impl->inputRotationLength;m_impl->active=-1;m_impl->hover=false;m_impl->hovered=-1;m_impl->Sync();m_impl->SyncView();for(auto& h:m_impl->handles)static_cast<FeatureSupport::RoutedHandleWidget*>(h.widget.GetPointer())->StopGesture();}
