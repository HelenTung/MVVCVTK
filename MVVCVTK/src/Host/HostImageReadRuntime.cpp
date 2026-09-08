#include "Host/Internal/HostImageReadRuntime.h"
#include "App/Services/AppServiceFactory.h"
#include "Data/DataService.h"
#include "Data/Internal/DataResourceUse.h"
#include <mutex>
#include <optional>
#include <utility>

struct HostImageReadRuntime::State final {
        struct ImageReadEntry final {
            ImageReadCallback callback;
            std::optional<ImageReadResult> result;
            bool isReady = false;
        };

        std::mutex mutex;
        std::shared_ptr<ImageReadEntry> imageRead;
        bool isActive = true;
    };


HostImageReadRuntime::HostImageReadRuntime() : m_state(std::make_shared<State>()) {}
HostImageReadRuntime::~HostImageReadRuntime() = default;

ImageReadAdmission HostImageReadRuntime::StartImageRead(
    const std::shared_ptr<AbstractDataManager>& data,
    const std::shared_ptr<AppTaskExecutor>& executor,
    ImageReadRequest request,
    ImageReadCallback onComplete)
{
    if (!onComplete) return ImageReadAdmission::InvalidRequest;
    if (!data || !executor) return ImageReadAdmission::Unavailable;
    VtkImageGridSnapshot input;
    std::shared_ptr<const DataResourceLease> readLease;
    std::shared_ptr<State::ImageReadEntry> entry;
    try {
        input = data->GetPrimaryImage();
        if (input) {
            auto lease = StartDataResourceUse(input->data, "queued-image-read");
            if (!lease) return ImageReadAdmission::Unavailable;
            readLease = std::move(*lease);
        }
        entry = std::make_shared<
            State::ImageReadEntry>();
        entry->callback = std::move(onComplete);
    }
    catch (...) {
        return ImageReadAdmission::Unavailable;
    }
    {
        const std::lock_guard<std::mutex> completeLock(
            m_state->mutex);
        if (!m_state->isActive) {
            return ImageReadAdmission::Stopping;
        }
        if (m_state->imageRead) {
            return ImageReadAdmission::Busy;
        }
        m_state->imageRead = entry;
    }

    const std::weak_ptr<State> weakComplete =
        m_state;
    AppTaskWork work;
    try {
        work = AppTaskWork([
            data,
            input = std::move(input),
            readLease = std::move(readLease),
            request = std::move(request),
            weakComplete,
            entry](const TaskStopToken stopToken) mutable {
            // callable 在完成结果消费前可能仍存活，工作结束即可归还源读取占用。
            const auto activeInput = std::move(input);
            const auto activeLease = std::move(readLease);
            ImageReadResult result;
            try {
                result = data->GetImageReadResult(
                    activeInput, request, stopToken);
            }
            catch (...) {
                result.error = stopToken.GetIsStopped()
                    ? ImageReadError::Cancelled
                    : ImageReadError::CopyFailed;
            }
            const auto complete = weakComplete.lock();
            if (!complete) return false;
            const std::lock_guard<std::mutex> completeLock(
                complete->mutex);
            if (!complete->isActive
                || complete->imageRead != entry) {
                return false;
            }
            entry->result = std::move(result);
            entry->isReady = true;
            return true;
        });
    }
    catch (...) {
        const std::lock_guard<std::mutex> completeLock(
            m_state->mutex);
        if (m_state->imageRead == entry) {
            m_state->imageRead.reset();
        }
        return ImageReadAdmission::Unavailable;
    }

    const auto admission = SendReadTask(executor, std::move(work));
    if (admission == TaskAdmissionResult::Accepted) {
        return ImageReadAdmission::Accepted;
    }
    {
        const std::lock_guard<std::mutex> completeLock(
            m_state->mutex);
        if (m_state->imageRead == entry) {
            m_state->imageRead.reset();
        }
    }
    switch (admission) {
    case TaskAdmissionResult::InvalidRequest:
        return ImageReadAdmission::InvalidRequest;
    case TaskAdmissionResult::Busy:
        return ImageReadAdmission::Busy;
    case TaskAdmissionResult::QueueFull:
        return ImageReadAdmission::QueueFull;
    case TaskAdmissionResult::Stopping:
        return ImageReadAdmission::Stopping;
    default:
        return ImageReadAdmission::Unavailable;
    }
}

void HostImageReadRuntime::SendComplete(
    const bool isStopping) noexcept
{
    ImageReadCallback callback;
    std::optional<ImageReadResult> result;
    if (m_state) {
        const std::lock_guard<std::mutex> lock(
            m_state->mutex);
        if (isStopping) m_state->isActive = false;
        const auto& entry = m_state->imageRead;
        if (!entry || (!m_state->isActive && !isStopping)) {
            return;
        }
        // StopLease 已停止并 join 共享 executor；若任务尚未来得及写入结果，
        // owner thread 在关闭 timer 前补齐 Cancelled 终态，Accepted 请求不会失去回调。
        if (isStopping && (!entry->isReady || !entry->result)) {
            ImageReadResult cancelled;
            cancelled.error = ImageReadError::Cancelled;
            entry->result = std::move(cancelled);
            entry->isReady = true;
        }
        if (entry->isReady && entry->result) {
            callback = std::move(entry->callback);
            result = std::move(entry->result);
            m_state->imageRead.reset();
        }
    }
    if (callback && result) {
        try {
            callback(std::move(*result));
        }
        catch (...) {
        }
    }
}
