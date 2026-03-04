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

InteractionResult TimeUpdateHandler::Handle(const InteractionEvent& eve)
{
    if (eve.vtkEventId != vtkCommand::TimerEvent) {
        return {};
    }

    if (!m_service) {
        // Timer 已处理，无业务操作
        return { true, false };
    }

    // 1. 驱动数据同步（后台线程写入的脏标记在此主线程消费）
    m_service->ProcessPendingUpdates();

    // 2. 检查渲染脏标记，仅在窗口有效时渲染
    if (m_service->IsDirty()) {
        if (m_renderWindow
            && m_renderWindow->GetMapped()
            && m_renderWindow->GetGenericWindowId())
        {
            m_renderWindow->Render();
        }
        m_service->SetDirty(false);
    }

    return { true, false };
}