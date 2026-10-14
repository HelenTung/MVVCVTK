#include "Interaction/NavigationBindings.h"
#include <array>
#include <cmath>
namespace {
constexpr std::array<std::string_view,static_cast<std::size_t>(NavigationAction::Count)> keys{
    NavigationBindingKeys::SliceForward,NavigationBindingKeys::SliceBackward,
    NavigationBindingKeys::CrosshairDrag,NavigationBindingKeys::WindowLevelDrag,NavigationBindingKeys::ZoomDrag,
    NavigationBindingKeys::PlaneDrag,NavigationBindingKeys::ModelPanDrag,NavigationBindingKeys::ModelScaleDrag,
    NavigationBindingKeys::ModelSecondaryPanDrag,NavigationBindingKeys::ZoomWheelForward,
    NavigationBindingKeys::ZoomWheelBackward,NavigationBindingKeys::PanDrag,NavigationBindingKeys::CameraRotateDrag,
    NavigationBindingKeys::CameraPanDrag,NavigationBindingKeys::CameraZoomDrag,
    NavigationBindingKeys::CameraZoomForward,NavigationBindingKeys::CameraZoomBackward,
    NavigationBindingKeys::CameraPanShift,NavigationBindingKeys::CameraRollCtrl,NavigationBindingKeys::CameraZoomShiftCtrl,NavigationBindingKeys::Precision,NavigationBindingKeys::Accelerated};
}
NavigationBindings::NavigationBindings()=default;
void NavigationBindings::SetDefaults() const {
    if(!m_viewId.empty()||!m_controller.expired())return;
    m_defaults=std::make_shared<InteractionController>();m_controller=m_defaults;RegisterActions(*m_defaults);
}
void NavigationBindings::SetController(std::shared_ptr<InteractionController> c,std::string id) {
    m_controller=c;m_viewId=std::move(id);m_defaults.reset();
}
std::string_view NavigationBindings::GetKey(NavigationAction a) {
    const auto i=static_cast<std::size_t>(a);return i<keys.size()?keys[i]:std::string_view{};
}
bool NavigationBindings::GetMatched(NavigationAction a,const InteractionEvent& e) const {
    SetDefaults();const auto c=m_controller.lock();if(!c) return false;
    auto event=e;if(event.viewId.empty())event.viewId=m_viewId;
    return InteractionControllerAccess::Matches(*c,GetKey(a),event);
}
bool NavigationBindings::GetIsEnabled(NavigationAction a) const {
    SetDefaults();const auto c=m_controller.lock();if(!c)return false;
    const auto values=c->GetBindings(BindingScope::View(m_viewId));
    for(const auto& b:values) if(b.bindingKey==GetKey(a))return b.isEnabled&&!b.hasConflict;
    return false;
}
std::optional<InputBinding> NavigationBindings::GetInputBinding(NavigationAction a) const {
    SetDefaults();const auto c=m_controller.lock();if(!c)return {};
    for(const auto& b:c->GetBindings(BindingScope::View(m_viewId)))if(b.bindingKey==GetKey(a)&&b.isEnabled&&!b.hasConflict)return b.binding;
    return {};
}
void NavigationBindings::RegisterActions(InteractionController& c,std::function<ActionTriggerResult(std::string_view,const ActionContext&)> execute) {
    const auto add=[&](NavigationAction a,const char* action,InputBinding input,InteractionViewClass view,
        const char* scene="navigation",bool enabled=true) {
        InteractionActionInfo info;info.actionId=action;info.ownerId="navigation";info.isTriggerable=std::string_view(action)=="navigation.zoom"||std::string_view(action)=="navigation.pan"||std::string_view(action)=="navigation.rotate"||std::string_view(action)=="navigation.sliceScroll"||std::string_view(action)=="navigation.windowLevel";
        BindingScope scope;scope.viewClass=view;
        std::vector<InputTriggerKind> triggers{input.trigger};
        if(input.trigger==InputTriggerKind::WheelForward||input.trigger==InputTriggerKind::WheelBackward)triggers={InputTriggerKind::WheelForward,InputTriggerKind::WheelBackward};
        if(std::string_view(action)=="navigation.zoom")triggers={InputTriggerKind::Drag,InputTriggerKind::WheelForward,InputTriggerKind::WheelBackward};
        info.bindings.push_back({std::string(GetKey(a)),input,enabled,scope,scene,std::move(triggers)});
        // 同一操作可能具有多个绑定项，统一登记后再由视图执行。
        return info;
    };
    std::vector<InteractionActionInfo> actions;
    const auto push=[&](InteractionActionInfo info) {
        for(auto& a:actions) if(a.actionId==info.actionId) {a.bindings.push_back(std::move(info.bindings.front()));return;}
        actions.push_back(std::move(info));
    };
    const auto slice=InteractionViewClass::Slice,three=InteractionViewClass::ThreeD;
    push(add(NavigationAction::SliceForward,"navigation.sliceScroll",{InputTriggerKind::WheelForward,InputMouseButton::None,0,0,0,true},slice));
    push(add(NavigationAction::SliceBackward,"navigation.sliceScroll",{InputTriggerKind::WheelBackward,InputMouseButton::None,0,0,0,true},slice));
    push(add(NavigationAction::CrosshairDrag,"navigation.crosshair",{InputTriggerKind::Drag,InputMouseButton::Primary,1,2},slice));
    push(add(NavigationAction::WindowLevelDrag,"navigation.windowLevel",{InputTriggerKind::Drag,InputMouseButton::Primary,0,3},slice));
    push(add(NavigationAction::ZoomDrag,"navigation.zoom",{InputTriggerKind::Drag,InputMouseButton::Secondary},slice));
    push(add(NavigationAction::PlaneDrag,"navigation.plane",{InputTriggerKind::Drag,InputMouseButton::Primary},three,"plane"));
    push(add(NavigationAction::ModelPanDrag,"navigation.modelPan",{InputTriggerKind::Drag,InputMouseButton::Primary,1,2},three,"modelTransform"));
    push(add(NavigationAction::ModelScaleDrag,"navigation.modelScale",{InputTriggerKind::Drag,InputMouseButton::Primary,3,0},three,"modelTransform"));
    push(add(NavigationAction::ModelSecondaryPanDrag,"navigation.modelPan",{InputTriggerKind::Drag,InputMouseButton::Secondary},three,"modelTransform"));
    push(add(NavigationAction::ZoomWheelForward,"navigation.zoom",{InputTriggerKind::WheelForward,InputMouseButton::None},slice,"navigation",false));
    push(add(NavigationAction::ZoomWheelBackward,"navigation.zoom",{InputTriggerKind::WheelBackward,InputMouseButton::None},slice,"navigation",false));
    push(add(NavigationAction::PanDrag,"navigation.pan",{InputTriggerKind::Drag,InputMouseButton::Middle},slice));
    push(add(NavigationAction::CameraRotateDrag,"navigation.rotate",{InputTriggerKind::Drag,InputMouseButton::Primary,0,3},three));
    push(add(NavigationAction::CameraPanDrag,"navigation.pan",{InputTriggerKind::Drag,InputMouseButton::Middle},three));
    push(add(NavigationAction::CameraZoomDrag,"navigation.zoom",{InputTriggerKind::Drag,InputMouseButton::Secondary},three));
    push(add(NavigationAction::CameraZoomForward,"navigation.zoom",{InputTriggerKind::WheelForward,InputMouseButton::None},three));
    push(add(NavigationAction::CameraZoomBackward,"navigation.zoom",{InputTriggerKind::WheelBackward,InputMouseButton::None},three));
    push(add(NavigationAction::CameraPanShift,"navigation.pan",{InputTriggerKind::Drag,InputMouseButton::Primary,1,2},three));
    push(add(NavigationAction::CameraRollCtrl,"navigation.rotate",{InputTriggerKind::Drag,InputMouseButton::Primary,2,1},three));
    push(add(NavigationAction::CameraZoomShiftCtrl,"navigation.zoom",{InputTriggerKind::Drag,InputMouseButton::Primary,3,0},three));
    push(add(NavigationAction::Precision,"navigation.precision",{InputTriggerKind::Modifiers,InputMouseButton::None,4,0},InteractionViewClass::Any,""));
    push(add(NavigationAction::Accelerated,"navigation.accelerated",{InputTriggerKind::Modifiers,InputMouseButton::None,2,0},InteractionViewClass::Any,""));
    for(auto& a:actions) {
        const auto id=a.actionId;
        InteractionControllerAccess::Register(c,std::move(a),{},[&c,id,execute](const ActionContext& ctx) {
            if(execute)return execute(id,ctx);
            const auto n=c.GetNavigation(ctx.viewId);if(!n)return ActionTriggerResult{};
            NavigationStatus s=NavigationStatus::Unsupported;
            if(id=="navigation.zoom")s=n->ZoomBy(std::pow(n->GetOptions().zoom.wheelFactorPerStep,ctx.input.wheelDelta));
            else if(id=="navigation.pan")s=n->PanByPixels(ctx.input.deltaX,ctx.input.deltaY);
            else if(id=="navigation.rotate")s=n->RotateBy(ctx.input.deltaX,ctx.input.deltaY,0);
            else if(id=="navigation.sliceScroll")s=n->ScrollSlices(static_cast<int>(ctx.input.wheelDelta));
            return ActionTriggerResult{s==NavigationStatus::Applied?ActionTriggerStatus::Completed:ActionTriggerStatus::Failed,0};
        });
    }
}
