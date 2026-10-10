#pragma once
#include "SurfaceCudaTypes.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <system_error>

struct SurfaceGpuFinalizedBatch final {
    std::size_t accepted = 0;
    double cpuMs = 0;
};

// 仅持有普通 CPU 样本副本；CUDA ticket、pinned 槽和上下文始终属于调用线程。
// Callback 只读输入、写独占输出区间；其生命周期覆盖本对象及全部 worker。
template<class Callback>
class SurfaceGpuFinalizer final {
public:
    static constexpr std::size_t sampleBytes =
        surfaceCudaSlotCount * surfaceCudaBatchCapacity * sizeof(SurfaceCudaSample);
    SurfaceGpuFinalizer(unsigned requested, const Callback &callback) : m_callback(callback),
        m_samples(std::make_unique<SurfaceCudaSample[]>(surfaceCudaSlotCount * surfaceCudaBatchCapacity))
    {
        for (; m_count < std::min(requested, surfaceCudaSlotCount); ++m_count) {
            try { m_workers[m_count] = std::thread([this] { Run(); }); }
            catch (const std::system_error &) { break; }
            catch (const std::bad_alloc &) { break; }
        }
    }
    ~SurfaceGpuFinalizer() { Stop(); }
    SurfaceGpuFinalizer(const SurfaceGpuFinalizer &) = delete;
    SurfaceGpuFinalizer &operator=(const SurfaceGpuFinalizer &) = delete;
    unsigned GetThreadCount() const noexcept { return m_count; }

    // 同槽上一任务必须先 Finish；之后复制样本，允许调用方立即重用 CUDA 槽。
    void Submit(unsigned slot, std::size_t begin, std::size_t count, const SurfaceCudaSample *samples)
    {
        auto *copy = m_samples.get() + slot * surfaceCudaBatchCapacity;
        if (samples) std::memcpy(copy, samples, count * sizeof(SurfaceCudaSample));
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto &task = m_tasks[slot];
            task.begin = begin; task.count = count; task.hasSamples = samples != nullptr;
            task.state = State::Ready;
        }
        m_changed.notify_all();
    }
    SurfaceGpuFinalizedBatch Finish(unsigned slot)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        auto &task = m_tasks[slot];
        if (task.state == State::Free) return {};
        m_changed.wait(lock, [&] { return task.state == State::Done; });
        const auto result = task.result;
        const auto failure = task.failure;
        task = {};
        lock.unlock();
        if (failure) std::rethrow_exception(failure);
        return result;
    }
    // 正常路径先 Finish 全部槽；异常/取消路径停止尚未领取的任务并等待运行中任务退出。
    void Stop() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopped = true;
        }
        m_changed.notify_all();
        for (auto &worker : m_workers) if (worker.joinable()) worker.join();
    }
private:
    enum class State { Free, Ready, Running, Done };
    struct Task final {
        State state = State::Free;
        std::size_t begin = 0, count = 0;
        bool hasSamples = false;
        SurfaceGpuFinalizedBatch result;
        std::exception_ptr failure;
    };
    void Run() noexcept
    {
        for (;;) {
            unsigned slot = 0;
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [&] {
                return m_stopped || std::any_of(m_tasks.begin(), m_tasks.end(),
                    [](const auto &task) { return task.state == State::Ready; });
            });
            if (m_stopped) return;
            while (m_tasks[slot].state != State::Ready) ++slot;
            auto &task = m_tasks[slot];
            task.state = State::Running;
            lock.unlock();
            SurfaceGpuFinalizedBatch result;
            std::exception_ptr failure;
            try {
                result = m_callback(task.begin, task.count,
                    task.hasSamples ? m_samples.get() + slot * surfaceCudaBatchCapacity : nullptr);
            } catch (...) { failure = std::current_exception(); }
            lock.lock();
            task.result = result; task.failure = failure; task.state = State::Done;
            lock.unlock();
            m_changed.notify_all();
        }
    }
    const Callback &m_callback;
    std::unique_ptr<SurfaceCudaSample[]> m_samples;
    std::array<Task, surfaceCudaSlotCount> m_tasks;
    std::array<std::thread, surfaceCudaSlotCount> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    unsigned m_count = 0;
    bool m_stopped = false;
};
