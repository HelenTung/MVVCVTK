/*
// 1. 创建 Router 并装配 Handler
InteractionRouter m_router;
m_router.Add(std::make_unique<TimeUpdateHandler>(service.get(), renderWindow));
m_router.Add(std::make_unique<Viewer2DHandler>(service.get(), picker, renderer));
m_router.Add(std::make_unique<Viewer3DHandler>(service.get(), picker, renderer));

// 2. 在 VTK 事件回调中填充 InteractionEvent 并分发
InteractionEvent eve;
eve.vtkEventId = eventId;
eve.iren = iren;
eve.x = iren->GetEventPosition()[0];
eve.y = iren->GetEventPosition()[1];
eve.shift = iren->GetShiftKey() != 0;
eve.ctrl = iren->GetControlKey() != 0;
eve.alt = iren->GetAltKey() != 0;
eve.keyCode = iren->GetKeyCode();
eve.keySym = iren->GetKeySym() ? iren->GetKeySym() : "";
eve.vizMode = m_currentMode;   // Qt 侧维护的当前模式
eve.toolMode = m_toolMode;      // Qt 侧维护的当前工具

// Timer 广播，其余 FirstMatch
auto dispatchMode = (eventId == vtkCommand::TimerEvent)
? RouterDispatchMode::Broadcast
    : RouterDispatchMode::FirstMatch;

auto result = m_router.Dispatch(eve, dispatchMode);

// 3. 根据结果决定是否中止 VTK 默认事件
if (result.abortVtk && m_eventCallback) {
    m_eventCallback->SetAbortFlag(1);
}
*/


/*
std::atomic<bool> m_isDirty        // "本帧画面需要重新渲染"
// 写：任何参数变化后设 true
// 读：TimeUpdateHandler 检查后调 Render()，然后设 false

std::atomic<bool> m_needsSync      // "SharedState 有参数变化，需要同步到 Strategy"
// 写：Observer 回调（后台/主线程均可）
// 读：ProcessPendingUpdates → PostData_SyncStateToStrategy


std::atomic<int>  m_pendingFlags   // "哪些参数发生了变化"（UpdateFlags 的位掩码）
// 写：Observer 回调中 fetch_or 累积
// 读：PostData_SyncStateToStrategy 中 exchange(0) 取出并清零
// 目的：精确告诉 Strategy 只更新有变化的部分，避免全量刷新


std::atomic<int>  m_pendingVizModeInt  // "用户意图切换到哪个可视化模式"
// 写：PreInit_SetVizMode（前处理阶段）
// 读：PostData_RebuildPipeline 取出后创建对应 Strategy
// 用 int 而非 enum 是因为 atomic 不直接支持 enum class


std::atomic<bool> m_needsDataRefresh   // "数据已就绪，需要重建整个渲染管线"
// 写：Observer 收到 DataReady 时（后台线程）
// 读：ProcessPendingUpdates，消费后重建 Strategy + SetInputData


std::atomic<bool> m_needsCacheClear    // "Strategy 缓存需要清空"
// 写：DataReady 时后台线程调 RequestClearStrategyCache()
// 读：ProcessPendingUpdates 第一步消费（必须主线程执行 Detach）
// 与 m_needsDataRefresh 分离的原因：Detach 必须先于 RebuildPipeline


std::atomic<bool> m_needsLoadFailed    // "数据加载失败，需要清理现场"
// 写：Observer 收到 LoadFailed 时（后台线程）
// 读：ProcessPendingUpdates，消费后清空 Strategy 缓存
*/