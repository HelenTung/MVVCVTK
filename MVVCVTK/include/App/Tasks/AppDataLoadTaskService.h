#pragma once

#include "AppInterfaces.h"
#include "AppState.h"
#include "AppTaskCallbackState.h"
#include <array>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

// 加载链路需要先写 LoadState，再构造后台 I/O 任务，最后把回调延迟到主线程策略同步之后执行。
// 这里不直接启动线程，是为了让 VizService 继续统一托管 future 和 detach 策略；
// 这样加载细节可以拆出，同时主线程收口顺序仍只有一个编排入口。
class AppDataLoadTaskService
{
public:
    AppDataLoadTaskService(std::shared_ptr<AbstractDataManager> dataManager,
        std::shared_ptr<SharedInteractionState> sharedState);

    std::optional<std::packaged_task<void()>> BuildLoadFileTask(
        const std::string& path,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool isSuccess)> onComplete);

    std::optional<std::packaged_task<void()>> BuildReloadFromBufferTask(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool isSuccess)> onComplete);

    void SetFileLoadCallbackReady(bool isSuccess);
    void SetReloadReady(bool isSuccess);
    bool ResetFileCallback();
    bool ResetReloadCallback();
    void SendFileLoadCallback();
    void SendReloadCallback();

private:
    // 与构造出的 packaged_task 共享拥有 DataManager；保证后台 I/O 期间数据入口存活。
    std::shared_ptr<AbstractDataManager> m_dataManager;
    // 与后台任务共享拥有状态真源；任务只发布 load 终态，不直接触碰渲染对象。
    std::shared_ptr<SharedInteractionState> m_sharedState;
    // Build 阶段登记 callback，主线程完成 DataReady/失败收敛后生产结果，SendUpdates 领取并锁外执行。
    AppTaskCallbackState m_fileLoadCallbackState;
    // reload 的 pending image 提交完成后由主线程生产结果，SendUpdates 领取并锁外执行 submit/UI 回调。
    AppTaskCallbackState m_reloadLoadCallbackState;
};
