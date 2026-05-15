#pragma once

#include "AppTypes.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

using ObserverCallback = std::function<void(UpdateFlags)>;

struct ObserverEntry {
    std::weak_ptr<void> owner;
    ObserverCallback callback;
};

class IStateEventSink {
public:
    virtual ~IStateEventSink() = default;
    virtual void SetFlagsPublished(UpdateFlags flags) = 0;
};

class IStateEventSource {
public:
    virtual ~IStateEventSource() = default;
    virtual void SetObserver(std::shared_ptr<void> owner, ObserverCallback callback) = 0;
};

class SharedStateBroadcaster
    : public IStateEventSink
    , public IStateEventSource {
public:
    void SetObserver(std::shared_ptr<void> owner, ObserverCallback callback) override {
        if (!owner || !callback) {
            return;
        }

        std::lock_guard<std::mutex> lk(m_mutex);
        m_observers.erase(
            std::remove_if(m_observers.begin(), m_observers.end(),
                [](const ObserverEntry& entry) {
                    return entry.owner.expired();
                }),
            m_observers.end());
        m_observers.push_back({ std::move(owner), std::move(callback) });
    }

    void SetFlagsPublished(UpdateFlags flags) override {
        std::vector<ObserverCallback> callbacks;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            // 先在锁内清理失效观察者并复制回调，再在锁外执行回调。
            // 这样可以避免观察者在回调里继续写 SharedState / 重新订阅时造成锁递归或长时间持锁。
            for (auto it = m_observers.begin(); it != m_observers.end(); ) {
                if (it->owner.expired()) {
                    it = m_observers.erase(it);
                }
                else {
                    callbacks.push_back(it->callback);
                    ++it;
                }
            }
        }

        for (auto& callback : callbacks) {
            if (callback) {
                callback(flags);
            }
        }
    }

private:
    mutable std::mutex m_mutex;
    std::vector<ObserverEntry> m_observers;
};
