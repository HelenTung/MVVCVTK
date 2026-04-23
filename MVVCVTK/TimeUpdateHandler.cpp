#include "TimeUpdateHandler.h"
#include "AppInterfaces.h"
#include <vtkCommand.h>
#include <vtkRenderWindow.h>

TimeUpdateHandler::TimeUpdateHandler(AbstractInteractiveService* service,
    vtkRenderWindow* renderWindow)
    : m_service(service)
    , m_renderWindow(renderWindow)
{
}

InteractionResult TimeUpdateHandler::GetHandleResult(const InteractionEvent& eve)
{
    if (eve.vtkEventId != vtkCommand::TimerEvent) {
        return {};
    }

    if (!m_service) {
        // Timer 已处理，无业务操作
        return { true, false };
    }

    // 1. 驱动数据同步（后台线程写入的脏标记在此主线程消费）
    m_service->SetPendingUpdatesProcessed();

    // needRender: 本帧是否存在待消费的渲染请求，先原子消费，避免渲染期间的新脏标记被误清掉
    const bool needRender = m_service->SetDirtyConsumed();

    // 2. 检查渲染脏标记，仅在窗口有效时渲染
    if (needRender) {
        if (m_renderWindow
            && m_renderWindow->GetMapped()
            && m_renderWindow->GetGenericWindowId())
        {
            m_renderWindow->Render();
        }
        else {
            m_service->SetDirtyMarked();
        }
    }

    return { true, false };
}