#include "Host/Internal/HostViewTransaction.h"
#include "Interaction/AbstractViewContext.h"
#include "Interaction/InteractionPorts.h"
#include <utility>

HostViewTransaction::HostViewTransaction(std::shared_ptr<AppViewPort> view,
    std::shared_ptr<RenderUpdatePort> update,
    std::shared_ptr<AbstractViewContext> context, std::function<bool()> stopView)
    : m_view(std::move(view)), m_update(std::move(update)),
      m_context(std::move(context)), m_stopView(std::move(stopView))
{
}

bool HostViewTransaction::SetView(const AppViewUpdate& update,
    const std::optional<bool> isAxesVisible) const
{
    if (!m_view || !m_update || ((update.mode || isAxesVisible) && !m_context)) return false;
    // App -> camera -> axes -> dirty；补偿逆序，失败后停用 View。
    const AppViewState oldState = m_view->GetViewState();
    const bool oldAxes = m_context
        && m_context->GetOrientationAxesVisible();
    if (!m_view->SendViewUpdate(update)) return false;
    const AppViewState nextState = m_view->GetViewState();
    if (nextState.revision <= oldState.revision) {
        if (m_stopView) (void)m_stopView();
        return false;
    }

    const auto stopOnRestoreFail = [&]() {
        if (m_stopView) (void)m_stopView();
    };
    const auto restore = [&](const bool hasAxesChanged) {
        bool isRestored = true;
        if (hasAxesChanged && m_context) {
            isRestored = m_context
                ->SetOrientationAxesVisible(oldAxes) && isRestored;
        }
        if (update.mode && m_context) {
            isRestored = m_context
                ->SetCameraStyle(oldState.mode) && isRestored;
        }
        isRestored = m_view->SetViewState(
            oldState, nextState.revision) && isRestored;
        if (!isRestored) stopOnRestoreFail();
        return isRestored;
    };

    if (update.mode
        && !m_context->SetCameraStyle(*update.mode)) {
        (void)restore(false);
        return false;
    }
    if (isAxesVisible) {
        if (!m_context->SetOrientationAxesVisible(
                *isAxesVisible)) {
            (void)restore(true);
            return false;
        }
        // 方向轴属于 context，不发布 SharedState flags；显式标脏让 Qt Timer 产生下一帧。
        if (!m_update->SetRenderNeeded()) {
            (void)restore(true);
            return false;
        }
    }
    return true;
}

bool HostViewTransaction::ResetView() const
{
    if (!m_view || !m_update || !m_context) return false;
    const AppViewState oldState = m_view->GetViewState();
    const auto oldCamera = m_context->GetCameraState();
    if (!oldCamera) return false;

    AppViewUpdate viewUpdate;
    viewUpdate.windowLevelMode = WindowLevelMode::Auto;
    if (!m_view->SendViewUpdate(viewUpdate)) return false;
    const AppViewState nextState = m_view->GetViewState();
    if (nextState.revision <= oldState.revision) {
        if (m_stopView) (void)m_stopView();
        return false;
    }

    const auto restore = [&](const bool hasCameraChanged) {
        bool isRestored = true;
        if (hasCameraChanged) {
            isRestored = m_context->SetCameraState(*oldCamera)
                && isRestored;
        }
        isRestored = m_view->SetViewState(
            oldState, nextState.revision) && isRestored;
        if (!isRestored && m_stopView) {
            (void)m_stopView();
        }
        return isRestored;
    };

    if (!m_context->ResetCamera()) {
        (void)restore(true);
        return false;
    }
    // 相机不发布 App 状态事件；显式补帧与窗宽窗位提交组成同一 Host 事务。
    if (m_update->SetRenderNeeded()) return true;
    (void)restore(true);
    return false;
}
