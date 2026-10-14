#pragma once
#include "Interaction/InteractionController.h"
#include "Interaction/InteractionTypes.h"

struct InteractionView final {
    std::string id;
    InteractionViewClass viewClass=InteractionViewClass::Any;
};
class InteractionControllerAccess final {
public:
    static void Start(InteractionController&,std::vector<InteractionView>,
        std::function<std::shared_ptr<ViewNavigation>(const std::string&)>,
        std::function<NavigationStatus(const std::string&,std::shared_ptr<ViewNavigation>)>);
    static void Stop(InteractionController&);
    static bool GetIsValid(const InteractionController&);
    static bool PrepareKeyEvent(InteractionController&,InteractionEvent&);
    static void SetKeyHandled(InteractionController&,const InteractionEvent&,bool);
    static void ClearKeys(InteractionController&,const std::string&);
    static void SetFallbackAction(InteractionController&,std::function<ActionTriggerResult(std::string_view,const ActionContext&)>);
    static void SetBindingsChanged(InteractionController&,std::function<void()>);
    static bool Register(InteractionController&,InteractionActionInfo,
        std::vector<std::string> viewIds,std::function<ActionTriggerResult(const ActionContext&)>);
    static void RemoveOwner(InteractionController&,std::string_view ownerId);
    static void SetViewClass(InteractionController&,const std::string&,InteractionViewClass);
    static void NotifyBindingsChanged(InteractionController&);
    static bool SetOwner(InteractionController&,const std::string& viewId,std::string ownerId);
    static std::string GetOwner(const InteractionController&,const std::string& viewId);
    static bool Matches(const InteractionController&,std::string_view key,const InteractionEvent&);
    static std::vector<std::string> GetMatchedActions(const InteractionController&,const InteractionEvent&);
    static bool GetCancelMatched(const InteractionController&,const InteractionEvent&);
};
