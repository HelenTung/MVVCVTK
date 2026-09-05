#pragma once
#include "IInteractionHandler.h"
#include "Interaction/InteractionPorts.h"

class vtkRenderWindow;

// ─────────────────────────────────────────────────────────────────────
// TimeUpdateHandler — 处理语义化 Timer 事件（心跳）
//
// 职责：
//   1. 驱动 RenderUpdatePort::SendUpdates()（数据同步、策略重建）
//   2. 检查渲染脏标记，触发 RenderWindow::Render()
//   3. Render 成功或本轮无需 Render 后派发 completion
//
// Router 分发策略：建议用 Broadcast，确保与其他 Timer Handler 共存时均能执行
// ─────────────────────────────────────────────────────────────────────
class TimeUpdateHandler : public IInteractionHandler
{
public:
    TimeUpdateHandler(RenderUpdatePort* updatePort,
        vtkRenderWindow* renderWindow);

    InteractionResult Send(const InteractionEvent& eve) override;

private:
    // 均为非拥有观察指针；StdViewContext 持有对应对象，并在 port/window 换绑时重建 Router。
    RenderUpdatePort* m_updatePort = nullptr;
    vtkRenderWindow* m_renderWindow = nullptr;
};
