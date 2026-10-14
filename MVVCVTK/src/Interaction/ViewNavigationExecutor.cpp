#include "Interaction/ViewNavigationExecutor.h"
#include "Interaction/InputBindingMatch.h"
#include <vtkCamera.h>
#include <vtkMath.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkTransform.h>
#include <vtkSmartPointer.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool Finite(const std::array<double,3>& p) { return std::all_of(p.begin(),p.end(),[](double x){return std::isfinite(x);}); }
InteractionResult Result(NavigationStatus s) {
    return {true,true,s==NavigationStatus::Applied,s==NavigationStatus::Applied?InteractionFailureReason::None:InteractionFailureReason::StateRejected};
}
bool IsWheel(InteractionEventKind k) { return k==InteractionEventKind::WheelForward||k==InteractionEventKind::WheelBackward; }
}
class ViewNavigationExecutor::Impl final {
public:
    InteractionPorts ports;
    vtkRenderer* renderer=nullptr;
    std::shared_ptr<NavigationEndpoint> endpoint=std::make_shared<NavigationEndpoint>();
    std::shared_ptr<ViewNavigation> navigation=std::make_shared<ViewNavigation>();
    double referenceHeight=0;
    DataRevisionRef referenceSource;
    double sliceRemainder=0;
    NavigationAction action=NavigationAction::Count;
    NavigationOptions options;
    WindowLevelRequest initialWindowLevel;
    std::optional<InputBinding> precisionBinding,acceleratedBinding;
    ZoomAnchor anchor;
    InteractionEventKind release=InteractionEventKind::None;
    std::array<double,2> lastPosition{};
    std::optional<std::array<double,2>> pointerPosition;
    bool isWheelGesture=false;
    InteractionSource source{"ViewNavigation",{}};
    explicit Impl(InteractionPorts p,vtkRenderer* r,std::function<bool()> valid,std::function<bool()> captured):ports(std::move(p)),renderer(r) {
        source.channelId=std::to_string(reinterpret_cast<std::uintptr_t>(this));
        endpoint->isValid=std::move(valid);endpoint->hasCapture=std::move(captured);
        endpoint->state=[this]{return State();};
        endpoint->zoom=[this](const auto& v){return Zoom(v);};
        endpoint->pan=[this](const auto& v){return Pan(v,displayScale);};
        endpoint->rotate=[this](const auto& v){return Rotate(v);};
        endpoint->scroll=[this](double v){return Scroll(v);};
        endpoint->windowLevel=[this](const auto& v){return WindowLevel(v);};
        endpoint->fit=[this]{return Fit();};
        (void)NavigationAccess::Bind(*navigation,endpoint);
    }
    bool Valid() const { return endpoint->owner==std::this_thread::get_id()&&endpoint->isValid&&endpoint->isValid()&&renderer&&renderer->GetActiveCamera(); }
    bool Slice() const { return currentMode==VizMode::SliceTop_down||currentMode==VizMode::SliceFront_back||currentMode==VizMode::SliceLeft_right; }
    VizMode currentMode=VizMode::Volume;
    double Height() const {
        auto* c=renderer->GetActiveCamera();
        return c->GetParallelProjection()?2*c->GetParallelScale():2*c->GetDistance()*std::tan(vtkMath::RadiansFromDegrees(c->GetViewAngle()/2));
    }
    std::optional<NavigationState> State() {
        if(!Valid()) return {};
        const auto source=ports.update->GetNavigationSource();if(source&&*source!=referenceSource){referenceSource=*source;referenceHeight=0;}
        const double h=Height();if(!std::isfinite(h)||h<=0)return {};
        if(referenceHeight<=0)referenceHeight=h;
        const auto wl=ports.slice->GetWindowLevel();
        return NavigationState{referenceHeight/h,h,wl.windowWidth,wl.windowCenter,Slice(),renderer->GetActiveCamera()->GetParallelProjection()!=0};
    }
    bool DisplayWorld(double x,double y,const std::array<double,3>& plane,std::array<double,3>& out) const {
        auto* camera=renderer->GetActiveCamera();
        double normal[3];camera->GetDirectionOfProjection(normal);
        std::array<double,3> a{},b{};
        for(int i=0;i<2;++i) {
            renderer->SetDisplayPoint(x,y,i);renderer->DisplayToWorld();const auto* p=renderer->GetWorldPoint();
            if(!p||!std::isfinite(p[3])||p[3]==0)return false;
            auto& target=i==0?a:b;for(int j=0;j<3;++j)target[j]=p[j]/p[3];
        }
        std::array<double,3> ray{b[0]-a[0],b[1]-a[1],b[2]-a[2]};
        const double denominator=vtkMath::Dot(ray.data(),normal);
        if(!std::isfinite(denominator)||std::abs(denominator)<=std::numeric_limits<double>::epsilon())return false;
        const std::array<double,3> d{plane[0]-a[0],plane[1]-a[1],plane[2]-a[2]};
        const double t=vtkMath::Dot(d.data(),normal)/denominator;
        for(int j=0;j<3;++j)out[j]=a[j]+t*ray[j];return Finite(out);
    }
    bool Anchor(const ZoomAnchor& a,std::array<double,3>& world,std::array<double,2>& display) const {
        auto* camera=renderer->GetActiveCamera();const auto* focal=camera->GetFocalPoint();
        std::array<double,3> plane{focal[0],focal[1],focal[2]};
        const auto* size=renderer->GetSize();const auto* origin=renderer->GetOrigin();
        if(!size||size[0]<=0||size[1]<=0||a.mode>ZoomAnchorMode::WorldPoint)return false;
        display={origin[0]+size[0]*0.5,origin[1]+size[1]*0.5};
        if(a.mode==ZoomAnchorMode::WorldPoint||a.mode==ZoomAnchorMode::Crosshair) {
            world=a.mode==ZoomAnchorMode::Crosshair?ports.slice->GetCursorWorld():a.point;
            if(!Finite(world))return false;
            renderer->SetWorldPoint(world[0],world[1],world[2],1);renderer->WorldToDisplay();const auto* d=renderer->GetDisplayPoint();
            display={d[0],d[1]};return std::isfinite(d[0])&&std::isfinite(d[1]);
        }
        if(a.mode==ZoomAnchorMode::Pointer&&pointerPosition)display=*pointerPosition;
        if(a.mode==ZoomAnchorMode::DisplayPoint)display={a.point[0],a.point[1]};
        if(a.mode==ZoomAnchorMode::NormalizedPoint) {
            if(a.point[0]<0||a.point[0]>1||a.point[1]<0||a.point[1]>1)return false;
            display={origin[0]+size[0]*a.point[0],origin[1]+size[1]*a.point[1]};
        }
        // 切片相机的焦平面由现有实际切片方向和变换确定，不写死世界轴。
        return std::isfinite(display[0])&&std::isfinite(display[1])&&DisplayWorld(display[0],display[1],plane,world);
    }
    NavigationStatus CameraChange(const std::function<bool(vtkCamera*)>& change) {
        if(!Valid())return NavigationStatus::Unbound;
        auto* camera=renderer->GetActiveCamera();auto previous=vtkSmartPointer<vtkCamera>::New();previous->DeepCopy(camera);
        try {
            if(!change(camera)) {camera->DeepCopy(previous);return NavigationStatus::Invalid;}
        } catch(...) {camera->DeepCopy(previous);return NavigationStatus::Failed;}
        const auto* position=camera->GetPosition();const auto* focal=camera->GetFocalPoint();const auto* up=camera->GetViewUp();
        if(!Finite({position[0],position[1],position[2]})||!Finite({focal[0],focal[1],focal[2]})||!Finite({up[0],up[1],up[2]})
            ||!std::isfinite(camera->GetParallelScale())||camera->GetParallelScale()<=0||!std::isfinite(camera->GetDistance())||camera->GetDistance()<=0) {
            camera->DeepCopy(previous);return NavigationStatus::Invalid;
        }
        renderer->ResetCameraClippingRange();
        if(!ports.update->SetRenderNeeded()) {camera->DeepCopy(previous);return NavigationStatus::Failed;}
        return NavigationStatus::Applied;
    }
    NavigationStatus Zoom(const ZoomRequest& r) {
        if(!Valid())return NavigationStatus::Unbound;
        if(!std::isfinite(r.factor)||r.factor<=0)return NavigationStatus::Invalid;
        if(!State())return NavigationStatus::Invalid;
        if(r.factor==1)return NavigationStatus::Applied;
        std::array<double,3> p{};std::array<double,2> d{};if(!Anchor(r.anchor,p,d))return NavigationStatus::Invalid;
        return CameraChange([&](vtkCamera* camera){
            if(camera->GetParallelProjection()) {
                const auto scale=camera->GetParallelScale()/r.factor;if(!std::isfinite(scale)||scale<=0)return false;
                camera->SetParallelScale(scale);
            } else {
                const auto distance=camera->GetDistance()/r.factor;if(!std::isfinite(distance)||distance<=0)return false;
                const auto* focal=camera->GetFocalPoint();double normal[3];camera->GetDirectionOfProjection(normal);
                camera->SetPosition(focal[0]-normal[0]*distance,focal[1]-normal[1]*distance,focal[2]-normal[2]*distance);
            }
            std::array<double,3> q{};if(!DisplayWorld(d[0],d[1],p,q))return false;
            const auto* pos=camera->GetPosition();const auto* focal=camera->GetFocalPoint();
            const std::array<double,3> nextPos{pos[0]+p[0]-q[0],pos[1]+p[1]-q[1],pos[2]+p[2]-q[2]};
            const std::array<double,3> nextFocal{focal[0]+p[0]-q[0],focal[1]+p[1]-q[1],focal[2]+p[2]-q[2]};
            if(!Finite(nextPos)||!Finite(nextFocal))return false;
            camera->SetPosition(nextPos.data());camera->SetFocalPoint(nextFocal.data());return true;
        });
    }
    NavigationStatus Pan(const PanRequest& r,double scale) {
        if(!Valid())return NavigationStatus::Unbound;
        if(!std::isfinite(r.deltaX)||!std::isfinite(r.deltaY)||!std::isfinite(scale)||scale<=0)return NavigationStatus::Invalid;
        std::array<double,3> p{},q{};std::array<double,2> d{};
        if(!Anchor({},p,d)||!DisplayWorld(d[0]-r.deltaX*scale,d[1]-r.deltaY*scale,p,q))return NavigationStatus::Invalid;
        return CameraChange([&](vtkCamera* camera){
            const auto* pos=camera->GetPosition();const auto* f=camera->GetFocalPoint();
            const std::array<double,3> np{pos[0]+q[0]-p[0],pos[1]+q[1]-p[1],pos[2]+q[2]-p[2]};
            const std::array<double,3> nf{f[0]+q[0]-p[0],f[1]+q[1]-p[1],f[2]+q[2]-p[2]};
            if(!Finite(np)||!Finite(nf))return false;camera->SetPosition(np.data());camera->SetFocalPoint(nf.data());return true;
        });
    }
    NavigationStatus Rotate(const RotateRequest& r) {
        if(Slice())return NavigationStatus::Unsupported;
        if(!Valid())return NavigationStatus::Unbound;
        if(!std::isfinite(r.yawDegrees)||!std::isfinite(r.pitchDegrees)||!std::isfinite(r.rollDegrees))return NavigationStatus::Invalid;
        std::array<double,3> pivot{};std::array<double,2> d{};if(!Anchor(r.pivot,pivot,d))return NavigationStatus::Invalid;
        return CameraChange([&](vtkCamera* camera){
            double up[3],normal[3],right[3];std::copy_n(camera->GetViewUp(),3,up);camera->GetDirectionOfProjection(normal);vtkMath::Cross(normal,up,right);vtkMath::Normalize(right);
            auto t=vtkSmartPointer<vtkTransform>::New();t->Translate(pivot.data());
            t->RotateWXYZ(r.yawDegrees,up);t->RotateWXYZ(r.pitchDegrees,right);t->RotateWXYZ(r.rollDegrees,normal);t->Translate(-pivot[0],-pivot[1],-pivot[2]);
            double p[3],f[3],u[3];t->TransformPoint(camera->GetPosition(),p);t->TransformPoint(camera->GetFocalPoint(),f);t->TransformVector(up,u);
            camera->SetPosition(p);camera->SetFocalPoint(f);camera->SetViewUp(u);camera->OrthogonalizeViewUp();return true;
        });
    }
    NavigationStatus Scroll(double count) {
        if(!Valid())return NavigationStatus::Unbound;if(!Slice())return NavigationStatus::Unsupported;
        if(!std::isfinite(count)||std::abs(count+sliceRemainder)>std::numeric_limits<int>::max())return NavigationStatus::Invalid;
        const double total=count+sliceRemainder;const int whole=static_cast<int>(total);
        if(whole&&!ports.slice->SetSliceScroll(whole))return NavigationStatus::Failed;
        sliceRemainder=total-whole;return NavigationStatus::Applied;
    }
    NavigationStatus WindowLevel(const WindowLevelRequest& r) {
        if(!Valid())return NavigationStatus::Unbound;
        if(!std::isfinite(r.width)||r.width<=0||!std::isfinite(r.center))return NavigationStatus::Invalid;
        return ports.slice->SetWindowLevel(r.width,r.center)?NavigationStatus::Applied:NavigationStatus::Failed;
    }
    NavigationStatus Fit() {
        const auto result=CameraChange([this](vtkCamera*){renderer->ResetCamera();return true;});
        if(result==NavigationStatus::Applied)referenceHeight=Height();return result;
    }
    NavigationStatus Cleanup() {
        if(action==NavigationAction::Count)return NavigationStatus::Applied;
        if(!ports.state->SetInteracting(source,false))return NavigationStatus::Failed;
        action=NavigationAction::Count;release=InteractionEventKind::None;isWheelGesture=false;return NavigationStatus::Applied;
    }
    NavigationContext Context() {
        NavigationContext c;c.isSliceView=Slice();const auto* size=renderer->GetSize();c.viewportSize={size[0],size[1]};const auto* origin=renderer->GetOrigin();c.viewportOrigin={origin[0],origin[1]};
        c.crosshairWorld=ports.slice->GetCursorWorld();if(const auto state=State())c.state=*state;c.options=options;c.startWindowLevel=initialWindowLevel;return c;
    }
    NavigationStatus Calculate(const NavigationInput& input,bool starting) {
        auto context=Context();NavigationStatus result=NavigationStatus::Unsupported;
        NavigationAccess::SetCalculating(*navigation,true);
        try {
            if(action==NavigationAction::ZoomDrag||action==NavigationAction::ZoomWheelForward||action==NavigationAction::ZoomWheelBackward
                ||action==NavigationAction::CameraZoomDrag||action==NavigationAction::CameraZoomForward||action==NavigationAction::CameraZoomBackward||action==NavigationAction::CameraZoomShiftCtrl) {
                auto r=navigation->BuildZoom(input,context);NavigationAccess::SetCalculating(*navigation,false);
                if(starting) {
                    std::array<double,3> p{};std::array<double,2> d{};if(!Anchor(r.anchor,p,d))return NavigationStatus::Invalid;
                    anchor=ZoomAnchor::WorldPoint(p);
                }
                r.anchor=anchor;result=Zoom(r);
            } else if(action==NavigationAction::PanDrag||action==NavigationAction::CameraPanDrag||action==NavigationAction::CameraPanShift) {
                auto r=navigation->BuildPan(input,context);NavigationAccess::SetCalculating(*navigation,false);result=Pan(r,displayScale);
            } else if(action==NavigationAction::CameraRotateDrag||action==NavigationAction::CameraRollCtrl) {
                auto r=navigation->BuildRotate(input,context);NavigationAccess::SetCalculating(*navigation,false);
                if(starting){std::array<double,3> p{};std::array<double,2> d{};if(!Anchor(r.pivot,p,d))return NavigationStatus::Invalid;anchor=ZoomAnchor::WorldPoint(p);}
                r.pivot=anchor;result=Rotate(r);
            } else if(action==NavigationAction::SliceForward||action==NavigationAction::SliceBackward) {
                auto r=navigation->BuildSliceScroll(input,context);NavigationAccess::SetCalculating(*navigation,false);result=Scroll(r.count);
            } else if(action==NavigationAction::WindowLevelDrag) {
                auto r=navigation->BuildWindowLevel(input,context);NavigationAccess::SetCalculating(*navigation,false);result=WindowLevel(r);
            }
        } catch(...) {NavigationAccess::SetCalculating(*navigation,false);result=NavigationStatus::Failed;}
        return result;
    }
    double displayScale=1;
};
ViewNavigationExecutor::ViewNavigationExecutor(InteractionPorts p,vtkRenderer* r,std::function<bool()> valid,std::function<bool()> captured):m_impl(std::make_unique<Impl>(std::move(p),r,std::move(valid),std::move(captured))) {}
ViewNavigationExecutor::~ViewNavigationExecutor(){(void)m_impl->Cleanup();NavigationAccess::Unbind(*m_impl->navigation);m_impl->endpoint->isValid={};}
std::shared_ptr<ViewNavigation> ViewNavigationExecutor::GetNavigation() const{return m_impl->navigation;}
NavigationStatus ViewNavigationExecutor::SetNavigation(std::shared_ptr<ViewNavigation> n) {
    if(!m_impl->Valid())return NavigationStatus::Unbound;
    if(m_impl->endpoint->isCalculating)return NavigationStatus::Busy;
    if(!n)n=std::make_shared<ViewNavigation>();if(n==m_impl->navigation)return NavigationStatus::Applied;
    const auto s=NavigationAccess::Bind(*n,m_impl->endpoint);if(s!=NavigationStatus::Applied)return s;
    NavigationAccess::Unbind(*m_impl->navigation);m_impl->navigation=std::move(n);return NavigationStatus::Applied;
}
void ViewNavigationExecutor::SetMode(VizMode mode){m_impl->currentMode=mode;}
void ViewNavigationExecutor::SetPointerPosition(std::optional<std::array<double,2>> position,double scale) {
    if(m_impl->endpoint->owner!=std::this_thread::get_id())return;
    if(!std::isfinite(scale)||scale<=0||position&&(!std::isfinite((*position)[0])||!std::isfinite((*position)[1])))return;
    m_impl->pointerPosition=position;m_impl->displayScale=scale;
}
void ViewNavigationExecutor::ResetReference(){m_impl->referenceHeight=0;}
InteractionCaptureKey ViewNavigationExecutor::GetCaptureKey() const noexcept{return {0,static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this))};}
InteractionEventKind ViewNavigationExecutor::GetReleaseKind() const noexcept{return m_impl->release;}
bool ViewNavigationExecutor::GetIsActive() const noexcept{return m_impl->action!=NavigationAction::Count;}
InteractionResult ViewNavigationExecutor::Send(const InteractionEvent& e,const NavigationBindings& bindings) {
    auto& p=*m_impl;
    if(e.eventKind==InteractionEventKind::Cancel)return GetIsActive()?Result(p.Cleanup()):InteractionResult{};
    if(GetIsActive()&&(e.eventKind==p.release||p.isWheelGesture&&e.phase==InputPhase::End))return Result(p.Cleanup());
    if(!p.Valid()||e.area!=InputArea::View)return {};
    p.currentMode=e.vizMode;
    const bool wheel=IsWheel(e.eventKind);
    const bool active=GetIsActive();
    if(active&&!wheel&&e.eventKind!=InteractionEventKind::PointerMove)return {true,true};
    if(!active) {
        for(auto a:{NavigationAction::SliceForward,NavigationAction::SliceBackward,NavigationAction::WindowLevelDrag,
            NavigationAction::ZoomDrag,NavigationAction::ZoomWheelForward,NavigationAction::ZoomWheelBackward,
            NavigationAction::PanDrag,NavigationAction::CameraRotateDrag,NavigationAction::CameraPanDrag,
            NavigationAction::CameraZoomDrag,NavigationAction::CameraZoomForward,NavigationAction::CameraZoomBackward,
            NavigationAction::CameraPanShift,NavigationAction::CameraRollCtrl,NavigationAction::CameraZoomShiftCtrl})
            if(bindings.GetMatched(a,e)){p.action=a;break;}
        if(p.action==NavigationAction::Count)return {};
        if(p.endpoint->hasCapture&&p.endpoint->hasCapture()){p.action=NavigationAction::Count;return Result(NavigationStatus::Busy);}
        p.options=p.navigation->GetOptions();{const auto wl=p.ports.slice->GetWindowLevel();p.initialWindowLevel={wl.windowWidth,wl.windowCenter};}p.precisionBinding=bindings.GetInputBinding(NavigationAction::Precision);p.acceleratedBinding=bindings.GetInputBinding(NavigationAction::Accelerated);p.isWheelGesture=wheel&&e.phase!=InputPhase::Discrete;
        p.release=wheel?InteractionEventKind::None:e.eventKind==InteractionEventKind::MiddlePress?InteractionEventKind::MiddleRelease
            :e.eventKind==InteractionEventKind::SecondaryPress?InteractionEventKind::SecondaryRelease:InteractionEventKind::PrimaryRelease;
        if(!p.ports.state->SetInteracting(p.source,true)){p.action=NavigationAction::Count;return Result(NavigationStatus::Failed);}
    }
    const auto position=e.displayPosition.value_or(std::array<double,2>{static_cast<double>(e.x),static_cast<double>(e.y)});
    if(!std::isfinite(e.displayScale)||e.displayScale<=0){(void)p.Cleanup();return Result(NavigationStatus::Invalid);}
    p.displayScale=e.displayScale;
    NavigationInput input;input.displayPosition=position;input.hasPointer=true;input.isWheel=wheel;input.phase=e.phase;
    input.wheelUnit=e.wheelUnit;input.displayScale=e.displayScale;input.isRollMode=p.action==NavigationAction::CameraRollCtrl;
    input.isPrecisionMode=p.precisionBinding&&InputBindingMatch::Matches(*p.precisionBinding,e);input.isAccelerated=p.acceleratedBinding&&InputBindingMatch::Matches(*p.acceleratedBinding,e);
    if(active&&!wheel){input.deltaX=position[0]/e.displayScale-p.lastPosition[0];input.deltaY=position[1]/e.displayScale-p.lastPosition[1];}
    if(wheel)input.wheelDelta=e.wheelDelta.value_or(e.eventKind==InteractionEventKind::WheelForward?1.0:-1.0);
    // 反向绑定只改变操作方向，不改变一次滚轮的真实幅度。
    if(p.action==NavigationAction::SliceForward)input.wheelDelta=std::abs(input.wheelDelta);
    if(p.action==NavigationAction::SliceBackward)input.wheelDelta=-std::abs(input.wheelDelta);
    p.lastPosition={position[0]/e.displayScale,position[1]/e.displayScale};
    const auto status=p.Calculate(input,!active);
    if(status!=NavigationStatus::Applied||wheel&&!p.isWheelGesture)(void)p.Cleanup();
    return Result(status);
}
NavigationStatus ViewNavigationExecutor::Trigger(std::string_view id,const NavigationInput& input) {
    auto& p=*m_impl;if(!p.Valid())return NavigationStatus::Unbound;
    if(p.endpoint->isCalculating||(p.endpoint->hasCapture&&p.endpoint->hasCapture()))return NavigationStatus::Busy;
    if(id=="navigation.zoom")p.action=NavigationAction::ZoomDrag;
    else if(id=="navigation.pan")p.action=NavigationAction::PanDrag;
    else if(id=="navigation.rotate")p.action=NavigationAction::CameraRotateDrag;
    else if(id=="navigation.sliceScroll")p.action=NavigationAction::SliceForward;
    else if(id=="navigation.windowLevel")p.action=NavigationAction::WindowLevelDrag;
    else return NavigationStatus::Unsupported;
    p.options=p.navigation->GetOptions();{const auto wl=p.ports.slice->GetWindowLevel();p.initialWindowLevel={wl.windowWidth,wl.windowCenter};}p.displayScale=input.displayScale;
    auto value=input;
    if(id=="navigation.zoom"&&value.deltaY==0){value.isWheel=true;if(value.wheelDelta==0)value.wheelDelta=1;}
    const auto result=p.Calculate(value,true);p.action=NavigationAction::Count;return result;
}
namespace {
class ViewNavigationCapture final:public IInteractionCapture {
public:
    ViewNavigationCapture(std::shared_ptr<ViewNavigationExecutor> e,const NavigationBindings& b):m_executor(std::move(e)),m_bindings(b){}
    InteractionCaptureKey GetKey() const noexcept override{return m_executor->GetCaptureKey();}
    InteractionEventKind GetReleaseKind() const noexcept override{return m_executor->GetReleaseKind();}
    bool GetIsActive() const noexcept override{return m_executor->GetIsActive();}
    InteractionResult Send(const InteractionEvent& e) override{return m_executor->Send(e,m_bindings);}
private:std::shared_ptr<ViewNavigationExecutor> m_executor;const NavigationBindings& m_bindings;
};
}
ViewNavigationHandler::ViewNavigationHandler(std::shared_ptr<ViewNavigationExecutor> e,const NavigationBindings& b):m_executor(std::move(e)),m_bindings(b){}
InteractionResult ViewNavigationHandler::Send(const InteractionEvent& e){return m_executor->Send(e,m_bindings);}
InteractionDispatch ViewNavigationHandler::Route(const InteractionEvent& e){
    const auto result=Send(e);
    return {result,result.isHandled&&m_executor->GetIsActive()?std::make_unique<ViewNavigationCapture>(m_executor,m_bindings):nullptr,!result.isSucceeded&&m_executor->GetIsActive()};
}
