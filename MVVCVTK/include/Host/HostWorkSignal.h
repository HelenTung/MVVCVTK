#pragma once

#include <atomic>
#include <functional>
#include <utility>

// 纯工作门铃，不持有 Session identity。通知只负责排队；Stop 前已接纳的
// 在途通知允许晚到，接收方必须按自身生命周期拒绝失效的排队工作。
class HostWorkSignal final {
public:
    explicit HostWorkSignal(std::function<void()> onWorkAvailable)
        : m_onWorkAvailable(std::move(onWorkAvailable)) {}

    bool SendWorkAvailable() noexcept
    {
        auto expected = State::Idle;
        if (!m_state.compare_exchange_strong(expected, State::Pending)) {
            return expected == State::Pending;
        }
        try {
            if (m_onWorkAvailable) m_onWorkAvailable();
        }
        catch (...) {
            // 保留 Pending；宿主仍可显式 SendUpdates 恢复。
        }
        return true;
    }

    void SendUpdates() noexcept
    {
        auto expected = State::Pending;
        (void)m_state.compare_exchange_strong(expected, State::Idle);
    }

    void Stop() noexcept { m_state.store(State::Stopped); }

private:
    enum class State { Idle, Pending, Stopped };
    std::atomic<State> m_state{State::Idle};
    const std::function<void()> m_onWorkAvailable;
};
