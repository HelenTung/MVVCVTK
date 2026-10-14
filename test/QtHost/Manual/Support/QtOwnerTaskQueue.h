// 测试用途：用 Windows 事件可靠唤醒 Qt owner 线程；不轮询、不在生产者线程执行任务。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <QObject>
#include <QWinEventNotifier>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace Manual {
class QtOwnerTaskQueue final : public QObject {
    struct State {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::mutex mutex;
        bool stopped = false;
        std::vector<std::function<void()>> tasks;
        State() { if(!event)throw std::runtime_error("Cannot create Qt owner work event."); }
        ~State() { CloseHandle(event); }
    };
    std::shared_ptr<State> m_state;
    QWinEventNotifier m_notifier;
public:
    explicit QtOwnerTaskQueue(QObject* parent=nullptr)
        : QObject(parent), m_state(std::make_shared<State>()), m_notifier(m_state->event,this)
    {
        connect(&m_notifier,&QWinEventNotifier::activated,this,[this]{
            std::vector<std::function<void()>> tasks;
            {
                const std::lock_guard<std::mutex> lock(m_state->mutex);
                // 清事件与领取固定批次共用锁；新任务只能在其后再次置位。
                ResetEvent(m_state->event);
                tasks.swap(m_state->tasks);
            }
            for(auto& task:tasks) {
                try {task();}
                catch (...) {std::fputs("Qt owner task failed\n",stderr);}
            }
        });
    }
    ~QtOwnerTaskQueue() override
    {
        m_notifier.setEnabled(false);
        const std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->stopped=true;
        m_state->tasks.clear();
    }
    std::function<bool(std::function<void()>)> GetPoster() const
    {
        const std::weak_ptr<State> weak=m_state;
        return [weak](std::function<void()> task) {
            const auto state=weak.lock();
            if(!state || !task)return false;
            const std::lock_guard<std::mutex> lock(state->mutex);
            if(state->stopped)return false;
            try {state->tasks.push_back(std::move(task));}
            catch (...) {return false;}
            if(SetEvent(state->event))return true;
            state->tasks.pop_back();return false;
        };
    }
};
}
