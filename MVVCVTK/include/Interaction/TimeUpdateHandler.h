#pragma once
#include "IInteractionHandler.h"

class InteractiveService;
class vtkRenderWindow;

// ─────────────────────────────────────────────────────────────────────
// TimeUpdateHandler — 处理 VTK TimerEvent（心跳）
//
// 职责：
//   1. 驱动 Service::SendUpdates()（数据同步、策略重建）
//   2. 检查渲染脏标记，触发 RenderWindow::Render()
//   3. 重置脏标记
//
// Router 分发策略：建议用 Broadcast，确保与其他 Timer Handler 共存时均能执行
// ─────────────────────────────────────────────────────────────────────
class TimeUpdateHandler : public IInteractionHandler
{
public:
    TimeUpdateHandler(InteractiveService* service,
        vtkRenderWindow* renderWindow);

    InteractionResult Send(const InteractionEvent& eve) override;

private:
    InteractiveService* m_service = nullptr;
    vtkRenderWindow* m_renderWindow = nullptr;
};