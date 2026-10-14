#pragma once
#include "Interaction/ViewNavigation.h"
#include <functional>
#include <thread>

// 私有执行关联；鼠标与直接调用共用相同执行函数。
struct NavigationEndpoint final {
    std::thread::id owner=std::this_thread::get_id();
    std::function<bool()> isValid;
    std::function<bool()> hasCapture;
    std::function<std::optional<NavigationState>()> state;
    std::function<NavigationStatus(const ZoomRequest&)> zoom;
    std::function<NavigationStatus(const PanRequest&)> pan;
    std::function<NavigationStatus(const RotateRequest&)> rotate;
    std::function<NavigationStatus(double)> scroll;
    std::function<NavigationStatus(const WindowLevelRequest&)> windowLevel;
    std::function<NavigationStatus()> fit;
    bool isCalculating=false;
};
class NavigationAccess final {
public:
    static NavigationStatus Bind(ViewNavigation&,const std::shared_ptr<NavigationEndpoint>&);
    static void Unbind(ViewNavigation&);
    static void SetCalculating(ViewNavigation&,bool);
};
