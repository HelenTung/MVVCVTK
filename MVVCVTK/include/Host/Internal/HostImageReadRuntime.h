#pragma once
#include "Data/ImageReadTypes.h"
#include <functional>
#include <memory>
class AbstractDataManager;
class AppTaskExecutor;

// 只拥有读取请求槽；共享 executor 的停止与 join 仍归 Session。
class HostImageReadRuntime final {
public:
    HostImageReadRuntime();
    ~HostImageReadRuntime();
    HostImageReadRuntime(const HostImageReadRuntime&) = delete;
    HostImageReadRuntime& operator=(const HostImageReadRuntime&) = delete;
    ImageReadAdmission StartImageRead(const std::shared_ptr<AbstractDataManager>& data,
        const std::shared_ptr<AppTaskExecutor>& executor, ImageReadRequest request,
        std::function<void(ImageReadResult)> onComplete);
    void SendComplete(bool isStopping) noexcept;
private:
    struct State;
    std::shared_ptr<State> m_state;
};
