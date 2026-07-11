#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>

// 后台任务完成时不能直接调用 UI / VTK 相关回调，因为调用方常常假设回调运行在主线程。
// 这个状态对象把“登记回调、后台投递结果、主线程消费执行”拆成三步：
// 1. 业务入口登记回调；
// 2. 后台线程只移动回调和结果快照，不执行未知用户代码；
// 3. 更新消费线程用 exchange 领取一次 pending 位，再在锁外执行回调；本对象不负责切换线程。
class AppTaskCallbackState
{
public:
    void SetCallback(std::function<void(bool)> callback) {
        std::lock_guard<std::mutex> lk(m_mutex);
        // 加载任务先登记闭包，完成线程随后通过无显式 callback 的 SetCallbackReady() 领取它。
        m_callback = std::move(callback);
    }

    void SetCallbackReady(bool isSuccess, std::function<void(bool)> callback = nullptr) {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!callback) {
            // A. 未随完成结果携带闭包时，必须从同一互斥区领取先前登记的任务闭包。
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

        // 闭包与结果先在互斥区内组成完整 payload，再发布原子门铃；消费者看到门铃后会在此锁上接管。
        m_isNextOk = isSuccess;
        m_hasPendingCallback = true;
    }

    void SendCallback() {
        std::function<void(bool)> callback;
        bool isSuccess = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            // 一次接管当前 pending 闭包和对应结果；新的完成投递会留在槽中等待下一次 Send。
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
        // 这里只领取可轮询的门铃，不在原子位上承载 callback 或结果 payload。
        return m_hasPendingCallback.exchange(false);
    }

private:
    // 同时保护 active/pending callback 与 m_isNextOk；原子门铃只发布“有 payload”，不替代此锁。
    mutable std::mutex m_mutex;
    // 业务方已登记但后台任务尚未领取的闭包，仅在 m_mutex 内移动。
    std::function<void(bool)> m_callback;
    // 完成线程已投递、等待更新消费线程接管的单闭包或按完成顺序串接的组合闭包。
    std::function<void(bool)> m_pendingCallback;
    // 单闭包使用当前结果；组合闭包把每个任务的结果捕获进各自 closure，不共用此值。
    bool m_isNextOk{ false };
    // 完成线程发布，更新消费线程 ResetCallback() 清零；payload 始终由 m_mutex 保护。
    std::atomic<bool> m_hasPendingCallback{ false };
};
