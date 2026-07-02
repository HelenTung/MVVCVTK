#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>

class AppTaskCallbackState
{
public:
    void SetCallback(std::function<void(bool)> callback) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_callback = std::move(callback);
    }

    void SetCallbackReady(bool success, std::function<void(bool)> callback = nullptr) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (callback) {
                m_pendingCallback = std::move(callback);
            }
            else if (m_callback) {
                callback = std::move(m_callback);
                m_callback = nullptr;
                m_pendingCallback = std::move(callback);
            }
            else {
                return;
            }
            m_pendingResult = success;
            m_hasPendingCallback = true;
        }
    }

    void ExecutePendingCallback() {
        std::function<void(bool)> callback;
        bool success = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            callback = std::move(m_pendingCallback);
            m_pendingCallback = nullptr;
            success = m_pendingResult;
        }
        if (callback) {
            callback(success);
        }
    }

    bool GetPendingCallbackConsumed() {
        return m_hasPendingCallback.exchange(false);
    }

private:
    mutable std::mutex m_mutex;
    std::function<void(bool)> m_callback; // 当前业务方注册但尚未触发的回调
    std::function<void(bool)> m_pendingCallback; // 已由后台任务准备好、等待主线程执行的回调
    bool m_pendingResult{ false }; // 与 m_pendingCallback 对应的任务结果快照
    std::atomic<bool> m_hasPendingCallback{ false }; // 后台线程只投递结果，主线程在心跳阶段统一执行回调
};
