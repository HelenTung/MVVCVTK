#pragma once
#include "App/Services/AppServiceFactory.h"

// Host 私有固定任务 lane；worker 生命周期与 AppRuntime 状态独立。
class AppTaskExecutor final {
public:
    AppTaskExecutor(AppWorkerStart, std::function<void()>);
    ~AppTaskExecutor();
    TaskAdmissionResult SendTask(AppTaskWork, LoadEventKind);
    TaskAdmissionResult SendRenderTask(AppTaskWork);
    bool StartDataStop();
    bool StartStop();
    bool Stop(std::chrono::steady_clock::time_point);
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
