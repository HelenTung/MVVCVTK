#pragma once

#include "AppTypes.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

using ObserverCallback = std::function<void(UpdateFlags)>; // 状态广播回调：只传递增量标志，不直接暴露 SharedState 内部数据

struct ObserverEntry {
    std::weak_ptr<void> owner; // 非拥有生命周期锚点；失效条目在注册或广播时清理
    ObserverCallback callback; // 广播层持有的可调用对象副本；复制出锁后执行
};

class IStateEventSink {
public:
    virtual ~IStateEventSink() = default;
    // SharedInteractionState 通过该接口向外发布“有哪些状态发生了变化”。
    virtual void SendFlags(UpdateFlags flags) = 0;
};

class IStateEventSource {
public:
    virtual ~IStateEventSource() = default;
    // Service / 其他观察者通过该接口订阅状态变化，不直接依赖 SharedInteractionState 具体实现。
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
                [&owner](const ObserverEntry& entry) {
                    auto currentOwner = entry.owner.lock();
                    return !currentOwner || currentOwner == owner;
                }),
            m_observers.end());
        m_observers.push_back({ std::move(owner), std::move(callback) });
    }

    void SendFlags(UpdateFlags flags) override {
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
                try { callback(flags); }
                catch (...) {
                    // 观察者异常只影响自身，后续观察者仍必须收到同一终态。
                }
            }
        }
    }

private:
    // 只保护 m_observers 的注册、替换和失效清理；业务 callback 不在此锁内执行。
    mutable std::mutex m_mutex;
    // SetObserver 生产条目；同 owner 的旧条目被替换，owner 失效后由后续注册或广播清除。
    std::vector<ObserverEntry> m_observers;
};
