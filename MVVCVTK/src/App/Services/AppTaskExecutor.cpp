#include "App/Services/Internal/AppTaskExecutor.h"
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

// 每个 AppRuntime 只创建固定 worker；任务本身由 future 回传，worker 不捕获 runtime。
class AppTaskExecutor::Impl final {
    class TaskLane final {
        struct QueuedTask final {
            std::uint64_t id = 0;
            AppTaskWork work;
            TaskStopSource stopSource;
        };

    public:
        TaskLane(
            std::size_t workerCount,
            std::size_t taskLimit,
            const AppWorkerStart& workerStart,
            std::function<void()> onWorkAvailable)
            : m_taskLimit(taskLimit)
            , m_onWorkAvailable(std::move(onWorkAvailable))
            , m_workerStart(workerStart)
        {
            if (workerCount == 0 || taskLimit == 0
                || !m_workerStart) {
                throw std::invalid_argument(
                    "TaskLane requires workers and capacity.");
            }
            CreateWorkers(workerCount);
        }

        ~TaskLane()
        {
            (void)Stop(
                std::chrono::steady_clock::time_point::max());
        }

        TaskLane(const TaskLane&) = delete;
        TaskLane& operator=(const TaskLane&) = delete;

        TaskAdmissionResult SendTask(AppTaskWork work)
        {
            if (!work.valid()) {
                return TaskAdmissionResult::InvalidRequest;
            }

            std::uint64_t taskId = 0;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_isAccepting) {
                    return TaskAdmissionResult::Stopping;
                }
                if (m_stopSources.size() >= m_taskLimit) {
                    return TaskAdmissionResult::QueueFull;
                }
                if (m_nextId == 0
                    || m_nextId
                        == std::numeric_limits<std::uint64_t>::max()) {
                    if (!m_stopSources.empty()) {
                        return TaskAdmissionResult::QueueFull;
                    }
                    m_nextId = 1;
                }
                taskId = m_nextId++;
                TaskStopSource stopSource;
                try {
                    const auto inserted = m_stopSources.emplace(
                        taskId, stopSource);
                    try {
                        m_tasks.push_back({
                            taskId,
                            std::move(work),
                            std::move(stopSource)
                        });
                    }
                    catch (...) {
                        m_stopSources.erase(inserted.first);
                        return TaskAdmissionResult::Unavailable;
                    }
                }
                catch (...) {
                    return TaskAdmissionResult::Unavailable;
                }
            }
            m_ready.notify_one();
            return TaskAdmissionResult::Accepted;
        }

        bool StartStop()
        {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_isAccepting = false;
                m_isStopping = true;
                for (auto& entry : m_stopSources) {
                    (void)entry.second.Stop();
                }
                if (m_liveWorkers == 0) {
                    m_isStopped = true;
                }
            }
            m_ready.notify_all();
            m_stopped.notify_all();
            return true;
        }

        bool Stop(
            const std::chrono::steady_clock::time_point deadline)
        {
            (void)StartStop();
            bool isStopped = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                const auto stopped = [this] { return m_isStopped; };
                if (deadline
                    == std::chrono::steady_clock::time_point::max()) {
                    m_stopped.wait(lock, stopped);
                    isStopped = true;
                }
                else {
                    isStopped = m_stopped.wait_until(
                        lock, deadline, stopped);
                }
            }
            if (!isStopped) return false;

            const std::lock_guard<std::mutex> joinLock(m_joinMutex);
            for (auto& worker : m_workers) {
                if (worker.joinable()) worker.join();
            }
            return true;
        }

    private:
        void CreateWorkers(const std::size_t workerCount)
        {
            try {
                m_workers.reserve(workerCount);
                for (std::size_t index = 0;
                    index < workerCount; ++index) {
                    auto worker = m_workerStart(
                        [this] { OnWorker(); });
                    if (!worker.joinable()) {
                        throw std::runtime_error(
                            "Task worker did not start.");
                    }
                    {
                        const std::lock_guard<std::mutex> lock(m_mutex);
                        ++m_liveWorkers;
                    }
                    m_workers.push_back(std::move(worker));
                }
            }
            catch (...) {
                (void)StartStop();
                for (auto& worker : m_workers) {
                    if (worker.joinable()) worker.join();
                }
                throw;
            }
        }

        void OnWorker()
        {
            while (true) {
                QueuedTask task;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_ready.wait(lock, [this] {
                        return m_isStopping || !m_tasks.empty();
                    });
                    if (m_tasks.empty()) {
                        if (!m_isStopping) continue;
                        if (m_liveWorkers > 0) --m_liveWorkers;
                        if (m_liveWorkers == 0) {
                            m_isStopped = true;
                            lock.unlock();
                            m_stopped.notify_all();
                        }
                        return;
                    }
                    task = std::move(m_tasks.front());
                    m_tasks.pop_front();
                }

                try {
                    task.work(task.stopSource.GetToken());
                }
                catch (...) {
                    // packaged_task 已把业务异常写入 future；这里只隔离无效调用等框架异常。
                }

                {
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    m_stopSources.erase(task.id);
                }
                // future 已 ready，lane 容量已释放；解锁后才发纯工作门铃。
                try { if (m_onWorkAvailable) m_onWorkAvailable(); }
                catch (...) {}
            }
        }

        const std::size_t m_taskLimit;
        const std::function<void()> m_onWorkAvailable;
        AppWorkerStart m_workerStart;
        std::mutex m_mutex;
        std::condition_variable m_ready;
        std::condition_variable m_stopped;
        std::deque<QueuedTask> m_tasks;
        std::map<std::uint64_t, TaskStopSource> m_stopSources;
        std::vector<std::thread> m_workers;
        std::mutex m_joinMutex;
        std::uint64_t m_nextId = 1;
        std::size_t m_liveWorkers = 0;
        bool m_isAccepting = true;
        bool m_isStopping = false;
        bool m_isStopped = false;
    };

