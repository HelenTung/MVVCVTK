#include "InteractionControllerTests.h"
#include "Host/HostInputRegistry.h"
#include "Host/HostCommandRouter.h"
#include "Host/HostRoutes.h"
#include "ViewContext.h"
#include "Interaction/InteractionControllerAccess.h"
#include "Interaction/NavigationBindings.h"
#include <iostream>

int InteractionControllerSuite::GetFailCount() const {
    int failures=0;const auto check=[&](bool ok,const char* text){if(!ok){++failures;std::cerr<<text<<'\n';}};
    auto views=std::make_shared<HostRouteStub>();
    auto first=std::make_shared<ViewContextStub>();auto second=std::make_shared<ViewContextStub>();
    views->CreateView("first",HostRenderViewRole::FrontBackSlice,first);
    views->CreateView("second",HostRenderViewRole::FrontBackSlice,second);
    HostInputRegistry registry(views->GetViewDirectory());
    HostViewTargets all;all.viewIds={"first","second"};check(registry.Start(all),"统一输入注册失败");
    const auto c=registry.GetController();
    BindingPatch patch;
    InputBinding zoom{InputTriggerKind::WheelForward,InputMouseButton::None,2,5};
    patch.Replace(std::string(NavigationBindingKeys::ZoomWheelForward),zoom,BindingScope::Slices());
    check(c->ApplyBindings(patch).status==InputBindingStatus::Conflict,"单独抢占 Ctrl 滚轮必须报告冲突");
    patch.Replace(std::string(NavigationBindingKeys::SliceForward),{InputTriggerKind::WheelForward,InputMouseButton::None,0,2},BindingScope::Slices());
    check(c->ApplyBindings(patch).status==InputBindingStatus::Applied,"批量让键应同时生效");
    BindingPatch perView;perView.Disable(std::string(NavigationBindingKeys::ZoomWheelForward),BindingScope::View("first"));
    check(c->ApplyBindings(perView).status==InputBindingStatus::Applied,"视图禁用失败");
    InteractionEvent wheel;wheel.viewId="first";wheel.vizMode=VizMode::SliceFront_back;wheel.eventKind=InteractionEventKind::WheelForward;wheel.isCtrlDown=true;
    check(!InteractionControllerAccess::Matches(*c,NavigationBindingKeys::ZoomWheelForward,wheel),"Disable 不得恢复下层默认");
    wheel.viewId="second";check(InteractionControllerAccess::Matches(*c,NavigationBindingKeys::ZoomWheelForward,wheel),"视图覆盖不应串窗口");
    perView.changes.clear();perView.UseDefault(std::string(NavigationBindingKeys::ZoomWheelForward),BindingScope::View("first"));
    check(c->ApplyBindings(perView).status==InputBindingStatus::Applied,"恢复继承失败");
    wheel.viewId="first";check(InteractionControllerAccess::Matches(*c,NavigationBindingKeys::ZoomWheelForward,wheel),"UseDefault 应继承切片层");
    BindingPatch invalidDefault; BindingScope invalidScope; invalidScope.area=static_cast<InputArea>(255);
    invalidDefault.UseDefault(std::string(NavigationBindingKeys::ZoomWheelForward),invalidScope);
    check(c->ApplyBindings(invalidDefault).status==InputBindingStatus::Invalid,"UseDefault 也必须校验作用域");
    InteractionEvent physical;physical.viewId="first";physical.eventKind=InteractionEventKind::KeyPress;physical.key='E';
    (void)InteractionControllerAccess::PrepareKeyEvent(*c,physical);InteractionControllerAccess::SetKeyHandled(*c,physical,true);
    physical.eventKind=InteractionEventKind::TextInput;
    check(InteractionControllerAccess::PrepareKeyEvent(*c,physical),"已处理按键的 TextInput 应被消费");
    physical.viewId="second";physical.eventKind=InteractionEventKind::KeyRelease;
    check(InteractionControllerAccess::PrepareKeyEvent(*c,physical),"焦点换视图后的 release 应清理原物理键");
    physical.eventKind=InteractionEventKind::KeyPress;physical.isAutoRepeat=false;
    (void)InteractionControllerAccess::PrepareKeyEvent(*c,physical);check(!physical.isAutoRepeat,"释放后重新按下不能误判为 repeat");
    InteractionControllerAccess::ClearKeys(*c,"second");
    BindingPatch future;future.Disable("future.commit.key");check(c->ApplyBindings(future).status==InputBindingStatus::Applied,"未装配模块配置应可保留");
    const auto saved=c->GetSettings();check(!saved.bindings.empty(),"设置应保存可枚举的值");
    int count=0;std::string target;
    ApplicationAction app;app.info.actionId="application.exportData";app.info.ownerId="application";
    app.info.bindings.push_back({"application.exportData.key",{InputTriggerKind::KeyPress,InputMouseButton::None,2,5,'E'},true,{},"",{InputTriggerKind::KeyPress}});
    app.execute=[&](const ActionContext& context){++count;target=context.viewId;return ActionTriggerResult{ActionTriggerStatus::Accepted,42};};
    check(c->RegisterAction(std::move(app)),"应用动作登记失败");
    InteractionEvent key;key.eventKind=InteractionEventKind::KeyPress;key.key='E';key.isCtrlDown=true;
    const auto result=second->OnInput(key);check(result.isHandled&&count==1&&target=="second","快捷键必须使用实际 viewId");
    key.isAutoRepeat=true;(void)second->OnInput(key);check(count==1,"一次性操作不应接受自动重复");
    ActionContext context;context.viewId="first";
    const auto trigger=c->Trigger("application.exportData",context);
    check(trigger.status==ActionTriggerStatus::Accepted&&trigger.requestId==42&&count==2,"Trigger 必须保留异步接纳语义");
    BindingPatch disable;disable.Disable("application.exportData.key");check(c->ApplyBindings(disable).status==InputBindingStatus::Applied,"禁用应用快捷键失败");
    key.isAutoRepeat=false;(void)second->OnInput(key);check(count==2,"禁用后旧键不得执行");
    check(c->Trigger("application.exportData",context).status==ActionTriggerStatus::Accepted&&count==3,"禁用快捷键不应禁用按钮");
    check(c->Trigger("interaction.confirm",context).status==ActionTriggerStatus::Unavailable,"无当前工具不能猜测确认对象");
    ApplicationAction duplicate;duplicate.info.actionId="application.duplicate";duplicate.info.ownerId="application";
    duplicate.info.bindings.push_back({"application.exportData.key",{InputTriggerKind::KeyPress,InputMouseButton::None,0,7,'D'}});
    duplicate.execute=[](const ActionContext&){return ActionTriggerResult{ActionTriggerStatus::Completed,0};};
    check(!c->RegisterAction(std::move(duplicate)),"同一 bindingKey 不得跨动作重复登记");
    ApplicationAction conflicting;conflicting.info.actionId="application.another";conflicting.info.ownerId="application";
    conflicting.info.bindings.push_back({"application.another.key",{InputTriggerKind::KeyPress,InputMouseButton::None,2,5,'e'},true,{},"",{InputTriggerKind::KeyPress}});
    conflicting.execute=[](const ActionContext&){return ActionTriggerResult{ActionTriggerStatus::Completed,0};};
    check(c->RegisterAction(std::move(conflicting)),"动态动作冲突不应阻止装配");
    BindingPatch restore;restore.UseDefault("application.exportData.key");
    check(c->ApplyBindings(restore).status==InputBindingStatus::Conflict,"键码大小写归一后也必须参与冲突检查");
    check(c->RemoveAction("application.another"),"动态动作卸载失败");
    check(c->RemoveAction("application.exportData"),"应用动作卸载失败");
    check(registry.Stop(),"输入停止失败");
    check(c->ApplyBindings({}).status==InputBindingStatus::Failed&&c->Trigger("application.exportData",context).status==ActionTriggerStatus::Unavailable,"旧控制器在停止后必须失效");
    return failures;
}
