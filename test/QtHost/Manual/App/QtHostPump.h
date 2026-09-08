// 测试用途：为 Qt 功能测试合并工作通知，在所属线程推进更新并按需绘制可见视图。
#pragma once
#include "Host/VtkAppHostSession.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>

namespace Manual {
class QtHostPump final : public QObject {
public:
    explicit QtHostPump(QObject* parent = nullptr);
    ~QtHostPump() override;
    void SetConfig(HostSessionConfig& config);
    void SetSession(const std::shared_ptr<VtkAppHostSession>& session);
    void SendUpdates();
    void SendRender();
    void Stop();
    void SetVisible();
    std::function<bool(const std::string&)> getVisible;
    std::function<void()> onUpdated;
    std::function<void()> onRendered;
    std::function<void(const QString&)> onError;
    std::function<void(const QString&, qint64)> onTiming;
    std::uint64_t GetUpdateCount() const { return m_updateCount; }
    std::uint64_t GetRenderCount() const { return m_renderCount; }
    QJsonObject GetDiagnostics() const;
    bool GetIsRenderPending(const std::string& id) const { return m_pending.count(id) != 0 || m_signal->isQueued.load(); }
    bool GetIsViewPending(const std::string& id) const { return m_pending.count(id) != 0; }
private:
    struct Signal {
        std::atomic<bool> isQueued{false};
        std::atomic<bool> isStopped{false};
        QObject* receiver = nullptr; // Stop 成功前保留 receiver。
        QtHostPump* owner = nullptr;
    };
    std::shared_ptr<Signal> m_signal;
    std::weak_ptr<VtkAppHostSession> m_session;
    std::function<void()> m_queueUpdates;
    QTimer m_renderTimer;
    bool m_renderQueued = false;
    std::set<std::string> m_pending;
    std::chrono::steady_clock::time_point m_nextFrame{};
    std::uint64_t m_updateCount = 0;
    std::uint64_t m_renderCount = 0;
    std::uint64_t m_updateFailures = 0, m_pendingUpdateFailures = 0;
    QJsonArray m_failureSamples;
};
}
