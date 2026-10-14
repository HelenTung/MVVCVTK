#pragma once
#include "Interaction/InputBindings.h"
#include "Interaction/ViewNavigation.h"
#include <functional>
#include <memory>
#include <string_view>

struct InteractionActionBinding final {
    std::string bindingKey;
    InputBinding defaultBinding;
    bool isEnabledByDefault=true;
    BindingScope scope;
    // 同一场景内的同级输入不能重叠；工具场景由所属模块激活。
    std::string scene;
    std::vector<InputTriggerKind> supportedTriggers;
};
struct InteractionActionInfo final {
    std::string actionId;
    std::string ownerId;
    std::vector<InteractionActionBinding> bindings;
    bool isCurrentToolCommand=false;
    bool isTriggerable=false;
};
struct ActionContext final {
    std::string viewId;
    InputArea area=InputArea::View;
    NavigationInput input;
};
enum class ActionTriggerStatus : std::uint8_t {
    Completed, Accepted, Unavailable, Invalid, Busy, Failed
};
struct ActionTriggerResult final {
    ActionTriggerStatus status=ActionTriggerStatus::Unavailable;
    std::uint64_t requestId=0;
};
struct NavigationSettings final { std::string viewId; NavigationOptions options; };
struct InteractionSettings final {
    std::vector<BindingChange> bindings;
    std::vector<NavigationSettings> navigation;
};
struct ApplicationAction final {
    InteractionActionInfo info;
    // owner thread 同步调用；异步动作返回 Accepted，沿用原业务完成链。
    std::function<ActionTriggerResult(const ActionContext&)> execute;
};
class InteractionControllerAccess;
// Session 统一配置句柄；调度和捕获规则不能继承重写。
class InteractionController final {
public:
    InteractionController();
    ~InteractionController();
    InteractionController(const InteractionController&)=delete;
    InteractionController& operator=(const InteractionController&)=delete;
    BindingUpdateResult ApplyBindings(const BindingPatch& patch);
    BindingUpdateResult ImportBindings(const IInputBindings& source,const BindingScope& scope={});
    std::vector<EffectiveBinding> GetBindings(const BindingScope& scope={}) const;
    std::vector<InteractionActionInfo> GetActions() const;
    std::shared_ptr<ViewNavigation> GetNavigation(const std::string& viewId);
    NavigationStatus SetNavigation(const std::string& viewId,std::shared_ptr<ViewNavigation> navigation);
    ActionTriggerResult Trigger(std::string_view actionId,const ActionContext& context);
    bool RegisterAction(ApplicationAction action);
    bool RemoveAction(std::string_view actionId);
    InteractionSettings GetSettings() const;
    BindingUpdateResult LoadSettings(const InteractionSettings& settings);
private:
    friend class InteractionControllerAccess;
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
