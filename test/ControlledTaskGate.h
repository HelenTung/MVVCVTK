// 测试门闩只控制只读数据源的返回顺序，不改变产品取消或发布逻辑。
#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace TaskInterleaving {
// 沿用 RoiEditingTests::TestWorkerDestruction 的 10 秒同步保护。
// 这是失效时释放门闩的安全期限，不是产品响应时延指标；断言比较返回与释放的先后。
inline constexpr auto synchronizationTimeout = std::chrono::seconds(10);

class Gate final {
public:
    void Arm() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_armed = true;
    }
    void Hold() {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_armed || m_released) return;
        m_entered = true;
        m_changed.notify_all();
        m_changed.wait(lock, [this] { return m_released; });
    }
    bool WaitEntered() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, synchronizationTimeout, [this] { return m_entered; });
    }
    void Release() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_changed.notify_all();
    }
    bool GetIsHeld() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_entered && !m_released;
    }
private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_armed = false;
    bool m_entered = false;
    bool m_released = false;
};

class ReleaseOnExit final {
public:
    explicit ReleaseOnExit(std::shared_ptr<Gate> gate) : m_gate(std::move(gate)) {}
    ~ReleaseOnExit() { m_gate->Release(); }
    ReleaseOnExit(const ReleaseOnExit&) = delete;
    ReleaseOnExit& operator=(const ReleaseOnExit&) = delete;
private:
    std::shared_ptr<Gate> m_gate;
};

struct OwnerCallObservation final {
    bool returnedWhileHeld = false;
    double elapsedMs = 0;
};

// 调用仍在 owner 线程；旁观线程仅等待“已返回”，在安全期限后释放数据读取。
// 正常非等待实现会在门闩仍关闭时返回，因此此断言不依赖机器运行速度。
inline OwnerCallObservation ObserveOwnerCall(const std::shared_ptr<Gate>& gate,
    const std::function<void()>& call)
{
    std::promise<void> entered, returned;
    auto enteredFuture = entered.get_future();
    auto returnedFuture = returned.get_future();
    OwnerCallObservation observation;
    std::thread release([&] {
        enteredFuture.wait();
        observation.returnedWhileHeld = returnedFuture.wait_for(synchronizationTimeout)
            == std::future_status::ready;
        gate->Release();
    });
    const auto started = std::chrono::steady_clock::now();
    entered.set_value();
    std::exception_ptr error;
    try { call(); } catch (...) { error = std::current_exception(); }
    observation.elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    returned.set_value();
    release.join();
    if (error) std::rethrow_exception(error);
    return observation;
}
} // namespace TaskInterleaving
