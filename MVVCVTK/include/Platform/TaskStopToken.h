#pragma once

#include <atomic>
#include <memory>
#include <utility>

// C++17 下的协作式停止令牌。令牌只观察状态，不拥有任何任务或线程。
class TaskStopToken final {
public:
    TaskStopToken() = default;

    bool GetIsStopped() const noexcept
    {
        return m_isStopped
            && m_isStopped->load(std::memory_order_acquire);
    }

private:
    friend class TaskStopSource;

    explicit TaskStopToken(
        std::shared_ptr<std::atomic<bool>> isStopped) noexcept
        : m_isStopped(std::move(isStopped))
    {
    }

    std::shared_ptr<std::atomic<bool>> m_isStopped;
};

// Source 可复制以便执行器同时登记 queued/running 任务；所有副本控制同一状态。
class TaskStopSource final {
public:
    TaskStopSource()
        : m_isStopped(std::make_shared<std::atomic<bool>>(false))
    {
    }

    TaskStopToken GetToken() const noexcept
    {
        return TaskStopToken(m_isStopped);
    }

    bool Stop() noexcept
    {
        m_isStopped->store(true, std::memory_order_release);
        return true;
    }

private:
    std::shared_ptr<std::atomic<bool>> m_isStopped;
};
