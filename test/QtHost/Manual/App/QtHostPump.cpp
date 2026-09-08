// 测试用途：为 Qt 功能测试合并工作通知，在所属线程推进更新并按需绘制可见视图。
#include "QtHostPump.h"
#include "Support/JsonInput.h"
#include <QCoreApplication>
#include <QMetaObject>
#include <QElapsedTimer>
#include <cstdio>

namespace Manual {
QtHostPump::QtHostPump(QObject* parent) : QObject(parent), m_signal(std::make_shared<Signal>())
{
    m_signal->receiver = QCoreApplication::instance();
    m_signal->owner = this;
    m_renderTimer.setSingleShot(true);
    m_renderTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_renderTimer, &QTimer::timeout, this, [this] { SendRender(); });
}
QtHostPump::~QtHostPump() { Stop(); }
void QtHostPump::SetConfig(HostSessionConfig& config)
{
    config.driveMode = HostDriveMode::HostDriven;
    // QApplication 比 Session 活得更久；最终停止投递不依赖算法页面。
    auto* receiver = QCoreApplication::instance();
    config.sendOwnerTask = [receiver](std::function<void()> task) {
        return receiver && task && QMetaObject::invokeMethod(receiver, std::move(task), Qt::QueuedConnection);
    };
    const std::weak_ptr<Signal> weak = m_signal;
    m_queueUpdates = [weak] {
        const auto signal = weak.lock();
        if (!signal || signal->isStopped.load() || signal->isQueued.exchange(true)) return;
        const bool posted = QMetaObject::invokeMethod(signal->receiver, [weak] {
            const auto signal = weak.lock();
            if (!signal || signal->isStopped.load()) return;
            // 在消费前清门铃；本轮更新期间的新通知可再次排队。
            signal->isQueued.store(false);
            signal->owner->SendUpdates();
        }, Qt::QueuedConnection);
        if (!posted) {
            signal->isQueued.store(false);
            std::fputs("Qt Host work event delivery failed\n", stderr);
        }
    };
    config.onWorkAvailable = m_queueUpdates;
}
void QtHostPump::SetSession(const std::shared_ptr<VtkAppHostSession>& session) { m_session = session; }
void QtHostPump::SendUpdates()
{
    const auto session = m_session.lock();
    if (!session || m_signal->isStopped.load()) return;
    QElapsedTimer clock; clock.start();
    const auto update = session->SendUpdates();
    if (onTiming) onTiming("Host.Update", clock.nsecsElapsed());
    ++m_updateCount;
    if (update.status == HostUpdateStatus::Failed) {
        ++m_updateFailures; ++m_pendingUpdateFailures;
        QJsonArray views; QString first; bool different = false;
        for (const auto& view : session->GetRenderViewStates()) {
            const auto binding = GetRefText(view.dataRevision) + ":" + QString::number(view.bindingRevision);
            if (first.isEmpty()) first = binding; else different = different || first != binding;
            views.append(QJsonObject{{"id", QString::fromStdString(view.id)}, {"dataRevision", GetRefText(view.dataRevision)}, {"bindingRevision", QString::number(view.bindingRevision)}});
        }
        const QJsonObject sample{{"sceneEpoch", QString::number(update.sceneEpoch)}, {"differentViewInputs", different}, {"views", views}};
        if (m_failureSamples.isEmpty() || m_failureSamples.last() != sample) {
            if (m_failureSamples.size() == 8) m_failureSamples.removeFirst();
            m_failureSamples.append(sample);
        }
        if (m_pendingUpdateFailures == 1 && onError) onError(different ? "Host 更新失败：五视图输入版本尚未一致，等待后续工作事件。" : "Host 更新失败；诊断已记录，等待后续工作或窗口事件。");
    } else if (update.status == HostUpdateStatus::Completed && m_pendingUpdateFailures) {
        if (onError) onError(QString("Host 更新已恢复；此前连续 %1 次未提交，诊断保留在测试记录中。").arg(m_pendingUpdateFailures));
        m_pendingUpdateFailures = 0;
    }
    for (const auto& id : update.renderViewIds) m_pending.insert(id);
    if (onUpdated) onUpdated();
    SetVisible();
}
void QtHostPump::SetVisible()
{
    if (m_signal->isStopped.load() || m_renderTimer.isActive() || m_renderQueued) return;
    for (const auto& id : m_pending) if (getVisible && getVisible(id)) {
        m_renderQueued = true;
        QMetaObject::invokeMethod(this, [this] { m_renderQueued = false; SendRender(); }, Qt::QueuedConnection);
        break;
    }
}
void QtHostPump::SendRender()
{
    const auto session = m_session.lock();
    if (!session || m_signal->isStopped.load()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextFrame) {
        m_renderTimer.start(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(m_nextFrame - now).count()) + 1);
        return;
    }
    HostRenderRequest request;
    request.desiredUpdateRate = 30.0;
    for (const auto& id : m_pending) if (getVisible && getVisible(id)) request.viewIds.push_back(id);
    if (request.viewIds.empty()) return;
    QElapsedTimer clock; clock.start();
    const auto result = session->SendRender(request);
    if (onTiming) onTiming("Host.Render", clock.nsecsElapsed());
    ++m_renderCount;
    m_nextFrame = now + std::chrono::milliseconds(33);
    bool wasRendered = false;
    for (const auto& view : result.views) {
        if (onTiming && view.status == HostRenderStatus::Rendered)
            onTiming("Render." + QString::fromStdString(view.viewId), static_cast<qint64>(view.durationUs) * 1000);
        wasRendered = wasRendered || view.status == HostRenderStatus::Rendered;
        if (view.status == HostRenderStatus::Rendered || view.status == HostRenderStatus::Unchanged)
            m_pending.erase(view.viewId);
        else if (view.status == HostRenderStatus::Failed && onError) onError("视图绘制失败：" + QString::fromStdString(view.viewId) + "，保留绘制需求等待后续事件。");
    }
    // GPU effect 在绘制结束后才可提交；由本次真实绘制事件安排一次 owner 更新，空闲不续约。
    if (wasRendered && m_queueUpdates) m_queueUpdates();
    if (onRendered) onRendered();
    // Deferred 等待后续工作/窗口恢复通知，避免持续重试造成空闲自激。
}
void QtHostPump::Stop()
{
    m_signal->isStopped.store(true);
    m_renderTimer.stop();
    m_session.reset();
}
QJsonObject QtHostPump::GetDiagnostics() const
{
    return {{"updateFailures", QString::number(m_updateFailures)}, {"pendingUpdateFailures", QString::number(m_pendingUpdateFailures)}, {"samples", m_failureSamples}};
}
}
