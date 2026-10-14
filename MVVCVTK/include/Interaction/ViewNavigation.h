#pragma once
#include "Interaction/NavigationTypes.h"
#include <memory>

class NavigationAccess;
// owner thread 控制句柄；弱关联实际视图，不持有整个 Session。
class ViewNavigation {
public:
    ViewNavigation();
    virtual ~ViewNavigation();
    ViewNavigation(const ViewNavigation&)=delete;
    ViewNavigation& operator=(const ViewNavigation&)=delete;
    NavigationStatus SetOptions(const NavigationOptions& options);
    NavigationOptions GetOptions() const;
    std::optional<NavigationState> GetState() const;
    NavigationStatus ZoomBy(double factor,const ZoomAnchor& anchor=ZoomAnchor::ViewportCenter());
    NavigationStatus SetZoomRatio(double ratio,const ZoomAnchor& anchor=ZoomAnchor::ViewportCenter());
    NavigationStatus PanByPixels(double deltaX,double deltaY);
    NavigationStatus RotateBy(double yaw,double pitch,double roll,const ZoomAnchor& pivot=ZoomAnchor::ViewportCenter());
    NavigationStatus ScrollSlices(int count);
    NavigationStatus SetWindowLevel(double width,double center);
    NavigationStatus FitToView();
    // 同步纯计算；不得重入视图写入，异常结束该次导航。
    virtual ZoomRequest BuildZoom(const NavigationInput&,const NavigationContext&) const;
    virtual PanRequest BuildPan(const NavigationInput&,const NavigationContext&) const;
    virtual RotateRequest BuildRotate(const NavigationInput&,const NavigationContext&) const;
    virtual SliceScrollRequest BuildSliceScroll(const NavigationInput&,const NavigationContext&) const;
    virtual WindowLevelRequest BuildWindowLevel(const NavigationInput&,const NavigationContext&) const;
private:
    friend class NavigationAccess;
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
