#include "Interaction/Viewer2DHandler.h"
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <cstdint>
Viewer2DHandler::Viewer2DHandler(InteractionStatePort* state,SliceInputPort* slice,ModelInputPort* model,
    RenderUpdatePort* update,vtkPropPicker* picker,vtkRenderer* renderer,const NavigationBindings* bindings)
    :m_statePort(state),m_slicePort(slice),m_modelPort(model),m_updatePort(update),m_picker(picker),m_renderer(renderer),
    m_bindings(bindings?bindings:&m_defaultBindings) {
    m_source={"Viewer2D",std::to_string(reinterpret_cast<std::uintptr_t>(this))};
}
Viewer2DHandler::~Viewer2DHandler(){if(m_statePort)(void)m_statePort->SetInteracting(m_source,false);}
InteractionResult Viewer2DHandler::Send(const InteractionEvent& e) {
    const auto result=[](bool ok){return InteractionResult{true,true,ok,ok?InteractionFailureReason::None:InteractionFailureReason::StateRejected};};
    if(m_isDragCrosshair&&(e.eventKind==InteractionEventKind::Cancel||e.eventKind==m_dragReleaseKind)) {
        if(!m_statePort->SetInteracting(m_source,false))return result(false);
        m_isDragCrosshair=false;m_dragReleaseKind=InteractionEventKind::None;return result(true);
    }
    const bool slice=e.vizMode==VizMode::SliceTop_down||e.vizMode==VizMode::SliceFront_back||e.vizMode==VizMode::SliceLeft_right;
    if(!slice||!m_statePort||!m_slicePort||!m_modelPort||!m_updatePort)return {};
    if(m_bindings->GetMatched(NavigationAction::CrosshairDrag,e)) {
        m_isDragCrosshair=m_statePort->SetInteracting(m_source,true);
        m_dragReleaseKind=e.eventKind==InteractionEventKind::MiddlePress?InteractionEventKind::MiddleRelease
            :e.eventKind==InteractionEventKind::SecondaryPress?InteractionEventKind::SecondaryRelease:InteractionEventKind::PrimaryRelease;
        return result(m_isDragCrosshair);
    }
    if(m_isDragCrosshair&&e.eventKind==InteractionEventKind::PointerMove) {
        if(!m_picker||!m_renderer||!m_picker->Pick(e.x,e.y,0,m_renderer))return result(false);
        const auto* p=m_picker->GetPickPosition();if(!p||!m_modelPort->GetPointVisible({p[0],p[1],p[2]}))return result(false);
        const int axis=e.vizMode==VizMode::SliceTop_down?2:e.vizMode==VizMode::SliceFront_back?1:0;
        return result(m_slicePort->SetCursorWorld({p[0],p[1],p[2]},axis)&&m_updatePort->SetRenderNeeded());
    }
    return {};
}
