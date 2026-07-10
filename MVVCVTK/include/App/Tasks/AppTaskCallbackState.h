#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>

// 后台任务完成时不能直接调用 UI / VTK 相关回调，因为调用方常常假设回调运行在主线程。
// 这个状态对象把“登记回调、后台投递结果、主线程消费执行”拆成三步：
// 1. 业务入口登记回调；
// 2. 后台线程只移动回调和结果快照，不执行未知用户代码；
// 3. 主线程心跳用 exchange 消费一次 pending 位，再在锁外执行回调。
class AppTaskCallbackState
{
public:
    void SetCallback(std::function<void(bool)> callback) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_callback = std::move(callback);
    }

    void SetCallbackReady(bool isSuccess, std::function<void(bool)> callback = nullptr) {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!callback) {
            if (!m_callback) {
                return;
            }
            callback = std::move(m_callback);
            m_callback = nullptr;
        }

        if (m_pendingCallback) {
            // 1. 旧闭包和它的结果先按值快照，避免后一个后台任务覆盖前一个结果。
            // 2. 组合闭包按完成顺序调用，且每个业务回调只接收自己的结果。
            // 3. 仍只占用现有 pending 槽，主线程继续在锁外一次性执行全部回调。
            auto priorCallback = std::move(m_pendingCallback);
            const bool isPriorOk = m_isNextOk;
            m_pendingCallback = [
                priorCallback = std::move(priorCallback),
                isPriorOk,
                callback = std::move(callback),
                isSuccess](bool) mutable
            {
                priorCallback(isPriorOk);
                callback(isSuccess);
            };
        }
        else {
            m_pendingCallback = std::move(callback);
        }

        m_isNextOk = isSuccess;
        m_hasPendingCallback = true;
    }

    void SendCallback() {
        std::function<void(bool)> callback;
        bool isSuccess = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            callback = std::move(m_pendingCallback);
            m_pendingCallback = nullptr;
            isSuccess = m_isNextOk;
        }
        // 回调可能反向触发 UI、状态或服务调用；锁外执行能避免业务回调重入时和本状态对象死锁。
        if (callback) {
            callback(isSuccess);
        }
    }

    bool ResetCallback() {
        return m_hasPendingCallback.exchange(false);
    }

private:
    mutable std::mutex m_mutex;
    std::function<void(bool)> m_callback; // 当前业务方注册但尚未触发的回调
    std::function<void(bool)> m_pendingCallback; // 已由后台任务准备好、等待主线程执行的回调或组合闭包
    bool m_isNextOk{ false }; // 单回调的结果快照；组合闭包自行保存每个任务的结果
    std::atomic<bool> m_hasPendingCallback{ false }; // 后台线程只投递结果，主线程在心跳阶段统一执行回调
};
