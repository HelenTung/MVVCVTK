#include "TimeUpdateHandler.h"
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderWindow.h>

#include <chrono>

TimeUpdateHandler::TimeUpdateHandler(RenderUpdatePort* updatePort,
    vtkRenderWindow* renderWindow)
    : m_updatePort(updatePort)
    , m_renderWindow(renderWindow)
{
}

InteractionResult TimeUpdateHandler::Send(const InteractionEvent& eve)
{
    if (eve.eventKind != InteractionEventKind::Timer) {
        return {};
    }

    // TimerEvent 是整套前后处理分离链路的“主线程心跳”：
    // 所有后台线程只负责写状态/置脏，真正消费这些状态并决定是否渲染都在这里统一收口。

    if (!m_updatePort) {
        // Timer 已处理，无业务操作
        return { true, false };
    }

    // 1. 驱动数据同步（后台线程写入的脏标记在此主线程消费）
    if (!m_updatePort->SendUpdates()) {
        // 未在 owner thread 获得同步资格时保留 pending/dirty，等待下一次合法心跳。
        return { true, false };
    }

    // needRender: 本帧是否存在待消费的渲染请求，先原子消费，避免渲染期间的新脏标记被误清掉
    const bool hasRenderNeed = m_updatePort->ResetRenderNeeded();

    // 2. 检查渲染脏标记，仅在窗口有效时渲染
    if (hasRenderNeed) {
        auto* genericWindow =
            vtkGenericOpenGLRenderWindow::SafeDownCast(
                m_renderWindow);
        const bool isNativeWindowReady =
            m_renderWindow
            && m_renderWindow->GetMapped()
            && m_renderWindow->GetGenericWindowId();
        const bool isQtWindowReady =
            genericWindow
            && genericWindow->GetReadyForRendering();
        if (isNativeWindowReady || isQtWindowReady)
        {
            const auto renderStart = std::chrono::steady_clock::now();
            m_renderWindow->Render();
            const auto renderUs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - renderStart).count());
            m_updatePort->SetRenderComplete(
                renderUs == 0 ? std::uint64_t{ 1 } : renderUs);
        }
        else {
            (void)m_updatePort->SetRenderNeeded();
        }
    }

    return { true, false };
}
