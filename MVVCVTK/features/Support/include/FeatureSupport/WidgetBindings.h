#pragma once
#include "Interaction/InteractionController.h"
#include <vtkAbstractWidget.h>
#include <vtkCommand.h>
#include <vtkEvent.h>
#include <vtkWidgetEvent.h>
#include <vtkWidgetEventTranslator.h>
#include <vtkRenderer.h>
#include <vector>

namespace FeatureSupport {
struct WidgetBinding final { std::string key; unsigned long action=vtkWidgetEvent::Select; };
// 只投影已声明且受支持的拖动绑定；原生 widget 是唯一业务执行入口。
inline void SetWidgetBindings(vtkAbstractWidget* widget,const std::vector<EffectiveBinding>& values,
    const std::vector<WidgetBinding>& mappings) {
    if(!widget)return;
    const bool enabled=widget->GetEnabled()!=0;
    auto* renderer=widget->GetCurrentRenderer();
    if(enabled)widget->SetEnabled(0);
    auto* translator=widget->GetEventTranslator();
    for(const auto event:{vtkCommand::LeftButtonPressEvent,vtkCommand::MiddleButtonPressEvent,vtkCommand::RightButtonPressEvent,
        vtkCommand::LeftButtonReleaseEvent,vtkCommand::MiddleButtonReleaseEvent,vtkCommand::RightButtonReleaseEvent,
        vtkCommand::KeyPressEvent,vtkCommand::KeyReleaseEvent,vtkCommand::CharEvent})translator->RemoveTranslation(event);
    for(const auto& mapping:mappings)for(const auto& value:values)if(value.bindingKey==mapping.key
        &&value.isEnabled&&value.isAvailable&&!value.hasConflict) {
        const auto& b=value.binding;
        const auto press=b.button==InputMouseButton::Primary?vtkCommand::LeftButtonPressEvent
            :b.button==InputMouseButton::Middle?vtkCommand::MiddleButtonPressEvent:vtkCommand::RightButtonPressEvent;
        const auto release=b.button==InputMouseButton::Primary?vtkCommand::LeftButtonReleaseEvent
            :b.button==InputMouseButton::Middle?vtkCommand::MiddleButtonReleaseEvent:vtkCommand::RightButtonReleaseEvent;
        for(int mask=0;mask<8;++mask)if((mask&b.requiredModifiers)==b.requiredModifiers&&!(mask&b.forbiddenModifiers))
            translator->SetTranslation(press,mask,0,0,nullptr,mapping.action);
        // 释放跟随已捕获按钮，不能因中途松开修饰键而失效。
        translator->SetTranslation(release,vtkWidgetEvent::EndSelect);
    }
    if(enabled){widget->SetCurrentRenderer(renderer);widget->SetEnabled(1);}
}
}
