#include "Interaction/NavigationAccess.h"
#include <cmath>
#include <algorithm>

class ViewNavigation::Impl final {
public:
    NavigationOptions options;
    std::weak_ptr<NavigationEndpoint> endpoint;
    std::thread::id owner=std::this_thread::get_id();
    NavigationStatus GetStatus(bool write=true) const {
        if (owner!=std::this_thread::get_id()) return NavigationStatus::WrongThread;
        const auto p=endpoint.lock();
        if (!p || !p->isValid || !p->isValid()) return NavigationStatus::Unbound;
        return write&&(p->isCalculating||(p->hasCapture&&p->hasCapture())) ? NavigationStatus::Busy : NavigationStatus::Applied;
    }
};
ViewNavigation::ViewNavigation():m_impl(std::make_unique<Impl>()) {}
ViewNavigation::~ViewNavigation()=default;
NavigationStatus ViewNavigation::SetOptions(const NavigationOptions& o) {
    if (m_impl->owner!=std::this_thread::get_id()) return NavigationStatus::WrongThread;
    const auto positive=[](double v){return std::isfinite(v)&&v>0;};
    if (!positive(o.zoom.wheelFactorPerStep)
        ||!positive(o.zoom.dragSensitivity)||!positive(o.zoom.pixelSensitivity)
        ||!positive(o.zoom.precisionMultiplier)||!positive(o.pan.sensitivity)
        ||!positive(o.rotate.degreesPerPixel)||o.slice.countPerStep<=0||o.slice.acceleratedCount<=0
        ||!positive(o.windowLevel.widthSensitivity)||!positive(o.windowLevel.centerSensitivity)
        ||o.zoom.dragAnchor>ZoomAnchorMode::WorldPoint||o.zoom.wheelAnchor>ZoomAnchorMode::WorldPoint
        ||o.rotate.pivot>ZoomAnchorMode::WorldPoint) return NavigationStatus::Invalid;
    if (const auto p=m_impl->endpoint.lock();p&&p->isCalculating) return NavigationStatus::Busy;
    m_impl->options=o; return NavigationStatus::Applied;
}
NavigationOptions ViewNavigation::GetOptions() const { return m_impl->owner==std::this_thread::get_id()?m_impl->options:NavigationOptions{}; }
std::optional<NavigationState> ViewNavigation::GetState() const {
    if(m_impl->GetStatus(false)!=NavigationStatus::Applied) return {};
    return m_impl->endpoint.lock()->state();
}
NavigationStatus ViewNavigation::ZoomBy(double f,const ZoomAnchor& a) {
    if(!std::isfinite(f)||f<=0) return NavigationStatus::Invalid;
    const auto s=m_impl->GetStatus(); if(s!=NavigationStatus::Applied) return s;
    return m_impl->endpoint.lock()->zoom({f,a});
}
NavigationStatus ViewNavigation::SetZoomRatio(double r,const ZoomAnchor& a) {
    if(!std::isfinite(r)||r<=0) return NavigationStatus::Invalid;
    const auto state=GetState(); if(!state) {const auto s=m_impl->GetStatus();return s==NavigationStatus::Applied?NavigationStatus::Failed:s;}
    return ZoomBy(r/state->zoomRatio,a);
}
NavigationStatus ViewNavigation::PanByPixels(double x,double y) {
    if(!std::isfinite(x)||!std::isfinite(y)) return NavigationStatus::Invalid;
    const auto s=m_impl->GetStatus(); if(s!=NavigationStatus::Applied) return s;
    return m_impl->endpoint.lock()->pan({x,y});
}
NavigationStatus ViewNavigation::RotateBy(double y,double p,double r,const ZoomAnchor& a) {
    if(!std::isfinite(y)||!std::isfinite(p)||!std::isfinite(r)) return NavigationStatus::Invalid;
    const auto s=m_impl->GetStatus(); if(s!=NavigationStatus::Applied) return s;
    return m_impl->endpoint.lock()->rotate({y,p,r,a});
}
NavigationStatus ViewNavigation::ScrollSlices(int n) {
    const auto s=m_impl->GetStatus(); if(s!=NavigationStatus::Applied) return s;
    return m_impl->endpoint.lock()->scroll(n);
}
NavigationStatus ViewNavigation::SetWindowLevel(double w,double c) {
    if(!std::isfinite(w)||w<=0||!std::isfinite(c)) return NavigationStatus::Invalid;
    const auto s=m_impl->GetStatus(); if(s!=NavigationStatus::Applied) return s;
    return m_impl->endpoint.lock()->windowLevel({w,c});
}
NavigationStatus ViewNavigation::FitToView() {
    const auto s=m_impl->GetStatus(); if(s!=NavigationStatus::Applied) return s;
    return m_impl->endpoint.lock()->fit();
}
ZoomRequest ViewNavigation::BuildZoom(const NavigationInput& i,const NavigationContext& c) const {
    const auto& o=c.options.zoom;
    double logFactor=i.isWheel
        ? (i.wheelUnit==WheelUnit::Steps ? std::log(o.wheelFactorPerStep)*i.wheelDelta : o.pixelSensitivity*i.wheelDelta)
        : -o.dragSensitivity*i.deltaY;
    logFactor*=i.isPrecisionMode?o.precisionMultiplier:1;
    if(o.isInverted) logFactor=-logFactor;
    ZoomAnchor a; a.mode=i.isWheel?o.wheelAnchor:o.dragAnchor;
    if(a.mode==ZoomAnchorMode::Pointer) a=i.hasPointer?ZoomAnchor::DisplayPoint(i.displayPosition[0],i.displayPosition[1]):ZoomAnchor::ViewportCenter();
    if(a.mode==ZoomAnchorMode::Crosshair&&c.crosshairWorld) a=ZoomAnchor::WorldPoint(*c.crosshairWorld);
    return {std::exp(logFactor),a};
}
PanRequest ViewNavigation::BuildPan(const NavigationInput& i,const NavigationContext& c) const {
    const auto s=c.options.pan.sensitivity*(c.options.pan.isInverted?-1:1);
    return {i.deltaX*s,i.deltaY*s};
}
RotateRequest ViewNavigation::BuildRotate(const NavigationInput& i,const NavigationContext& c) const {
    if(i.isRollMode) {
        const double x=i.displayPosition[0]-c.viewportOrigin[0]-c.viewportSize[0]*0.5,y=i.displayPosition[1]-c.viewportOrigin[1]-c.viewportSize[1]*0.5;
        const double previous=std::atan2(y-i.deltaY*i.displayScale,x-i.deltaX*i.displayScale);
        const double current=std::atan2(y,x);
        return {0,0,(current-previous)*180/3.14159265358979323846,ZoomAnchor::ViewportCenter()};
    }
    ZoomAnchor p; p.mode=c.options.rotate.pivot;
    if(p.mode==ZoomAnchorMode::Pointer) p=i.hasPointer?ZoomAnchor::DisplayPoint(i.displayPosition[0],i.displayPosition[1]):ZoomAnchor::ViewportCenter();
    if(p.mode==ZoomAnchorMode::Crosshair&&c.crosshairWorld) p=ZoomAnchor::WorldPoint(*c.crosshairWorld);
    return {-i.deltaX*c.options.rotate.degreesPerPixel,i.deltaY*c.options.rotate.degreesPerPixel,0,p};
}
SliceScrollRequest ViewNavigation::BuildSliceScroll(const NavigationInput& i,const NavigationContext& c) const {
    return {i.wheelDelta*(i.isAccelerated?c.options.slice.acceleratedCount:c.options.slice.countPerStep)};
}
WindowLevelRequest ViewNavigation::BuildWindowLevel(const NavigationInput& i,const NavigationContext& c) const {
    const auto& o=c.options.windowLevel;
    const auto initial=c.startWindowLevel.value_or(WindowLevelRequest{c.state.windowWidth,c.state.windowCenter});
    const double dx=i.deltaX*i.displayScale/std::max(1,c.viewportSize[0]);
    const double dy=i.deltaY*i.displayScale/std::max(1,c.viewportSize[1]);
    // 沿用原调窗的幅度与方向，增量始终按手势开始时的 WW/WC 计算。
    return {std::max(0.01,c.state.windowWidth+dx*std::max(0.01,std::abs(initial.width))*o.widthSensitivity*(o.isWidthInverted?-1:1)),
        c.state.windowCenter-dy*std::max(0.01,std::abs(initial.center))*o.centerSensitivity*(o.isCenterInverted?-1:1)};
}
NavigationStatus NavigationAccess::Bind(ViewNavigation& n,const std::shared_ptr<NavigationEndpoint>& p) {
    if(n.m_impl->owner!=std::this_thread::get_id()||p->owner!=std::this_thread::get_id()) return NavigationStatus::WrongThread;
    const auto current=n.m_impl->endpoint.lock();
    if(current&&current!=p) return NavigationStatus::Busy;
    n.m_impl->endpoint=p; return NavigationStatus::Applied;
}
void NavigationAccess::Unbind(ViewNavigation& n) { n.m_impl->endpoint.reset(); }
void NavigationAccess::SetCalculating(ViewNavigation& n,bool value) {
    if(const auto p=n.m_impl->endpoint.lock()) p->isCalculating=value;
}
