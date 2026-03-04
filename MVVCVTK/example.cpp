//// 1. 创建 Router 并装配 Handler
//InteractionRouter m_router;
//m_router.Add(std::make_unique<TimeUpdateHandler>(service.get(), renderWindow));
//m_router.Add(std::make_unique<Viewer2DHandler>(service.get(), picker, renderer));
//m_router.Add(std::make_unique<Viewer3DHandler>(service.get(), picker, renderer));
//
//// 2. 在 VTK 事件回调中填充 InteractionEvent 并分发
//InteractionEvent eve;
//eve.vtkEventId = eventId;
//eve.iren = iren;
//eve.x = iren->GetEventPosition()[0];
//eve.y = iren->GetEventPosition()[1];
//eve.shift = iren->GetShiftKey() != 0;
//eve.ctrl = iren->GetControlKey() != 0;
//eve.alt = iren->GetAltKey() != 0;
//eve.keyCode = iren->GetKeyCode();
//eve.keySym = iren->GetKeySym() ? iren->GetKeySym() : "";
//eve.vizMode = m_currentMode;   // Qt 侧维护的当前模式
//eve.toolMode = m_toolMode;      // Qt 侧维护的当前工具
//
//// Timer 广播，其余 FirstMatch
//auto dispatchMode = (eventId == vtkCommand::TimerEvent)
//? RouterDispatchMode::Broadcast
//    : RouterDispatchMode::FirstMatch;
//
//auto result = m_router.Dispatch(eve, dispatchMode);
//
//// 3. 根据结果决定是否中止 VTK 默认事件
//if (result.abortVtk && m_eventCallback) {
//    m_eventCallback->SetAbortFlag(1);
//}	