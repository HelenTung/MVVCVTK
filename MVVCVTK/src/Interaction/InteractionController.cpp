#include "Interaction/InteractionControllerAccess.h"
#include "Interaction/InputBindingMatch.h"
#include <algorithm>
#include <map>
#include <set>
#include <thread>

namespace {
bool SameScope(const BindingScope& a,const BindingScope& b) {
    return a.viewId==b.viewId&&a.viewClass==b.viewClass&&a.area==b.area;
}
bool Applies(const BindingScope& scope,const BindingScope& view) {
    return scope.area==view.area&&(scope.viewId.empty()||scope.viewId==view.viewId)
        &&(scope.viewClass==InteractionViewClass::Any||scope.viewClass==view.viewClass);
}
int Rank(const BindingScope& s) { return !s.viewId.empty()?3:s.viewClass!=InteractionViewClass::Any?2:1; }
bool IsSlice(VizMode m) { return m==VizMode::SliceTop_down||m==VizMode::SliceFront_back||m==VizMode::SliceLeft_right; }
}
class InteractionController::Impl final {
public:
    struct Entry final {
        InteractionActionInfo info;
        std::vector<std::string> views;
        std::function<ActionTriggerResult(const ActionContext&)> execute;
        bool isApplication=false;
    };
    struct Cache final { BindingScope scope; std::vector<EffectiveBinding> values; };
    mutable std::vector<Cache> cache;
    const std::thread::id owner=std::this_thread::get_id();
    bool isValid=true;
    bool isTriggering=false;
    bool isApplying=false;
    bool isImporting=false;
    std::function<void()> onBindingsChanged;
    std::function<ActionTriggerResult(std::string_view,const ActionContext&)> fallback;
    std::function<ActionTriggerResult(const ActionContext&)> cancelCapture;
    std::vector<InteractionView> views;
    std::vector<std::shared_ptr<Entry>> actions;
    std::vector<BindingChange> changes;
    std::map<std::string,std::string> owners;
    struct KeyOwner final {std::string viewId;bool isHandled=false;};
    std::map<std::uint32_t,KeyOwner> pressedKeys;
    std::function<std::shared_ptr<ViewNavigation>(const std::string&)> getNavigation;
    std::function<NavigationStatus(const std::string&,std::shared_ptr<ViewNavigation>)> setNavigation;
    bool Valid() const { return isValid&&owner==std::this_thread::get_id(); }
    void NotifyBindingsChanged() {
        if(!onBindingsChanged)return;
        struct Guard {bool& flag;bool previous;~Guard(){flag=previous;}} guard{isApplying,isApplying};
        isApplying=true;onBindingsChanged();
    }
    bool ValidKeys(const InteractionActionInfo& info) const {
        std::set<std::string> keys;
        for(const auto& b:info.bindings) {
            if(b.bindingKey.empty()||!keys.insert(b.bindingKey).second||!InputBindingMatch::Valid(b.defaultBinding))return false;
            for(const auto& e:actions)for(const auto& existing:e->info.bindings)if(existing.bindingKey==b.bindingKey)return false;
        }
        return true;
    }
    BindingScope Scope(const std::string& id,InputArea area=InputArea::View) const {
        BindingScope scope;scope.viewId=id;scope.area=area;
        const auto found=std::find_if(views.begin(),views.end(),[&](const auto& v){return v.id==id;});
        if(found!=views.end()) scope.viewClass=found->viewClass;
        return scope;
    }
    EffectiveBinding Resolve(const std::shared_ptr<Entry>& e,const InteractionActionBinding& b,
        const BindingScope& scope,const std::vector<BindingChange>& source) const {
        EffectiveBinding value{b.bindingKey,e->info.actionId,e->info.ownerId,b.defaultBinding,
            BindingOrigin::Default,b.isEnabledByDefault,true,false};
        if(!Applies(b.scope,scope)||(!e->views.empty()&&std::find(e->views.begin(),e->views.end(),scope.viewId)==e->views.end())) value.isAvailable=false;
        value.viewId=scope.viewId;
        int rank=0;
        for(const auto& c:source) if(c.bindingKey==b.bindingKey&&Applies(c.scope,scope)&&Rank(c.scope)>rank) {
            rank=Rank(c.scope);value.origin=rank==3?BindingOrigin::View:rank==2?BindingOrigin::ViewClass:BindingOrigin::Session;
            value.isEnabled=c.choice==InputBindingOverride::Replace;
            if(value.isEnabled) value.binding=c.binding;
        }
        if(value.isEnabled&&!b.supportedTriggers.empty()&&std::find(b.supportedTriggers.begin(),b.supportedTriggers.end(),value.binding.trigger)==b.supportedTriggers.end()) {
            value.isEnabled=false;value.status=InputBindingStatus::Unsupported;
        }
        return value;
    }
    std::vector<EffectiveBinding> Bindings(const BindingScope& scope,const std::vector<BindingChange>& source) const {
        std::vector<EffectiveBinding> out;
        std::vector<std::string> scenes;
        for(const auto& e:actions) for(const auto& b:e->info.bindings) {
            auto value=Resolve(e,b,scope,source);
            if(!value.isAvailable) continue;
            out.push_back(std::move(value));scenes.push_back(b.scene);
        }
        for(std::size_t i=0;i<out.size();++i) for(std::size_t j=i+1;j<out.size();++j) {
            if(out[i].isEnabled&&out[j].isEnabled&&scenes[i]==scenes[j]
                &&out[i].actionId!=out[j].actionId&&InputBindingMatch::Overlaps(out[i].binding,out[j].binding)) {
                out[i].hasConflict=true;out[j].hasConflict=true;
            }
        }
        for(const auto& c:source) if(Applies(c.scope,scope)&&std::none_of(out.begin(),out.end(),[&](const auto& b){return b.bindingKey==c.bindingKey;}))
            out.push_back({c.bindingKey,{},{},c.binding,Rank(c.scope)==3?BindingOrigin::View:Rank(c.scope)==2?BindingOrigin::ViewClass:BindingOrigin::Session,c.choice==InputBindingOverride::Replace,false,false});
        return out;
    }
    const std::vector<EffectiveBinding>& CurrentBindings(const BindingScope& scope) const {
        for(const auto& entry:cache)if(SameScope(entry.scope,scope))return entry.values;
        cache.push_back({scope,Bindings(scope,changes)});return cache.back().values;
    }
    BindingUpdateResult Check(const std::vector<BindingChange>& candidate) const {
        BindingUpdateResult result{InputBindingStatus::Applied,{}};
        for(const auto& c:candidate) {
            if(c.bindingKey.empty()||c.choice>InputBindingOverride::Disable||c.scope.area>InputArea::Application
                ||c.scope.viewClass>InteractionViewClass::ThreeD
                ||(c.choice==InputBindingOverride::Replace&&!InputBindingMatch::Valid(c.binding))) {
                result.status=InputBindingStatus::Invalid;result.diagnostics.push_back({c.bindingKey,{},{},result.status});continue;
            }
            for(const auto& e:actions) for(const auto& b:e->info.bindings) if(b.bindingKey==c.bindingKey
                &&c.choice==InputBindingOverride::Replace&&!b.supportedTriggers.empty()
                &&std::find(b.supportedTriggers.begin(),b.supportedTriggers.end(),c.binding.trigger)==b.supportedTriggers.end()) {
                result.status=InputBindingStatus::Unsupported;result.diagnostics.push_back({c.bindingKey,{},{},result.status});
            }
        }
        if(result.status!=InputBindingStatus::Applied) return result;
        auto testViews=views;
        if(testViews.empty()) testViews={{"",InteractionViewClass::Slice},{"",InteractionViewClass::ThreeD}};
        for(const auto& v:testViews) for(const auto area:{InputArea::View,InputArea::Text,InputArea::Modal,InputArea::Application}) {
            auto scope=Scope(v.id,area);scope.viewClass=v.viewClass;const auto values=Bindings(scope,candidate);
            for(const auto& b:values) if(b.hasConflict) {
                result.status=InputBindingStatus::Conflict;
                result.diagnostics.push_back({b.bindingKey,{},v.id,result.status});
            }
        }
        return result;
    }
};
InteractionController::InteractionController():m_impl(std::make_unique<Impl>()) {
    const std::pair<const char*,InputBinding> commands[]{
        {"interaction.confirm",{InputTriggerKind::KeyPress,InputMouseButton::None,0,7,13}},
        {"interaction.cancel",{InputTriggerKind::KeyPress,InputMouseButton::None,0,7,27}},
        {"interaction.undo",{InputTriggerKind::KeyPress,InputMouseButton::None,2,5,'Z'}},
        {"interaction.redo",{InputTriggerKind::KeyPress,InputMouseButton::None,2,5,'Y'}}};
    for(const auto& c:commands) {
        InteractionActionInfo info;info.actionId=c.first;info.ownerId="interaction";info.isTriggerable=true;
        info.bindings.push_back({std::string(c.first)+".key",c.second,true,{},"editing",{InputTriggerKind::KeyPress}});
        m_impl->actions.push_back(std::make_shared<Impl::Entry>(Impl::Entry{std::move(info),{},{},false}));
    }
}
InteractionController::~InteractionController()=default;
BindingUpdateResult InteractionController::ApplyBindings(const BindingPatch& patch) {
    if(!m_impl->Valid()) return {InputBindingStatus::Failed,{}};
    if(m_impl->isTriggering||m_impl->isApplying||m_impl->isImporting) return {InputBindingStatus::Busy,{}};
    struct Guard {bool& flag;Guard(bool& v):flag(v){flag=true;}~Guard(){flag=false;}} guard(m_impl->isApplying);
    auto candidate=m_impl->changes;
    for(const auto& c:patch.changes) {
        if(c.bindingKey.empty()||c.choice>InputBindingOverride::Disable
            ||c.scope.area>InputArea::Application||c.scope.viewClass>InteractionViewClass::ThreeD)
            return {InputBindingStatus::Invalid,{{c.bindingKey,{},{},InputBindingStatus::Invalid}}};
        candidate.erase(std::remove_if(candidate.begin(),candidate.end(),[&](const auto& p){return p.bindingKey==c.bindingKey&&SameScope(p.scope,c.scope);}),candidate.end());
        if(c.choice!=InputBindingOverride::UseDefault) candidate.push_back(c);
    }
    auto result=m_impl->Check(candidate);
    if(result.status==InputBindingStatus::Applied) {
        m_impl->changes=std::move(candidate);m_impl->cache.clear();
        if(m_impl->onBindingsChanged)m_impl->onBindingsChanged();
    }
    return result;
}
BindingUpdateResult InteractionController::ImportBindings(const IInputBindings& source,const BindingScope& scope) {
    if(!m_impl->Valid()||m_impl->isImporting||m_impl->isApplying||m_impl->isTriggering)return {InputBindingStatus::Busy,{}};
    BindingPatch patch;
    {
        struct Guard {bool& flag;Guard(bool& v):flag(v){flag=true;}~Guard(){flag=false;}} guard(m_impl->isImporting);
        for(const auto& a:GetActions())for(const auto& b:a.bindings) {
            InputBinding value;const auto choice=source.GetOverride(b.bindingKey,value);
            patch.changes.push_back({b.bindingKey,choice,value,scope});
        }
    }
    return ApplyBindings(patch);
}
std::vector<EffectiveBinding> InteractionController::GetBindings(const BindingScope& scope) const {
    if(!m_impl->Valid()) return {};
    if(!scope.viewId.empty()||scope.viewClass!=InteractionViewClass::Any)return m_impl->Bindings(scope.viewId.empty()?scope:m_impl->Scope(scope.viewId,scope.area),m_impl->changes);
    std::vector<EffectiveBinding> out;
    for(const auto viewClass:{InteractionViewClass::Any,InteractionViewClass::Slice,InteractionViewClass::ThreeD}) {
        auto query=scope;query.viewClass=viewClass;
        for(auto value:m_impl->Bindings(query,m_impl->changes))if(std::none_of(out.begin(),out.end(),[&](const auto& b){return b.bindingKey==value.bindingKey;}))out.push_back(std::move(value));
    }
    return out;
}
std::vector<InteractionActionInfo> InteractionController::GetActions() const {
    std::vector<InteractionActionInfo> out;if(!m_impl->Valid()) return out;
    for(const auto& e:m_impl->actions) out.push_back(e->info);return out;
}
std::shared_ptr<ViewNavigation> InteractionController::GetNavigation(const std::string& id) {
    if(id.empty()||std::none_of(m_impl->views.begin(),m_impl->views.end(),[&](const auto& v){return v.id==id;}))return {};
    return m_impl->Valid()&&m_impl->getNavigation?m_impl->getNavigation(id):nullptr;
}
NavigationStatus InteractionController::SetNavigation(const std::string& id,std::shared_ptr<ViewNavigation> n) {
    if(id.empty()||std::none_of(m_impl->views.begin(),m_impl->views.end(),[&](const auto& v){return v.id==id;}))return NavigationStatus::Invalid;
    return m_impl->Valid()&&m_impl->setNavigation?m_impl->setNavigation(id,std::move(n)):NavigationStatus::Unbound;
}
ActionTriggerResult InteractionController::Trigger(std::string_view id,const ActionContext& context) {
    if(!m_impl->Valid()) return {};
    if(context.viewId.empty()&&(id.substr(0,11)=="navigation."||id.substr(0,12)=="interaction."))return {ActionTriggerStatus::Invalid,0};
    if(!context.viewId.empty()&&std::none_of(m_impl->views.begin(),m_impl->views.end(),[&](const auto& v){return v.id==context.viewId;}))return {ActionTriggerStatus::Invalid,0};
    if(m_impl->isTriggering||m_impl->isApplying||m_impl->isImporting) return {ActionTriggerStatus::Busy,0};
    if(id=="interaction.cancel"&&m_impl->cancelCapture) {
        const auto cancel=m_impl->cancelCapture;m_impl->isTriggering=true;
        ActionTriggerResult result;
        try { result=cancel(context); } catch(...) { result={ActionTriggerStatus::Failed,0}; }
        m_impl->isTriggering=false;
        if(result.status!=ActionTriggerStatus::Unavailable||!m_impl->Valid())return result;
    }
    const auto owner=InteractionControllerAccess::GetOwner(*this,context.viewId);
    const bool isGeneric=id.substr(0,12)=="interaction.";
    std::shared_ptr<Impl::Entry> selected;
    for(const auto& e:m_impl->actions) if(e->info.actionId==id&&e->execute&&e->info.isTriggerable
        &&(!isGeneric||e->info.ownerId==owner)
        &&(!e->info.isCurrentToolCommand||e->info.ownerId==owner)
        &&(e->views.empty()||std::find(e->views.begin(),e->views.end(),context.viewId)!=e->views.end())) {
        if(selected) return {ActionTriggerStatus::Invalid,0};selected=e;
    }
    if(!selected) return isGeneric&&m_impl->fallback?m_impl->fallback(id,context):ActionTriggerResult{};
    m_impl->isTriggering=true;
    ActionTriggerResult result;
    try { result=selected->execute(context); } catch(...) { result={ActionTriggerStatus::Failed,0}; }
    m_impl->isTriggering=false;
    return result;
}
bool InteractionController::RegisterAction(ApplicationAction a) {
    if(!m_impl->Valid()||m_impl->isTriggering||m_impl->isApplying||m_impl->isImporting||!a.execute||a.info.actionId.empty()||a.info.ownerId.empty()) return false;
    if(std::any_of(m_impl->actions.begin(),m_impl->actions.end(),[&](const auto& e){return e->info.actionId==a.info.actionId&&e->info.ownerId==a.info.ownerId;})) return false;
    if(!m_impl->ValidKeys(a.info))return false;
    a.info.isTriggerable=true;
    for(auto& b:a.info.bindings) {
        if(b.bindingKey.empty()||!InputBindingMatch::Valid(b.defaultBinding)||b.defaultBinding.trigger==InputTriggerKind::Drag||b.defaultBinding.trigger==InputTriggerKind::Modifiers)return false;
        b.supportedTriggers={InputTriggerKind::KeyPress,InputTriggerKind::PointerPress,InputTriggerKind::WheelForward,InputTriggerKind::WheelBackward};
    }
    m_impl->actions.push_back(std::make_shared<Impl::Entry>(Impl::Entry{std::move(a.info),{},std::move(a.execute),true}));m_impl->cache.clear();m_impl->NotifyBindingsChanged();return true;
}
bool InteractionController::RemoveAction(std::string_view id) {
    if(!m_impl->Valid()||m_impl->isTriggering||m_impl->isApplying||m_impl->isImporting) return false;
    m_impl->actions.erase(std::remove_if(m_impl->actions.begin(),m_impl->actions.end(),[&](const auto& e){return e->isApplication&&e->info.actionId==id;}),m_impl->actions.end());m_impl->cache.clear();m_impl->NotifyBindingsChanged();return true;
}
InteractionSettings InteractionController::GetSettings() const {
    InteractionSettings out;if(!m_impl->Valid()) return out;out.bindings=m_impl->changes;
    if(m_impl->getNavigation) for(const auto& v:m_impl->views) if(const auto n=m_impl->getNavigation(v.id)) out.navigation.push_back({v.id,n->GetOptions()});
    return out;
}
BindingUpdateResult InteractionController::LoadSettings(const InteractionSettings& settings) {
    if(!m_impl->Valid()||m_impl->isTriggering||m_impl->isApplying||m_impl->isImporting) return {InputBindingStatus::Failed,{}};
    struct Guard {bool& flag;Guard(bool& v):flag(v){flag=true;}~Guard(){flag=false;}} guard(m_impl->isApplying);
    auto result=m_impl->Check(settings.bindings);if(result.status!=InputBindingStatus::Applied) return result;
    std::vector<std::pair<std::shared_ptr<ViewNavigation>,NavigationOptions>> previous;
    for(const auto& entry:settings.navigation) {
        ViewNavigation check;if(check.SetOptions(entry.options)!=NavigationStatus::Applied) return {InputBindingStatus::Invalid,{}};
        const auto n=GetNavigation(entry.viewId);if(!n) return {InputBindingStatus::Invalid,{}};
        previous.emplace_back(n,n->GetOptions());
    }
    for(std::size_t i=0;i<previous.size();++i) if(previous[i].first->SetOptions(settings.navigation[i].options)!=NavigationStatus::Applied) {
        for(std::size_t j=0;j<i;++j) (void)previous[j].first->SetOptions(previous[j].second);
        return {InputBindingStatus::Busy,{}};
    }
    m_impl->changes=settings.bindings;m_impl->cache.clear();if(m_impl->onBindingsChanged)m_impl->onBindingsChanged();return result;
}
void InteractionControllerAccess::Start(InteractionController& c,std::vector<InteractionView> views,
    std::function<std::shared_ptr<ViewNavigation>(const std::string&)> get,
    std::function<NavigationStatus(const std::string&,std::shared_ptr<ViewNavigation>)> set) {
    c.m_impl->cache.clear();c.m_impl->isValid=true;c.m_impl->views=std::move(views);c.m_impl->getNavigation=std::move(get);c.m_impl->setNavigation=std::move(set);
}
bool InteractionControllerAccess::PrepareKeyEvent(InteractionController& c,InteractionEvent& event) {
    if(!c.m_impl->Valid())return false;
    const auto key=InputBindingMatch::GetKey(event);if(!key)return false;
    if(event.eventKind==InteractionEventKind::KeyPress) {
        if(!c.m_impl->pressedKeys.emplace(key,InteractionController::Impl::KeyOwner{event.viewId,false}).second)event.isAutoRepeat=true;
    } else if(event.eventKind==InteractionEventKind::KeyRelease) {
        const auto found=c.m_impl->pressedKeys.find(key);if(found==c.m_impl->pressedKeys.end())return false;
        const bool handled=found->second.isHandled;c.m_impl->pressedKeys.erase(found);return handled;
    }
    if(event.eventKind==InteractionEventKind::TextInput) {
        const auto found=c.m_impl->pressedKeys.find(key);return found!=c.m_impl->pressedKeys.end()&&found->second.isHandled;
    }
    return false;
}
void InteractionControllerAccess::SetKeyHandled(InteractionController& c,const InteractionEvent& event,bool handled) {
    if(event.eventKind!=InteractionEventKind::KeyPress)return;
    const auto found=c.m_impl->pressedKeys.find(InputBindingMatch::GetKey(event));
    if(found!=c.m_impl->pressedKeys.end())found->second.isHandled=found->second.isHandled||handled;
}
void InteractionControllerAccess::ClearKeys(InteractionController& c,const std::string& viewId) {
    for(auto it=c.m_impl->pressedKeys.begin();it!=c.m_impl->pressedKeys.end();)if(it->second.viewId==viewId)it=c.m_impl->pressedKeys.erase(it);else ++it;
}
void InteractionControllerAccess::SetFallbackAction(InteractionController& c,std::function<ActionTriggerResult(std::string_view,const ActionContext&)> callback){c.m_impl->fallback=std::move(callback);}
void InteractionControllerAccess::SetCaptureCancel(InteractionController& c,std::function<ActionTriggerResult(const ActionContext&)> callback){c.m_impl->cancelCapture=std::move(callback);}
void InteractionControllerAccess::SetBindingsChanged(InteractionController& c,std::function<void()> callback){c.m_impl->onBindingsChanged=std::move(callback);}
bool InteractionControllerAccess::GetIsValid(const InteractionController& c){return c.m_impl->Valid();}
void InteractionControllerAccess::Stop(InteractionController& c) {
    c.m_impl->cache.clear();c.m_impl->isValid=false;c.m_impl->fallback={};c.m_impl->cancelCapture={};c.m_impl->onBindingsChanged={};c.m_impl->getNavigation={};c.m_impl->setNavigation={};c.m_impl->actions.clear();c.m_impl->owners.clear();c.m_impl->pressedKeys.clear();
}
bool InteractionControllerAccess::Register(InteractionController& c,InteractionActionInfo info,std::vector<std::string> ids,
    std::function<ActionTriggerResult(const ActionContext&)> execute) {
    if(!c.m_impl->Valid()||info.actionId.empty()||info.ownerId.empty()) return false;
    for(const auto& e:c.m_impl->actions) if(e->info.actionId==info.actionId&&e->info.ownerId==info.ownerId) return false;
    if(!c.m_impl->ValidKeys(info))return false;
    c.m_impl->actions.push_back(std::make_shared<InteractionController::Impl::Entry>(InteractionController::Impl::Entry{std::move(info),std::move(ids),std::move(execute),false}));c.m_impl->cache.clear();return true;
}
void InteractionControllerAccess::RemoveOwner(InteractionController& c,std::string_view id) {
    c.m_impl->actions.erase(std::remove_if(c.m_impl->actions.begin(),c.m_impl->actions.end(),[&](const auto& e){return e->info.ownerId==id;}),c.m_impl->actions.end());
    c.m_impl->cache.clear();for(auto& p:c.m_impl->owners) if(p.second==id) p.second.clear();
}
void InteractionControllerAccess::NotifyBindingsChanged(InteractionController& c){if(c.m_impl->Valid()&&c.m_impl->onBindingsChanged)c.m_impl->onBindingsChanged();}
void InteractionControllerAccess::SetViewClass(InteractionController& c,const std::string& id,InteractionViewClass kind) {
    if(!c.m_impl->Valid())return;
    for(auto& view:c.m_impl->views)if(view.id==id&&view.viewClass!=kind){view.viewClass=kind;c.m_impl->cache.clear();NotifyBindingsChanged(c);return;}
}
bool InteractionControllerAccess::SetOwner(InteractionController& c,const std::string& view,std::string owner) {
    if(!c.m_impl->Valid()||std::none_of(c.m_impl->views.begin(),c.m_impl->views.end(),[&](const auto& v){return v.id==view;})) return false;
    c.m_impl->owners[view]=std::move(owner);return true;
}
std::string InteractionControllerAccess::GetOwner(const InteractionController& c,const std::string& view) {
    const auto found=c.m_impl->owners.find(view);return found==c.m_impl->owners.end()?std::string{}:found->second;
}
bool InteractionControllerAccess::Matches(const InteractionController& c,std::string_view key,const InteractionEvent& event) {
    if(!c.m_impl->Valid()) return false;
    auto scope=c.m_impl->Scope(event.viewId,event.area);
    if(scope.viewClass==InteractionViewClass::Any) scope.viewClass=IsSlice(event.vizMode)?InteractionViewClass::Slice:InteractionViewClass::ThreeD;
    const auto& values=c.m_impl->CurrentBindings(scope);
    const auto owner=GetOwner(c,event.viewId);
    for(const auto& e:c.m_impl->actions) for(const auto& b:e->info.bindings) if(b.bindingKey==key) {
        if(e->info.isCurrentToolCommand&&e->info.ownerId!=owner) return false;
        if(!b.scene.empty()&&b.scene!="editing"&&b.scene!="navigation"&&b.scene!="modelTransform"&&b.scene!="plane"&&b.scene.substr(0,7)!="target:"&&b.scene.substr(0,7)!="widget:"&&b.scene!=owner) return false;
        if(b.scene=="modelTransform"&&event.toolMode!=ToolMode::ModelTransform) return false;
        if(b.scene=="navigation"&&event.toolMode==ToolMode::ModelTransform) return false;
        for(const auto& v:values) if(v.bindingKey==key) return v.isAvailable&&v.isEnabled&&!v.hasConflict&&InputBindingMatch::Matches(v.binding,event);
    }
    return false;
}
std::vector<std::string> InteractionControllerAccess::GetMatchedActions(const InteractionController& c,const InteractionEvent& e) {
    std::vector<std::string> out;
    for(const auto& a:c.m_impl->actions) for(const auto& b:a->info.bindings) if(Matches(c,b.bindingKey,e)
        &&std::find(out.begin(),out.end(),a->info.actionId)==out.end()) out.push_back(a->info.actionId);
    return out;
}
bool InteractionControllerAccess::GetCancelMatched(const InteractionController& c,const InteractionEvent& e) {
    const auto actions=GetMatchedActions(c,e);return std::find(actions.begin(),actions.end(),"interaction.cancel")!=actions.end();
}