public:
    explicit Impl(AppWorkerStart workerStart,
        std::function<void()> onWorkAvailable)
        : m_workerStart(GetWorkerStart(std::move(workerStart)))
        , m_loadLane(1, 1, m_workerStart, onWorkAvailable)
        , m_exportLane(2, 2, m_workerStart, onWorkAvailable)
        , m_renderLane(1, 1, m_workerStart, std::move(onWorkAvailable))
    {
    }

    TaskAdmissionResult SendTask(
        AppTaskWork work,
        const LoadEventKind loadKind)
    {
        return loadKind == LoadEventKind::None
            ? m_exportLane.SendTask(std::move(work))
            : m_loadLane.SendTask(std::move(work));
    }

    TaskAdmissionResult SendRenderTask(AppTaskWork work)
    {
        return m_renderLane.SendTask(std::move(work));
    }

    bool StartDataStop()
    {
        const bool isLoadSet=m_loadLane.StartStop();
        const bool isExportSet=m_exportLane.StartStop();
        return isLoadSet&&isExportSet;
    }

    bool StartStop()
    {
        const bool isLoadSet = m_loadLane.StartStop();
        const bool isExportSet = m_exportLane.StartStop();
        const bool isRenderSet = m_renderLane.StartStop();
        return isLoadSet && isExportSet && isRenderSet;
    }

    bool Stop(
        const std::chrono::steady_clock::time_point deadline)
    {
        (void)StartStop();
        const bool isLoadStopped = m_loadLane.Stop(deadline);
        const bool isExportStopped = m_exportLane.Stop(deadline);
        const bool isRenderStopped = m_renderLane.Stop(deadline);
        return isLoadStopped && isExportStopped && isRenderStopped;
    }

private:
    static AppWorkerStart GetWorkerStart(
        AppWorkerStart workerStart)
    {
        if (workerStart) return workerStart;
        return [](AppWorkerWork work) {
            return std::thread(std::move(work));
        };
    }

    AppWorkerStart m_workerStart;
    TaskLane m_loadLane;
    TaskLane m_exportLane;
    TaskLane m_renderLane;
};


AppTaskExecutor::AppTaskExecutor(AppWorkerStart start, std::function<void()> wake)
    : m_impl(std::make_unique<Impl>(std::move(start), std::move(wake))) {}
AppTaskExecutor::~AppTaskExecutor() = default;
TaskAdmissionResult AppTaskExecutor::SendTask(AppTaskWork work, LoadEventKind kind) { return m_impl->SendTask(std::move(work),kind); }
TaskAdmissionResult AppTaskExecutor::SendRenderTask(AppTaskWork work) { return m_impl->SendRenderTask(std::move(work)); }
bool AppTaskExecutor::StartDataStop() { return m_impl->StartDataStop(); }
bool AppTaskExecutor::StartStop() { return m_impl->StartStop(); }
bool AppTaskExecutor::Stop(std::chrono::steady_clock::time_point deadline) { return m_impl->Stop(deadline); }

std::shared_ptr<AppTaskExecutor> CreateAppTaskExecutor(
    AppWorkerStart workerStart, std::function<void()> onWorkAvailable)
{
    return std::make_shared<AppTaskExecutor>(
        std::move(workerStart), std::move(onWorkAvailable));
}

TaskAdmissionResult SendReadTask(
    const std::shared_ptr<AppTaskExecutor>& executor,
    AppTaskWork work)
{
    return executor
        ? executor->SendTask(
            std::move(work), LoadEventKind::None)
        : TaskAdmissionResult::Unavailable;
}

bool SendRenderTask(
    const std::shared_ptr<AppTaskExecutor>& executor,
    RenderLaneWork work)
{
    return executor
        && executor->SendRenderTask(std::move(work))
            == TaskAdmissionResult::Accepted;
}
