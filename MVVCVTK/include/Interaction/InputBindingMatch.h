#pragma once
#include "Interaction/InteractionTypes.h"
#include "Interaction/InputBindings.h"
#include <cctype>

namespace InputBindingMatch {
inline std::uint32_t NormalizeKey(std::uint32_t key) { return key>='a'&&key<='z'?key-'a'+'A':key; }
inline std::uint32_t GetKey(const InteractionEvent& e) {
    if(e.key) return NormalizeKey(e.key);
    if(e.keySym=="Escape") return static_cast<std::uint32_t>(InputKey::Escape);
    if(e.keySym=="Return"||e.keySym=="Enter"||e.keySym=="KP_Enter") return static_cast<std::uint32_t>(InputKey::Enter);
    if(e.keySym=="Tab")return 9;
    if(e.keySym=="BackSpace")return 8;
    const char* names[]={"Delete","Left","Right","Up","Down","Home","End","Prior","Next","F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"};
    for(std::uint32_t i=0;i<sizeof(names)/sizeof(names[0]);++i)if(e.keySym==names[i])return static_cast<std::uint32_t>(InputKey::Delete)+i;
    if(e.keySym.size()==1) return static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(e.keySym[0])));
    return static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(e.keyCode)));
}
inline bool Valid(const InputBinding& b) {
    if((b.requiredModifiers|b.forbiddenModifiers)&~7 ||(b.requiredModifiers&b.forbiddenModifiers)) return false;
    switch(b.trigger) {
    case InputTriggerKind::Modifiers:return b.button==InputMouseButton::None&&b.key==0;
    case InputTriggerKind::KeyPress:return b.button==InputMouseButton::None&&b.key!=0;
    case InputTriggerKind::Drag:case InputTriggerKind::PointerPress:
        return b.button!=InputMouseButton::None&&b.button<=InputMouseButton::Middle&&b.key==0;
    case InputTriggerKind::WheelForward:case InputTriggerKind::WheelBackward:
        return b.button==InputMouseButton::None&&b.key==0;
    } return false;
}
inline bool Matches(const InputBinding& b,const InteractionEvent& e) {
    bool trigger=false;
    switch(b.trigger) {
    case InputTriggerKind::Modifiers:trigger=true;break;
    case InputTriggerKind::KeyPress:trigger=e.eventKind==InteractionEventKind::KeyPress&&GetKey(e)==NormalizeKey(b.key)&&(!e.isAutoRepeat||b.allowsRepeat);break;
    case InputTriggerKind::Drag:case InputTriggerKind::PointerPress:
        trigger=(b.button==InputMouseButton::Primary&&e.eventKind==InteractionEventKind::PrimaryPress)
            ||(b.button==InputMouseButton::Secondary&&e.eventKind==InteractionEventKind::SecondaryPress)
            ||(b.button==InputMouseButton::Middle&&e.eventKind==InteractionEventKind::MiddlePress);break;
    case InputTriggerKind::WheelForward:trigger=e.eventKind==InteractionEventKind::WheelForward;break;
    case InputTriggerKind::WheelBackward:trigger=e.eventKind==InteractionEventKind::WheelBackward;break;
    }
    const auto modifiers=(e.isShiftDown?1:0)|(e.isCtrlDown?2:0)|(e.isAltDown?4:0);
    return trigger&&(modifiers&b.requiredModifiers)==b.requiredModifiers&&!(modifiers&b.forbiddenModifiers);
}
inline bool Overlaps(const InputBinding& a,const InputBinding& b) {
    if(a.trigger==InputTriggerKind::Modifiers||b.trigger==InputTriggerKind::Modifiers)return false;
    const bool pointerA=a.trigger==InputTriggerKind::Drag||a.trigger==InputTriggerKind::PointerPress;
    const bool pointerB=b.trigger==InputTriggerKind::Drag||b.trigger==InputTriggerKind::PointerPress;
    return (a.trigger==b.trigger||(pointerA&&pointerB))&&a.button==b.button&&NormalizeKey(a.key)==NormalizeKey(b.key)
        &&!(a.requiredModifiers&b.forbiddenModifiers)&&!(b.requiredModifiers&a.forbiddenModifiers);
}
}
