#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace FeatureInternal {
// 被现有 Feature worker 消费；自身不创建线程。与算法队列共用锁和唤醒条件。
class ReadTaskQueue final {
public:
    using Cancel=std::function<bool()>;
    using Task=std::function<void(const Cancel&)>;
    struct Job final {Task task;std::shared_ptr<std::atomic<bool>> cancelled;};
    std::mutex mutex;
    std::condition_variable ready;
    std::function<void()> onPending;
    std::shared_ptr<std::atomic<bool>> Submit(Task task)
    {
        auto token=std::make_shared<std::atomic<bool>>(false);
        std::function<void()> notify;
        {const std::lock_guard<std::mutex> lock(mutex);
            if(m_stopping||m_pending.size()+(m_active?1:0)>=8)return {};
            notify=onPending;
            m_pending.push_back({std::move(task),token});
        }
        ready.notify_one();try{if(notify)notify();}catch(...){}return token;
    }
    bool GetIsStopping() {const std::lock_guard<std::mutex> lock(mutex);return m_stopping;}
    // 以下方法由拥有 mutex 的 worker/接管点调用。
    bool GetHasPendingLocked() const noexcept {return !m_pending.empty();}
    std::optional<Job> TakeLocked()
    {
        if(m_pending.empty()||m_stopping)return {};
        auto job=std::move(m_pending.front());m_pending.pop_front();m_active=job.cancelled;return job;
    }
    void FinishLocked() noexcept {m_active.reset();}
    void CancelLocked() noexcept
    {
        if(m_active)m_active->store(true,std::memory_order_release);
        for(auto& job:m_pending)job.cancelled->store(true,std::memory_order_release);
        m_pending.clear();
    }
    void StopLocked() noexcept {m_stopping=true;CancelLocked();onPending={};}
private:
    std::deque<Job> m_pending;
    std::shared_ptr<std::atomic<bool>> m_active;
    bool m_stopping=false;
};
} // namespace FeatureInternal
