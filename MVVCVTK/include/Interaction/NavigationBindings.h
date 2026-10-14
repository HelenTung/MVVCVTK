#pragma once
#include "Interaction/InteractionControllerAccess.h"
#include <memory>

enum class NavigationAction : std::size_t {
    SliceForward, SliceBackward, CrosshairDrag, WindowLevelDrag, ZoomDrag,
    PlaneDrag, ModelPanDrag, ModelScaleDrag, ModelSecondaryPanDrag,
    ZoomWheelForward, ZoomWheelBackward, PanDrag, CameraRotateDrag, CameraPanDrag,
    CameraZoomDrag, CameraZoomForward, CameraZoomBackward, CameraPanShift, CameraRollCtrl, CameraZoomShiftCtrl, Precision, Accelerated, Count
};
// 只读匹配视图，所有有效绑定来自 Session 的同一控制器。
class NavigationBindings final {
public:
    NavigationBindings();
    void SetController(std::shared_ptr<InteractionController> controller,std::string viewId);
    bool GetMatched(NavigationAction,const InteractionEvent&) const;
    bool GetIsEnabled(NavigationAction) const;
    std::optional<InputBinding> GetInputBinding(NavigationAction) const;
    static void RegisterActions(InteractionController&,std::function<ActionTriggerResult(std::string_view,const ActionContext&)> execute={});
    static std::string_view GetKey(NavigationAction);
private:
    void SetDefaults() const;
    mutable std::shared_ptr<InteractionController> m_defaults;
    mutable std::weak_ptr<InteractionController> m_controller;
    std::string m_viewId;
};
